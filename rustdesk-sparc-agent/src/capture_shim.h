/*
 * capture_shim.h -- screen capture for Solaris 10 / Xsun, as Rust sees it.
 *
 * Same contract as the IRIX port's shim, so the Rust side above it is the same
 * shape; what differs is what the server offers underneath.
 *
 *   - There is no SGI-SCREEN-CAPTURE here. Change tracking is the standard
 *     DAMAGE extension, which reports *rectangles only* -- no pixels -- so a
 *     poll that sees damage still has to read the screen.
 *   - Reading it is MIT-SHM, and that is not an optimisation. Measured on the
 *     Blade at 1280x1024: XGetImage 200 ms, XShmGetImage 5.3 ms. Forty times.
 *     A core-protocol agent would manage five frames a second before encoding
 *     a single pixel, so the fallback path exists to diagnose, not to serve.
 *   - The cursor is never composited into the screen's contents by X, so
 *     rd_capture_cursor_embedded is always 0 and the agent sends a shape.
 *     XFIXES has the real one; see cursor_shim.c.
 *
 * An idle poll costs no pixels at all: with nothing damaged, poll returns 0
 * without reading the screen, which is what makes a 5 ms full-screen read an
 * acceptable answer to "something changed".
 *
 * MEMORY ORDER IS A,R,G,B -- the unused byte first, blue last. Measured with
 * probes/xprobe.c on this machine (32bpp, MSBFirst, R=00ff0000 G=0000ff00
 * B=000000ff), not read off a header. That is the PowerPC Mac's order, and it
 * is *not* the IRIX port's A,B,G,R, so this port converts with the Mac's code.
 * A server that disagrees is reported by rd_capture_pixel_order rather than
 * silently swapping red and blue.
 */

#ifndef RD_CAPTURE_SHIM_H
#define RD_CAPTURE_SHIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rd_capture rd_capture;

/* Capture paths, best first. Reported by rd_capture_path so the agent can log
 * what it got, and forced by rd_capture_open_forced so a fallback is exercised
 * deliberately rather than for the first time on someone else's machine. */
#define RD_PATH_DAMAGE   0   /* DAMAGE tells us what changed, MIT-SHM reads it */
#define RD_PATH_SHM      1   /* MIT-SHM, whole screen every poll */
#define RD_PATH_GETIMAGE 2   /* XGetImage; correct, and forty times slower */

/* What rd_capture_pixel_order returns. */
#define RD_ORDER_ARGB    0   /* what this machine does */
#define RD_ORDER_BGRA    1   /* a little-endian server; convert accordingly */

/* The screen's size over a bare X connection: no shared memory, no damage
 * interest. Returns 0 on success. Separate from rd_capture_open because the
 * caller polls it, and building a whole capture context per poll means a 5 MB
 * shmget and an XShmAttach each time. */
int rd_display_size(int *w, int *h);

/* Open the display and set up the best available capture path. `display` may
 * be NULL, meaning $DISPLAY. NULL on failure; rd_capture_last_error says why. */
rd_capture *rd_capture_open(const char *display);

/* As rd_capture_open, but refuse any path better than `max_path`. */
rd_capture *rd_capture_open_forced(const char *display, int max_path);

void rd_capture_close(rd_capture *c);

int rd_capture_width(const rd_capture *c);
int rd_capture_height(const rd_capture *c);
int rd_capture_depth(const rd_capture *c);       /* the screen's depth */
int rd_capture_stride(const rd_capture *c);      /* bytes per canvas row */
int rd_capture_path(const rd_capture *c);        /* RD_PATH_* in use */
int rd_capture_pixel_order(const rd_capture *c); /* RD_ORDER_* */
int rd_capture_cursor_embedded(const rd_capture *c);

/* The canvas: width*height 32-bit pixels. Valid until rd_capture_close, and
 * never to be written to. */
const unsigned char *rd_capture_buffer(const rd_capture *c);

/* Read the whole screen into the canvas. 0, or -1 on failure. */
int rd_capture_full(rd_capture *c);

/* Update the canvas with whatever changed since the last call, and report the
 * changed rectangles in screen coordinates.
 *
 * `rects` receives x,y,w,h per rectangle and needs room for 4*max_rects ints.
 * Returns the count, 0 if nothing changed, or -1 on failure. When the server
 * reports more rectangles than fit they are merged into their bounding box
 * rather than dropped, so the canvas and the returned list never disagree.
 *
 * The first call after opening, and any call after rd_capture_invalidate,
 * reports the whole screen. */
int rd_capture_poll(rd_capture *c, int *rects, int max_rects);

/* Make the next poll re-read and report everything. */
void rd_capture_invalidate(rd_capture *c);

/* Re-read one rectangle into the canvas, ignoring the damage list. Only for a
 * caller repairing a region the *peer* lost: a dropped frame is not damage and
 * the server will never report it again. Goes through XGetImage, so it costs
 * in proportion to the rectangle. 0, or -1. */
int rd_capture_read_rect(rd_capture *c, int x, int y, int w, int h);

/* Why the last call failed. Never NULL. */
const char *rd_capture_last_error(const rd_capture *c);

/* 1 once the X connection has died -- a server does get restarted, and the
 * agent has to notice and reopen rather than sit on a dead display. */
int rd_capture_is_dead(const rd_capture *c);

/*
 * Box-filter downscale **fused with the A,R,G,B -> I420 conversion**, over a
 * destination rectangle.
 *
 * The IRIX port has the same function for A,B,G,R canvases
 * (`rd_abgr_to_i420_rect`). This one is not a rename of it: this machine's
 * XVR-600 hands back A,R,G,B -- byte 1 is red, byte 3 is blue -- and reading
 * the IRIX loop's offsets here swaps red and blue in every frame, which over a
 * remote display looks like a fault in the client rather than in the agent.
 * `rd_capture_pixel_order` reports what the server actually said; the Rust side
 * refuses to call this when it is not RD_ORDER_ARGB.
 *
 * Three things it does that a scale-then-convert pair does not, each of which
 * was worth measuring on its own when the IRIX port was written:
 *
 *   - One walk of the canvas instead of two, with no full-size intermediate
 *     written and read back. On the emulated Indy at 1/2 those two steps cost
 *     386 + 306 ms, with a whole scaled framebuffer of memory traffic between
 *     them.
 *   - A destination rectangle, so a frame costs what the damage costs rather
 *     than what the screen costs. The DAMAGE extension hands us exact
 *     rectangles; the band model threw them away.
 *   - The right byte order, which is the part that differs here.
 *
 * `factor` must be a power of two (1, 2, 4, 8), and anything else is refused
 * rather than quietly rounded: that restriction is what lets the box average
 * fold into the BT.601 weights as a shift, instead of three integer divides
 * per destination pixel. Anything else returns -1 with nothing written.
 *
 * dx0/dy0/dx1/dy1 are in **destination** pixels, half-open, and are snapped
 * outward to even -- a 4:2:0 chroma sample is shared by a 2x2 destination
 * block, so a bound falling inside one would leave half of it unwritten.
 *
 * Returns 0 on success, -1 if the arguments do not make sense (in which case
 * nothing is written).
 */
int rd_argb_to_i420_rect(const unsigned char *src, size_t src_len, int src_stride,
                         unsigned char *yp, unsigned char *up, unsigned char *vp,
                         int dst_w, int dst_h, int chroma_stride,
                         int factor,
                         int dx0, int dy0, int dx1, int dy1);

#ifdef __cplusplus
}
#endif

#endif /* RD_CAPTURE_SHIM_H */
