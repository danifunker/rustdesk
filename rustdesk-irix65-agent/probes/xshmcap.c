/*
 * xshmcap.c -- the shared-memory dirty-rect capture path, measured.
 *
 * xcapture.c answered the format questions: ReadDisplay is present, it hands
 * back 32 bits per pixel as x,B,G,R in memory even though this screen is 8-bit
 * pseudocolour, and XRD_READ_POINTER composites the hardware cursor for us.
 * What it did not answer is how the agent should actually pull frames, and
 * that turns on XShmReadDisplayRects: it takes a *list* of rectangles and one
 * shared-memory destination, so a frame with three small changes costs three
 * small reads and one round trip rather than a full screen.
 *
 * The header states the signature and nothing else, so everything below is a
 * measurement rather than a reading of the documentation:
 *
 *   1. Does XShmCreateReadDisplayBuf expect us to have called XShmAttach, or
 *      does it attach the segment itself? Getting this wrong is a BadAccess
 *      at the first read, so the probe tries the documented order and reports
 *      exactly what happened.
 *   2. Where does each rectangle land in the destination buffer? One dstx/dsty
 *      pair covers the whole list, which can only mean the rectangles keep
 *      their relative geometry. Verified by reading two rectangles whose
 *      contents differ and finding them.
 *   3. What does a read cost as the rectangle list changes shape -- one big
 *      rectangle, a few medium ones, many thin ones? That is the difference
 *      between "dirty rectangles are worth the complexity" and "just send
 *      the screen".
 *   4. Is the shm path actually cheaper than XReadDisplay's protocol copy?
 *   5. Does a sampled grid (every Nth row) cost proportionally less? That is
 *      the change-detection budget, since this server has no DAMAGE extension
 *      and the agent has to find its own dirty rectangles.
 *   6. Does repeated capture stay stable, or does something leak or wedge?
 *
 * Cheapest first, unbuffered, and every step announces itself before it runs,
 * so a slow machine never looks like a hang.
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

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* X errors arrive asynchronously and the default handler exits. For a probe
 * that is deliberately trying calls whose contract is unknown, that turns a
 * finding into a silent death, so record them instead. */
static char last_err[256];
static int  err_count;

static int on_x_error(Display *d, XErrorEvent *e)
{
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    snprintf(last_err, sizeof last_err,
             "code=%d (%s) request=%d.%d resource=0x%lx",
             e->error_code, buf, e->request_code, e->minor_code,
             (unsigned long)e->resourceid);
    err_count++;
    return 0;
}

static void clear_err(void) { last_err[0] = 0; err_count = 0; }

static int had_err(Display *d)
{
    XSync(d, False);
    return err_count;
}

static void report_err(const char *what)
{
    if (err_count)
        printf("  X ERROR during %s: %s (%d total)\n", what, last_err, err_count);
}

/* Sum of a buffer, used only to prove that a read actually wrote something
 * and to tell two regions apart without dumping pixels. */
static unsigned long checksum(const unsigned char *p, int n)
{
    unsigned long s = 0;
    int i;
    for (i = 0; i < n; i++) s = s * 31u + p[i];
    return s;
}

static int nonzero_bytes(const unsigned char *p, int n)
{
    int i, c = 0;
    for (i = 0; i < n; i++) if (p[i]) c++;
    return c;
}

int main(int argc, char **argv)
{
    Display *dpy;
    Window root;
    int scr, W, H;
    int ev = 0, er = 0, maj = 0, min = 0;
    int shm_maj = 0, shm_min = 0;
    Bool shm_pixmaps = False;
    XShmSegmentInfo shminfo;
    ShmReadDisplayBuf *rdbuf = NULL;
    unsigned char *buf = NULL;
    size_t bufsz;
    unsigned long hints_ret = 0;
    XRectangle rects[256];
    double t0, t1;
    int i, n;
    int iters = argc > 1 ? atoi(argv[1]) : 8;

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&shminfo, 0, sizeof shminfo);

    printf("step 1: opening display\n");
    dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("  XOpenDisplay FAILED\n"); return 1; }
    XSetErrorHandler(on_x_error);
    scr  = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    W    = DisplayWidth(dpy, scr);
    H    = DisplayHeight(dpy, scr);
    printf("  %dx%d depth %d, image byte order %s\n", W, H,
           DefaultDepth(dpy, scr),
           ImageByteOrder(dpy) == MSBFirst ? "MSBFirst" : "LSBFirst");

    printf("step 2: extensions\n");
    {
        int next = 0, k;
        char **list = XListExtensions(dpy, &next);
        printf("  %d advertised:", next);
        for (k = 0; k < next; k++) printf(" %s", list[k]);
        printf("\n");
        for (k = 0; k < next; k++)
            if (strcmp(list[k], "DAMAGE") == 0)
                printf("  DAMAGE is present -- change detection could use it\n");
        XFreeExtensionList(list);
    }
    if (!XReadDisplayQueryExtension(dpy, &ev, &er)) {
        printf("  ReadDisplay ABSENT -- nothing below applies\n");
        return 2;
    }
    XReadDisplayQueryVersion(dpy, &maj, &min);
    printf("  ReadDisplay %d.%d (event base %d, error base %d)\n", maj, min, ev, er);
    if (!XShmQueryExtension(dpy)) {
        printf("  MIT-SHM ABSENT -- no shared-memory path on this server\n");
        return 2;
    }
    XShmQueryVersion(dpy, &shm_maj, &shm_min, &shm_pixmaps);
    printf("  MIT-SHM %d.%d, shared pixmaps %s\n", shm_maj, shm_min,
           shm_pixmaps ? "yes" : "no");

    printf("step 3: allocating a %dx%d 32-bit shared segment\n", W, H);
    bufsz = (size_t)W * (size_t)H * 4u;
    shminfo.shmid = shmget(IPC_PRIVATE, bufsz, IPC_CREAT | 0777);
    if (shminfo.shmid < 0) {
        printf("  shmget(%lu) failed: %s\n", (unsigned long)bufsz, strerror(errno));
        return 3;
    }
    shminfo.shmaddr = (char *)shmat(shminfo.shmid, NULL, 0);
    if (shminfo.shmaddr == (char *)-1) {
        printf("  shmat failed: %s\n", strerror(errno));
        return 3;
    }
    shminfo.readOnly = False;
    buf = (unsigned char *)shminfo.shmaddr;
    printf("  shmid %d at %p, %lu bytes\n", shminfo.shmid, (void *)buf,
           (unsigned long)bufsz);

    printf("step 4: XShmAttach\n");
    clear_err();
    if (!XShmAttach(dpy, &shminfo)) printf("  XShmAttach returned False\n");
    if (had_err(dpy)) { report_err("XShmAttach"); }
    else printf("  attached cleanly\n");

    printf("step 5: XShmCreateReadDisplayBuf\n");
    clear_err();
    rdbuf = XShmCreateReadDisplayBuf(dpy, (char *)buf, &shminfo, W, H);
    if (had_err(dpy)) report_err("XShmCreateReadDisplayBuf");
    if (!rdbuf) { printf("  returned NULL -- cannot continue\n"); goto cleanup; }
    printf("  buf %dx%d offset %d shminfo %p (ours %p)\n",
           rdbuf->width, rdbuf->height, rdbuf->offset,
           (void *)rdbuf->shminfo, (void *)&shminfo);

    /* 6. One small rectangle, to establish that the call works at all and to
     * see the pixel layout the shm path delivers. */
    printf("step 6: one 64x64 rect at (0,0)\n");
    memset(buf, 0, 64 * 64 * 4);
    rects[0].x = 0; rects[0].y = 0; rects[0].width = 64; rects[0].height = 64;
    clear_err();
    t0 = now_ms();
    n = XShmReadDisplayRects(dpy, root, rects, 1, rdbuf, 0, 0, 0, &hints_ret);
    XSync(dpy, False);
    t1 = now_ms();
    printf("  status %d in %.1f ms, hints_ret=0x%lx\n", n, t1 - t0, hints_ret);
    report_err("XShmReadDisplayRects");
    printf("  first 4 words:");
    for (i = 0; i < 4; i++)
        printf(" %02x%02x%02x%02x", buf[i*4], buf[i*4+1], buf[i*4+2], buf[i*4+3]);
    printf("   (memory order is byte0..byte3 as printed)\n");
    printf("  nonzero bytes in the first 64 rows: %d of %d\n",
           nonzero_bytes(buf, 64 * 64 * 4), 64 * 64 * 4);

    /* 7. Two rectangles far apart, to learn where each lands. If the call
     * preserves geometry, the second rect's data sits at its own y offset in
     * the destination, and the gap between them stays untouched. */
    printf("step 7: two rects, (0,0,64,64) and (256,256,64,64), dst (0,0)\n");
    memset(buf, 0, bufsz);
    rects[0].x = 0;   rects[0].y = 0;   rects[0].width = 64; rects[0].height = 64;
    rects[1].x = 256; rects[1].y = 256; rects[1].width = 64; rects[1].height = 64;
    clear_err();
    t0 = now_ms();
    n = XShmReadDisplayRects(dpy, root, rects, 2, rdbuf, 0, 0, 0, &hints_ret);
    XSync(dpy, False);
    t1 = now_ms();
    printf("  status %d in %.1f ms\n", n, t1 - t0);
    report_err("two-rect read");
    printf("  nonzero at row 0   (dst offset 0):        %d\n",
           nonzero_bytes(buf, 64 * 4));
    printf("  nonzero at row 256 (dst offset %d): %d\n",
           256 * W * 4, nonzero_bytes(buf + (size_t)256 * W * 4 + 256 * 4, 64 * 4));
    printf("  nonzero at row 128 (should be untouched): %d\n",
           nonzero_bytes(buf + (size_t)128 * W * 4, 64 * 4));

    /* 8. Cost by shape. The same total pixel count split three ways. */
    printf("step 8: cost by rectangle shape\n");
    {
        struct { const char *name; int nr; int rw; int rh; } shapes[] = {
            { "1 x 256x256",      1, 256, 256 },
            { "4 x 128x128",      4, 128, 128 },
            { "16 x 64x64",      16,  64,  64 },
            { "64 x 32x32",      64,  32,  32 },
        };
        int s;
        for (s = 0; s < 4; s++) {
            int k;
            for (k = 0; k < shapes[s].nr; k++) {
                rects[k].x = (short)((k * 97) % (W - shapes[s].rw));
                rects[k].y = (short)((k * 53) % (H - shapes[s].rh));
                rects[k].width  = (unsigned short)shapes[s].rw;
                rects[k].height = (unsigned short)shapes[s].rh;
            }
            clear_err();
            t0 = now_ms();
            n = XShmReadDisplayRects(dpy, root, rects, shapes[s].nr, rdbuf,
                                     0, 0, 0, &hints_ret);
            XSync(dpy, False);
            t1 = now_ms();
            printf("  %-14s %5d px total  %8.1f ms  status %d%s\n",
                   shapes[s].name,
                   shapes[s].nr * shapes[s].rw * shapes[s].rh,
                   t1 - t0, n, err_count ? "  [X error]" : "");
        }
    }

    /* 9. Full screen, shm, versus the same through XReadDisplay's copy. */
    printf("step 9: full screen, shm vs protocol\n");
    rects[0].x = 0; rects[0].y = 0;
    rects[0].width = (unsigned short)W; rects[0].height = (unsigned short)H;
    clear_err();
    t0 = now_ms();
    n = XShmReadDisplayRects(dpy, root, rects, 1, rdbuf, 0, 0, 0, &hints_ret);
    XSync(dpy, False);
    t1 = now_ms();
    printf("  shm       %8.1f ms   %.2f MB/s   checksum %lu\n", t1 - t0,
           (double)bufsz / 1048576.0 / ((t1 - t0) / 1000.0),
           checksum(buf, 4096));
    report_err("full-screen shm read");
    {
        XImage *im;
        t0 = now_ms();
        im = XReadDisplay(dpy, root, 0, 0, W, H, 0, &hints_ret);
        t1 = now_ms();
        printf("  protocol  %8.1f ms%s\n", t1 - t0, im ? "" : "  (returned NULL)");
        if (im) XDestroyImage(im);
    }

    /* 10. A sampled grid. No DAMAGE extension means the agent has to find its
     * own dirty rectangles, and reading every Nth row is the cheapest honest
     * way to notice that something moved. */
    printf("step 10: sampled grid for change detection\n");
    {
        int strides[] = { 4, 8, 16, 32 };
        int s;
        for (s = 0; s < 4; s++) {
            int stride = strides[s], k = 0;
            for (i = 0; i < H && k < 256; i += stride, k++) {
                rects[k].x = 0; rects[k].y = (short)i;
                rects[k].width = (unsigned short)W; rects[k].height = 1;
            }
            clear_err();
            t0 = now_ms();
            n = XShmReadDisplayRects(dpy, root, rects, k, rdbuf, 0, 0, 0, &hints_ret);
            XSync(dpy, False);
            t1 = now_ms();
            printf("  every %2d rows (%3d rects, %6d px)  %8.1f ms  status %d%s\n",
                   stride, k, k * W, t1 - t0, n, err_count ? "  [X error]" : "");
        }
    }

    /* 11. The cursor, on the shm path this time. */
    printf("step 11: XRD_READ_POINTER on the shm path\n");
    rects[0].x = 0; rects[0].y = 0; rects[0].width = 128; rects[0].height = 128;
    clear_err();
    hints_ret = 0;
    t0 = now_ms();
    n = XShmReadDisplayRects(dpy, root, rects, 1, rdbuf, 0, 0,
                             XRD_READ_POINTER, &hints_ret);
    XSync(dpy, False);
    t1 = now_ms();
    printf("  %.1f ms hints_ret=0x%lx %s\n", t1 - t0, hints_ret,
           (hints_ret & XRD_READ_POINTER)
               ? "<-- honoured, cursor composited"
               : "<-- NOT honoured, agent must draw the cursor");
    report_err("pointer read");

    /* 12. Steady state. Something that leaks or wedges shows up here and not
     * in a single call. */
    printf("step 12: %d back-to-back half-screen reads\n", iters);
    {
        double lo = 1e9, hi = 0, sum = 0;
        rects[0].x = 0; rects[0].y = 0;
        rects[0].width = (unsigned short)W; rects[0].height = (unsigned short)(H / 2);
        for (i = 0; i < iters; i++) {
            clear_err();
            t0 = now_ms();
            n = XShmReadDisplayRects(dpy, root, rects, 1, rdbuf, 0, 0, 0, &hints_ret);
            XSync(dpy, False);
            t1 = now_ms();
            if (t1 - t0 < lo) lo = t1 - t0;
            if (t1 - t0 > hi) hi = t1 - t0;
            sum += t1 - t0;
            if (err_count) { printf("  iteration %d: X error %s\n", i, last_err); break; }
        }
        printf("  min %.1f  avg %.1f  max %.1f ms over %d reads\n",
               lo, sum / (double)iters, hi, iters);
    }

    /* 13. The 8-bit fallback needs the colormap, since XGetImage on this
     * screen hands back palette indices with no masks at all. */
    printf("step 13: default colormap read (the XGetImage fallback needs it)\n");
    {
        XColor cols[256];
        int ncol = DisplayCells(dpy, scr);
        if (ncol > 256) ncol = 256;
        for (i = 0; i < ncol; i++) { cols[i].pixel = i; cols[i].flags = DoRed|DoGreen|DoBlue; }
        clear_err();
        t0 = now_ms();
        XQueryColors(dpy, DefaultColormap(dpy, scr), cols, ncol);
        t1 = now_ms();
        printf("  %d cells in %.1f ms; entry 0 = %04x/%04x/%04x, entry 255 = %04x/%04x/%04x\n",
               ncol, t1 - t0, cols[0].red, cols[0].green, cols[0].blue,
               cols[ncol-1].red, cols[ncol-1].green, cols[ncol-1].blue);
        report_err("XQueryColors");
    }

    printf("done\n");

cleanup:
    if (rdbuf) XShmDestroyReadDisplayBuf(rdbuf);
    XShmDetach(dpy, &shminfo);
    XSync(dpy, False);
    if (shminfo.shmaddr && shminfo.shmaddr != (char *)-1) shmdt(shminfo.shmaddr);
    if (shminfo.shmid >= 0) shmctl(shminfo.shmid, IPC_RMID, NULL);
    XCloseDisplay(dpy);
    return 0;
}
