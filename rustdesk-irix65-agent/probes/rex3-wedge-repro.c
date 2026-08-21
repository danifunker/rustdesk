/*
 * rex3-wedge-repro.c -- minimal reproducer: read the whole screen in a loop
 * until the X server stops answering.
 *
 * Standalone on purpose. Nothing here belongs to any particular application;
 * it is the smallest thing that provokes the fault, so it can be handed to
 * someone working on the emulator without any of the surrounding project.
 *
 *   cc -o rex3-wedge-repro rex3-wedge-repro.c -lXext -lX11
 *   DISPLAY=:0 ./rex3-wedge-repro [iterations]
 *
 * WHAT THIS DOES AND DOES NOT SHOW. None of these modes reproduces the stall
 * reliably. Measured on IRIX 6.5.22m under iris:
 *
 *   mode 0, 50 back-to-back full-screen reads      -- no stall
 *   mode 1, 40 reads with the cursor composited    -- no stall
 *   mode 2, 40 damage polls                        -- no stall
 *   mode 3, 12 reads with a 5 s CPU burn between   -- stalled once, then a
 *                                                     second identical run
 *                                                     completed all 12
 *
 * So the fault is intermittent and is not isolated by any single one of these.
 * The workload that does provoke it nearly every time is a real screen-sharing
 * session: capture, several seconds of VP8 encoding, repeat, with a second X
 * connection open for input injection. This file exists to rule things out and
 * to give somebody a starting point, not as a one-command reproducer.
 *
 * Unbuffered and prints before each read, so a slow machine is distinguishable
 * from a hang: if you see "iteration N: reading..." and nothing after it, that
 * read is the one that never returned.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/readdisplay.h>
#include <X11/extensions/sgicap.h>

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 50;
    /* mode: 0 = plain full-screen read (hints 0)
     *       1 = same, but ask for the cursor to be composited in
     *       2 = SGI-SCREEN-CAPTURE damage path, which is what the agent uses
     *       3 = full-screen read, then burn CPU for a few seconds, repeat.
     *           The application that provokes this spends ~6 s encoding video
     *           between reads, and back-to-back reads do NOT provoke it. */
    int mode = argc > 2 ? atoi(argv[2]) : 0;
    Display *dpy;
    Window root;
    XShmSegmentInfo si;
    ShmReadDisplayBuf *rdbuf;
    XRectangle r;
    unsigned long hints = 0;
    unsigned char *buf;
    size_t sz;
    int W, H, ev, er, maj, min, i;
    Bool pm;

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&si, 0, sizeof si);

    dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("XOpenDisplay failed\n"); return 1; }
    root = DefaultRootWindow(dpy);
    W = DisplayWidth(dpy, DefaultScreen(dpy));
    H = DisplayHeight(dpy, DefaultScreen(dpy));
    printf("display %dx%d depth %d\n", W, H, DefaultDepth(dpy, DefaultScreen(dpy)));

    if (!XShmQueryExtension(dpy)) { printf("no MIT-SHM\n"); return 2; }
    XShmQueryVersion(dpy, &maj, &min, &pm);
    if (!XReadDisplayQueryExtension(dpy, &ev, &er)) { printf("no ReadDisplay\n"); return 2; }

    sz = (size_t)W * H * 4;
    si.shmid = shmget(IPC_PRIVATE, sz, IPC_CREAT | 0600);
    if (si.shmid < 0) { printf("shmget: %s\n", strerror(errno)); return 3; }
    si.shmaddr = (char *)shmat(si.shmid, NULL, 0);
    si.readOnly = False;
    buf = (unsigned char *)si.shmaddr;
    XShmAttach(dpy, &si);
    XSync(dpy, False);
    shmctl(si.shmid, IPC_RMID, NULL);

    rdbuf = XShmCreateReadDisplayBuf(dpy, (char *)buf, &si, W, H);
    if (!rdbuf) { printf("XShmCreateReadDisplayBuf returned NULL\n"); return 3; }

    r.x = 0; r.y = 0;
    r.width = (unsigned short)W;
    r.height = (unsigned short)H;

    printf("mode %d (%s)\n", mode,
           mode == 0 ? "plain read" :
           mode == 1 ? "read with XRD_READ_POINTER" :
           mode == 2 ? "SGICapQueryCopyAndReset" :
                       "read, then burn CPU");

    if (mode == 2) {
        SGICapInterestType interest;
        Time when;
        int count = 0, ordering = 0;
        if (!SGICapQueryExtension(dpy, &ev, &er)) {
            printf("no SGI-SCREEN-CAPTURE\n");
            return 2;
        }
        interest = SGICapRegisterInterest(dpy, root, 0, 0, (unsigned)W, (unsigned)H);
        XSync(dpy, False);
        if (!interest) { printf("RegisterInterest returned 0\n"); return 3; }
        SGICapStart(dpy, interest);
        XSync(dpy, False);
        for (i = 1; i <= iters; i++) {
            XRectangle *got;
            double t0, t1;
            printf("iteration %d: QueryCopyAndReset ...", i);
            t0 = now_ms();
            got = SGICapQueryCopyAndReset(dpy, interest, &when, &count,
                                          &ordering, rdbuf);
            XSync(dpy, False);
            t1 = now_ms();
            printf(" %.0f ms, %d rect(s)\n", t1 - t0, count);
            if (got) XFree(got);
        }
        printf("completed %d damage polls without a stall\n", iters);
        SGICapStop(dpy, interest);
        SGICapWithdrawInterest(dpy, interest);
        return 0;
    }

    for (i = 1; i <= iters; i++) {
        double t0, t1;
        printf("iteration %d: reading %dx%d ...", i, W, H);
        t0 = now_ms();
        XShmReadDisplayRects(dpy, root, &r, 1, rdbuf, 0, 0,
                             mode == 1 ? XRD_READ_POINTER : 0, &hints);
        XSync(dpy, False);          /* the call that does not come back */
        t1 = now_ms();
        printf(" %.0f ms\n", t1 - t0);
        if (mode == 3) {
            /* Burn wall-clock the way an encoder would: busy, not sleeping,
             * so the emulated CPU is genuinely occupied. */
            double until = now_ms() + 5000.0;
            volatile unsigned long acc = 0;
            printf("  burning 5 s of CPU ...");
            while (now_ms() < until) { int k; for (k = 0; k < 100000; k++) acc += k; }
            printf(" done\n");
        }
    }

    printf("completed %d full-screen reads without a stall\n", iters);
    XShmDestroyReadDisplayBuf(rdbuf);
    XShmDetach(dpy, &si);
    XSync(dpy, False);
    shmdt(si.shmaddr);
    return 0;
}
