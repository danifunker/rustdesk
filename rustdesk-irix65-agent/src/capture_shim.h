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

#include <setjmp.h>

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

/* X error handling, shared with input_shim.c, which talks to X too.
 *
 * rd_x_handlers installs the protocol- and I/O-error handlers. Xlib keeps one
 * of each per process, so every file that opens a Display calls it first.
 *
 * The guard is how a connection that dies is survived rather than exiting the
 * process: setjmp on rd_x_guard_jmp(), rd_x_guard_arm(), talk to X, then
 * rd_x_guard_disarm(). If the connection dies in between, the I/O handler
 * disarms and longjmps back, and setjmp returns non-zero. Both belong to the
 * CALLING THREAD -- each session has its own thread, and a global landing site
 * sent one session's I/O error onto another session's stack. Do not nest. */
void rd_x_handlers(void);
jmp_buf *rd_x_guard_jmp(void);
void rd_x_guard_arm(void);
void rd_x_guard_disarm(void);

/* The screen's size, over a bare X connection: no shared memory, no damage
 * interest, no ReadDisplay probe. Returns 0 on success.
 *
 * Separate from rd_capture_open because the caller polls it. Answering it by
 * building a whole capture context -- which is what this used to do -- meant a
 * 5 MB shmget, an XShmAttach, an SGICapRegisterInterest and a ReadDisplay read
 * *per message-loop pass*, which is both the reason the agent generated so much
 * traffic and the reason it leaked a Display every iteration. */
int rd_display_size(int *w, int *h);

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

/*
 * Box-filter downscale **fused with the A,B,G,R -> I420 conversion**, over a
 * destination rectangle.
 *
 * Replaces `rd_scale_abgr` followed by `rd_argb_to_i420_rows`. Three things
 * change, and each was worth measuring on its own:
 *
 *   - One walk of the canvas instead of two, with no full-size intermediate
 *     written and read back. At 1/2 on the emulated Indy the two steps cost
 *     386 + 306 ms; there is a whole scaled framebuffer's worth of memory
 *     traffic in the gap between them.
 *   - A destination rectangle, so a frame costs what the damage costs rather
 *     than what the screen costs. The server hands us exact rectangles; the
 *     band model threw them away.
 *   - The right byte order. `rd_argb_to_i420_rows` reads A,R,G,B, which is the
 *     Mac's framebuffer, not this one -- it swaps red and blue here.
 *
 * `factor` must be a power of two (1, 2, 4, 8). That is not an arbitrary
 * restriction: it is what lets the box average fold into the BT.601 weights as
 * a shift, and a per-pixel integer divide on an R5000 is ~35 cycles times three
 * channels times every destination pixel.
 *
 * dx0/dy0/dx1/dy1 are in **destination** pixels, half-open, and are snapped
 * outward to even -- a 4:2:0 chroma sample is shared by a 2x2 destination
 * block, so a bound falling inside one would leave half of it unwritten.
 *
 * Returns 0 on success, -1 if the arguments do not make sense (in which case
 * nothing is written).
 */
int rd_abgr_to_i420_rect(const unsigned char *src, size_t src_len, int src_stride,
                         unsigned char *yp, unsigned char *up, unsigned char *vp,
                         int dst_w, int dst_h, int chroma_stride,
                         int factor,
                         int dx0, int dy0, int dx1, int dy1);

#ifdef __cplusplus
}
#endif

#endif /* RD_CAPTURE_SHIM_H */
