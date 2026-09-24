#include "report.h"

#include "dnr.h"
#include "traps.h"
#include "../core/console.h"
#include "../core/https.h"
#include "../core/sha256.h"

#include <Multiverse.h>
#include <stdio.h>
#include <string.h>

enum { R_OFF, R_BAD_URL, R_RESOLVE, R_IDLE, R_CONNECTING, R_EXCHANGE, R_CLOSING };

static struct {
    int state;
    report_config c;
    console_url u;
    cdv_tcp *t;
    void (*say)(const char *);
    uint32_t ip;
    unsigned long next_at;      /* ticks: when the next request is due */
    unsigned long started_at;   /* ticks: when the current step began */
    int want_sysinfo;
    int retried;                /* the current request has had its fresh connection */
    int fault;                  /* a failure has been logged and not yet cleared */
    int listed;                 /* the console took the inventory */
    const char *path;
    char body[512];
    /* bytes the stream received that TLS has not taken yet */
    const uint8_t *rx;
    size_t rx_len;
    /* a send in flight: its length */
    size_t tx_len;
    uint32_t counter;
    unsigned long last_ok;
    int fresh;                  /* the request in flight opened its connection */
    int lookup;                 /* the API server's name being looked up, or -1 */
    char status[48];
} R;

static cdv_https H;
static uint8_t *iobuf;

#define TICKS_PER_BEAT (CONSOLE_INTERVAL_MS * 60 / 1000)

/* ---- what the machine is -------------------------------------------------------- */

static long gestalt(OSType sel)
{
    long v = 0;
    if (Gestalt(sel, &v) != noErr)
        return -1;
    return v;
}

void report_describe_cpu(char *out, size_t cap)
{
    long cpu = gestalt('cput'), mhz = gestalt('pclk');
    const char *name = NULL;
    char buf[32];
    if (cpu < 0) {
        long p = gestalt('proc'); /* 1: 68000 ... 5: 68040 */
        cpu = p > 0 ? p - 1 : 0;
    }
    if (cpu < 0x100) {
        snprintf(buf, sizeof buf, "680%ld0", cpu);
        name = buf;
    } else {
        switch (cpu) {
        case 0x101: name = "PowerPC 601"; break;
        case 0x103: name = "PowerPC 603"; break;
        case 0x104: name = "PowerPC 604"; break;
        case 0x106: name = "PowerPC 603e"; break;
        case 0x107: name = "PowerPC 603ev"; break;
        case 0x108: name = "PowerPC G3 (750)"; break;
        case 0x109: name = "PowerPC 604e"; break;
        case 0x10A: name = "PowerPC 604ev"; break;
        case 0x10C: name = "PowerPC G4 (7400)"; break;
        case 0x110: name = "PowerPC G4 (7410)"; break;
        case 0x111: name = "PowerPC G4 (7450)"; break;
        case 0x112: name = "PowerPC G4 (7455)"; break;
        case 0x113: name = "PowerPC G4 (7447)"; break;
        case 0x120: name = "PowerPC G4 (7448)"; break;
        default:
            snprintf(buf, sizeof buf, "PowerPC (0x%lx)", cpu);
            name = buf;
        }
    }
    if (mhz > 0)
        snprintf(out, cap, "%s, %ld MHz", name, (mhz + 500000) / 1000000);
    else
        snprintf(out, cap, "%s", name);
}

void report_describe_memory(char *out, size_t cap)
{
    long ram = gestalt('ram ');
    if (ram > 0)
        snprintf(out, cap, "%ld MB", ram / (1024L * 1024L));
    else
        out[0] = 0;
}

void report_describe_os(char *out, size_t cap)
{
    long v = gestalt('sysv'), bbox = gestalt('bbox');
    int major = (int)((v >> 8) & 0xFF), minor = (int)((v >> 4) & 0xF), bug = (int)(v & 0xF);
    char ver[16];
    if (major >= 0x10)
        major = (major >> 4) * 10 + (major & 0xF); /* BCD: 0x10 is 10 */
    if (bug)
        snprintf(ver, sizeof ver, "%d.%d.%d", major, minor, bug);
    else
        snprintf(ver, sizeof ver, "%d.%d", major, minor);
    /* Apple called it "System" until 7.5.5 and "Mac OS" from 7.6. */
    snprintf(out, cap, "%s %s%s", v < 0x760 ? "System" : "Mac OS", ver,
             bbox >= 0 ? " (Classic, Mac OS X)" : "");
}

/* ---- the clock, for certificate dates -------------------------------------- */

/* __DATE__: "Sep 24 2026". A Mac whose clock is before the build has a flat
 * PRAM battery, not a time machine; the build date is the better guess, and
 * certificates valid today were valid then (Let's Encrypt's last 90 days). */
static uint32_t build_days(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    int m = (int)((strstr(months, (char[]){ d[0], d[1], d[2], 0 }) - months) / 3) + 1;
    int day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
    int y = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
    return https_days(y, m, day);
}

static void utc_now(uint32_t *days, uint32_t *secs)
{
    unsigned long local;
    uint32_t utc, floor = build_days();
    GetDateTime(&local);
    utc = (uint32_t)(local - (unsigned long)cdv_gmt_delta());
    /* 1 January 1904, the Mac's epoch, is day 695421 of the reckoning BearSSL
     * uses (from 1 January of year 0). */
    *days = 695421UL + utc / 86400UL;
    *secs = utc % 86400UL;
    if (*days < floor) {
        static int told;
        if (!told && R.say) {
            R.say("the Mac's clock is behind this build; using the build date");
            told = 1;
        }
        *days = floor;
        *secs = 0;
    }
}

/* Random bytes for TLS, from this loop alone: the engine owns the shared
 * RNG at interrupt time, and two contexts drawing from it at once could see
 * the same bytes. A secret only this Mac knows, a counter and the timers. */
static void tls_seed(uint8_t out[32])
{
    sha256_ctx c;
    uint32_t v[3];
    v[0] = ++R.counter;
    v[1] = cdv_microseconds();
    v[2] = TickCount();
    sha256_init(&c);
    sha256_update(&c, R.c.secret, 32);
    sha256_update(&c, (const uint8_t *)"cdv tls seed", 12);
    sha256_update(&c, (const uint8_t *)v, sizeof v);
    sha256_final(&c, out);
}

/* ---- the conversation ------------------------------------------------------------ */

static void set_status(const char *s)
{
    strncpy(R.status, s, sizeof R.status - 1);
}

static void fault(const char *msg)
{
    if (!R.fault && R.say)
        R.say(msg);
    R.fault = 1;
    set_status("console: unreachable");
}

static void drop_connection(void)
{
    if (R.t->state != T_IDLE)
        tcp_abort(R.t);
    https_disconnected(&H);
    R.rx_len = 0;
    R.tx_len = 0;
    R.state = R_CLOSING;
}

/* Build the request that is due. */
static void compose(void)
{
    if (R.want_sysinfo) {
        char cpu[48], mem[16], os[48];
        report_describe_cpu(cpu, sizeof cpu);
        report_describe_memory(mem, sizeof mem);
        report_describe_os(os, sizeof os);
        console_sysinfo_json(R.body, sizeof R.body, R.c.id, R.c.uuid, cpu, mem, os,
                             R.c.hostname, R.c.username);
        R.path = "/api/sysinfo";
    } else {
        console_heartbeat_json(R.body, sizeof R.body, R.c.id, R.c.uuid);
        R.path = "/api/heartbeat";
    }
}

static int send_request(void)
{
    char full[96];
    snprintf(full, sizeof full, "%s%s", R.u.prefix, R.path);
    return https_post(&H, R.u.host, full, R.body);
}

static void connect_now(void)
{
    if (R.t->used) {
        /* Open Transport's MacTCP will not reuse a stream: see tcp_renew. */
        if (tcp_renew(R.t) != noErr) {
            fault("console: no TCP stream");
            R.next_at = TickCount() + TICKS_PER_BEAT;
            R.state = R_IDLE;
            return;
        }
    }
    tcp_connect(R.t, R.ip, R.u.port);
    R.started_at = TickCount();
    R.state = R_CONNECTING;
    set_status(R.u.secure ? "console: connecting (TLS)" : "console: connecting");
}

/* A reply arrived. */
static void answered(void)
{
    char line[96];
    if (H.status < 200 || H.status >= 300) {
        if (H.status >= 300 && H.status < 400 && H.location[0])
            snprintf(line, sizeof line, "console: %s moved to %.60s", R.path, H.location);
        else
            snprintf(line, sizeof line, "console: %s answered %d", R.path, H.status);
        fault(line);
        R.next_at = TickCount() + TICKS_PER_BEAT;
        return;
    }
    if (R.fault && R.say)
        R.say("console: reachable again");
    R.fault = 0;
    R.last_ok = TickCount();
    if (R.fresh && R.say) {
        snprintf(line, sizeof line, "console: connected%s, first answer in %lu ms",
                 R.u.secure ? " over TLS" : "", (TickCount() - R.started_at) * 50 / 3);
        R.say(line);
    }
    R.fresh = 0;
    if (!strcmp(R.path, "/api/sysinfo")) {
        R.want_sysinfo = 0;
        if (!strncmp(H.body, "SYSINFO_UPDATED", 15)) {
            if (!R.listed && R.say)
                R.say("console: in the device list");
            R.listed = 1;
        } else if (R.say) {
            snprintf(line, sizeof line, "console: inventory answer %.40s", H.body);
            R.say(line);
        }
        R.next_at = TickCount(); /* and a heartbeat straight after */
    } else {
        if (console_wants_sysinfo(H.body)) {
            R.want_sysinfo = 1;
            R.next_at = TickCount();
        } else {
            R.next_at = TickCount() + TICKS_PER_BEAT;
        }
    }
    set_status(R.listed ? "console: listed" : "console: reporting");
}

/* A request failed. A kept-open connection the server has since closed is
 * the everyday case: try once more on a fresh one. */
static void failed(void)
{
    char line[80];
    int was_tls = H.err == HE_TLS;
    drop_connection();
    if (!R.retried && !was_tls) {
        R.retried = 1;
        R.next_at = TickCount();
        return;
    }
    if (was_tls)
        snprintf(line, sizeof line, "console: TLS failed (BearSSL error %d)", H.tls_err);
    else
        snprintf(line, sizeof line, "console: %s got no answer", R.path);
    fault(line);
    R.next_at = TickCount() + TICKS_PER_BEAT;
}

/* Move bytes between the stream and the client. */
static void pump(void)
{
    const uint8_t *p;
    size_t n;
    int moved = 1;
    while (moved) {
        moved = 0;
        /* out */
        if (R.tx_len) {
            size_t k = tcp_sent(R.t);
            if (k) {
                https_out_done(&H, k);
                R.tx_len = 0;
                moved = 1;
            }
        }
        if (!R.tx_len && tcp_send_idle(R.t) && (n = https_out(&H, &p)) != 0) {
            tcp_send(R.t, p, n);
            R.tx_len = n; /* only a flag: tcp_sent() says how much went */
            if (!tcp_send_idle(R.t))
                moved = 1;
            else
                R.tx_len = 0;
        }
        /* in */
        if (!R.rx_len) {
            const uint8_t *d;
            size_t len;
            if (tcp_recv(R.t, &d, &len)) {
                R.rx = d;
                R.rx_len = len;
            }
        }
        if (R.rx_len) {
            uint8_t *room;
            size_t k = https_in(&H, &room);
            if (k) {
                if (k > R.rx_len)
                    k = R.rx_len;
                memcpy(room, R.rx, k);
                R.rx += k;
                R.rx_len -= k;
                https_in_done(&H, k); /* where a handshake spends its seconds */
                moved = 1;
            }
        }
        if (tcp_failed(R.t) && !R.rx_len) {
            https_eof(&H);
            return;
        }
    }
}

void report_start(const report_config *c, cdv_tcp *t, void (*say)(const char *))
{
    memset(&R, 0, sizeof R);
    R.lookup = -1;
    R.c = *c;
    R.t = t;
    R.say = say;
    R.want_sysinfo = 1; /* introduce ourselves rather than wait to be asked */
    if (!c->url[0] || !t) {
        R.state = R_OFF;
        return;
    }
    if (!console_parse_url(c->url, &R.u)) {
        R.state = R_BAD_URL;
        set_status("console: not a URL");
        if (say)
            say("console: the API server setting is not a URL");
        return;
    }
    if (R.u.secure && !iobuf) {
        iobuf = (uint8_t *)NewPtr(HTTPS_IOBUF);
        if (!iobuf) {
            R.state = R_OFF;
            if (say)
                say("console: not enough memory for TLS");
            return;
        }
    }
    R.state = R_RESOLVE;
    set_status("console: looking up");
}

void report_stop(void)
{
    if (R.lookup >= 0)
        dnr_forget(R.lookup);
    R.lookup = -1;
    if (R.state >= R_CONNECTING && R.t)
        tcp_abort(R.t);
    R.state = R_OFF;
}

void report_step(void)
{
    unsigned long now = TickCount();
    switch (R.state) {
    case R_RESOLVE: {
        /* Polled, never waited on: a lookup with no network takes as long
         * as the resolver needs to give up, and would stop the whole Mac. */
        uint32_t ip;
        int r;
        if (now < R.next_at)
            break;
        if (R.lookup < 0) {
            R.lookup = dnr_start(R.u.host);
            R.started_at = now;
            if (R.lookup < 0) {
                fault("console: no resolver to find the API server with");
                R.next_at = now + 4 * TICKS_PER_BEAT;
            }
            break;
        }
        r = dnr_poll(R.lookup, &ip);
        if (r == 0 && now - R.started_at < 15 * 60)
            break;
        if (r == 0)
            dnr_forget(R.lookup);
        R.lookup = -1;
        if (r == 1) {
            R.ip = ip;
            R.state = R_IDLE;
            R.next_at = now;
        } else {
            fault("console: cannot find the API server's address");
            R.next_at = now + 4 * TICKS_PER_BEAT;
        }
        break;
    }
    case R_IDLE:
        if (now < R.next_at)
            break;
        compose();
        R.retried = 0;
        if (https_needs_reconnect(&H))
            connect_now();
        else if (send_request()) {
            R.started_at = now;
            R.state = R_EXCHANGE;
        } else {
            drop_connection();
        }
        break;
    case R_CONNECTING: {
        int r = tcp_poll(R.t);
        if (r == 1) {
            uint8_t seed[32];
            uint32_t days, secs;
            utc_now(&days, &secs);
            tls_seed(seed);
            https_connected(&H, R.u.secure, R.u.host, iobuf, HTTPS_IOBUF, days, secs, seed);
            memset(seed, 0, sizeof seed);
            R.fresh = 1;
            if (!send_request()) {
                failed();
                break;
            }
            R.started_at = now;
            R.state = R_EXCHANGE;
        } else if (r == -1 || now - R.started_at > 30 * 60) {
            drop_connection();
            fault("console: cannot connect to the API server");
            R.next_at = now + TICKS_PER_BEAT;
        }
        break;
    }
    case R_EXCHANGE: {
        int st;
        tcp_poll(R.t);
        pump();
        st = https_poll(&H);
        if (st == HS_DONE) {
            answered();
            if (https_needs_reconnect(&H))
                drop_connection();
            else
                R.state = R_IDLE;
        } else if (st == HS_FAILED) {
            failed();
        } else if (now - R.started_at > 60 * 60) {
            H.state = HS_FAILED;
            H.err = HE_CLOSED;
            failed();
        }
        break;
    }
    case R_CLOSING:
        tcp_poll(R.t);
        if (R.t->state == T_IDLE)
            R.state = R_IDLE;
        break;
    default:
        break;
    }
}

const char *report_status(void)
{
    return R.state == R_OFF ? "" : R.status;
}
