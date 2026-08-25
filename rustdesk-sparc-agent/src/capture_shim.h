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

#ifdef __cplusplus
}
#endif

#endif /* RD_CAPTURE_SHIM_H */
