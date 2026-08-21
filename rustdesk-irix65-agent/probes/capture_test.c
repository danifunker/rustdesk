/*
 * capture_test.c -- exercise capture_shim.c on the machine it has to work on.
 *
 * Three things this checks that a compile cannot:
 *
 *   1. The damage path returns rectangles whose pixels really are in the canvas
 *      at the coordinates it reported. The probe draws known blocks and looks
 *      for them, rather than trusting the count.
 *   2. The fallback path works. It is forced with RD_PATH_GETIMAGE even though
 *      this server has every extension, because otherwise its first run would
 *      be on a machine nobody is watching.
 *   3. Steady-state cost, both idle and busy, since that is what decides the
 *      agent's frame budget.
 *
 * Takes an optional iteration count.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "../src/capture_shim.h"

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static const char *path_name(int p)
{
    switch (p) {
    case RD_PATH_DAMAGE:   return "damage (SGI-SCREEN-CAPTURE + ReadDisplay/shm)";
    case RD_PATH_READDISP: return "readdisplay (shm, whole screen per frame)";
    case RD_PATH_GETIMAGE: return "getimage (plain Xlib fallback)";
    default:               return "?";
    }
}

/* A second connection whose only job is to make the screen change, so the
 * damage path has something to find. */
struct painter {
    Display *dpy;
    Window   win;
    GC       gc;
    int      black, white;
};

static int painter_open(struct painter *p, int x, int y, int w, int h)
{
    int scr;
    p->dpy = XOpenDisplay(NULL);
    if (!p->dpy) return -1;
    scr = DefaultScreen(p->dpy);
    p->black = BlackPixel(p->dpy, scr);
    p->white = WhitePixel(p->dpy, scr);
    p->win = XCreateSimpleWindow(p->dpy, RootWindow(p->dpy, scr), x, y, w, h,
                                 0, p->white, p->black);
    XMapRaised(p->dpy, p->win);
    p->gc = XCreateGC(p->dpy, p->win, 0, NULL);
    XSync(p->dpy, False);
    return 0;
}

static void painter_block(struct painter *p, int x, int y, int w, int h, int white)
{
    XSetForeground(p->dpy, p->gc, white ? p->white : p->black);
    XFillRectangle(p->dpy, p->win, p->gc, x, y, (unsigned)w, (unsigned)h);
    XSync(p->dpy, False);
}

static void painter_close(struct painter *p)
{
    if (!p->dpy) return;
    XFreeGC(p->dpy, p->gc);
    XDestroyWindow(p->dpy, p->win);
    XCloseDisplay(p->dpy);
    p->dpy = NULL;
}

/* Is any pixel in this rectangle non-black? Used to confirm that a reported
 * rectangle actually carries pixels. */
static int rect_has_ink(const unsigned char *buf, int stride, int x, int y,
                        int w, int h)
{
    int yy, xx;
    for (yy = y; yy < y + h; yy++) {
        const unsigned char *row = buf + (size_t)yy * stride;
        for (xx = x; xx < x + w; xx++)
            if (row[xx * 4 + 1] || row[xx * 4 + 2] || row[xx * 4 + 3])
                return 1;
    }
    return 0;
}

static void run(int max_path, int iters, struct painter *pnt)
{
    rd_capture *c;
    int rects[4 * 64];
    int n, i, w, h, stride;
    const unsigned char *buf;
    double t0, t1, sum = 0, lo = 1e9, hi = 0;

    printf("\n=== opening with max_path=%d ===\n", max_path);
    c = rd_capture_open_forced(NULL, max_path);
    if (!c) { printf("  open FAILED\n"); return; }

    w = rd_capture_width(c);
    h = rd_capture_height(c);
    stride = rd_capture_stride(c);
    buf = rd_capture_buffer(c);
    printf("  path            %s\n", path_name(rd_capture_path(c)));
    printf("  geometry        %dx%d, screen depth %d, canvas stride %d\n",
           w, h, rd_capture_depth(c), stride);
    printf("  cursor embedded %s\n",
           rd_capture_cursor_embedded(c) ? "yes -- agent must not send a shape"
                                         : "no -- agent has to draw one");

    t0 = now_ms();
    if (rd_capture_full(c) != 0) {
        printf("  full read FAILED: %s\n", rd_capture_last_error(c));
        rd_capture_close(c);
        return;
    }
    t1 = now_ms();
    printf("  full read       %.1f ms (%.2f MB/s)\n", t1 - t0,
           ((double)stride * h / 1048576.0) / ((t1 - t0) / 1000.0));

    /* First poll must report the whole screen. */
    n = rd_capture_poll(c, rects, 64);
    printf("  first poll      %d rect(s)%s\n", n,
           (n == 1 && rects[2] == w && rects[3] == h) ? " -- whole screen, correct" : "");

    /* Idle: nothing is drawing, so this is the floor cost per frame. */
    lo = 1e9; hi = 0; sum = 0;
    for (i = 0; i < 5; i++) {
        t0 = now_ms();
        n = rd_capture_poll(c, rects, 64);
        t1 = now_ms();
        if (t1 - t0 < lo) lo = t1 - t0;
        if (t1 - t0 > hi) hi = t1 - t0;
        sum += t1 - t0;
        if (n < 0) { printf("  idle poll FAILED: %s\n", rd_capture_last_error(c)); break; }
    }
    printf("  idle poll       min %.1f avg %.1f max %.1f ms, last count %d\n",
           lo, sum / 5.0, hi, n);

    /* Busy: draw a known block, poll, and check the canvas really has it. */
    if (pnt) {
        int found = 0, reported = 0, missed = 0;
        lo = 1e9; hi = 0; sum = 0;
        for (i = 0; i < iters; i++) {
            int bx = 8 + (i * 29) % 200, by = 8 + (i * 19) % 140;
            painter_block(pnt, bx, by, 48, 36, 1);   /* white on black */
            t0 = now_ms();
            n = rd_capture_poll(c, rects, 64);
            t1 = now_ms();
            if (n < 0) { printf("  busy poll FAILED: %s\n", rd_capture_last_error(c)); break; }
            if (t1 - t0 < lo) lo = t1 - t0;
            if (t1 - t0 > hi) hi = t1 - t0;
            sum += t1 - t0;
            reported += n;
            if (n > 0) {
                int k, any = 0;
                for (k = 0; k < n && k < 64; k++) {
                    int rx = rects[k * 4], ry = rects[k * 4 + 1];
                    int rw = rects[k * 4 + 2], rh = rects[k * 4 + 3];
                    if (rx < 0 || ry < 0 || rx + rw > w || ry + rh > h) {
                        printf("  rect %d out of bounds: %d,%d %dx%d\n", k, rx, ry, rw, rh);
                        continue;
                    }
                    if (rect_has_ink(buf, stride, rx, ry, rw, rh)) any = 1;
                }
                if (any) found++; else missed++;
            }
            painter_block(pnt, bx, by, 48, 36, 0);   /* back to black */
        }
        printf("  busy poll       min %.1f avg %.1f max %.1f ms\n",
               lo, sum / (double)iters, hi);
        printf("  rects reported  %d over %d frames; canvas had the pixels %d time(s), missed %d\n",
               reported, iters, found, missed);
    }

    /* Downscale, the agent's main lever on encode cost. */
    {
        int f;
        unsigned char *dst = (unsigned char *)malloc((size_t)w * h);
        if (dst) {
            for (f = 2; f <= 4; f += 2) {
                t0 = now_ms();
                i = rd_scale_abgr(buf, w, h, stride, dst, f);
                t1 = now_ms();
                printf("  downscale 1/%d    %.1f ms -> %dx%d\n", f, t1 - t0, i, h / f);
            }
            free(dst);
        }
    }

    printf("  dead?           %s\n", rd_capture_is_dead(c) ? "yes" : "no");
    rd_capture_close(c);
    printf("  closed cleanly\n");
}

/* Open, use, close, and open again -- the sequence the agent runs every time
 * this X server wedges and gets restarted. Worth its own mode: the first run of
 * this probe passed on a first open and failed on a second one in the same
 * process, which would have shown up in the field as "capture works until X
 * restarts, then never again". */
static void run_reopen(int path, int iters, struct painter *pnt)
{
    int i;
    printf("\n### reopen test: three consecutive opens of path %d ###\n", path);
    for (i = 0; i < 3; i++) {
        printf("--- open %d ---\n", i + 1);
        run(path, iters, pnt);
    }
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 8;
    const char *mode = argc > 2 ? argv[2] : "all";
    struct painter pnt;
    struct painter *pp;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("capture_test: %d busy iterations, mode %s\n", iters, mode);

    memset(&pnt, 0, sizeof pnt);
    pp = (painter_open(&pnt, 100, 100, 320, 240) == 0) ? &pnt : NULL;
    if (!pp)
        printf("could not open a second connection to draw with; "
               "damage checks will be idle-only\n");

    if (strcmp(mode, "all") == 0) {
        run(RD_PATH_DAMAGE, iters, pp);
        run(RD_PATH_READDISP, iters, pp);
        run(RD_PATH_GETIMAGE, iters, pp);
    } else if (strcmp(mode, "reopen") == 0) {
        run_reopen(RD_PATH_DAMAGE, iters, pp);
    } else if (strcmp(mode, "reopen1") == 0) {
        run_reopen(RD_PATH_READDISP, iters, pp);
    } else {
        run(atoi(mode), iters, pp);
    }

    if (pp) painter_close(pp);
    printf("\ncapture_test done\n");
    return 0;
}
