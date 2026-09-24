#include "engine.h"
#include "cursor.h"
#include "../core/dns.h"
#include "../core/rdv.h"
#include "../core/rng.h"
#include "input.h"
#include "traps.h"

#include <string.h>

#define HEARTBEAT_MS 20
#define SCAN_MS 60            /* start a frame at most this often... */
#define SCAN_IDLE_MS 250      /* ...or this often once the screen has been still */
#define IDLE_AFTER 8          /* scans that found nothing before slowing down */
#define REFRESH_MS 400        /* re-send one row of macroblocks this often */
#define SLICE_TICKS 2         /* a slice ends after this many 60ths of a second */

engine_flags eng;
engine_name name_q;
engine_clip clip_out, clip_in;

static engine_ctx X;
static uint32_t conn_count;

/* ---- the log ring: one writer here, one reader in the main loop ----------- */

#define LOGN 16
static char logbuf[LOGN][80];
static volatile unsigned logw, logr;

void engine_log(const char *msg)
{
    unsigned w = logw;
    if (w - logr >= LOGN)
        return; /* the main loop is behind; drop rather than block */
    {
        size_t n = strlen(msg);
        if (n > sizeof logbuf[0] - 1)
            n = sizeof logbuf[0] - 1;
        memcpy(logbuf[w % LOGN], msg, n);
        logbuf[w % LOGN][n] = 0;
    }
    logw = w + 1;
}

const char *engine_next_log(void)
{
    static char line[80];
    if (logr == logw)
        return NULL;
    memcpy(line, logbuf[logr % LOGN], sizeof line);
    logr++;
    return line;
}

/* Append a signed number to a string: printf is not safe here. */
static char *put_num(char *p, long v)
{
    char d[12];
    int n = 0;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    if (v < 0)
        *p++ = '-';
    do {
        d[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n)
        *p++ = d[--n];
    *p = 0;
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

/* Where a stream is stuck, for the log. */
static void log_stream(const char *what, const cdv_tcp *t)
{
    char line[80], *p = put_str(line, what);
    p = put_num(put_str(p, " state "), t->state);
    p = put_num(put_str(p, " open "), t->open_pb.ioResult);
    p = put_num(put_str(p, " ctl "), t->ctl_pb.ioResult);
    p = put_num(put_str(p, " rcv "), t->rcv_busy ? t->rcv_pb.ioResult : 0);
    p = put_num(put_str(p, " snd "), t->snd_busy ? t->snd_pb.ioResult : 0);
    engine_log(line);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(TickCount() * 50UL / 3UL);
}

/* ---- video, as a small state machine run a slice at a time --------------- */

enum { V_IDLE, V_SCAN, V_ENC };

static struct {
    int state;
    int key, row, count, band;
    long clut_seen;
    uint32_t last, last_refresh;
    int q, still;
    uint8_t *dst;
    size_t cap;
    unsigned long t_start, t_scanned; /* ticks, for the timing log */
} V;

#ifdef VP8E_PROFILE
uint32_t vp8e_clock(void)
{
    return cdv_microseconds();
}

static void log_profile(void)
{
    static const char *names[5] = { "mode ", " quant ", " recon ", " exact ", " tokens " };
    uint32_t t[5];
    char line[80], *p = line;
    int i;
    vp8e_profile(X.enc, t);
    for (i = 0; i < 5; i++) {
        const char *q;
        for (q = names[i]; *q; q++)
            *p++ = *q;
        p = put_num(p, t[i] / 1000);
    }
    *p = 0;
    engine_log(line);
}
#endif

static void log_timing(int key, unsigned long scan, unsigned long enc, unsigned long bytes, int mbs)
{
    char line[80], *p = line;
    const char *k = key ? "key: " : "frame: ", *q;
    for (q = k; *q; q++)
        *p++ = *q;
    p = put_num(p, (unsigned long)mbs);
    for (q = " mb, scan "; *q; q++)
        *p++ = *q;
    p = put_num(p, scan * 50 / 3);
    for (q = " ms, encode "; *q; q++)
        *p++ = *q;
    p = put_num(p, enc * 50 / 3);
    for (q = " ms, "; *q; q++)
        *p++ = *q;
    p = put_num(p, bytes);
    for (q = " bytes"; *q; q++)
        *p++ = *q;
    *p = 0;
    engine_log(line);
}

static void video_reset(void)
{
    if (V.state == V_ENC) {
        vp8e_abandon(X.enc);
        cdv_video_abort(X.sess);
    }
    V.state = V_IDLE;
}

static void video_step(void)
{
    cdv_screen *scr = X.scr;
    unsigned long start = TickCount();
    uint32_t now = now_ms();

    if (V.state == V_IDLE) {
        /* A still screen is scanned less often, which is most of the time: a
         * full compare is tens of milliseconds on a 68040. Input from the
         * peer is the best predictor of change, so it resets the pace. */
        if (now - V.last < (V.still >= IDLE_AFTER ? SCAN_IDLE_MS : SCAN_MS) ||
            X.sess->vstate != VID_FREE)
            return;
        V.last = now;
        V.key = cdv_take_refresh(X.sess);
        if (scr->clut_seq != V.clut_seen) {
            V.clut_seen = scr->clut_seq;
            V.key = 1;
        }
        V.band = -1;
        if (now - V.last_refresh >= REFRESH_MS) {
            static int next;
            V.band = next;
            next = (next + 1) % scr->mbh;
            V.last_refresh = now;
        }
        V.row = V.count = 0;
        V.t_start = TickCount();
        V.state = V_SCAN;
    }

    if (V.state == V_SCAN) {
        while (V.row < scr->mbh && TickCount() - start < SLICE_TICKS) {
            V.count += screen_scan_rows(scr, V.row, V.row + 1, V.band, V.key, vp8e_exact_map(X.enc));
            V.row++;
        }
        if (V.row < scr->mbh)
            return;
        if (!V.count) {
            if (V.still < IDLE_AFTER)
                V.still++;
            V.state = V_IDLE;
            return;
        }
        V.still = 0;
        V.t_scanned = TickCount();
        V.dst = cdv_video_begin(X.sess, &V.cap);
        {
            vp8e_src src;
            src.y = scr->Y;
            src.u = scr->U;
            src.v = scr->V;
            src.ystride = scr->ystride;
            src.uvstride = scr->uvstride;
            if (!V.dst || !vp8e_begin(X.enc, &src, V.key ? NULL : scr->dirty, V.key, V.dst, V.cap)) {
                cdv_video_abort(X.sess);
                V.state = V_IDLE;
                return;
            }
        }
        V.state = V_ENC;
    }

    if (V.state == V_ENC) {
        int done = 0;
        do
            done = vp8e_rows(X.enc, 1);
        while (!done && TickCount() - start < SLICE_TICKS);
        if (!done)
            return;
        {
            size_t len = vp8e_end(X.enc);
            vp8e_stats st;
            vp8e_last_stats(X.enc, &st);
            if (!len) {
                /* Did not fit. Coarser, and a keyframe, next time. */
                cdv_video_abort(X.sess);
                V.q = V.q + 16 > 127 ? 127 : V.q + 16;
                vp8e_set_q(X.enc, V.q);
                X.sess->refresh = 1;
                engine_log("frame too large; lowering quality");
            } else if (!st.changed) {
                /* Nothing the peer would see: the reconstruction is exactly
                 * what it already has, so the frame need not go at all. */
                cdv_video_abort(X.sess);
            } else {
                cdv_video_commit(X.sess, len, st.key);
                if (st.key || (eng.frames & 63) == 0) {
                    log_timing(st.key, V.t_scanned - V.t_start, TickCount() - V.t_scanned, len,
                               st.mbs - st.skipped);
#ifdef VP8E_PROFILE
                    log_profile();
#endif
                }
                if (st.key && V.q != X.q) {
                    V.q = X.q;
                    vp8e_set_q(X.enc, V.q);
                }
                eng.frames++;
                eng.bytes += len;
            }
        }
        V.state = V_IDLE;
    }
}

/* ---- the pointer, when the picture does not have it ----------------------- */

static struct {
    uint32_t sum;
    int x, y;
    unsigned long quiet_until; /* ticks: the peer is moving it; do not echo */
} C;

static void cursor_step(void)
{
    static uint8_t rgba[16 * 16 * 4];
    int hx, hy, x, y;
    uint32_t sum;
    if (X.ident->cursor_embedded)
        return;
    sum = cursor_shape(rgba, &hx, &hy);
    if (sum != C.sum) {
        C.sum = sum;
        cdv_send_cursor(X.sess, sum, hx, hy, 16, 16, rgba);
    }
    cursor_where(&x, &y);
    if ((x != C.x || y != C.y) && TickCount() >= C.quiet_until) {
        cdv_send_cursor_pos(X.sess, x, y);
    }
    C.x = x;
    C.y = y;
}

void engine_peer_moved_pointer(void)
{
    C.quiet_until = TickCount() + 20; /* a third of a second */
}

/* ---- the session, on whichever connection carries it ----------------------- */

static cdv_tcp *S;           /* the connection carrying the session, or NULL */
static unsigned long S_since; /* ticks: when it began */

#define LOGIN_TIMEOUT (20 * 60) /* ticks a connection may take to log in */

static void end_session(const char *why)
{
    if (!S)
        return;
    engine_log(why);
    video_reset();
    input_release_all();
    eng.live = 0;
    eng.secure = 0;
    tcp_abort(S);
    S = NULL;
}

/* A connection is up and wants a session. One peer at a time: a logged-in
 * session keeps the machine and the newcomer is turned away; one still
 * waiting for its login gives way -- a client may open one route and then
 * take another, and the first never logs in. */
static void attach(cdv_tcp *t, int secure)
{
    char line[48];
    if (S && S != t) {
        if (X.sess->state == CDV_LIVE) {
            engine_log("a second peer was turned away");
            tcp_abort(t);
            return;
        }
        end_session("an earlier connection gave way");
    }
    net_addr_string(t->remote, line);
    engine_log(secure ? "connection through the ID server" : "connection");
    engine_log(line);
    S = t;
    S_since = TickCount();
    conn_count++;
    rng_add(&S_since, sizeof S_since);
    cdv_init(X.sess, X.outq, X.outcap, X.inq, X.incap, X.hooks, X.ident,
             TickCount() ^ (conn_count << 16) ^ rng_u32());
    cdv_start(X.sess, now_ms(), secure);
    memset(&C, 0, sizeof C);
    C.sum = 0xFFFFFFFF;
    memset(&V, 0, sizeof V);
    V.q = X.q;
    vp8e_abandon(X.enc); /* the first frame of a session is a keyframe */
}

static void session_step(void)
{
    const uint8_t *data;
    size_t len;
    int i;

    for (i = 0; i < 4 && tcp_recv(S, &data, &len); i++) {
        cdv_feed(X.sess, data, len);
        V.still = 0; /* the peer is doing something: look sooner */
    }
    len = tcp_sent(S);
    if (len)
        cdv_out_consume(X.sess, len);
    cdv_tick(X.sess, now_ms());
    eng.live = X.sess->state == CDV_LIVE;
    eng.secure = X.sess->enc;

    if (eng.live) {
        if (eng.announce) {
            eng.announce = 0;
            cdv_send_display(X.sess, X.scr->width, X.scr->height);
            X.sess->refresh = 1;
        }
        if (clip_out.ready) {
            cdv_send_clipboard(X.sess, clip_out.text, clip_out.len);
            clip_out.ready = 0;
        }
        video_step();
        cursor_step();
    }
    if (tcp_send_idle(S)) {
        const uint8_t *o = cdv_out_peek(X.sess, &len);
        if (len)
            tcp_send(S, o, len);
    }

    if (tcp_failed(S) || S->state != T_OPEN) {
        end_session("connection lost");
    } else if (X.sess->state == CDV_CLOSED && tcp_send_idle(S)) {
        cdv_out_peek(X.sess, &len);
        if (!len)
            end_session("session closed");
    } else if (!eng.live && TickCount() - S_since > LOGIN_TIMEOUT) {
        end_session("no login; dropped");
    }
}

/* ---- DNS: one name at a time, over the rendezvous UDP stream -------------- */

static struct {
    int busy, server;         /* which of X.dns is being asked */
    uint16_t id;
    unsigned long sent;
    char name[64];
    ip_addr ip;
    int result;               /* 1 answered, -1 failed, 0 pending */
} D;

static void dns_send(void)
{
    uint8_t q[128];
    size_t n = dns_query(q, sizeof q, D.id, D.name);
    if (n && udp_send(X.rdv, X.dns[D.server], 53, q, n))
        D.sent = TickCount();
}

static void dns_start(const char *name)
{
    memset(&D, 0, sizeof D);
    strncpy(D.name, name, sizeof D.name - 1);
    D.id = (uint16_t)rng_u32();
    D.busy = 1;
    dns_send();
}

static void dns_step(void)
{
    if (!D.busy)
        return;
    if (!D.sent) {
        dns_send();
    } else if (TickCount() - D.sent > 2 * 60) {
        /* No answer in two seconds: the next server, or give up. */
        if (++D.server >= X.ndns) {
            D.busy = 0;
            D.result = -1;
            return;
        }
        D.sent = 0;
        dns_send();
    }
}

/* ---- the ID server -------------------------------------------------------- */

enum { A_NONE, A_HELPER_CONNECT, A_HELPER_SEND, A_RELAY_RESOLVE, A_RELAY_CONNECT, A_RELAY_SEND };

static struct {
    int enabled;
    char host[64];
    tcp_port port;
    ip_addr ip;
    unsigned long retry_at;
    cdv_rdv r;
    rdv_action act;
    int step;
    unsigned long step_since;
    char relay_host[64];
    tcp_port relay_port;
    ip_addr relay_ip;
    char cached_name[64];
    ip_addr cached_ip;
    unsigned long asked_at;
    int fallback;
    uint8_t msg[512];
    size_t msglen;
} R;

static void action_step(void)
{
    if (R.step == A_NONE)
        return;
    if (TickCount() - R.step_since > 15 * 60) {
        {
            char line[40];
            put_num(put_str(line, "the ID server's request timed out at step "), R.step);
            engine_log(line);
        }
        log_stream("helper", X.helper);
        tcp_abort(X.helper);
        if (S != X.relay)
            tcp_abort(X.relay);
        R.step = A_NONE;
        return;
    }
    switch (R.step) {
    case A_HELPER_CONNECT:
        if (X.helper->state == T_IDLE) {
            if (X.helper->used)
                X.helper->renew_req = 1; /* a fresh stream first: see tcp_renew */
            else if (!X.helper->renew_req)
                tcp_connect(X.helper, R.ip, R.port);
        } else if (X.helper->state == T_OPEN) {
            if (R.act.kind == RDV_LOCAL)
                R.msglen = rdv_local_addr(&R.r, &R.act, X.my_ip, X.local_port, R.msg, sizeof R.msg);
            else
                R.msglen = rdv_relay_response(&R.r, &R.act, R.msg, sizeof R.msg);
            tcp_send(X.helper, R.msg, R.msglen);
            R.step = A_HELPER_SEND;
        }
        break;
    case A_HELPER_SEND:
        if (X.helper->state != T_OPEN) {
            R.step = A_NONE;
            break;
        }
        if (tcp_sent(X.helper) || tcp_send_idle(X.helper)) {
            tcp_close(X.helper);
            if (R.act.kind == RDV_LOCAL) {
                engine_log("told the ID server our local address");
                R.step = A_NONE; /* the peer comes to the local listener */
            } else {
                R.step = A_RELAY_RESOLVE;
                rdv_split_host(R.act.relay, R.relay_host, sizeof R.relay_host, &R.relay_port,
                               RELAY_PORT);
                if (dns_parse_ip(R.relay_host, &R.relay_ip))
                    R.step = A_RELAY_CONNECT;
                else if (!strcmp(R.relay_host, R.host)) {
                    R.relay_ip = R.ip;
                    R.step = A_RELAY_CONNECT;
                } else if (R.cached_ip && !strcmp(R.relay_host, R.cached_name)) {
                    R.relay_ip = R.cached_ip;
                    R.step = A_RELAY_CONNECT;
                } else {
                    /* The Mac's own resolver, through the main loop. */
                    strncpy(name_q.name, R.relay_host, sizeof name_q.name - 1);
                    name_q.ans = 0;
                    name_q.req = 1;
                    R.asked_at = TickCount();
                    R.fallback = 0;
                }
            }
        }
        break;
    case A_RELAY_RESOLVE:
        if (!R.fallback) {
            if (name_q.ans == 1) {
                R.relay_ip = name_q.ip;
            } else if (name_q.ans == -1 || TickCount() - R.asked_at > 3 * 60) {
                /* No answer from the Mac's resolver (or the main loop is held
                 * up): ask a DNS server ourselves. */
                name_q.req = 0;
                R.fallback = 1;
                dns_start(R.relay_host);
                break;
            } else {
                break;
            }
        } else {
            if (D.busy)
                break;
            if (D.result != 1) {
                engine_log("could not look up the relay");
                R.step = A_NONE;
                break;
            }
            R.relay_ip = D.ip;
        }
        strncpy(R.cached_name, R.relay_host, sizeof R.cached_name - 1);
        R.cached_ip = R.relay_ip;
        R.step = A_RELAY_CONNECT;
        break;
    case A_RELAY_CONNECT:
        if (S == X.relay && X.sess->state == CDV_LIVE) {
            R.step = A_NONE; /* busy with a peer already */
            break;
        }
        if (S == X.relay)
            end_session("an earlier relay session gave way");
        if (X.relay->state == T_IDLE) {
            if (X.relay->used)
                X.relay->renew_req = 1; /* a fresh stream first: see tcp_renew */
            else if (!X.relay->renew_req)
                tcp_connect(X.relay, R.relay_ip, R.relay_port);
        } else if (X.relay->state == T_OPEN) {
            R.msglen = rdv_request_relay(&R.r, &R.act, R.msg, sizeof R.msg);
            tcp_send(X.relay, R.msg, R.msglen);
            R.step = A_RELAY_SEND;
        }
        break;
    case A_RELAY_SEND:
        if (X.relay->state != T_OPEN) {
            engine_log("the relay closed the connection");
            R.step = A_NONE;
            break;
        }
        if (tcp_sent(X.relay) || tcp_send_idle(X.relay)) {
            engine_log("joined the relay");
            R.step = A_NONE;
            attach(X.relay, 1);
        }
        break;
    }
}

static void rdv_step(void)
{
    const uint8_t *d;
    size_t n;
    ip_addr from;
    uint16_t port;
    uint8_t out[512];

    if (!R.enabled)
        return;
    dns_step();

    /* The server's address first. */
    if (!R.ip) {
        if (!D.busy && TickCount() >= R.retry_at) {
            if (D.result == 1 && !strcmp(D.name, R.host)) {
                R.ip = D.ip;
                eng.server_ip = R.ip;
                eng.rdv_state = RS_REGISTERING;
                rdv_init(&R.r);
            } else if (D.result == -1 && !strcmp(D.name, R.host)) {
                eng.rdv_state = RS_NO_DNS;
                R.retry_at = TickCount() + 30 * 60;
                D.result = 0;
            } else {
                eng.rdv_state = RS_RESOLVING;
                dns_start(R.host);
            }
        }
    }

    while (udp_recv(X.rdv, &d, &n, &from, &port)) {
        if (port == 53) {
            int rv = D.busy ? dns_answer(d, n, D.id, &D.ip) : 0;
            if (rv) {
                D.result = rv;
                D.busy = 0;
            }
        } else if (R.ip && from == R.ip) {
            rdv_action act;
            size_t replen;
            int kind = rdv_input(&R.r, d, n, now_ms(), &act, out, sizeof out, &replen);
            if (replen)
                udp_send(X.rdv, R.ip, R.port, out, replen);
            if (kind == RDV_REGISTERED) {
                eng.rdv_state = RS_REGISTERED;
                engine_log("registered with the ID server");
            } else if (kind == RDV_REFUSED) {
                eng.rdv_state = RS_REFUSED;
                eng.rdv_refused = act.refuse_code;
                engine_log("the ID server refused the registration");
            } else if (kind == RDV_RELAY || kind == RDV_LOCAL) {
                if (R.step != A_NONE)
                    engine_log("a new request replaces one in progress");
                tcp_abort(X.helper);
                R.act = act;
                R.step = A_HELPER_CONNECT;
                R.step_since = TickCount();
                engine_log(kind == RDV_LOCAL ? "a peer on this network asks for us"
                                             : "a peer asks for us through the relay");
            }
        }
        udp_done(X.rdv);
    }

    if (R.ip) {
        if ((n = rdv_tick(&R.r, now_ms(), out, sizeof out)) != 0)
            udp_send(X.rdv, R.ip, R.port, out, n);
        if (R.r.registered && eng.rdv_state != RS_REGISTERED)
            eng.rdv_state = RS_REGISTERED;
    }
    action_step();
}

static void lan_step(void)
{
    const uint8_t *d;
    size_t n;
    ip_addr from;
    uint16_t port;
    if (!X.lan || !X.lan->stream)
        return;
    while (udp_recv(X.lan, &d, &n, &from, &port)) {
        uint8_t out[256];
        char mine[20];
        size_t m;
        net_addr_string(X.my_ip, mine);
        m = lan_answer(d, n, X.ident->id, R.r.registered ? X.ident->id : mine, X.ident->hostname,
                       out, sizeof out);
        udp_done(X.lan);
        if (m)
            udp_send(X.lan, from, port, out, m);
    }
}

/* ---- the tick ------------------------------------------------------------- */

void engine_setup(const engine_ctx *ctx)
{
    X = *ctx;
    /* Also a restart (Stop, then Start, or new settings): nothing of the last
     * run's streams survives -- they were released and made again. */
    S = NULL;
    eng.live = 0;
    eng.secure = 0;
    eng.rdv_state = RS_OFF;
    eng.drop_req = 0;
    memset((void *)&name_q, 0, sizeof name_q);
    memset(&V, 0, sizeof V);
    V.q = X.q;
    memset(&R, 0, sizeof R);
    memset(&D, 0, sizeof D);
    if (X.server && X.server[0]) {
        R.enabled = 1;
        rdv_split_host(X.server, R.host, sizeof R.host, &R.port, RDV_PORT);
        if (dns_parse_ip(R.host, &R.ip)) {
            eng.server_ip = R.ip;
            eng.rdv_state = RS_REGISTERING;
        } else {
            eng.rdv_state = RS_RESOLVING;
        }
        strncpy(R.r.id, X.ident->id, sizeof R.r.id - 1);
        memcpy(R.r.uuid, X.uuid, 16);
        memcpy(R.r.pk, X.pk, 32);
        strncpy(R.r.key, X.key ? X.key : "", sizeof R.r.key - 1);
        strncpy(R.r.relay, X.relay_host ? X.relay_host : "", sizeof R.r.relay - 1);
        rdv_init(&R.r);
    }
}

void engine_replace_video(vp8e *enc, uint8_t *outq, size_t outcap)
{
    X.enc = enc;
    X.outq = outq;
    X.outcap = outcap;
    memset(&V, 0, sizeof V);
    V.q = X.q;
}

static void listen_step(cdv_tcp *t, tcp_port port, int secure)
{
    int r = tcp_poll(t);
    if (r == 1)
        attach(t, secure);
    else if (t->state == T_IDLE && S != t)
        tcp_listen(t, port);
}

void engine_tick(void)
{
    eng.ticks++;
    if (eng.drop_req) {
        end_session(eng.drop_req == DROP_USER ? "disconnected the peer"
                                              : "the screen changed; please reconnect");
        eng.drop_req = 0;
    }
    if (eng.suspend_req) {
        video_reset();
        eng.suspended = 1;
        return;
    }
    eng.suspended = 0;

    listen_step(X.direct, X.direct_port, 0);
    if (R.enabled)
        listen_step(X.local, X.local_port, 1);
    tcp_poll(X.helper);
    tcp_poll(X.relay);
    rdv_step();
    lan_step();
    if (S)
        session_step();
}

/* ---- Time Manager heartbeat and the deferred task -------------------------- */

/* The extended Time Manager record (Inside Macintosh: Processes 3-22), with
 * our A5 after it where the 68k glue can find it from A1. Both records are
 * the system's, so they keep 68k alignment on PowerPC too. */
#if CDV_PPC
#pragma pack(push, 2)
#endif
typedef struct {
    QElemPtr qLink;
    short qType;
    ProcPtr tmAddr;
    long tmCount;
    long tmWakeUp;
    long tmReserved;
    long a5;
} cdv_tm;

typedef struct {
    QElemPtr qLink;
    short qType;
    short dtFlags;
    ProcPtr dtAddr;
    long dtParam;
    long dtReserved;
} cdv_dt;

#if CDV_PPC
#pragma pack(pop)
#endif

enum { dtQType = 7 };

static cdv_tm tm;
static cdv_dt dt;
static volatile int dt_pending, running;

/* Called by the glue at interrupt time. Queue the work; do none of it. */
void cdv_tm_fire(void)
{
    if (!running)
        return;
    if (!dt_pending) {
        dt_pending = 1;
        if (cdv_dt_install(&dt) != noErr)
            dt_pending = 0;
    }
    PrimeTime((QElemPtr)&tm, HEARTBEAT_MS);
}

/* Called by the glue at deferred-task time. */
void cdv_dt_fire(void)
{
    engine_tick();
    dt_pending = 0;
}

#if CDV_PPC
/* PowerPC: the Time Manager and the Deferred Task Manager call through Mixed
 * Mode, so each routine gets a descriptor. Both are "register, A1, four
 * bytes, no result" (uppTimerProcInfo = uppDeferredTaskProcInfo = 0xB802);
 * native code has no A5 to set up. */
static void tm_proc(void *task)
{
    (void)task;
    cdv_tm_fire();
}

static void dt_proc(long param)
{
    (void)param;
    cdv_dt_fire();
}

OSErr engine_start(void)
{
    memset(&tm, 0, sizeof tm);
    tm.tmAddr = (ProcPtr)NewRoutineDescriptor((ProcPtr)tm_proc, 0x0000B802, 1 /* PowerPC */);
    memset(&dt, 0, sizeof dt);
    dt.qType = dtQType;
    dt.dtAddr = (ProcPtr)NewRoutineDescriptor((ProcPtr)dt_proc, 0x0000B802, 1);
    if (!tm.tmAddr || !dt.dtAddr)
        return memFullErr;
    running = 1;
    cdv_ins_xtime(&tm);
    PrimeTime((QElemPtr)&tm, HEARTBEAT_MS);
    return noErr;
}
#else
extern void cdv_tm_glue(void);
extern void cdv_dt_glue(void);

OSErr engine_start(void)
{
    long a5;
    __asm__ volatile("move.l %%a5,%0" : "=r"(a5));
    memset(&tm, 0, sizeof tm);
    tm.tmAddr = (ProcPtr)cdv_tm_glue;
    tm.a5 = a5;
    memset(&dt, 0, sizeof dt);
    dt.qType = dtQType;
    dt.dtAddr = (ProcPtr)cdv_dt_glue;
    dt.dtParam = a5;
    running = 1;
    cdv_ins_xtime(&tm);
    PrimeTime((QElemPtr)&tm, HEARTBEAT_MS);
    return noErr;
}
#endif

void engine_stop(void)
{
    running = 0;
    RmvTime((QElemPtr)&tm);
    while (dt_pending)
        ;
}
