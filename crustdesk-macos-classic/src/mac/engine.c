#include "engine.h"
#include "input.h"

#include <string.h>

#define HEARTBEAT_MS 20
#define SCAN_MS 60            /* start a frame at most this often... */
#define SCAN_IDLE_MS 250      /* ...or this often once the screen has been still */
#define IDLE_AFTER 8          /* scans that found nothing before slowing down */
#define REFRESH_MS 400        /* re-send one row of macroblocks this often */
#define SLICE_TICKS 2         /* a slice ends after this many 60ths of a second */

engine_flags eng;

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

/* "key 1234 ms scan, 5678 ms encode, 99 KB" without printf. */
static char *put_num(char *p, unsigned long v)
{
    char tmp[12];
    int n = 0;
    do
        tmp[n++] = (char)('0' + v % 10);
    while ((v /= 10) != 0);
    while (n)
        *p++ = tmp[--n];
    return p;
}

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
                if (st.key || (eng.frames & 63) == 0)
                    log_timing(st.key, V.t_scanned - V.t_start, TickCount() - V.t_scanned, len,
                               st.mbs - st.skipped);
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

/* ---- the tick ------------------------------------------------------------- */

void engine_setup(const engine_ctx *ctx)
{
    X = *ctx;
    memset(&V, 0, sizeof V);
    V.q = X.q;
}

void engine_replace_video(vp8e *enc, uint8_t *outq, size_t outcap)
{
    X.enc = enc;
    X.outq = outq;
    X.outcap = outcap;
    memset(&V, 0, sizeof V);
    V.q = X.q;
}

void engine_tick(void)
{
    cdv_net *net = X.net;
    const uint8_t *data;
    size_t len;
    int i;

    eng.ticks++;
    if (eng.suspend_req) {
        video_reset();
        eng.suspended = 1;
        return;
    }
    eng.suspended = 0;
    if (eng.need_reset)
        return;
    if (net->err && net->state == NET_LISTENING) {
        /* A listen that failed: abort and listen again. */
        engine_log("listen failed; listening again");
        net_reset_async(net);
    }

    if (net_accepted(net)) {
        engine_log("connection");
        conn_count++;
        cdv_init(X.sess, X.outq, X.outcap, X.inq, X.incap, X.hooks, X.ident,
                 TickCount() ^ (conn_count << 16));
        cdv_start(X.sess, now_ms());
        memset(&V, 0, sizeof V);
        V.q = X.q;
        vp8e_abandon(X.enc); /* the first frame of a session is a keyframe */
    }
    if (net->state != NET_CONNECTED)
        return;

    for (i = 0; i < 4 && net_recv(net, &data, &len); i++) {
        cdv_feed(X.sess, data, len);
        V.still = 0; /* the peer is doing something: look sooner */
    }
    len = net_sent(net);
    if (len)
        cdv_out_consume(X.sess, len);
    cdv_tick(X.sess, now_ms());
    eng.live = X.sess->state == CDV_LIVE;

    if (eng.live) {
        if (eng.announce) {
            eng.announce = 0;
            cdv_send_display(X.sess, X.scr->width, X.scr->height);
            X.sess->refresh = 1;
        }
        video_step();
    }
    if (net_send_idle(net)) {
        const uint8_t *o = cdv_out_peek(X.sess, &len);
        if (len)
            net_send(net, o, len);
    }

    if (!net_failed(net) && X.sess->state == CDV_CLOSED && net_send_idle(net))
        cdv_out_peek(X.sess, &len);
    else
        len = 1;
    if (net_failed(net) || !len) {
        engine_log(net_failed(net) ? "connection lost" : "session closed");
        video_reset();
        input_release_all();
        eng.live = 0;
        net_reset_async(net);
    }
}

/* ---- Time Manager heartbeat and the deferred task -------------------------- */

/* The extended Time Manager record (Inside Macintosh: Processes 3-22), with
 * our A5 after it where the glue can find it from A1. */
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

enum { dtQType = 7 };

extern void cdv_tm_glue(void);
extern void cdv_dt_glue(void);

static cdv_tm tm;
static cdv_dt dt;
static volatile int dt_pending, running;

static OSErr dt_install(cdv_dt *task)
{
    register long a0 __asm__("a0") = (long)task;
    register long d0 __asm__("d0");
    __asm__ volatile(".short 0xA082" : "=d"(d0), "+a"(a0) : : "d1", "d2", "a1", "cc", "memory");
    return (OSErr)d0;
}

static void ins_xtime(cdv_tm *t)
{
    register long a0 __asm__("a0") = (long)t;
    __asm__ volatile(".short 0xA458" : "+a"(a0) : : "d0", "d1", "d2", "a1", "cc", "memory");
}

/* Called by the glue at interrupt time. Queue the work; do none of it. */
void cdv_tm_fire(void)
{
    if (!running)
        return;
    if (!dt_pending) {
        dt_pending = 1;
        if (dt_install(&dt) != noErr)
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
    ins_xtime(&tm);
    PrimeTime((QElemPtr)&tm, HEARTBEAT_MS);
    return noErr;
}

void engine_stop(void)
{
    running = 0;
    RmvTime((QElemPtr)&tm);
    while (dt_pending)
        ;
}
