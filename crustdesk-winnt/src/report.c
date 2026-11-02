/* The console reporter (see report.h): the IRIX port's, as a thread. Its
 * policy is the Mac's: one connection kept open across heartbeats (a
 * handshake is seconds of arithmetic), one retry on a fresh connection when
 * a kept one has gone stale, a failure logged once until it clears. */
#include "report.h"
#include "log.h"
#include "plat.h"
#include "console.h"
#include "https.h"
#include "sha256.h"

#include "win.h"
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONNECT_SECS 15
#define REQUEST_SECS 120 /* a first request carries the handshake */

static report_config C;
static console_url U;
static cdv_https H;
static uint8_t iobuf[HTTPS_IOBUF];
static SOCKET fd = INVALID_SOCKET;
static uint32_t counter;
static HANDLE thread, stop_ev;

/* ---- what the machine is ----------------------------------------------------- */

static int reg_string(HKEY root, const char *key, const char *name, char *out, DWORD cap)
{
    HKEY k;
    DWORD type, len = cap;
    LONG r;
    if (RegOpenKeyEx(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    r = RegQueryValueEx(k, name, NULL, &type, (LPBYTE)out, &len);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ)
        return 0;
    out[cap - 1] = 0;
    return 1;
}

static int reg_dword(HKEY root, const char *key, const char *name, DWORD *out)
{
    HKEY k;
    DWORD type, len = sizeof *out;
    LONG r;
    if (RegOpenKeyEx(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    r = RegQueryValueEx(k, name, NULL, &type, (LPBYTE)out, &len);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_DWORD;
}

void report_describe(char *cpu, size_t ncpu, char *mem, size_t nmem, char *os, size_t nos)
{
    static const char cpukey[] = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    SYSTEM_INFO si;
    MEMORYSTATUS ms;
    OSVERSIONINFO vi;
    char ident[96], *arch;
    DWORD mhz = 0;
    BOOL vok;
    GetSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
    case PROCESSOR_ARCHITECTURE_MIPS: arch = "MIPS"; break;
    case PROCESSOR_ARCHITECTURE_ALPHA: arch = "Alpha"; break;
    case PROCESSOR_ARCHITECTURE_PPC: arch = "PowerPC"; break;
#ifdef PROCESSOR_ARCHITECTURE_ARM
    /* PINT's headers: NT 4.0 for ARM on the Raspberry Pi */
    case PROCESSOR_ARCHITECTURE_ARM: arch = "ARM"; break;
#endif
    default: arch = "unknown"; break;
    }
    if (!reg_string(HKEY_LOCAL_MACHINE, cpukey, "Identifier", ident, sizeof ident))
        strcpy(ident, arch);
    reg_dword(HKEY_LOCAL_MACHINE, cpukey, "~MHz", &mhz);
    if (si.dwNumberOfProcessors > 1)
        snprintf(cpu, ncpu, "%lu x %s%s%s", si.dwNumberOfProcessors, strncmp(ident, arch, strlen(arch)) ? arch : "",
                 strncmp(ident, arch, strlen(arch)) ? " " : "", ident);
    else
        snprintf(cpu, ncpu, "%s%s%s", strncmp(ident, arch, strlen(arch)) ? arch : "",
                 strncmp(ident, arch, strlen(arch)) ? " " : "", ident);
    if (mhz) {
        size_t n = strlen(cpu);
        snprintf(cpu + n, ncpu - n, ", %lu MHz", mhz);
    }
    ms.dwLength = sizeof ms;
    GlobalMemoryStatus(&ms);
    snprintf(mem, nmem, "%lu MB", (unsigned long)((ms.dwTotalPhys + 512 * 1024) >> 20));
    vi.dwOSVersionInfoSize = sizeof vi;
    vok = GetVersionEx(&vi);
    if (vok && vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
        snprintf(os, nos, "Windows %s (%lu.%lu.%lu%s)",
                 vi.dwMinorVersion >= 90 ? "Me" : vi.dwMinorVersion >= 10 ? "98" : "95",
                 vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber & 0xffff,
                 vi.szCSDVersion);
    else if (vok)
        snprintf(os, nos, "Windows NT %lu.%lu (build %lu%s%s) %s", vi.dwMajorVersion,
                 vi.dwMinorVersion, vi.dwBuildNumber & 0xffff, vi.szCSDVersion[0] ? ", " : "",
                 vi.szCSDVersion, arch);
    else
        snprintf(os, nos, "Windows NT %s", arch);
}

/* ---- the clock, for certificate dates ------------------------------------------ */

/* __DATE__: "Oct 29 2026". A clock before the build is a wrong clock, and the
 * build date the better guess: certificates valid today were valid then. */
static uint32_t build_days(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    char mon[4];
    int m, day, y;
    memcpy(mon, d, 3);
    mon[3] = 0;
    m = (int)((strstr(months, mon) - months) / 3) + 1;
    day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
    y = atoi(d + 7);
    return https_days(y, m, day);
}

static void utc_now(uint32_t *days, uint32_t *secs)
{
    SYSTEMTIME t;
    uint32_t floor = build_days();
    GetSystemTime(&t);
    *days = https_days(t.wYear, t.wMonth, t.wDay);
    *secs = (uint32_t)(t.wHour * 3600 + t.wMinute * 60 + t.wSecond);
    if (*days < floor) {
        static int told;
        if (!told)
            cdv_log("console: the clock is before this build; using the build date");
        told = 1;
        *days = floor;
        *secs = 0;
    }
}

/* Random bytes for TLS: a secret only this machine has, a counter, the
 * clocks. (rng.c belongs to the agent's thread.) */
static void tls_seed(uint8_t out[32])
{
    sha256_ctx c;
    LARGE_INTEGER pc;
    FILETIME ft;
    uint32_t v[4];
    QueryPerformanceCounter(&pc);
    GetSystemTimeAsFileTime(&ft);
    v[0] = ++counter;
    v[1] = pc.LowPart;
    v[2] = ft.dwLowDateTime;
    v[3] = GetTickCount();
    sha256_init(&c);
    sha256_update(&c, C.secret, 32);
    sha256_update(&c, (const uint8_t *)"cdv tls seed", 12);
    sha256_update(&c, (const uint8_t *)v, sizeof v);
    sha256_final(&c, out);
}

/* ---- the connection -------------------------------------------------------------- */

static void drop(void)
{
    if (fd != INVALID_SOCKET)
        closesocket(fd);
    fd = INVALID_SOCKET;
    https_disconnected(&H);
}

static int dial(char *why, size_t cap)
{
    struct hostent *he;
    struct sockaddr_in sa;
    unsigned long a = inet_addr(U.host), nb = 1;
    fd_set wr, ex;
    struct timeval tv;
    uint32_t days, secs;
    uint8_t seed[32];
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(U.port);
    if (a != INADDR_NONE) {
        sa.sin_addr.s_addr = a;
    } else {
        he = gethostbyname(U.host);
        if (!he || he->h_length != 4) {
            snprintf(why, cap, "cannot resolve %s", U.host);
            return 0;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        snprintf(why, cap, "no socket (%d)", WSAGetLastError());
        return 0;
    }
    ioctlsocket(fd, FIONBIO, &nb);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0 &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        snprintf(why, cap, "cannot connect to %s (%d)", U.host, WSAGetLastError());
        drop();
        return 0;
    }
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(fd, &wr);
    FD_SET(fd, &ex);
    tv.tv_sec = CONNECT_SECS;
    tv.tv_usec = 0;
    if (select(0, NULL, &wr, &ex, &tv) <= 0 || FD_ISSET(fd, &ex)) {
        snprintf(why, cap, "cannot connect to %s", U.host);
        drop();
        return 0;
    }
    utc_now(&days, &secs);
    tls_seed(seed);
    https_connected(&H, U.secure, U.host, iobuf, sizeof iobuf, days, secs, seed);
    return 1;
}

/* Move bytes until the request is answered, fails, or runs out of time. */
static int exchange(void)
{
    DWORD end = GetTickCount() + REQUEST_SECS * 1000;
    for (;;) {
        const uint8_t *o;
        uint8_t *in;
        size_t n;
        fd_set rd, wr;
        struct timeval tv;
        int st = https_poll(&H), want_w, want_r;
        long left;
        if (st != HS_BUSY)
            return st;
        left = (long)(end - GetTickCount());
        if (left <= 0 || WaitForSingleObject(stop_ev, 0) == WAIT_OBJECT_0)
            return -1;
        want_w = https_out(&H, &o) != 0;
        want_r = https_in(&H, &in) != 0;
        if (!want_w && !want_r)
            return -1; /* neither side can move: a bug */
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        if (want_w)
            FD_SET(fd, &wr);
        if (want_r)
            FD_SET(fd, &rd);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select(0, &rd, &wr, NULL, &tv) <= 0)
            continue;
        if (FD_ISSET(fd, &wr)) {
            n = https_out(&H, &o);
            if (n) {
                int w = send(fd, (const char *)o, (int)n, 0);
                if (w < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                    return -1;
                if (w > 0)
                    https_out_done(&H, (size_t)w);
            }
        }
        if (FD_ISSET(fd, &rd)) {
            n = https_in(&H, &in);
            if (n) {
                int r = recv(fd, (char *)in, (int)n, 0);
                if (r == 0 || (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK))
                    https_eof(&H);
                else if (r > 0)
                    https_in_done(&H, (size_t)r);
            }
        }
    }
}

/* ---- the conversation ------------------------------------------------------------ */

/* After a failure: 15 s, doubling to 5 minutes. */
static DWORD backoff(int *fails)
{
    DWORD d = (CONSOLE_INTERVAL_MS / 1000) << (*fails < 5 ? *fails : 5);
    if (d > 300)
        d = 300;
    ++*fails;
    return d * 1000;
}

static DWORD WINAPI run(LPVOID unused)
{
    char body[512], line[160], why[96];
    DWORD wait = 0, t0;
    int want_sysinfo = 1, fault = 0, listed = 0, retried = 0, fresh = 0, fails = 0, st;
    const char *path;
    (void)unused;
    for (;;) {
        if (WaitForSingleObject(stop_ev, wait) == WAIT_OBJECT_0)
            break;
        wait = 0;
        if (fd == INVALID_SOCKET) {
            if (!dial(why, sizeof why)) {
                if (!fault)
                    cdv_log("console: %s", why);
                fault = 1;
                wait = backoff(&fails);
                continue;
            }
            fresh = 1;
        }
        if (want_sysinfo) {
            char cpu[96], mem[16], os[96];
            report_describe(cpu, sizeof cpu, mem, sizeof mem, os, sizeof os);
            if (!listed)
                cdv_log("console: sending the inventory: %s; %s; %s; %s, user %s", cpu, mem, os,
                        C.hostname, C.username());
            console_sysinfo_json(body, sizeof body, C.id, C.uuid, cpu, mem, os, C.hostname,
                                 C.username());
            path = "/api/sysinfo";
        } else {
            console_heartbeat_json(body, sizeof body, C.id, C.uuid);
            path = "/api/heartbeat";
        }
        snprintf(line, sizeof line, "%s%s", U.prefix, path);
        t0 = GetTickCount();
        if (!https_post(&H, U.host, line, body)) {
            cdv_log("console: the %s request does not fit", path);
            break;
        }
        st = exchange();
        if (st != HS_DONE) {
            int tls = st == HS_FAILED && H.err == HE_TLS;
            drop();
            if (WaitForSingleObject(stop_ev, 0) == WAIT_OBJECT_0)
                break;
            /* a kept connection the server has since closed: once more, fresh */
            if (!retried && !fresh && !tls) {
                retried = 1;
                continue;
            }
            if (!fault) {
                if (tls)
                    cdv_log("console: TLS failed (BearSSL error %d)", H.tls_err);
                else
                    cdv_log("console: %s got no answer (%s after %lu s)", path,
                            st < 0 ? "timed out" : H.err == HE_CLOSED ? "closed" : "failed",
                            (GetTickCount() - t0) / 1000);
            }
            fault = 1;
            retried = 0;
            wait = backoff(&fails);
            continue;
        }
        retried = 0;
        if (H.status < 200 || H.status >= 300) {
            if (!fault)
                cdv_log("console: %s answered %d%s%.60s", path, H.status,
                        H.location[0] ? ", moved to " : "", H.location);
            fault = 1;
            drop();
            wait = backoff(&fails);
            continue;
        }
        if (fault)
            cdv_log("console: reachable again");
        fault = 0;
        fails = 0;
        if (fresh)
            cdv_log("console: connected to %s%s, first answer in %lu s", U.host,
                    U.secure ? " over TLS" : "", (GetTickCount() - t0) / 1000);
        fresh = 0;
        if (!strcmp(path, "/api/sysinfo")) {
            want_sysinfo = 0;
            if (!strncmp(H.body, "SYSINFO_UPDATED", 15)) {
                if (!listed)
                    cdv_log("console: in the device list");
                listed = 1;
            } else {
                cdv_log("console: inventory answer %.40s", H.body);
            }
        } else if (console_wants_sysinfo(H.body)) {
            want_sysinfo = 1;
        } else {
            wait = CONSOLE_INTERVAL_MS;
        }
        if (https_needs_reconnect(&H))
            drop();
    }
    drop();
    return 0;
}

int report_start(const report_config *c)
{
    if (!console_parse_url(c->url, &U)) {
        cdv_log("console: api=%s is not a URL", c->url);
        return 0;
    }
    C = *c;
    stop_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    thread = plat_thread(run, NULL);
    if (!thread) {
        cdv_log("console: cannot start the reporter (error %lu)", GetLastError());
        return 0;
    }
    /* Below the agent: a handshake busies the CPU for seconds, and a peer
     * connecting meanwhile gives up if the agent does not answer in one. */
    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    return 1;
}

void report_stop(void)
{
    if (!thread)
        return;
    SetEvent(stop_ev);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    CloseHandle(stop_ev);
    thread = NULL;
}
