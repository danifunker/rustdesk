/*
 * cursor_shim.c -- the pointer's actual shape, over XFIXES.
 *
 * X does not composite the cursor into a drawable's contents, so it is never
 * in what capture reads and a peer sees no pointer unless the agent sends one.
 * The PowerPC port could only send a stock arrow -- the system-wide shape lives
 * behind private CoreGraphics calls there -- but XFIXES hands over the real
 * one, so this port sends what is actually on screen, resize edges, I-beams and
 * all.
 *
 * Presents the same two functions `cursor.rs` already calls on the Mac:
 * a seed that changes when the shape changes, and the image as RGBA rows.
 *
 * ONE PIXEL IS ONE `unsigned long`, NOT FOUR BYTES. XFixesCursorImage stores
 * each 32-bit ARGB value in a whole long, which is 8 bytes on sparcv9 -- read
 * it as a packed u32 array and every second pixel is wrong, on this machine
 * specifically. Measured with probes/xprobe.c before any of this was written.
 *
 * The alpha is premultiplied, as it is in the CoreGraphics data the Mac port
 * passes through, so the peer sees the same thing from both.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

static Display          *g_dpy;
static int               g_have_xfixes;
/* The last image fetched, kept because `cursor.rs` asks for the seed on every
 * pass and the image only when it changed: fetching once and answering both
 * from the cache costs one round trip instead of two. */
static XFixesCursorImage *g_cached;

static int on_x_error(Display *d, XErrorEvent *e)
{
    char buf[96];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "cursor: X error %d (%s), request %d.%d\n",
            e->error_code, buf, e->request_code, e->minor_code);
    return 0;
}

static Display *dpy(void)
{
    int ev, er;
    if (g_dpy)
        return g_dpy;
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "cursor: XOpenDisplay failed; no pointer shape will be sent\n");
        return NULL;
    }
    XSetErrorHandler(on_x_error);
    g_have_xfixes = XFixesQueryExtension(g_dpy, &ev, &er) ? 1 : 0;
    if (!g_have_xfixes)
        fprintf(stderr, "cursor: XFIXES absent; falling back to a drawn arrow\n");
    return g_dpy;
}

/* Fetch and cache. Returns the cached image, or NULL. */
static XFixesCursorImage *fetch(void)
{
    if (!dpy() || !g_have_xfixes)
        return NULL;
    if (g_cached) {
        XFree(g_cached);
        g_cached = NULL;
    }
    g_cached = XFixesGetCursorImage(g_dpy);
    return g_cached;
}

/* A number that changes whenever the pointer changes shape.
 *
 * XFIXES keeps a serial per cursor, which is exactly this, so no hashing of
 * pixels is needed. Polled every pass by the caller, so it is one round trip
 * and no allocation beyond the image itself. */
int rd_cursor_seed(void)
{
    XFixesCursorImage *ci = fetch();
    return ci ? (int)ci->cursor_serial : 0;
}

/* The pointer as it looks right now, as RGBA rows. Returns the bytes written,
 * or -1. A cursor too large for `out` is skipped rather than truncated: the
 * caller keeps the shape it had, which is better than half a picture. */
int rd_cursor_image(unsigned char *out, int out_len, int *w, int *h,
                    int *hotx, int *hoty)
{
    XFixesCursorImage *ci;
    int x, y, width, height, written;

    if (!out || !w || !h || !hotx || !hoty || out_len <= 0)
        return -1;

    ci = g_cached ? g_cached : fetch();
    if (!ci)
        return -1;

    width  = ci->width;
    height = ci->height;
    if (width <= 0 || height <= 0)
        return -1;
    written = width * height * 4;
    if (written > out_len)
        return -1;

    for (y = 0; y < height; y++) {
        /* `pixels` is unsigned long *, one per pixel -- see the header. */
        const unsigned long *src = ci->pixels + (size_t)y * width;
        unsigned char *dst = out + (size_t)y * width * 4;
        for (x = 0; x < width; x++) {
            unsigned long p = src[x];
            dst[x * 4 + 0] = (unsigned char)((p >> 16) & 0xff);  /* r */
            dst[x * 4 + 1] = (unsigned char)((p >>  8) & 0xff);  /* g */
            dst[x * 4 + 2] = (unsigned char)( p        & 0xff);  /* b */
            dst[x * 4 + 3] = (unsigned char)((p >> 24) & 0xff);  /* a */
        }
    }

    *w = width;
    *h = height;
    *hotx = ci->xhot;
    *hoty = ci->yhot;
    return written;
}
