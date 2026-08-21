/*
 * sgicap.c -- SGI-SCREEN-CAPTURE, the damage tracker this server already has.
 *
 * The capture design assumed the agent would have to find its own dirty
 * rectangles, because this X server predates the DAMAGE extension by a decade
 * and does not advertise it. It advertises SGI-SCREEN-CAPTURE instead, and
 * that turns out to be the same idea built for exactly this job:
 *
 *   SGICapRegisterInterest(dpy, drawable, x, y, w, h)  -> a handle
 *   SGICapStart(dpy, handle)                            -- begin accumulating
 *   SGICapQueryAndReset(dpy, handle, &t, &n, &order)    -> the damaged rects
 *   SGICapQueryCopyAndReset(dpy, handle, &t, &n, &order, shmbuf)
 *                                                       -> rects AND pixels,
 *                                                          one round trip
 *
 * If QueryCopyAndReset works, the agent's frame loop is one call: it returns
 * what changed and the changed pixels together, already in shared memory.
 *
 * The probe generates its own damage rather than waiting for something else to
 * move, so the answers do not depend on an idle desktop: it maps a window and
 * draws a moving block, polls, and reports what came back. It also polls once
 * with nothing drawn, because "no damage" has to be distinguishable from "the
 * call does not work".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/readdisplay.h>
#include <X11/extensions/sgicap.h>

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static char last_err[256];
static int  err_count;

static int on_x_error(Display *d, XErrorEvent *e)
{
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    snprintf(last_err, sizeof last_err, "code=%d (%s) request=%d.%d",
             e->error_code, buf, e->request_code, e->minor_code);
    err_count++;
    return 0;
}

static void clear_err(void) { last_err[0] = 0; err_count = 0; }

static int check_err(Display *d, const char *what)
{
    XSync(d, False);
    if (err_count) { printf("  X ERROR in %s: %s\n", what, last_err); return 1; }
    return 0;
}

static const char *order_name(int o)
{
    switch (o) {
    case Unsorted:  return "Unsorted";
    case YSorted:   return "YSorted";
    case YXSorted:  return "YXSorted";
    case YXBanded:  return "YXBanded";
    default:        return "?";
    }
}

static void print_rects(XRectangle *r, int n, int limit)
{
    int i;
    long area = 0;
    for (i = 0; i < n; i++) area += (long)r[i].width * (long)r[i].height;
    printf("    %d rect(s), %ld px total", n, area);
    if (n) printf(", first:");
    for (i = 0; i < n && i < limit; i++)
        printf(" (%d,%d %ux%u)", r[i].x, r[i].y, r[i].width, r[i].height);
    if (n > limit) printf(" ...");
    printf("\n");
}

int main(int argc, char **argv)
{
    Display *dpy;
    Window root, win;
    GC gc;
    int scr, W, H;
    int ev = 0, er = 0, maj = 0, min = 0;
    SGICapInterestType interest;
    XShmSegmentInfo shminfo;
    ShmReadDisplayBuf *rdbuf = NULL;
    unsigned char *buf = NULL;
    size_t bufsz;
    Time when;
    int count = 0, ordering = 0;
    XRectangle *rects;
    double t0, t1;
    int i;
    int iters = argc > 1 ? atoi(argv[1]) : 10;

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&shminfo, 0, sizeof shminfo);

    printf("step 1: display\n");
    dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("  XOpenDisplay FAILED\n"); return 1; }
    XSetErrorHandler(on_x_error);
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    W = DisplayWidth(dpy, scr);
    H = DisplayHeight(dpy, scr);
    printf("  %dx%d depth %d\n", W, H, DefaultDepth(dpy, scr));

    printf("step 2: SGI-SCREEN-CAPTURE present?\n");
    clear_err();
    if (!SGICapQueryExtension(dpy, &ev, &er)) {
        printf("  ABSENT\n");
        return 2;
    }
    printf("  present, event base %d, error base %d\n", ev, er);
    if (SGICapQueryVersion(dpy, &maj, &min))
        printf("  version %d.%d\n", maj, min);
    else
        printf("  QueryVersion returned 0 (%s)\n", err_count ? last_err : "no error");

    printf("step 3: a window to damage, so the answers do not need an idle desktop\n");
    win = XCreateSimpleWindow(dpy, root, 64, 64, 320, 240, 1,
                              WhitePixel(dpy, scr), BlackPixel(dpy, scr));
    XSelectInput(dpy, win, ExposureMask);
    XMapRaised(dpy, win);
    XSync(dpy, False);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, WhitePixel(dpy, scr));
    XFillRectangle(dpy, win, gc, 0, 0, 320, 240);
    XSync(dpy, False);
    check_err(dpy, "window setup");
    printf("  window 0x%lx mapped at 64,64 320x240\n", (unsigned long)win);

    printf("step 4: register interest in the whole root window\n");
    clear_err();
    interest = SGICapRegisterInterest(dpy, root, 0, 0, (unsigned)W, (unsigned)H);
    if (check_err(dpy, "SGICapRegisterInterest")) printf("  (continuing anyway)\n");
    printf("  interest handle 0x%lx%s\n", (unsigned long)interest,
           interest ? "" : "  <-- zero, the call did not take");
    if (!interest) goto done;

    printf("step 5: start accumulating\n");
    clear_err();
    SGICapStart(dpy, interest);
    check_err(dpy, "SGICapStart");

    printf("step 6: poll with nothing drawn -- 'no damage' must be distinct from 'broken'\n");
    XSync(dpy, False);
    clear_err();
    t0 = now_ms();
    rects = SGICapQueryAndReset(dpy, interest, &when, &count, &ordering);
    t1 = now_ms();
    printf("  %.1f ms, count %d, ordering %s, time %lu\n",
           t1 - t0, count, order_name(ordering), (unsigned long)when);
    if (rects) { print_rects(rects, count, 6); XFree(rects); }
    check_err(dpy, "SGICapQueryAndReset (idle)");

    printf("step 7: draw, then poll -- does the damage come back?\n");
    for (i = 0; i < 3; i++) {
        int x = 10 + i * 40, y = 10 + i * 30;
        XSetForeground(dpy, gc, (i & 1) ? BlackPixel(dpy, scr) : WhitePixel(dpy, scr));
        XFillRectangle(dpy, win, gc, x, y, 60, 40);
        XSync(dpy, False);
        clear_err();
        t0 = now_ms();
        rects = SGICapQueryAndReset(dpy, interest, &when, &count, &ordering);
        t1 = now_ms();
        printf("  drew (%d,%d 60x40) in the window -> %.1f ms, ordering %s\n",
               x, y, t1 - t0, order_name(ordering));
        if (rects) { print_rects(rects, count, 6); XFree(rects); }
        else printf("    NULL returned, count %d\n", count);
        check_err(dpy, "SGICapQueryAndReset");
    }

    printf("step 8: shared segment for the copy variant\n");
    bufsz = (size_t)W * (size_t)H * 4u;
    shminfo.shmid = shmget(IPC_PRIVATE, bufsz, IPC_CREAT | 0777);
    if (shminfo.shmid < 0) { printf("  shmget: %s\n", strerror(errno)); goto done; }
    shminfo.shmaddr = (char *)shmat(shminfo.shmid, NULL, 0);
    if (shminfo.shmaddr == (char *)-1) { printf("  shmat: %s\n", strerror(errno)); goto done; }
    shminfo.readOnly = False;
    buf = (unsigned char *)shminfo.shmaddr;
    clear_err();
    XShmAttach(dpy, &shminfo);
    check_err(dpy, "XShmAttach");
    rdbuf = XShmCreateReadDisplayBuf(dpy, (char *)buf, &shminfo, W, H);
    if (!rdbuf) { printf("  XShmCreateReadDisplayBuf returned NULL\n"); goto done; }
    printf("  %dx%d shm buffer ready\n", rdbuf->width, rdbuf->height);

    printf("step 9: QueryCopyAndReset -- rects and pixels in one round trip\n");
    for (i = 0; i < iters; i++) {
        int x = 8 + (i * 23) % 240, y = 8 + (i * 17) % 180;
        unsigned long before, after;
        memset(buf, 0, 4096);
        XSetForeground(dpy, gc, (i & 1) ? BlackPixel(dpy, scr) : WhitePixel(dpy, scr));
        XFillRectangle(dpy, win, gc, x, y, 64, 48);
        XSync(dpy, False);

        before = 0;
        clear_err();
        t0 = now_ms();
        rects = SGICapQueryCopyAndReset(dpy, interest, &when, &count, &ordering, rdbuf);
        t1 = now_ms();
        after = 0;
        {   /* did any pixels actually land where the first rect says? */
            int nz = 0, k;
            if (rects && count > 0) {
                size_t off = ((size_t)rects[0].y * (size_t)W + (size_t)rects[0].x) * 4u;
                for (k = 0; k < 256 && off + k < bufsz; k++) if (buf[off + k]) nz++;
            }
            after = (unsigned long)nz;
        }
        printf("  iter %2d: %7.1f ms  count %3d  ordering %-8s  nonzero-at-first-rect %lu\n",
               i, t1 - t0, count, order_name(ordering), after);
        if (rects) { print_rects(rects, count, 4); XFree(rects); }
        else printf("    NULL returned\n");
        if (check_err(dpy, "SGICapQueryCopyAndReset")) break;
    }

    printf("step 10: stop and withdraw\n");
    clear_err();
    SGICapStop(dpy, interest);
    check_err(dpy, "SGICapStop");
    SGICapWithdrawInterest(dpy, interest);
    check_err(dpy, "SGICapWithdrawInterest");

done:
    if (rdbuf) XShmDestroyReadDisplayBuf(rdbuf);
    if (shminfo.shmaddr && shminfo.shmaddr != (char *)-1) {
        XShmDetach(dpy, &shminfo);
        XSync(dpy, False);
        shmdt(shminfo.shmaddr);
    }
    if (shminfo.shmid > 0) shmctl(shminfo.shmid, IPC_RMID, NULL);
    printf("done\n");
    XCloseDisplay(dpy);
    return 0;
}
