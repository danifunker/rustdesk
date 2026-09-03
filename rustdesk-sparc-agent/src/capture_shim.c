/*
 * capture_shim.c -- see capture_shim.h for the contract and for what was
 * measured on this machine. This file records the handling.
 */

#include "capture_shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#if RD_HAVE_XDAMAGE
#include <X11/extensions/Xdamage.h>
#else
/* Solaris 9 has no DAMAGE: Xsun does not advertise the extension and there is no
 * client library to link against. Everything below already copes with damage
 * being unusable -- rd_capture_poll compares canvases instead -- so report the
 * extension absent and let that path run. These exist only to compile. */
typedef unsigned long Damage;
typedef struct { XRectangle area; } XDamageNotifyEvent;
#define XDamageReportRawRectangles          0
#define XDamageNotify                       0
#define XDamageQueryExtension(dpy, ev, er)  (0)
#define XDamageCreate(dpy, d, level)        ((Damage)0)
#define XDamageDestroy(dpy, dmg)            ((void)0)
#define XDamageSubtract(dpy, dmg, r1, r2)   ((void)0)
#endif

#define MAX_ERR      256
/* Damage rectangles held between polls. Past this they merge into their
 * bounding box, which over-reports pixels but never under-reports them. */
#define MAX_ACC      64
/* Horizontal strips the hash path compares. Matches the Rust side's BANDS. */
#define RD_BANDS     64
/* Rows sampled per band. A change confined to unsampled rows is missed, which
 * is why this path is the fallback and not the design. */
#define HASH_ROW_STEP 4
/* How long the damage path may report silence before it has to prove it. */
#define VALIDATE_MS  2000

struct rd_capture {
    Display        *dpy;
    Window          root;
    int             screen;
    int             width, height, depth;
    int             path;
    int             order;
    int             swap_rb;    /* server hands back B and R the other way round */
    int             cursor_embedded;
    int             dead;

    /* The canvas. Under the shm paths it *is* the XImage's data, so a capture
     * lands in it with no copy; under XGetImage it is our own buffer. */
    XShmSegmentInfo shminfo;
    XImage         *shm_img;
    unsigned char  *buf;
    size_t          bufsz;
    int             stride;
    int             shm_ok;

    int             damage_ev_base, damage_er_base;
    Damage          damage;

    XRectangle      acc[MAX_ACC];
    int             acc_n;
    int             acc_overflow;

    unsigned long   band_hash[RD_BANDS];
    int             hash_valid;
    struct timeval  last_validate;

    int             force_full;
    char            err[MAX_ERR];
};

/* ------------------------------------------------------------------------ *
 * X error handling
 *
 * Two handlers, and both matter. The protocol handler must not exit: a
 * BadAccess from one optional call should not take the agent with it. The I/O
 * handler is the important one -- Xlib's default calls exit(), so an agent
 * without one dies silently when the server is restarted instead of
 * reconnecting. Xlib forbids returning from it, so it longjmps back to
 * whoever was talking to the server.
 * ------------------------------------------------------------------------ */

static char         g_last_x_err[MAX_ERR];
static int          g_x_err_count;
static jmp_buf      g_io_jmp;
static volatile int g_io_jmp_armed;

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
    if (g_io_jmp_armed) {
        g_io_jmp_armed = 0;
        longjmp(g_io_jmp, 1);
    }
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

/* RD_DEBUG=1 traces change detection to stderr. Worth keeping: what this path
 * decides is invisible from the outside, and the machine it runs on is usually
 * somewhere else. */
static int rd_debug(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RD_DEBUG") != NULL;
    return on;
}

/* ------------------------------------------------------------------------ */

/* Which order 32-bit pixels land in memory, from the server's own answer
 * rather than from an assumption about the machine. */
static int pixel_order_of(const XImage *im, int *out, int *swap_rb)
{
    if (im->bits_per_pixel != 32)
        return -1;
    if (im->green_mask != 0x0000ff00)
        return -1;
    if (im->red_mask == 0x00ff0000 && im->blue_mask == 0x000000ff) {
        *swap_rb = 0;
        *out = (im->byte_order == MSBFirst) ? RD_ORDER_ARGB : RD_ORDER_BGRA;
        return 0;
    }
    /* The other way round, which is what Solaris' Xsun reports for the XVR-600:
     * every TrueColor visual it offers has red in the low byte. Rather than
     * teach the whole conversion chain a third order, swap the two channels as
     * the canvas is filled and report the order everything downstream expects.
     * The Solaris 10 port never saw this because it ran against Xvfb. */
    if (im->red_mask == 0x000000ff && im->blue_mask == 0x00ff0000) {
        *swap_rb = 1;
        *out = (im->byte_order == MSBFirst) ? RD_ORDER_ARGB : RD_ORDER_BGRA;
        return 0;
    }
    return -1;
}

/* Exchange the two colour channels either side of green, in place. */
static void swap_rb_rect(rd_capture *c, int x, int y, int w, int h)
{
    int row, col;
    for (row = 0; row < h; row++) {
        unsigned char *p = c->buf + (size_t)(y + row) * c->stride + (size_t)x * 4;
        for (col = 0; col < w; col++, p += 4) {
            unsigned char t = p[1];
            p[1] = p[3];
            p[3] = t;
        }
    }
}

static int alloc_shm_canvas(rd_capture *c)
{
    Visual *vis = DefaultVisual(c->dpy, c->screen);
    int major = 0, minor = 0;
    Bool pixmaps = False;

    if (!XShmQueryVersion(c->dpy, &major, &minor, &pixmaps)) {
        err_set(c, "server has no MIT-SHM");
        return -1;
    }

    memset(&c->shminfo, 0, sizeof c->shminfo);
    c->shm_img = XShmCreateImage(c->dpy, vis, c->depth, ZPixmap, NULL,
                                 &c->shminfo, c->width, c->height);
    if (!c->shm_img) {
        err_set(c, "XShmCreateImage failed");
        return -1;
    }

    c->bufsz = (size_t)c->shm_img->bytes_per_line * (size_t)c->shm_img->height;
    c->shminfo.shmid = shmget(IPC_PRIVATE, c->bufsz, IPC_CREAT | 0600);
    if (c->shminfo.shmid < 0) {
        err_set(c, "shmget(%lu) failed", (unsigned long)c->bufsz);
        goto fail;
    }
    c->shminfo.shmaddr = (char *)shmat(c->shminfo.shmid, NULL, 0);
    if (c->shminfo.shmaddr == (char *)-1) {
        c->shminfo.shmaddr = NULL;
        err_set(c, "shmat failed");
        goto fail;
    }
    c->shm_img->data = c->shminfo.shmaddr;
    c->shminfo.readOnly = False;

    x_err_reset();
    if (!XShmAttach(c->dpy, &c->shminfo) || (XSync(c->dpy, False), g_x_err_count)) {
        err_set(c, "XShmAttach refused: %s",
                g_x_err_count ? g_last_x_err : "not a local display?");
        goto fail;
    }
    /* Mark it destroyed now that both ends hold it: the segment lives until
     * the last detach, and this way a crash does not leave 5 MB behind. */
    shmctl(c->shminfo.shmid, IPC_RMID, NULL);

    c->buf    = (unsigned char *)c->shm_img->data;
    c->stride = c->shm_img->bytes_per_line;
    c->shm_ok = 1;
    return 0;

fail:
    if (c->shminfo.shmaddr) { shmdt(c->shminfo.shmaddr); c->shminfo.shmaddr = NULL; }
    if (c->shminfo.shmid >= 0) shmctl(c->shminfo.shmid, IPC_RMID, NULL);
    if (c->shm_img) { c->shm_img->data = NULL; XDestroyImage(c->shm_img); c->shm_img = NULL; }
    return -1;
}

static void free_canvas(rd_capture *c)
{
    if (c->shm_ok) {
        XShmDetach(c->dpy, &c->shminfo);
        if (c->shm_img) { c->shm_img->data = NULL; XDestroyImage(c->shm_img); c->shm_img = NULL; }
        if (c->shminfo.shmaddr) shmdt(c->shminfo.shmaddr);
        c->shm_ok = 0;
        c->buf = NULL;
    } else if (c->buf) {
        free(c->buf);
        c->buf = NULL;
    }
}

/* Defined below, next to the rest of the change-detection code. */
static void rehash_all(rd_capture *c);
static int diff_bands(rd_capture *c, int *rects, int max_rects);

static rd_capture *open_common(const char *display, int max_path)
{
    rd_capture *c = (rd_capture *)calloc(1, sizeof *c);
    XImage *probe;
    int order;

    if (!c) return NULL;
    c->shminfo.shmid = -1;
    c->path = RD_PATH_GETIMAGE;
    c->force_full = 1;

    XSetErrorHandler(on_x_error);
    XSetIOErrorHandler(on_x_io_error);

    c->dpy = XOpenDisplay(display);
    if (!c->dpy) {
        err_set(c, "cannot open display %s",
                display ? display : (getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)"));
        return c;
    }
    c->screen = DefaultScreen(c->dpy);
    c->root   = RootWindow(c->dpy, c->screen);
    c->width  = DisplayWidth(c->dpy, c->screen);
    c->height = DisplayHeight(c->dpy, c->screen);
    c->depth  = DefaultDepth(c->dpy, c->screen);

    /* One small XGetImage answers the pixel-format question before anything
     * large is allocated on the strength of a guess. */
    x_err_reset();
    probe = XGetImage(c->dpy, c->root, 0, 0, 1, 1, AllPlanes, ZPixmap);
    if (!probe) {
        err_set(c, "XGetImage on the root window failed: %s",
                g_x_err_count ? g_last_x_err : "no reason given");
        return c;
    }
    if (pixel_order_of(probe, &order, &c->swap_rb) != 0) {
        err_set(c, "unsupported pixel format: depth %d, %d bpp, masks R=%08lx G=%08lx B=%08lx",
                probe->depth, probe->bits_per_pixel,
                probe->red_mask, probe->green_mask, probe->blue_mask);
        XDestroyImage(probe);
        return c;
    }
    c->order = order;
    XDestroyImage(probe);

    if (max_path <= RD_PATH_SHM && alloc_shm_canvas(c) == 0) {
        c->path = RD_PATH_SHM;
    } else {
        /* Either shm was refused or the caller asked for the slow path on
         * purpose. Either way the canvas is ours to allocate. */
        c->stride = c->width * 4;
        c->bufsz  = (size_t)c->stride * (size_t)c->height;
        c->buf    = (unsigned char *)calloc(1, c->bufsz);
        if (!c->buf) {
            err_set(c, "cannot allocate a %lu byte canvas", (unsigned long)c->bufsz);
            return c;
        }
        c->path = RD_PATH_GETIMAGE;
    }

    if (max_path <= RD_PATH_DAMAGE && c->path == RD_PATH_SHM) {
        if (XDamageQueryExtension(c->dpy, &c->damage_ev_base, &c->damage_er_base)) {
            x_err_reset();
            c->damage = XDamageCreate(c->dpy, c->root, XDamageReportRawRectangles);
            XSync(c->dpy, False);
            if (!g_x_err_count && c->damage)
                c->path = RD_PATH_DAMAGE;
        }
    }

    /* X never draws the cursor into a drawable's contents, so it is never in
     * what we read; the agent sends a shape instead. */
    c->cursor_embedded = 0;
    return c;
}

int rd_display_size(int *w, int *h)
{
    Display *d = XOpenDisplay(NULL);
    int s;
    if (!d) return -1;
    s = DefaultScreen(d);
    if (w) *w = DisplayWidth(d, s);
    if (h) *h = DisplayHeight(d, s);
    XCloseDisplay(d);
    return 0;
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
        /* Keep the object alive only long enough for the caller to read the
         * error out of it; rd_capture_open's contract is NULL on failure, so
         * the message goes to stderr as well. */
        fprintf(stderr, "rd_capture: %s\n", c->err);
        rd_capture_close(c);
        return NULL;
    }
    return c;
}

void rd_capture_close(rd_capture *c)
{
    if (!c) return;
    if (c->dpy) {
        if (c->damage) XDamageDestroy(c->dpy, c->damage);
        free_canvas(c);
        XCloseDisplay(c->dpy);
    } else {
        free_canvas(c);
    }
    free(c);
}

int rd_capture_width(const rd_capture *c)  { return c ? c->width  : 0; }
int rd_capture_height(const rd_capture *c) { return c ? c->height : 0; }
int rd_capture_depth(const rd_capture *c)  { return c ? c->depth  : 0; }
int rd_capture_stride(const rd_capture *c) { return c ? c->stride : 0; }
int rd_capture_path(const rd_capture *c)   { return c ? c->path   : -1; }
int rd_capture_is_dead(const rd_capture *c){ return c ? c->dead   : 1; }
int rd_capture_pixel_order(const rd_capture *c) { return c ? c->order : RD_ORDER_ARGB; }
int rd_capture_cursor_embedded(const rd_capture *c) { return c ? c->cursor_embedded : 0; }

const unsigned char *rd_capture_buffer(const rd_capture *c)
{
    return c ? c->buf : NULL;
}

const char *rd_capture_last_error(const rd_capture *c)
{
    return c ? c->err : "no capture context";
}

void rd_capture_invalidate(rd_capture *c) { if (c) c->force_full = 1; }

/* Copy an XImage's rows into the canvas at (x,y). The two strides differ often
 * enough -- the server pads rows to its own quantum -- that this is a row loop
 * rather than one memcpy. */
static void blit_image(rd_capture *c, XImage *im, int x, int y)
{
    int row;
    int bytes = im->width * 4;
    for (row = 0; row < im->height; row++) {
        memcpy(c->buf + (size_t)(y + row) * c->stride + (size_t)x * 4,
               im->data + (size_t)row * im->bytes_per_line,
               (size_t)bytes);
    }
    if (c->swap_rb)
        swap_rb_rect(c, x, y, im->width, im->height);
}

static int full_getimage(rd_capture *c)
{
    XImage *im;
    x_err_reset();
    im = XGetImage(c->dpy, c->root, 0, 0, c->width, c->height, AllPlanes, ZPixmap);
    if (!im) {
        err_set(c, "XGetImage(full): %s",
                g_x_err_count ? g_last_x_err : "no reason given");
        return -1;
    }
    blit_image(c, im, 0, 0);
    XDestroyImage(im);
    return 0;
}

int rd_capture_full(rd_capture *c)
{
    int rc = -1;

    if (!c) return -1;
    if (c->dead) { err_set(c, "X connection lost"); return -1; }

    WITH_X_GUARD(c, dead_out);
    if (c->shm_ok) {
        x_err_reset();
        if (!XShmGetImage(c->dpy, c->root, c->shm_img, 0, 0, AllPlanes)) {
            err_set(c, "XShmGetImage: %s",
                    g_x_err_count ? g_last_x_err : "refused");
        } else {
            XSync(c->dpy, False);
            rc = g_x_err_count ? -1 : 0;
            if (rc != 0) err_set(c, "XShmGetImage: %s", g_last_x_err);
            /* Nothing copied this one: the segment is the canvas. */
            if (rc == 0 && c->swap_rb)
                swap_rb_rect(c, 0, 0, c->width, c->height);
        }
    } else {
        rc = full_getimage(c);
    }
    END_X_GUARD();
    if (rc == 0) c->force_full = 0;
    return rc;

dead_out:
    return -1;
}

int rd_capture_read_rect(rd_capture *c, int x, int y, int w, int h)
{
    XImage *im;
    int rc = -1;

    if (!c) return -1;
    if (c->dead) { err_set(c, "X connection lost"); return -1; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > c->width || y + h > c->height) {
        err_set(c, "read_rect %dx%d+%d+%d is outside %dx%d",
                w, h, x, y, c->width, c->height);
        return -1;
    }

    WITH_X_GUARD(c, dead_out);
    x_err_reset();
    im = XGetImage(c->dpy, c->root, x, y, w, h, AllPlanes, ZPixmap);
    if (!im) {
        err_set(c, "XGetImage(%dx%d+%d+%d): %s", w, h, x, y,
                g_x_err_count ? g_last_x_err : "no reason given");
    } else {
        blit_image(c, im, x, y);
        XDestroyImage(im);
        rc = 0;
    }
    END_X_GUARD();
    return rc;

dead_out:
    return -1;
}

/* Add one rectangle to what this poll will report. Past MAX_ACC they collapse
 * into a bounding box: the canvas is read whole anyway, so a coarser report
 * costs encode time but never leaves the peer holding stale pixels. */
static void acc_add(rd_capture *c, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (c->acc_n < MAX_ACC && !c->acc_overflow) {
        c->acc[c->acc_n].x      = (short)x;
        c->acc[c->acc_n].y      = (short)y;
        c->acc[c->acc_n].width  = (unsigned short)w;
        c->acc[c->acc_n].height = (unsigned short)h;
        c->acc_n++;
        return;
    }
    if (!c->acc_overflow) {
        int i;
        int x0 = c->acc[0].x, y0 = c->acc[0].y;
        int x1 = x0 + c->acc[0].width, y1 = y0 + c->acc[0].height;
        for (i = 1; i < c->acc_n; i++) {
            if (c->acc[i].x < x0) x0 = c->acc[i].x;
            if (c->acc[i].y < y0) y0 = c->acc[i].y;
            if (c->acc[i].x + c->acc[i].width  > x1) x1 = c->acc[i].x + c->acc[i].width;
            if (c->acc[i].y + c->acc[i].height > y1) y1 = c->acc[i].y + c->acc[i].height;
        }
        c->acc[0].x = (short)x0; c->acc[0].y = (short)y0;
        c->acc[0].width = (unsigned short)(x1 - x0);
        c->acc[0].height = (unsigned short)(y1 - y0);
        c->acc_n = 1;
        c->acc_overflow = 1;
    }
    {
        int x0 = c->acc[0].x, y0 = c->acc[0].y;
        int x1 = x0 + c->acc[0].width, y1 = y0 + c->acc[0].height;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + w > x1) x1 = x + w;
        if (y + h > y1) y1 = y + h;
        c->acc[0].x = (short)x0; c->acc[0].y = (short)y0;
        c->acc[0].width = (unsigned short)(x1 - x0);
        c->acc[0].height = (unsigned short)(y1 - y0);
    }
}

/* Take every damage event the server has sent us so far. Raw-rectangle mode
 * reports each change as it happens; the Subtract keeps the damage object's
 * own region from accumulating behind them. */
static void drain_damage(rd_capture *c)
{
    XEvent e;
    XSync(c->dpy, False);
    while (XPending(c->dpy)) {
        XNextEvent(c->dpy, &e);
        if (e.type == c->damage_ev_base + XDamageNotify) {
            XDamageNotifyEvent *de = (XDamageNotifyEvent *)&e;
            acc_add(c, de->area.x, de->area.y, de->area.width, de->area.height);
        }
    }
    XDamageSubtract(c->dpy, c->damage, None, None);
}

/* FNV-1a over every HASH_ROW_STEP'th row of a band, sampling every other pixel.
 *
 * The two *middle* bytes of each pixel, specifically. One of the four is not
 * colour at all -- it is the padding in a 32-bit-per-pixel 24-bit image -- and
 * which end it sits at depends on the server's byte order: first under A,R,G,B,
 * last under B,G,R,A. Hashing it is how this was first written, and on this
 * machine that meant hashing a byte that is always zero: every band matched
 * every time and a screen with a clock ticking on it reported no change at all.
 * Bytes 1 and 2 are a colour channel under either order.
 *
 * Cheap enough to run on every poll: at 1280x1024 it reads about a quarter of
 * a 5 MB canvas. */
static unsigned long hash_band(const rd_capture *c, int y0, int y1)
{
    unsigned long h = 1469598103934665603UL;
    int y;
    for (y = y0; y < y1; y += HASH_ROW_STEP) {
        const unsigned char *row = c->buf + (size_t)y * c->stride;
        int i, n = c->width * 4;
        for (i = 0; i + 4 <= n; i += 8) {
            h ^= row[i + 1];
            h *= 1099511628211UL;
            h ^= row[i + 2];
            h *= 1099511628211UL;
        }
    }
    return h;
}

static void band_bounds(const rd_capture *c, int b, int *y0, int *y1)
{
    int rows = (c->height + RD_BANDS - 1) / RD_BANDS;
    int a = b * rows;
    int z = a + rows;
    if (a > c->height) a = c->height;
    if (z > c->height) z = c->height;
    *y0 = a;
    *y1 = z;
}

static void rehash_all(rd_capture *c)
{
    int b, y0, y1;
    for (b = 0; b < RD_BANDS; b++) {
        band_bounds(c, b, &y0, &y1);
        c->band_hash[b] = (y1 > y0) ? hash_band(c, y0, y1) : 0;
    }
    c->hash_valid = 1;
}

/* Compare the canvas against the last poll's hashes and report the bands that
 * moved, runs of adjacent bands merged into one rectangle. */
static int diff_bands(rd_capture *c, int *rects, int max_rects)
{
    int b, y0, y1, n = 0;
    int run_start = -1;
    int n_dirty = 0;
    unsigned long h;

    for (b = 0; b <= RD_BANDS; b++) {
        int dirty = 0;
        if (b < RD_BANDS) {
            band_bounds(c, b, &y0, &y1);
            if (y1 > y0) {
                h = hash_band(c, y0, y1);
                dirty = (h != c->band_hash[b]);
                if (dirty) n_dirty++;
                c->band_hash[b] = h;
            }
        }
        if (dirty && run_start < 0) {
            run_start = b;
        } else if (!dirty && run_start >= 0) {
            int ry0, ry1, tmp;
            band_bounds(c, run_start, &ry0, &tmp);
            band_bounds(c, b - 1, &tmp, &ry1);
            if (n < max_rects) {
                rects[n * 4 + 0] = 0;
                rects[n * 4 + 1] = ry0;
                rects[n * 4 + 2] = c->width;
                rects[n * 4 + 3] = ry1 - ry0;
                n++;
            } else if (n > 0) {
                /* Out of room: grow the last rectangle rather than drop this
                 * one, the same rule the damage path follows. */
                int last_y = rects[(n - 1) * 4 + 1];
                rects[(n - 1) * 4 + 3] = ry1 - last_y;
            }
            run_start = -1;
        }
    }
    if (rd_debug())
        fprintf(stderr, "diff_bands: %d/%d bands dirty -> %d rect(s)\n", n_dirty, RD_BANDS, n);
    return n;
}

static long ms_since(const struct timeval *then)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - then->tv_sec) * 1000L + (now.tv_usec - then->tv_usec) / 1000L;
}

/* Is the damage path telling the truth?
 *
 * It is here: a damage object on the root does report a child window's
 * drawing, measured with `xclock -update 1`, which produces one 181x181
 * rectangle a second at the clock's position. But whether a server does that
 * is not something to assume -- a compositing server redirects windows away
 * from the root, where the same code would see nothing -- and the failure is
 * the worst kind: a frozen screen, served confidently, with no error anywhere.
 *
 * A synthetic probe at startup (draw into a window of our own, see if it
 * reports) was tried and answered wrongly, so the check is against real work
 * instead: after VALIDATE_MS of reported silence, read the screen and compare.
 * If it moved while damage said nothing, damage is not usable here and the
 * path downgrades for good. One 5 ms read and one ~1 ms comparison per two
 * seconds of idle, which is what it costs to never be silently wrong. */
static int validate_quiet(rd_capture *c, int *rects, int max_rects)
{
    int n;

    if (ms_since(&c->last_validate) < VALIDATE_MS)
        return 0;
    gettimeofday(&c->last_validate, NULL);

    if (rd_capture_full(c) != 0) return -1;
    if (!c->hash_valid) { rehash_all(c); return 0; }

    n = diff_bands(c, rects, max_rects);
    if (n > 0) {
        c->path = RD_PATH_SHM;
        if (c->damage) {
            XDamageDestroy(c->dpy, c->damage);
            c->damage = 0;
        }
        err_set(c, "damage missed a change; comparing the canvas instead");
    }
    return n;
}

static int report_full(rd_capture *c, int *rects, int max_rects)
{
    if (max_rects < 1) return 0;
    rects[0] = 0;
    rects[1] = 0;
    rects[2] = c->width;
    rects[3] = c->height;
    return 1;
}

static int report_acc(rd_capture *c, int *rects, int max_rects)
{
    int i, n = c->acc_n < max_rects ? c->acc_n : max_rects;
    for (i = 0; i < n; i++) {
        rects[i * 4 + 0] = c->acc[i].x;
        rects[i * 4 + 1] = c->acc[i].y;
        rects[i * 4 + 2] = c->acc[i].width;
        rects[i * 4 + 3] = c->acc[i].height;
    }
    if (n < c->acc_n) {
        /* The caller's array is smaller than MAX_ACC. Same rule as overflow:
         * merge the tail into the last slot rather than dropping it. */
        int x0 = rects[(n - 1) * 4 + 0], y0 = rects[(n - 1) * 4 + 1];
        int x1 = x0 + rects[(n - 1) * 4 + 2], y1 = y0 + rects[(n - 1) * 4 + 3];
        for (i = n - 1; i < c->acc_n; i++) {
            if (c->acc[i].x < x0) x0 = c->acc[i].x;
            if (c->acc[i].y < y0) y0 = c->acc[i].y;
            if (c->acc[i].x + c->acc[i].width  > x1) x1 = c->acc[i].x + c->acc[i].width;
            if (c->acc[i].y + c->acc[i].height > y1) y1 = c->acc[i].y + c->acc[i].height;
        }
        rects[(n - 1) * 4 + 0] = x0;
        rects[(n - 1) * 4 + 1] = y0;
        rects[(n - 1) * 4 + 2] = x1 - x0;
        rects[(n - 1) * 4 + 3] = y1 - y0;
    }
    c->acc_n = 0;
    c->acc_overflow = 0;
    return n;
}

int rd_capture_poll(rd_capture *c, int *rects, int max_rects)
{
    if (!c || !rects || max_rects < 1) return -1;
    if (c->dead) { err_set(c, "X connection lost"); return -1; }

    if (c->force_full) {
        if (rd_capture_full(c) != 0) return -1;
        rehash_all(c);
        gettimeofday(&c->last_validate, NULL);
        if (c->path == RD_PATH_DAMAGE) {
            /* Damage reported up to now is stale once everything has been
             * read; drop it so the next poll reports only new changes. */
            WITH_X_GUARD(c, dead_out);
            drain_damage(c);
            END_X_GUARD();
            c->acc_n = 0;
            c->acc_overflow = 0;
        }
        return report_full(c, rects, max_rects);
    }

    if (c->path != RD_PATH_DAMAGE) {
        /* No usable damage: read the screen and find the change ourselves.
         * Reading is the cheap half over MIT-SHM -- 4.9 ms against the ~1 ms
         * the comparison costs -- so this is far closer to the damage path
         * than to sending a whole frame every time. */
        if (rd_capture_full(c) != 0) return -1;
        if (!c->hash_valid) { rehash_all(c); return report_full(c, rects, max_rects); }
        return diff_bands(c, rects, max_rects);
    }

    WITH_X_GUARD(c, dead_out);
    drain_damage(c);
    END_X_GUARD();

    /* Nothing reported: the common case on a desktop sitting still, and it
     * must not touch a pixel -- except once every VALIDATE_MS, to check that
     * the silence is real. */
    if (c->acc_n == 0)
        return validate_quiet(c, rects, max_rects);

    if (rd_capture_full(c) != 0)
        return -1;
    /* The whole canvas was just read, so the hashes may as well describe it:
     * that is what lets the next quiet stretch be validated for the price of
     * the comparison alone. */
    rehash_all(c);
    gettimeofday(&c->last_validate, NULL);
    return report_acc(c, rects, max_rects);

dead_out:
    return -1;
}

/* ---------------------------------------------------------------------------
 * Fused downscale + I420 conversion. See the header for why this exists, and
 * for why it is a separate function from the IRIX port's rather than a rename
 * of it: that one reads A,B,G,R and this canvas is A,R,G,B.
 *
 * The structure below is the IRIX one -- three kernels, chosen by factor --
 * because the reasons for each hold here too. What changed is every source
 * offset: byte 1 is red here and blue there, byte 3 is blue here and red
 * there. Green and alpha sit in the same places.
 * ------------------------------------------------------------------------- */

static unsigned char clamp8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)v;
}

/* BT.601 studio swing, taking a *sum* of `1 << sh` samples per channel rather
 * than an average, so the box divide folds into the shift and no per-pixel
 * integer division survives.
 *
 *   Y = (( 66R + 129G +  25B + 128) >> 8) +  16
 *   U = ((-38R -  74G + 112B + 128) >> 8) + 128
 *   V = ((112R -  94G -  18B + 128) >> 8) + 128
 *
 * Worst case magnitude is 129 * 255 * (1 << sh); at the largest factor this
 * accepts (8, so sh reaches 6 for luma and 8 for chroma) that is 8.4e6, well
 * inside a 32-bit int on a machine whose int is 32 bits even in LP64.
 */
#define Y_OF(r, g, b, sh) \
    clamp8(((((66 * (r) + 129 * (g) + 25 * (b)) >> (sh)) + 128) >> 8) + 16)
#define U_OF(r, g, b, sh) \
    clamp8(((((-38 * (r) - 74 * (g) + 112 * (b)) >> (sh)) + 128) >> 8) + 128)
#define V_OF(r, g, b, sh) \
    clamp8(((((112 * (r) - 94 * (g) - 18 * (b)) >> (sh)) + 128) >> 8) + 128)

int rd_argb_to_i420_rect(const unsigned char *src, size_t src_len, int src_stride,
                         unsigned char *yp, unsigned char *up, unsigned char *vp,
                         int dst_w, int dst_h, int chroma_stride,
                         int factor,
                         int dx0, int dy0, int dx1, int dy1)
{
    int by, bx, sh, step;

    if (!src || !yp || !up || !vp) return -1;
    if (dst_w < 2 || dst_h < 2 || chroma_stride < dst_w / 2) return -1;
    if (factor < 1) return -1;
    if (src_stride < dst_w * factor * 4) return -1;

    /* Power of two only -- the whole point of the shift form above. */
    switch (factor) {
        case 1: sh = 0; break;
        case 2: sh = 2; break;
        case 4: sh = 4; break;
        case 8: sh = 6; break;
        default: return -1;
    }

    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > dst_w) dx1 = dst_w;
    if (dy1 > dst_h) dy1 = dst_h;
    dx0 &= ~1;
    dy0 &= ~1;
    dx1 = (dx1 + 1) & ~1;
    dy1 = (dy1 + 1) & ~1;
    if (dx1 > (dst_w & ~1)) dx1 = dst_w & ~1;
    if (dy1 > (dst_h & ~1)) dy1 = dst_h & ~1;
    if (dx0 >= dx1 || dy0 >= dy1) return 0;

    /* Refuse rather than read off the end: the caller's geometry can go stale
     * if the display mode changes mid-frame, and a segfault is a worse outcome
     * than a skipped frame. */
    if (src_len < (size_t)src_stride * (size_t)(dy1 * factor)) return -1;

    if (factor == 1) {
        /* No box to average: the common shape reduces to a plain converter,
         * and writing it out separately keeps three loads per pixel instead of
         * three loads plus the accumulate the general path would do. */
        for (by = dy0 / 2; by < dy1 / 2; by++) {
            const unsigned char *row0 = src + (size_t)(by * 2) * src_stride;
            const unsigned char *row1 = row0 + src_stride;
            unsigned char *ya = yp + (size_t)(by * 2) * dst_w;
            unsigned char *yb = ya + dst_w;
            unsigned char *ua = up + (size_t)by * chroma_stride;
            unsigned char *va = vp + (size_t)by * chroma_stride;

            for (bx = dx0 / 2; bx < dx1 / 2; bx++) {
                const unsigned char *p0 = row0 + (size_t)bx * 8;
                const unsigned char *p1 = row1 + (size_t)bx * 8;
                /* A,R,G,B: byte 0 is alpha, byte 1 is RED, byte 3 is blue.
                 * The IRIX loop reads 1 as blue and 3 as red. */
                int r0 = p0[1], g0 = p0[2], b0 = p0[3];
                int r1 = p0[5], g1 = p0[6], b1 = p0[7];
                int r2 = p1[1], g2 = p1[2], b2 = p1[3];
                int r3 = p1[5], g3 = p1[6], b3 = p1[7];
                int r = r0 + r1 + r2 + r3;
                int g = g0 + g1 + g2 + g3;
                int b = b0 + b1 + b2 + b3;

                ya[bx * 2]     = Y_OF(r0, g0, b0, 0);
                ya[bx * 2 + 1] = Y_OF(r1, g1, b1, 0);
                yb[bx * 2]     = Y_OF(r2, g2, b2, 0);
                yb[bx * 2 + 1] = Y_OF(r3, g3, b3, 0);
                ua[bx] = U_OF(r, g, b, 2);
                va[bx] = V_OF(r, g, b, 2);
            }
        }
        return 0;
    }

    if (factor == 2) {
        /* The likely default, and worth its own kernel.
         *
         * A destination 2x2 block is a 4x4 source block: sixteen pixels at
         * fixed offsets from four row pointers. The general path below reaches
         * them through two loops whose trip count is a runtime value, so the
         * compiler cannot unroll either -- and at four iterations apiece the
         * compare-and-branch is a large fraction of the work being scheduled.
         * On the Indy this was measured at roughly half the general path's
         * cost for byte-identical output; not yet measured here.
         */
        for (by = dy0 / 2; by < dy1 / 2; by++) {
            const unsigned char *r0 = src + (size_t)(by * 4) * src_stride
                                    + (size_t)dx0 * 8;
            const unsigned char *r1 = r0 + src_stride;
            const unsigned char *r2 = r1 + src_stride;
            const unsigned char *r3 = r2 + src_stride;
            unsigned char *ya = yp + (size_t)(by * 2) * dst_w + dx0;
            unsigned char *yb = ya + dst_w;
            unsigned char *ua = up + (size_t)by * chroma_stride + dx0 / 2;
            unsigned char *va = vp + (size_t)by * chroma_stride + dx0 / 2;
            int blocks = (dx1 - dx0) / 2;

            for (bx = 0; bx < blocks; bx++) {
                int o = bx * 16;
                /* A,R,G,B: byte 0 alpha, 1 red, 2 green, 3 blue. Each of the
                 * four sums is one destination pixel's 2x2 source box. */
                int q0 = r0[o+1] + r0[o+5] + r1[o+1] + r1[o+5];
                int g0 = r0[o+2] + r0[o+6] + r1[o+2] + r1[o+6];
                int b0 = r0[o+3] + r0[o+7] + r1[o+3] + r1[o+7];
                int q1 = r0[o+9] + r0[o+13] + r1[o+9] + r1[o+13];
                int g1 = r0[o+10] + r0[o+14] + r1[o+10] + r1[o+14];
                int b1 = r0[o+11] + r0[o+15] + r1[o+11] + r1[o+15];
                int q2 = r2[o+1] + r2[o+5] + r3[o+1] + r3[o+5];
                int g2 = r2[o+2] + r2[o+6] + r3[o+2] + r3[o+6];
                int b2 = r2[o+3] + r2[o+7] + r3[o+3] + r3[o+7];
                int q3 = r2[o+9] + r2[o+13] + r3[o+9] + r3[o+13];
                int g3 = r2[o+10] + r2[o+14] + r3[o+10] + r3[o+14];
                int b3 = r2[o+11] + r2[o+15] + r3[o+11] + r3[o+15];

                ya[bx * 2]     = Y_OF(q0, g0, b0, 2);
                ya[bx * 2 + 1] = Y_OF(q1, g1, b1, 2);
                yb[bx * 2]     = Y_OF(q2, g2, b2, 2);
                yb[bx * 2 + 1] = Y_OF(q3, g3, b3, 2);
                {
                    int cr = q0 + q1 + q2 + q3;
                    int cg = g0 + g1 + g2 + g3;
                    int cb = b0 + b1 + b2 + b3;
                    ua[bx] = U_OF(cr, cg, cb, 4);
                    va[bx] = V_OF(cr, cg, cb, 4);
                }
            }
        }
        return 0;
    }

    /* Above 1/2, sample the box rather than averaging all of it.
     *
     * The conversion's cost is dominated by *reading the source*, not by
     * writing the output or by the arithmetic: at factor 4 each destination
     * pixel averages sixteen source pixels, so shrinking the output does not
     * shrink the work.
     *
     * Averaging a 2x2 sample of each box instead of all of it reads a quarter
     * of the source at factor 4 and a sixteenth at factor 8. The quality
     * argument is that at 1/4 a 1280x1024 desktop is already 320x256: text is
     * unreadable either way, and what the box filter buys at that point is a
     * slightly smoother version of something nobody is reading. At 1/2, where
     * text is marginal and each box is only four pixels anyway, the full
     * average is kept.
     *
     * `step` is the gap between the two samples in each direction, so the pair
     * straddles the box rather than sitting in its corner.
     */
    step = (factor >= 4) ? (factor / 2) : 1;
    if (step > 1) {
        /* Two samples per axis regardless of factor: the shift is fixed at 2. */
        for (by = dy0 / 2; by < dy1 / 2; by++) {
            const unsigned char *brow = src + (size_t)(by * 2 * factor) * src_stride;
            unsigned char *ya = yp + (size_t)(by * 2) * dst_w;
            unsigned char *yb = ya + dst_w;
            unsigned char *ua = up + (size_t)by * chroma_stride;
            unsigned char *va = vp + (size_t)by * chroma_stride;

            for (bx = dx0 / 2; bx < dx1 / 2; bx++) {
                int q, cr = 0, cg = 0, cb = 0;
                int sr[4], sg[4], sb[4];

                for (q = 0; q < 4; q++) {
                    const unsigned char *s0 = brow
                        + (size_t)((q >> 1) * factor) * src_stride
                        + (size_t)((bx * 2 + (q & 1)) * factor) * 4;
                    const unsigned char *s1 = s0 + (size_t)step * src_stride;
                    int o = step * 4;
                    int ar = s0[1] + s0[o + 1] + s1[1] + s1[o + 1];
                    int ag = s0[2] + s0[o + 2] + s1[2] + s1[o + 2];
                    int ab = s0[3] + s0[o + 3] + s1[3] + s1[o + 3];
                    sr[q] = ar; sg[q] = ag; sb[q] = ab;
                    cr += ar; cg += ag; cb += ab;
                }

                ya[bx * 2]     = Y_OF(sr[0], sg[0], sb[0], 2);
                ya[bx * 2 + 1] = Y_OF(sr[1], sg[1], sb[1], 2);
                yb[bx * 2]     = Y_OF(sr[2], sg[2], sb[2], 2);
                yb[bx * 2 + 1] = Y_OF(sr[3], sg[3], sb[3], 2);
                ua[bx] = U_OF(cr, cg, cb, 4);
                va[bx] = V_OF(cr, cg, cb, 4);
            }
        }
        return 0;
    }

    for (by = dy0 / 2; by < dy1 / 2; by++) {
        /* Top-left source pixel of this destination 2x2 block's row pair. */
        const unsigned char *brow = src + (size_t)(by * 2 * factor) * src_stride;
        unsigned char *ya = yp + (size_t)(by * 2) * dst_w;
        unsigned char *yb = ya + dst_w;
        unsigned char *ua = up + (size_t)by * chroma_stride;
        unsigned char *va = vp + (size_t)by * chroma_stride;

        for (bx = dx0 / 2; bx < dx1 / 2; bx++) {
            int q, cr = 0, cg = 0, cb = 0;
            int sr[4], sg[4], sb[4];

            for (q = 0; q < 4; q++) {
                const unsigned char *s0 = brow
                    + (size_t)((q >> 1) * factor) * src_stride
                    + (size_t)((bx * 2 + (q & 1)) * factor) * 4;
                int ky, kx, ar = 0, ag = 0, ab = 0;

                for (ky = 0; ky < factor; ky++) {
                    const unsigned char *s = s0 + (size_t)ky * src_stride;
                    for (kx = 0; kx < factor; kx++) {
                        ar += s[kx * 4 + 1];
                        ag += s[kx * 4 + 2];
                        ab += s[kx * 4 + 3];
                    }
                }
                sr[q] = ar; sg[q] = ag; sb[q] = ab;
                cr += ar; cg += ag; cb += ab;
            }

            ya[bx * 2]     = Y_OF(sr[0], sg[0], sb[0], sh);
            ya[bx * 2 + 1] = Y_OF(sr[1], sg[1], sb[1], sh);
            yb[bx * 2]     = Y_OF(sr[2], sg[2], sb[2], sh);
            yb[bx * 2 + 1] = Y_OF(sr[3], sg[3], sb[3], sh);
            /* Four boxes, so two more halvings than the luma shift. */
            ua[bx] = U_OF(cr, cg, cb, sh + 2);
            va[bx] = V_OF(cr, cg, cb, sh + 2);
        }
    }
    return 0;
}
