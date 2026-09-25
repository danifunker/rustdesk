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
engine_chat chat_out, chat_in;
engine_shot shot;
static int scan_ms = SCAN_MS; /* the peer's frames a second, as a scan interval */

static engine_ctx X;
static engine_slot_t SL[2];     /* the two sessions (see engine.h) */
static engine_slot_t *DS;       /* the desktop's slot, or NULL */
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
        if (DS)
            cdv_video_abort(&DS->sess);
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
        /* A screenshot borrows the frame buffer between frames. */
        if (shot.state == SHOT_WANTED && (&DS->sess)->vstate == VID_FREE) {
            shot.buf = cdv_big_begin((&DS->sess), &shot.cap);
            if (shot.buf)
                shot.state = SHOT_LENT;
        }
        if (shot.state == SHOT_DONE) {
            if (shot.conn == conn_count)
                cdv_screenshot_commit((&DS->sess), shot.len, shot.err[0] ? shot.err : NULL);
            shot.state = SHOT_NONE;
            return;
        }
        if (shot.state != SHOT_NONE)
            return;
        if (now - V.last < (V.still >= IDLE_AFTER ? SCAN_IDLE_MS : (uint32_t)scan_ms) ||
            (&DS->sess)->vstate != VID_FREE)
            return;
        V.last = now;
        V.key = cdv_take_refresh((&DS->sess));
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
        V.dst = cdv_video_begin((&DS->sess), &V.cap);
        {
            vp8e_src src;
            src.y = scr->Y;
            src.u = scr->U;
            src.v = scr->V;
            src.ystride = scr->ystride;
            src.uvstride = scr->uvstride;
            if (!V.dst || !vp8e_begin(X.enc, &src, V.key ? NULL : scr->dirty, V.key, V.dst, V.cap)) {
                cdv_video_abort((&DS->sess));
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
                cdv_video_abort((&DS->sess));
                V.q = V.q + 16 > 127 ? 127 : V.q + 16;
                vp8e_set_q(X.enc, V.q);
                (&DS->sess)->refresh = 1;
                engine_log("frame too large; lowering quality");
            } else if (!st.changed) {
                /* Nothing the peer would see: the reconstruction is exactly
                 * what it already has, so the frame need not go at all. */
                cdv_video_abort((&DS->sess));
            } else {
                cdv_video_commit((&DS->sess), len, st.key);
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
        cdv_send_cursor((&DS->sess), sum, hx, hy, 16, 16, rgba);
    }
    cursor_where(&x, &y);
    if ((x != C.x || y != C.y) && TickCount() >= C.quiet_until) {
        cdv_send_cursor_pos((&DS->sess), x, y);
    }
    C.x = x;
    C.y = y;
}

void engine_peer_moved_pointer(void)
{
    C.quiet_until = TickCount() + 20; /* a third of a second */
}

/* ---- the sessions, one per slot ------------------------------------------------ */


#define LOGIN_TIMEOUT (20 * 60) /* ticks a connection may take to log in */

engine_slot_t *engine_slot(int i)
{
    return &SL[i];
}

const char *engine_peer_name(void)
{
    return DS && DS->sess.peer_name[0] ? DS->sess.peer_name : NULL;
}

/* Is this stream carrying a session? */
static engine_slot_t *slot_of(const cdv_tcp *t)
{
    int i;
    for (i = 0; i < 2; i++)
        if (SL[i].t == t)
            return &SL[i];
    return NULL;
}

static void end_slot(engine_slot_t *s, const char *why)
{
    if (!s->t)
        return;
    engine_log(why);
    if (s == DS) {
        video_reset();
        input_release_all();
        eng.live = 0;
        eng.secure = 0;
        DS = NULL;
    }
    if (s->t->state != T_IDLE)
        tcp_abort(s->t);
    s->t = NULL;
    s->kind = K_PENDING;
    s->owner = O_ENGINE;
}

/* The desktop: what the drop request and a new desktop end. */
static void end_session(const char *why)
{
    if (DS)
        end_slot(DS, why);
}

/* At deferred-task time, from the session's login: a desktop takes the
 * video buffer (one at a time); a file manager is fine alongside it. */
static int hook_login(void *u, int file_transfer)
{
    engine_slot_t *s = (engine_slot_t *)u;
    if (file_transfer) {
        s->kind = K_FILE;
        return 1;
    }
    if (DS && DS != s)
        return 0;
    DS = s;
    s->kind = K_DESKTOP;
    cdv_set_video(&s->sess, X.outq, X.outcap);
    memset(&C, 0, sizeof C);
    C.sum = 0xFFFFFFFF;
    memset(&V, 0, sizeof V);
    X.q = X.q_base;       /* the last peer's view settings go with it */
    V.q = X.q;
    vp8e_set_q(X.enc, V.q);
    scan_ms = SCAN_MS;
    if (shot.state == SHOT_WANTED)
        shot.state = SHOT_NONE; /* asked for by the last peer */
    vp8e_abandon(X.enc); /* the first frame of a session is a keyframe */
    return 1;
}

/* File messages belong to the main loop, which feeds a file manager's
 * session once it owns the slot. */
static void hook_file(void *u, int field, const uint8_t *d, size_t n)
{
    engine_slot_t *s = (engine_slot_t *)u;
    if (s->owner == O_MAIN && X.file_hook)
        X.file_hook(s == &SL[0] ? 0 : 1, field, d, n);
}

/* A connection is up and wants a session: a free slot, or one still waiting
 * for its login, which gives way -- a client may open one route and then
 * take another, and the first never logs in. Two logged-in sessions (a
 * desktop and a file manager) and the newcomer is turned away. */
static void attach(cdv_tcp *t, int secure)
{
    char line[48];
    engine_slot_t *s = NULL;
    int i;
    for (i = 0; i < 2 && !s; i++)
        if (!SL[i].t)
            s = &SL[i];
    for (i = 0; i < 2 && !s; i++)
        if (SL[i].owner == O_ENGINE && SL[i].sess.state != CDV_LIVE) {
            end_slot(&SL[i], "an earlier connection gave way");
            s = &SL[i];
        }
    if (!s) {
        engine_log("a third peer was turned away");
        tcp_abort(t);
        return;
    }
    net_addr_string(t->remote, line);
    engine_log(secure ? "connection through the ID server" : "connection");
    engine_log(line);
    s->t = t;
    s->since = TickCount();
    s->kind = K_PENDING;
    s->owner = O_ENGINE;
    s->hooks = *X.hooks;
    s->hooks.login = hook_login;
    s->hooks.file = hook_file;
    s->hooks.user = s;
    conn_count++;
    rng_add(&s->since, sizeof s->since);
    cdv_init2(&s->sess, X.slot_ctl[s - SL], X.slot_ctlcap, NULL, 0, X.slot_in[s - SL],
              X.slot_incap, &s->hooks, X.ident, TickCount() ^ (conn_count << 16) ^ rng_u32());
    cdv_start(&s->sess, now_ms(), secure);
}

static void slot_step(engine_slot_t *s)
{
    const uint8_t *data;
    size_t len;
    int i, desk = s == DS;

    for (i = 0; i < 4 && s->owner == O_ENGINE && tcp_recv(s->t, &data, &len); i++) {
        cdv_feed(&s->sess, data, len);
        if (desk)
            V.still = 0; /* the peer is doing something: look sooner */
    }
    /* A file manager that has logged in goes to the main loop. */
    if (s->kind == K_FILE && s->sess.state == CDV_LIVE && s->owner == O_ENGINE) {
        s->owner = O_MAIN;
        return;
    }
    desk = s == DS;
    len = tcp_sent(s->t);
    if (len)
        cdv_out_consume(&s->sess, len);
    cdv_tick(&s->sess, now_ms());
    if (desk) {
        eng.live = s->sess.state == CDV_LIVE;
        eng.secure = s->sess.enc;
    }

    if (desk && eng.live) {
        if (eng.announce) {
            eng.announce = 0;
            cdv_send_display(&s->sess, X.scr->width, X.scr->height);
            s->sess.refresh = 1;
        }
        if (clip_out.ready) {
            cdv_send_clipboard(&s->sess, clip_out.text, clip_out.len);
            clip_out.ready = 0;
        }
        if (chat_out.ready) {
            cdv_send_chat(&s->sess, chat_out.text, chat_out.len);
            chat_out.ready = 0;
        }
        video_step();
        cursor_step();
    }
    if (tcp_send_idle(s->t)) {
        const uint8_t *o = cdv_out_peek(&s->sess, &len);
        if (len)
            tcp_send(s->t, o, len);
    }

    if (tcp_failed(s->t) || s->t->state != T_OPEN) {
        end_slot(s, "connection lost");
    } else if (s->sess.state == CDV_CLOSED && tcp_send_idle(s->t)) {
        cdv_out_peek(&s->sess, &len);
        if (!len)
            end_slot(s, "session closed");
    } else if (s->sess.state != CDV_LIVE && TickCount() - s->since > LOGIN_TIMEOUT) {
        end_slot(s, "no login; dropped");
    }
}

static void session_step(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        engine_slot_t *s = &SL[i];
        if (!s->t)
            continue;
        if (s->owner == O_ENGINE)
            slot_step(s);
        else if (s->owner == O_DONE) {
            /* The main loop is finished with a file manager: the stream has
             * been reset (or is being) -- the slot is free again. */
            s->t = NULL;
            s->kind = K_PENDING;
            s->owner = O_ENGINE;
        }
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

enum { SRV_ASK, SRV_WAIT, SRV_DNS };
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
    cdv_tcp *rs;                /* the relay stream this request is using */
    int srv;                    /* SRV_*: how the server's address is being found */
    unsigned long srv_asked;
    uint8_t msg[512];
    size_t msglen;
} R;

static void action_step(void)
{
    if (R.step == A_NONE)
        return;
    if (TickCount() - R.step_since > 15 * 60) {
        {
            char line[64];
            put_num(put_str(line, "the ID server's request timed out at step "), R.step);
            engine_log(line);
        }
        log_stream("helper", X.helper);
        tcp_abort(X.helper);
        if (R.rs && !slot_of(R.rs))
            tcp_abort(R.rs);
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
            } else if (name_q.ans == -1 || TickCount() - R.asked_at > 6 * 60) {
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
        /* A relay stream no session is on: two, for a desktop and a file
         * manager at once. Both busy with logged-in sessions: turn it down. */
        if (!R.rs) {
            cdv_tcp *c[2];
            int k;
            c[0] = X.relay;
            c[1] = X.relay2;
            for (k = 0; k < 2 && !R.rs; k++)
                if (c[k] && !slot_of(c[k]))
                    R.rs = c[k];
            for (k = 0; k < 2 && !R.rs; k++) {
                engine_slot_t *o = c[k] ? slot_of(c[k]) : NULL;
                if (o && o->owner == O_ENGINE && o->sess.state != CDV_LIVE) {
                    end_slot(o, "an earlier relay session gave way");
                    R.rs = c[k];
                }
            }
            if (!R.rs) {
                R.step = A_NONE; /* busy with peers already */
                break;
            }
        }
        if (R.rs->state == T_IDLE) {
            if (R.rs->used)
                R.rs->renew_req = 1; /* a fresh stream first: see tcp_renew */
            else if (!R.rs->renew_req)
                tcp_connect(R.rs, R.relay_ip, R.relay_port);
        } else if (R.rs->state == T_OPEN) {
            R.msglen = rdv_request_relay(&R.r, &R.act, R.msg, sizeof R.msg);
            tcp_send(R.rs, R.msg, R.msglen);
            R.step = A_RELAY_SEND;
        }
        break;
    case A_RELAY_SEND:
        if (R.rs->state != T_OPEN) {
            engine_log("the relay closed the connection");
            R.step = A_NONE;
            break;
        }
        if (tcp_sent(R.rs) || tcp_send_idle(R.rs)) {
            engine_log("joined the relay");
            R.step = A_NONE;
            attach(R.rs, 1);
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

    /* The server's address first: the Mac's resolver, through the main
     * loop; then DNS of our own, if that says no or takes too long. */
    if (!R.ip && TickCount() >= R.retry_at) {
        uint32_t found = 0;
        switch (R.srv) {
        case SRV_ASK:
            if (!name_q.req || name_q.ans) {
                strncpy(name_q.name, R.host, sizeof name_q.name - 1);
                name_q.ans = 0;
                name_q.req = 1;
                R.srv_asked = TickCount();
                R.srv = SRV_WAIT;
                eng.rdv_state = RS_RESOLVING;
            }
            break;
        case SRV_WAIT:
            if (name_q.ans == 1) {
                found = name_q.ip;
                name_q.req = 0;
            } else if (name_q.ans == -1 || TickCount() - R.srv_asked > 12 * 60) {
                name_q.req = 0;
                R.srv = SRV_DNS;
                dns_start(R.host);
            }
            break;
        case SRV_DNS:
            if (D.busy)
                break;
            if (D.result == 1 && !strcmp(D.name, R.host)) {
                found = D.ip;
            } else {
                eng.rdv_state = RS_NO_DNS;
                R.retry_at = TickCount() + 30 * 60;
                R.srv = SRV_ASK;
            }
            D.result = 0;
            break;
        }
        if (found) {
            R.ip = found;
            R.srv = SRV_ASK;
            eng.server_ip = R.ip;
            eng.rdv_state = RS_REGISTERING;
            engine_log("found the ID server");
            rdv_init(&R.r);
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
                R.rs = NULL; /* a relay stream is chosen when it is needed */
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

void engine_screenshot_request(void)
{
    if (shot.state == SHOT_NONE) {
        shot.err[0] = 0;
        shot.len = 0;
        shot.conn = conn_count;
        shot.state = SHOT_WANTED;
    }
}

void engine_set_view(int quality, int fps)
{
    if (quality >= 0 && quality <= 127) {
        X.q = quality;
        V.q = quality;
        vp8e_set_q(X.enc, quality);
    }
    if (fps > 0)
        scan_ms = 1000 / fps < SCAN_MS ? SCAN_MS : 1000 / fps;
}

void engine_setup(const engine_ctx *ctx)
{
    X = *ctx;
    X.q_base = X.q;
    /* Also a restart (Stop, then Start, or new settings): nothing of the last
     * run's streams survives -- they were released and made again. */
    memset(SL, 0, sizeof SL);
    DS = NULL;
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
    if (DS)
        cdv_set_video(&DS->sess, outq, outcap);
    memset(&V, 0, sizeof V);
    V.q = X.q;
}

static void listen_step(cdv_tcp *t, tcp_port port, int secure)
{
    int r;
    if (!t)
        return;
    if (slot_of(t)) {
        /* A session's stream: the main loop polls a file manager's own. */
        return;
    }
    r = tcp_poll(t);
    if (r == 1)
        attach(t, secure);
    else if (t->state == T_IDLE)
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
    listen_step(X.direct2, X.direct_port, 0);
    if (R.enabled) {
        listen_step(X.local, X.local_port, 1);
        listen_step(X.local2, X.local_port, 1);
    }
    tcp_poll(X.helper);
    if (X.relay && !slot_of(X.relay))
        tcp_poll(X.relay);
    if (X.relay2 && !slot_of(X.relay2))
        tcp_poll(X.relay2);
    rdv_step();
    lan_step();
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
