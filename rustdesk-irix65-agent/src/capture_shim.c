/*
 * capture_shim.c -- see capture_shim.h for what this is and why it is shaped
 * this way. Everything below was written against measurements from
 * probes/xshmcap.c and probes/sgicap.c on IRIX 6.5.22m; the header records the
 * findings, this file records the handling.
 */

#include "capture_shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/readdisplay.h>
#include <X11/extensions/sgicap.h>

#define MAX_ERR 192

/* Rows per read when the whole screen is wanted. 128 keeps each request well
 * under the size that wedges the server, and small enough that a stall is a
 * fraction of a frame rather than the whole one. */
#define RD_STRIP_ROWS 128

struct rd_capture {
    Display          *dpy;
    Window            root;
    int               screen;
    int               width, height, depth;
    int               path;
    int               cursor_embedded;
    int               dead;

    /* The shm canvas. `buf` is the attached segment; `stride` is width*4. */
    XShmSegmentInfo   shminfo;
    unsigned char    *buf;
    size_t            bufsz;
    int               stride;
    int               shm_ok;

    ShmReadDisplayBuf *rdbuf;

    SGICapInterestType interest;
    int                cap_started;

    /* Fallback path state: an XImage of the screen plus the colormap, since
     * XGetImage on an 8-bit screen returns palette indices with no masks. */
    XImage           *fb_image;
    unsigned long     palette[256];
    int               palette_valid;

    int               force_full;     /* next poll reports everything */
    char              err[MAX_ERR];
};

/* ------------------------------------------------------------------------ *
 * X error handling
 *
 * Two handlers, and both matter. The protocol-error handler must not exit,
 * because a BadAccess from one optional call should not take the agent with
 * it. The I/O-error handler is the important one: Xlib's default calls exit(),
 * so when this server wedges and gets restarted -- which it does -- an agent
 * without a handler dies silently instead of reconnecting. Xlib forbids
 * returning from it, so it longjmps back to whoever was talking to the server.
 * ------------------------------------------------------------------------ */

static char        g_last_x_err[MAX_ERR];
static int         g_x_err_count;
static jmp_buf     g_io_jmp;
static volatile int g_io_jmp_armed;
static volatile int g_io_died;

static int on_x_error(Display *d, XErrorEvent *e)
{
    char buf[96];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    snprintf(g_last_x_err, sizeof g_last_x_err,
             "X error %d (%s) request %d.%d", e->error_code, buf,
             e->request_code, e->minor_code);
    g_x_err_count++;
    return 0;
}

static int on_x_io_error(Display *d)
{
    (void)d;
    g_io_died = 1;
    if (g_io_jmp_armed) {
        g_io_jmp_armed = 0;
        longjmp(g_io_jmp, 1);
    }
    /* Nobody armed a landing site, and Xlib does not allow a return. Exiting
     * here would be the default behaviour anyway; at least say why. */
    fprintf(stderr, "rd_capture: X connection lost with no handler armed\n");
    exit(1);
    return 0;   /* not reached */
}

static void err_set(rd_capture *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
}

/* Run `stmt` with the I/O handler armed; on a dead connection, mark it and
 * jump to `on_death`. */
#define WITH_X_GUARD(c, on_death)                       \
    do {                                                \
        if (setjmp(g_io_jmp)) {                         \
            g_io_jmp_armed = 0;                         \
            (c)->dead = 1;                              \
            err_set((c), "X connection lost");          \
            goto on_death;                              \
        }                                               \
        g_io_jmp_armed = 1;                             \
    } while (0)

#define END_X_GUARD() do { g_io_jmp_armed = 0; } while (0)

static void x_err_reset(void) { g_x_err_count = 0; g_last_x_err[0] = 0; }

/* ------------------------------------------------------------------------ */

static int alloc_canvas(rd_capture *c)
{
    c->stride = c->width * 4;
    c->bufsz  = (size_t)c->stride * (size_t)c->height;

    c->shminfo.shmid = shmget(IPC_PRIVATE, c->bufsz, IPC_CREAT | 0600);
    if (c->shminfo.shmid < 0) {
        err_set(c, "shmget(%lu): %s", (unsigned long)c->bufsz, strerror(errno));
        return -1;
    }
    c->shminfo.shmaddr = (char *)shmat(c->shminfo.shmid, NULL, 0);
    if (c->shminfo.shmaddr == (char *)-1) {
        err_set(c, "shmat: %s", strerror(errno));
        c->shminfo.shmaddr = NULL;
        return -1;
    }
    c->shminfo.readOnly = False;
    c->buf = (unsigned char *)c->shminfo.shmaddr;
    memset(c->buf, 0, c->bufsz);

    /* Mark the segment for destruction now that it is attached: if the agent
     * crashes, IRIX still reclaims it. A leaked 5 MB segment per crash would
     * otherwise need a manual ipcrm. */
    x_err_reset();
    if (!XShmAttach(c->dpy, &c->shminfo)) {
        err_set(c, "XShmAttach refused");
        return -1;
    }
    XSync(c->dpy, False);
    if (g_x_err_count) {
        err_set(c, "XShmAttach: %s", g_last_x_err);
        return -1;
    }
    shmctl(c->shminfo.shmid, IPC_RMID, NULL);
    c->shm_ok = 1;
    return 0;
}

static void free_canvas(rd_capture *c)
{
    if (c->shm_ok && c->dpy && !c->dead) {
        XShmDetach(c->dpy, &c->shminfo);
        XSync(c->dpy, False);
    }
    if (c->shminfo.shmaddr && c->shminfo.shmaddr != (char *)-1)
        shmdt(c->shminfo.shmaddr);
    c->shminfo.shmaddr = NULL;
    c->buf = NULL;
    c->shm_ok = 0;
}

/* Read the palette once for the XGetImage fallback. An 8-bit screen hands back
 * indices with all masks zero, so without this the fallback produces a picture
 * that is technically correct and entirely grey. */
static void load_palette(rd_capture *c)
{
    XColor cols[256];
    int n = DisplayCells(c->dpy, c->screen), i;
    if (n > 256) n = 256;
    for (i = 0; i < n; i++) {
        cols[i].pixel = (unsigned long)i;
        cols[i].flags = DoRed | DoGreen | DoBlue;
    }
    x_err_reset();
    XQueryColors(c->dpy, DefaultColormap(c->dpy, c->screen), cols, n);
    XSync(c->dpy, False);
    for (i = 0; i < n; i++) {
        unsigned int r = cols[i].red   >> 8;
        unsigned int g = cols[i].green >> 8;
        unsigned int b = cols[i].blue  >> 8;
        /* Stored in canvas order: A,B,G,R. */
        c->palette[i] = 0xff000000u | (b << 16) | (g << 8) | r;
    }
    for (; i < 256; i++) c->palette[i] = 0xff000000u;
    c->palette_valid = 1;
}

static rd_capture *open_common(const char *display, int max_path)
{
    rd_capture *c;
    int ev = 0, er = 0, maj = 0, min = 0;
    Bool pixmaps = False;

    c = (rd_capture *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->path = RD_PATH_GETIMAGE;
    c->force_full = 1;
    strcpy(c->err, "no error");

    XSetErrorHandler(on_x_error);
    XSetIOErrorHandler(on_x_io_error);
    g_io_died = 0;

    c->dpy = XOpenDisplay(display);
    if (!c->dpy) {
        err_set(c, "XOpenDisplay(%s) failed", display ? display : "$DISPLAY");
        return c;   /* caller checks dpy; keeps the reason available */
    }
    c->screen = DefaultScreen(c->dpy);
    c->root   = RootWindow(c->dpy, c->screen);
    c->width  = DisplayWidth(c->dpy, c->screen);
    c->height = DisplayHeight(c->dpy, c->screen);
    c->depth  = DefaultDepth(c->dpy, c->screen);
    c->stride = c->width * 4;

    /* Path selection, best first, each step conditional on the last. */
    if (max_path <= RD_PATH_READDISP
        && XShmQueryExtension(c->dpy)
        && XShmQueryVersion(c->dpy, &maj, &min, &pixmaps)
        && XReadDisplayQueryExtension(c->dpy, &ev, &er)
        && alloc_canvas(c) == 0) {

        c->rdbuf = XShmCreateReadDisplayBuf(c->dpy, (char *)c->buf,
                                            &c->shminfo, c->width, c->height);
        XSync(c->dpy, False);
        if (c->rdbuf) {
            c->path = RD_PATH_READDISP;

            if (max_path <= RD_PATH_DAMAGE
                && SGICapQueryExtension(c->dpy, &ev, &er)) {
                x_err_reset();
                c->interest = SGICapRegisterInterest(c->dpy, c->root, 0, 0,
                                                     (unsigned)c->width,
                                                     (unsigned)c->height);
                XSync(c->dpy, False);
                if (c->interest && !g_x_err_count) {
                    SGICapStart(c->dpy, c->interest);
                    XSync(c->dpy, False);
                    if (!g_x_err_count) {
                        c->cap_started = 1;
                        c->path = RD_PATH_DAMAGE;
                    }
                }
            }
        } else {
            err_set(c, "XShmCreateReadDisplayBuf returned NULL");
        }
    }

    if (c->path == RD_PATH_GETIMAGE) {
        /* No canvas from the shm path, or it was refused: allocate plain
         * memory and read with XGetImage. */
        free_canvas(c);
        c->stride = c->width * 4;
        c->bufsz  = (size_t)c->stride * (size_t)c->height;
        c->buf    = (unsigned char *)calloc(1, c->bufsz);
        if (!c->buf) {
            err_set(c, "out of memory for a %dx%d canvas", c->width, c->height);
            return c;
        }
        load_palette(c);
    }

    /* Ask once whether the cursor gets composited, so the agent can decide
     * about cursor_embedded before the first frame goes out. */
    if (c->path != RD_PATH_GETIMAGE) {
        XRectangle r;
        unsigned long hints = 0;
        r.x = 0; r.y = 0;
        r.width = (unsigned short)(c->width < 32 ? c->width : 32);
        r.height = (unsigned short)(c->height < 32 ? c->height : 32);
        x_err_reset();
        XShmReadDisplayRects(c->dpy, c->root, &r, 1, c->rdbuf, 0, 0,
                             XRD_READ_POINTER, &hints);
        XSync(c->dpy, False);
        c->cursor_embedded = (hints & XRD_READ_POINTER) ? 1 : 0;
    }

    return c;
}

rd_capture *rd_capture_open(const char *display)
{
    return rd_capture_open_forced(display, RD_PATH_DAMAGE);
}

rd_capture *rd_capture_open_forced(const char *display, int max_path)
{
    rd_capture *c = open_common(display, max_path);
    if (!c) return NULL;
    if (!c->dpy || !c->buf) {
        /* Keep the message, drop everything else. */
        char keep[MAX_ERR];
        strncpy(keep, c->err, sizeof keep);
        keep[sizeof keep - 1] = 0;
        rd_capture_close(c);
        fprintf(stderr, "rd_capture_open: %s\n", keep);
        return NULL;
    }
    return c;
}

/*
 * Teardown deliberately does NOT call XCloseDisplay.
 *
 * Measured with probes/reopen.c: on this Xlib, one XCloseDisplay on a
 * connection that has used ReadDisplay leaves every *later* connection in the
 * process broken -- XShmReadDisplayRects comes back BadRequest with request
 * code 0, or BadLength against a nonsense opcode. Three connections in a row
 * work perfectly as long as XCloseDisplay is never called, and fail from the
 * second onwards as soon as it is. So the extension's state is process-global
 * and the close hook corrupts it.
 *
 * That is not a corner case for this agent. The IRIX X server wedges and gets
 * restarted, and the agent has to reopen the display when it does; if reopening
 * cannot work, the agent survives exactly one X restart before going blind for
 * the rest of its life, which is a miserable thing to diagnose from a distance.
 *
 * Closing the socket by hand gives the server a clean disconnect (so no client
 * slot leaks) while leaving Xlib's per-display structures untouched. The cost
 * is the Display allocation, leaked once per reconnect. Verified as variant 4.
 */
void rd_capture_close(rd_capture *c)
{
    if (!c) return;
    if (c->dpy && !c->dead) {
        if (c->cap_started) {
            SGICapStop(c->dpy, c->interest);
            SGICapWithdrawInterest(c->dpy, c->interest);
        }
        if (c->rdbuf) XShmDestroyReadDisplayBuf(c->rdbuf);
        if (c->fb_image) { XDestroyImage(c->fb_image); c->fb_image = NULL; }
    }
    if (c->shm_ok) free_canvas(c);
    else if (c->buf) free(c->buf);
    if (c->dpy && !c->dead) {
        XFlush(c->dpy);
        close(ConnectionNumber(c->dpy));   /* NOT XCloseDisplay -- see above */
    }
    free(c);
}

int rd_capture_width(const rd_capture *c)  { return c ? c->width  : 0; }
int rd_capture_height(const rd_capture *c) { return c ? c->height : 0; }
int rd_capture_depth(const rd_capture *c)  { return c ? c->depth  : 0; }
int rd_capture_stride(const rd_capture *c) { return c ? c->stride : 0; }
int rd_capture_path(const rd_capture *c)   { return c ? c->path   : -1; }
int rd_capture_is_dead(const rd_capture *c){ return c ? c->dead   : 1; }
int rd_capture_cursor_embedded(const rd_capture *c)
{
    return c ? c->cursor_embedded : 0;
}
const unsigned char *rd_capture_buffer(const rd_capture *c)
{
    return c ? c->buf : NULL;
}
const char *rd_capture_last_error(const rd_capture *c)
{
    return c ? c->err : "no capture";
}
void rd_capture_invalidate(rd_capture *c) { if (c) c->force_full = 1; }

/* Expand an 8-bit XImage through the palette into the canvas. Only the
 * fallback path needs this; ReadDisplay resolves the colormap itself. */
static void expand_indexed(rd_capture *c, XImage *im)
{
    int y, x;
    if (!c->palette_valid) load_palette(c);
    for (y = 0; y < c->height; y++) {
        const unsigned char *s = (const unsigned char *)im->data + (size_t)y * im->bytes_per_line;
        unsigned char *d = c->buf + (size_t)y * c->stride;
        for (x = 0; x < c->width; x++) {
            unsigned long p = c->palette[s[x]];
            d[x * 4 + 0] = (unsigned char)(p >> 24);
            d[x * 4 + 1] = (unsigned char)(p >> 16);
            d[x * 4 + 2] = (unsigned char)(p >> 8);
            d[x * 4 + 3] = (unsigned char)p;
        }
    }
}

/* Copy a 32-bit XImage into the canvas, row by row, in case the server's
 * bytes_per_line is padded beyond width*4. */
static void copy_direct(rd_capture *c, XImage *im)
{
    int y;
    for (y = 0; y < c->height; y++)
        memcpy(c->buf + (size_t)y * c->stride,
               im->data + (size_t)y * im->bytes_per_line,
               (size_t)c->stride);
}

static int full_getimage(rd_capture *c)
{
    XImage *im;
    x_err_reset();
    im = XGetImage(c->dpy, c->root, 0, 0, (unsigned)c->width,
                   (unsigned)c->height, AllPlanes, ZPixmap);
    XSync(c->dpy, False);
    if (!im) {
        err_set(c, "XGetImage failed%s%s",
                g_x_err_count ? ": " : "", g_x_err_count ? g_last_x_err : "");
        return -1;
    }
    if (im->bits_per_pixel == 8) expand_indexed(c, im);
    else if (im->bits_per_pixel == 32) copy_direct(c, im);
    else {
        err_set(c, "XGetImage returned %d bits per pixel, which is not handled",
                im->bits_per_pixel);
        XDestroyImage(im);
        return -1;
    }
    XDestroyImage(im);
    return 0;
}

int rd_capture_full(rd_capture *c)
{
    int rc = -1;
    XRectangle r;
    unsigned long hints_ret = 0;

    if (!c) return -1;
    if (c->dead) { err_set(c, "X connection lost"); return -1; }

    WITH_X_GUARD(c, dead_out);

    if (c->path == RD_PATH_GETIMAGE) {
        rc = full_getimage(c);
    } else {
        /* In horizontal strips, not one whole-screen rectangle.
         *
         * A single 1280x1024 ReadDisplay has been seen to wedge this X server
         * outright -- it stops answering, keeps its process, and stops
         * accumulating CPU -- while strip-sized reads of the same total area
         * have never done so, in hours of use. The cause is below Xlib (the
         * guest kernel sometimes reports "ng1 pixel dma timeout" alongside it),
         * so this is avoidance rather than a fix, and it costs nothing: the
         * per-read overhead is a couple of milliseconds against hundreds for
         * the pixels.
         *
         * It also gives the caller a chance to service input between strips,
         * which is why the PowerPC agent reads a band at a time as well.
         */
        int y, strip = RD_STRIP_ROWS;
        rc = 0;
        for (y = 0; y < c->height && rc == 0; y += strip) {
            int h = (y + strip <= c->height) ? strip : (c->height - y);
            r.x = 0;
            r.y = (short)y;
            r.width  = (unsigned short)c->width;
            r.height = (unsigned short)h;
            x_err_reset();
            XShmReadDisplayRects(c->dpy, c->root, &r, 1, c->rdbuf, 0, 0,
                                 XRD_READ_POINTER, &hints_ret);
            XSync(c->dpy, False);
            if (g_x_err_count) {
                err_set(c, "XShmReadDisplayRects(rows %d..%d): %s",
                        y, y + h, g_last_x_err);
                rc = -1;
            }
        }
        if (rc == 0)
            c->cursor_embedded = (hints_ret & XRD_READ_POINTER) ? 1 : 0;
    }
    END_X_GUARD();
    if (rc == 0) c->force_full = 0;
    return rc;

dead_out:
    return -1;
}

int rd_capture_read_rect(rd_capture *c, int x, int y, int w, int h)
{
    XRectangle r;
    unsigned long hints_ret = 0;
    int rc = -1;

    if (!c) return -1;
    if (c->dead) { err_set(c, "X connection lost"); return -1; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > c->width || y + h > c->height) {
        err_set(c, "rect %d,%d %dx%d is outside the %dx%d screen",
                x, y, w, h, c->width, c->height);
        return -1;
    }

    if (c->path == RD_PATH_GETIMAGE) {
        /* No partial read on the fallback: XGetImage of a sub-rectangle would
         * work, but the palette expansion is written against a full frame and a
         * second path through it is not worth the risk for a repair. */
        return rd_capture_full(c);
    }

    WITH_X_GUARD(c, dead_out);
    r.x = (short)x;
    r.y = (short)y;
    r.width = (unsigned short)w;
    r.height = (unsigned short)h;
    x_err_reset();
    XShmReadDisplayRects(c->dpy, c->root, &r, 1, c->rdbuf, 0, 0,
                         XRD_READ_POINTER, &hints_ret);
    XSync(c->dpy, False);
    if (g_x_err_count) {
        err_set(c, "XShmReadDisplayRects(rect): %s", g_last_x_err);
        rc = -1;
    } else {
        rc = 0;
    }
    END_X_GUARD();
    return rc;

dead_out:
    return -1;
}

/* The whole screen as a single rectangle, for the callers that need to report
 * "everything changed". */
static int report_full(rd_capture *c, int *rects, int max_rects)
{
    if (max_rects < 1) return 0;
    rects[0] = 0;
    rects[1] = 0;
    rects[2] = c->width;
    rects[3] = c->height;
    return 1;
}

int rd_capture_poll(rd_capture *c, int *rects, int max_rects)
{
    int n = -1;

    if (!c || !rects || max_rects < 1) return -1;
    if (c->dead) { err_set(c, "X connection lost"); return -1; }

    if (c->force_full) {
        if (rd_capture_full(c) != 0) return -1;
        /* Damage accumulated up to now is stale once everything has been
         * read; drop it so the next poll reports only new changes. */
        if (c->path == RD_PATH_DAMAGE) {
            Time t; int cnt = 0, ord = 0;
            XRectangle *r;
            WITH_X_GUARD(c, dead_out);
            x_err_reset();
            r = SGICapQueryAndReset(c->dpy, c->interest, &t, &cnt, &ord);
            XSync(c->dpy, False);
            END_X_GUARD();
            if (r) XFree(r);
        }
        return report_full(c, rects, max_rects);
    }

    WITH_X_GUARD(c, dead_out);

    if (c->path == RD_PATH_DAMAGE) {
        Time when;
        int count = 0, ordering = 0, i;
        XRectangle *r;

        x_err_reset();
        r = SGICapQueryCopyAndReset(c->dpy, c->interest, &when, &count,
                                    &ordering, c->rdbuf);
        XSync(c->dpy, False);
        if (g_x_err_count) {
            err_set(c, "SGICapQueryCopyAndReset: %s", g_last_x_err);
            if (r) XFree(r);
            END_X_GUARD();
            return -1;
        }
        if (!r || count <= 0) {
            if (r) XFree(r);
            END_X_GUARD();
            return 0;
        }
        if (count <= max_rects) {
            for (i = 0; i < count; i++) {
                rects[i * 4 + 0] = r[i].x;
                rects[i * 4 + 1] = r[i].y;
                rects[i * 4 + 2] = r[i].width;
                rects[i * 4 + 3] = r[i].height;
            }
            n = count;
        } else {
            /* More rectangles than the caller can hold. The pixels are already
             * in the canvas, so merging into the bounding box keeps the report
             * truthful -- dropping the tail would leave the peer with stale
             * pixels it was never told about. */
            int x0 = r[0].x, y0 = r[0].y;
            int x1 = r[0].x + (int)r[0].width, y1 = r[0].y + (int)r[0].height;
            for (i = 1; i < count; i++) {
                if (r[i].x < x0) x0 = r[i].x;
                if (r[i].y < y0) y0 = r[i].y;
                if (r[i].x + (int)r[i].width  > x1) x1 = r[i].x + (int)r[i].width;
                if (r[i].y + (int)r[i].height > y1) y1 = r[i].y + (int)r[i].height;
            }
            rects[0] = x0; rects[1] = y0;
            rects[2] = x1 - x0; rects[3] = y1 - y0;
            n = 1;
        }
        XFree(r);
        END_X_GUARD();
        return n;
    }

    /* No damage tracking: read everything and say so. Correct, just expensive;
     * the agent throttles its own frame rate. */
    END_X_GUARD();
    if (rd_capture_full(c) != 0) return -1;
    return report_full(c, rects, max_rects);

dead_out:
    return -1;
}

int rd_scale_abgr(const unsigned char *src, int sw, int sh, int src_stride,
                  unsigned char *dst, int factor)
{
    int dw, dh, dy, dx, ky, kx;
    if (!src || !dst || factor < 1 || sw <= 0 || sh <= 0) return 0;
    if (factor == 1) {
        for (dy = 0; dy < sh; dy++)
            memcpy(dst + (size_t)dy * sw * 4,
                   src + (size_t)dy * src_stride, (size_t)sw * 4);
        return sw;
    }
    dw = sw / factor;
    dh = sh / factor;
    if (dw <= 0 || dh <= 0) return 0;

    for (dy = 0; dy < dh; dy++) {
        unsigned char *drow = dst + (size_t)dy * dw * 4;
        for (dx = 0; dx < dw; dx++) {
            unsigned int b = 0, g = 0, r = 0;
            const unsigned char *s0 =
                src + (size_t)(dy * factor) * src_stride + (size_t)(dx * factor) * 4;
            for (ky = 0; ky < factor; ky++) {
                const unsigned char *s = s0 + (size_t)ky * src_stride;
                for (kx = 0; kx < factor; kx++) {
                    b += s[kx * 4 + 1];
                    g += s[kx * 4 + 2];
                    r += s[kx * 4 + 3];
                }
            }
            {
                unsigned int n = (unsigned int)(factor * factor);
                drow[dx * 4 + 0] = 0xff;
                drow[dx * 4 + 1] = (unsigned char)(b / n);
                drow[dx * 4 + 2] = (unsigned char)(g / n);
                drow[dx * 4 + 3] = (unsigned char)(r / n);
            }
        }
    }
    return dw;
}
