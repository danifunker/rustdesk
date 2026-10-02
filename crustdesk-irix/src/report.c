/* The console reporter: a child process that keeps the machine in a
 * console's device list (see report.h). The protocol and TLS are the shared
 * core's (console.c, https.c); this is the transport, written the plain way
 * -- blocking, with timeouts -- because it has a process to itself.
 *
 * The Mac does the same from its event loop (src/mac/report.c there), and its
 * policy is kept: one connection kept open across heartbeats (a handshake is
 * seconds of arithmetic), one retry on a fresh connection when a kept one has
 * gone stale, a failure logged once until it clears.
 */
#include "report.h"

#include "log.h"
#include "console.h"
#include "https.h"
#include "rng.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <invent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysmp.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#define CONNECT_SECS 15
#define REQUEST_SECS 120 /* a first request carries the handshake */

static report_config C;
static console_url U;
static cdv_https H;
static uint8_t iobuf[HTTPS_IOBUF];
static int fd = -1;
static uint32_t counter;

/* ---- what the machine is ----------------------------------------------------- */

static const char *chip_name(int state, char *buf, size_t cap)
{
    int imp = state >> 8 & 255, rev = state & 255;
    switch (imp) {
    case 0x01: return "R2000";
    case 0x02: return "R3000";
    case 0x04: return rev >= 0x40 ? "R4400" : "R4000";
    case 0x09: return "R10000";
    case 0x0e: return "R12000";
    case 0x10: return "R8000";
    case 0x20: return "R4600";
    case 0x21: return "R4700";
    case 0x23: return "R5000";
    case 0x27: return "RM7000";
    }
    snprintf(buf, cap, "MIPS (0x%x)", imp);
    return buf;
}

void report_describe(char *cpu, size_t ncpu, char *mem, size_t nmem, char *os, size_t nos)
{
    inventory_t *inv;
    struct utsname un;
    char buf[24];
    const char *chip = NULL;
    int mhz = 0, ncpus = (int)sysmp(MP_NPROCS);
    long mb = 0;
    setinvent();
    while ((inv = getinvent()) != NULL) {
        if (inv->inv_class == INV_PROCESSOR && inv->inv_type == INV_CPUBOARD) {
            mhz = (int)inv->inv_controller;
        } else if (inv->inv_class == INV_PROCESSOR && inv->inv_type == INV_CPUCHIP && !chip) {
            chip = chip_name(inv->inv_state, buf, sizeof buf);
        } else if (inv->inv_class == INV_MEMORY && inv->inv_type == INV_MAIN_MB) {
            mb = inv->inv_state;
        } else if (inv->inv_class == INV_MEMORY && inv->inv_type == INV_MAIN && !mb) {
            mb = (long)((unsigned long)inv->inv_state >> 20);
        }
    }
    endinvent();
    if (!chip)
        chip = "MIPS";
    if (ncpus > 1 && mhz)
        snprintf(cpu, ncpu, "%d x MIPS %s, %d MHz", ncpus, chip, mhz);
    else if (mhz)
        snprintf(cpu, ncpu, "MIPS %s, %d MHz", chip, mhz);
    else
        snprintf(cpu, ncpu, "MIPS %s", chip);
    if (mb > 0)
        snprintf(mem, nmem, "%ld MB", mb);
    else
        mem[0] = 0;
    if (uname(&un) >= 0)
        snprintf(os, nos, "%s %s", un.sysname, un.release);
    else
        snprintf(os, nos, "IRIX");
}

/* ---- the clock, for certificate dates ------------------------------------------ */

/* __DATE__: "Oct  2 2026". A clock before the build is a wrong clock, and the
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
    time_t t = time(NULL);
    struct tm *g = gmtime(&t);
    uint32_t floor = build_days();
    *days = https_days(g->tm_year + 1900, g->tm_mon + 1, g->tm_mday);
    *secs = (uint32_t)(g->tm_hour * 3600 + g->tm_min * 60 + g->tm_sec);
    if (*days < floor) {
        static int told;
        if (!told)
            cdv_log("console: the clock is before this build; using the build date");
        told = 1;
        *days = floor;
        *secs = 0;
    }
}

/* Random bytes for TLS: a secret only this machine has, a counter, the time
 * and the process -- the parent's random state was copied into us at fork. */
static void tls_seed(uint8_t out[32])
{
    sha256_ctx c;
    struct timeval tv;
    uint32_t v[4];
    gettimeofday(&tv, 0);
    v[0] = ++counter;
    v[1] = (uint32_t)tv.tv_sec;
    v[2] = (uint32_t)tv.tv_usec;
    v[3] = (uint32_t)getpid();
    sha256_init(&c);
    sha256_update(&c, C.secret, 32);
    sha256_update(&c, (const uint8_t *)"cdv tls seed", 12);
    sha256_update(&c, (const uint8_t *)v, sizeof v);
    sha256_final(&c, out);
}

/* ---- the connection -------------------------------------------------------------- */

static void drop(void)
{
    if (fd >= 0)
        close(fd);
    fd = -1;
    https_disconnected(&H);
}

static int dial(char *why, size_t cap)
{
    struct hostent *he;
    struct sockaddr_in sa;
    struct pollfd p;
    unsigned long a = inet_addr(U.host);
    int err = 0, len = sizeof err;
    uint32_t days, secs;
    uint8_t seed[32];
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(U.port);
    if (a != (unsigned long)-1 && a != 0xffffffffUL) {
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
    if (fd < 0) {
        snprintf(why, cap, "no socket: %s", strerror(errno));
        return 0;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 && errno != EINPROGRESS) {
        snprintf(why, cap, "cannot connect to %s: %s", U.host, strerror(errno));
        drop();
        return 0;
    }
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    if (poll(&p, 1, CONNECT_SECS * 1000) <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) < 0 || err) {
        snprintf(why, cap, "cannot connect to %s: %s", U.host, err ? strerror(err) : "timed out");
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
    time_t end = time(NULL) + REQUEST_SECS;
    for (;;) {
        const uint8_t *o;
        uint8_t *in;
        size_t n;
        struct pollfd p;
        int st = https_poll(&H), left;
        if (st != HS_BUSY)
            return st;
        left = (int)(end - time(NULL));
        if (left <= 0)
            return -1;
        p.fd = fd;
        p.events = 0;
        p.revents = 0;
        if (https_out(&H, &o))
            p.events |= POLLOUT;
        if (https_in(&H, &in))
            p.events |= POLLIN;
        if (!p.events)
            return -1; /* neither side can move: a bug */
        if (poll(&p, 1, left * 1000) <= 0)
            continue;
        if (p.revents & POLLOUT) {
            n = https_out(&H, &o);
            if (n) {
                int w = write(fd, o, n);
                if (w < 0 && errno != EAGAIN)
                    return -1;
                if (w > 0)
                    https_out_done(&H, (size_t)w);
            }
        }
        if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
            n = https_in(&H, &in);
            if (n) {
                int r = read(fd, in, n);
                if (r == 0 || (r < 0 && errno != EAGAIN))
                    https_eof(&H);
                else if (r > 0)
                    https_in_done(&H, (size_t)r);
            }
        }
    }
}

/* ---- the conversation ------------------------------------------------------------ */

/* After a failure: 15 s, doubling to 5 minutes. A handshake is ~6 s of the
 * R3000's time, and an unreachable console must not take a third of it. */
static time_t backoff(int *fails)
{
    int d = CONSOLE_INTERVAL_MS / 1000 << (*fails < 5 ? *fails : 5);
    if (d > 300)
        d = 300;
    ++*fails;
    return time(NULL) + d;
}

static void run(void)
{
    char body[512], line[160], why[96];
    time_t next = 0, t0;
    int want_sysinfo = 1, fault = 0, listed = 0, retried = 0, fresh = 0, fails = 0, st;
    const char *path;
    for (;;) {
        if (getppid() == 1)
            _exit(0); /* the agent is gone */
        if (time(NULL) < next) {
            sleep(1);
            continue;
        }
        if (fd < 0) {
            if (!dial(why, sizeof why)) {
                if (!fault)
                    cdv_log("console: %s", why);
                fault = 1;
                next = backoff(&fails);
                continue;
            }
            fresh = 1;
        }
        if (want_sysinfo) {
            char cpu[48], mem[16], os[48];
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
        t0 = time(NULL);
        if (!https_post(&H, U.host, line, body)) {
            cdv_log("console: the %s request does not fit", path);
            _exit(1);
        }
        st = exchange();
        if (st != HS_DONE) {
            int tls = st == HS_FAILED && H.err == HE_TLS;
            drop();
            /* a kept connection the server has since closed: once more, fresh */
            if (!retried && !fresh && !tls) {
                retried = 1;
                continue;
            }
            if (!fault) {
                if (tls)
                    cdv_log("console: TLS failed (BearSSL error %d)", H.tls_err);
                else
                    cdv_log("console: %s got no answer (%s after %ld s)", path,
                            st < 0 ? "timed out" : H.err == HE_CLOSED ? "closed" : "failed",
                            (long)(time(NULL) - t0));
            }
            fault = 1;
            retried = 0;
            next = backoff(&fails);
            continue;
        }
        retried = 0;
        if (H.status < 200 || H.status >= 300) {
            if (!fault)
                cdv_log("console: %s answered %d%s%.60s", path, H.status,
                        H.location[0] ? ", moved to " : "", H.location);
            fault = 1;
            drop();
            next = backoff(&fails);
            continue;
        }
        if (fault)
            cdv_log("console: reachable again");
        fault = 0;
        fails = 0;
        if (fresh)
            cdv_log("console: connected to %s%s, first answer in %ld s", U.host,
                    U.secure ? " over TLS" : "", (long)(time(NULL) - t0));
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
            next = time(NULL); /* and a heartbeat straight after */
        } else if (console_wants_sysinfo(H.body)) {
            want_sysinfo = 1;
            next = time(NULL);
        } else {
            next = time(NULL) + CONSOLE_INTERVAL_MS / 1000;
        }
        if (https_needs_reconnect(&H))
            drop();
    }
}

pid_t report_start(const report_config *c)
{
    pid_t pid;
    int i, keep = cdv_log_fd();
    if (!console_parse_url(c->url, &U)) {
        cdv_log("console: api=%s is not a URL", c->url);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        cdv_log("console: cannot fork: %s", strerror(errno));
        return 0;
    }
    if (pid > 0)
        return pid;
    /* The child: none of the agent's sockets or its X connection. */
    for (i = 3; i < 256; i++)
        if (i != keep)
            close(i);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    /* Below the agent: a handshake busies the CPU for seconds, and a peer
     * connecting meanwhile gives up if the agent does not answer in one
     * (RustDesk's client waits 1 s on a local connection). */
    nice(10);
    C = *c;
    run();
    _exit(0);
}
