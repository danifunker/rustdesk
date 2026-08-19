/*
 * capture_shim.h -- screen capture for IRIX, as the Rust side sees it.
 *
 * The interface is shaped by what this X server actually offers, which turned
 * out to be far better than the Mac's:
 *
 *   - SGI-SCREEN-CAPTURE tracks damage in the server. `rd_capture_poll` returns
 *     the changed rectangles *and* their pixels in one round trip, so a static
 *     desktop costs a few milliseconds rather than a full framebuffer read.
 *   - ReadDisplay hands back 32-bit pixels regardless of the screen's depth, so
 *     an 8-bit pseudocolour Indy needs no palette handling on the capture path.
 *   - XRD_READ_POINTER composites the hardware cursor into the image, so the
 *     agent can set cursor_embedded and never send a cursor shape.
 *
 * Pixels land in one persistent full-screen canvas in shared memory, and each
 * poll updates only the damaged parts of it. That is exactly the model a remote
 * desktop wants, so the canvas is exposed directly rather than copied.
 *
 * MEMORY ORDER IS A,B,G,R -- alpha (0xff) first, red last. Measured, not read
 * off a header. It is *not* the Mac's A,R,G,B, and it is not libyuv's "ARGB"
 * either; see convert.rs's note on why that name means the opposite of what it
 * looks like.
 */

#ifndef RD_CAPTURE_SHIM_H
#define RD_CAPTURE_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rd_capture rd_capture;

/* Capture paths, in the order the shim prefers them. Reported by
 * rd_capture_path so the agent can log which one it got, and forced by
 * rd_capture_open_forced so the fallback is exercised deliberately rather than
 * for the first time on someone else's machine. */
#define RD_PATH_DAMAGE   0   /* SGI-SCREEN-CAPTURE + ReadDisplay, shm */
#define RD_PATH_READDISP 1   /* ReadDisplay, shm, whole screen each frame */
#define RD_PATH_GETIMAGE 2   /* XGetImage + colormap; no SGI extensions at all */

/* Open the display and set up the best available capture path.
 * `display` may be NULL, meaning $DISPLAY. Returns NULL on failure; call
 * rd_capture_last_error for why. */
rd_capture *rd_capture_open(const char *display);

/* As rd_capture_open, but refuse to use any path better than `max_path`.
 * RD_PATH_GETIMAGE forces the pure-Xlib fallback even where the extensions
 * exist, which is the only honest way to test it. */
rd_capture *rd_capture_open_forced(const char *display, int max_path);

void rd_capture_close(rd_capture *c);

int rd_capture_width(const rd_capture *c);
int rd_capture_height(const rd_capture *c);
int rd_capture_depth(const rd_capture *c);      /* the screen's depth, not the canvas's */
int rd_capture_stride(const rd_capture *c);     /* bytes per canvas row */
int rd_capture_path(const rd_capture *c);       /* RD_PATH_* actually in use */
int rd_capture_cursor_embedded(const rd_capture *c); /* 1 if the cursor is composited */

/* The canvas: width*height 32-bit pixels, A,B,G,R in memory. Valid until
 * rd_capture_close. Never write to it. */
const unsigned char *rd_capture_buffer(const rd_capture *c);

/* Read the whole screen into the canvas. Returns 0, or -1 on failure. */
int rd_capture_full(rd_capture *c);

/* Update the canvas with whatever changed since the last call, and report the
 * changed rectangles in screen coordinates.
 *
 * `rects` receives x,y,w,h per rectangle and must have room for 4*max_rects
 * ints. Returns the number of rectangles written, 0 if nothing changed, or -1
 * on failure. When the server reports more rectangles than fit, they are merged
 * into their bounding box rather than dropped, so the canvas and the returned
 * list never disagree.
 *
 * The first call after opening, and any call after rd_capture_invalidate,
 * reports the whole screen. */
int rd_capture_poll(rd_capture *c, int *rects, int max_rects);

/* Make the next poll report and re-read everything. */
void rd_capture_invalidate(rd_capture *c);

/* Re-read one rectangle into the canvas, ignoring the damage list.
 *
 * The damage path keeps the canvas current on its own, so this is only for a
 * caller repairing a region the *peer* may have lost -- a dropped frame is not
 * damage, and the server will never report it again. Returns 0, or -1. */
int rd_capture_read_rect(rd_capture *c, int x, int y, int w, int h);

/* Why the last call failed. Never NULL. */
const char *rd_capture_last_error(const rd_capture *c);

/* 1 once the X connection has died -- the server does wedge and get restarted,
 * and the agent has to notice and reopen rather than sit on a dead display. */
int rd_capture_is_dead(const rd_capture *c);

/*
 * Box-filter downscale of an A,B,G,R canvas by an integer factor.
 *
 * Encoding cost is linear in pixel count, so halving each dimension is a 4x
 * saving and this is the main lever the agent has. Separate from capture
 * because the scale factor follows the peer's requested image quality, which
 * can change mid-session.
 *
 * `dst` must hold (sw/factor) * (sh/factor) pixels. Returns the destination
 * width, or 0 if the arguments do not make sense.
 */
int rd_scale_abgr(const unsigned char *src, int sw, int sh, int src_stride,
                  unsigned char *dst, int factor);

#ifdef __cplusplus
}
#endif

#endif /* RD_CAPTURE_SHIM_H */
