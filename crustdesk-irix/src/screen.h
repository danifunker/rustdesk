/* The IRIX screen: what changed, read back, as I420 for the encoder.
 *
 * Reading the framebuffer is the expensive part on an Indigo: LG1 hands pixels
 * back at about 3.5 million a second through MIT-SHM (234 ms for 1024x768), and
 * every request costs about 2 ms more. Reading the whole screen to find out what
 * changed would cap the agent at three frames a second before it encoded
 * anything. So it reads only where something was drawn:
 *
 *   - DEC-XTRAP reports every drawing request any client makes, with the
 *     window's position on the screen; the request's own coordinates give the
 *     rectangle. Those macroblocks are read back and compared.
 *   - SubstructureNotify on the root reports top-level windows mapping, moving
 *     and going away, which is when the server paints without being asked.
 *   - A band of the screen is read every pass regardless, round and round, for
 *     what X never sees: IRIS GL drawing, the server's own painting.
 *   - Colormap requests are trapped too: on an 8-bit screen a colormap change
 *     recolours pixels that never change value.
 */
#ifndef CDV_IRIX_SCREEN_H
#define CDV_IRIX_SCREEN_H

#include <stdint.h>
#include <X11/Xlib.h>
#include "vp8enc.h"

typedef struct {
    int width, height, mbw, mbh;
    uint8_t *y, *u, *v;   /* I420 of the whole screen, always current where sent */
    int ystride, uvstride;
    uint8_t *dirty;       /* mbw*mbh: changed since the last frame */
    int ndirty;
    /* mbw*mbh: where the contents came from, for the encoder's motion
     * vectors (scrolls and window moves), as of the last screen_collect */
    const vp8e_hint *hints;
    int nhints;
    uint8_t *flat;        /* mbw*mbh: one colour (cheap to code), as last converted */
    /* statistics of the last screen_collect, for the log */
    int reads, read_px, damaged, scanned;
    uint32_t read_ms, convert_ms;
} cdv_screen;

/* Open the display and set everything up; 0 on failure (message logged).
 * gamma: the IRIX display gamma to undo (1.7 is IRIX's default; 0 off). */
int screen_open(cdv_screen *s, const char *display, double gamma);
Display *screen_display(void);
/* The X connection died: forget it (screen_open again later). */
void screen_lost(void);
int screen_fd(void);

/* Start and stop watching (a session started or ended). Starting reads the
 * whole screen and marks it all dirty. */
void screen_start(cdv_screen *s);
void screen_stop(void);

/* Handle whatever arrived on the X connection (damage reports, window
 * changes). Cheap; call whenever the connection is readable. */
void screen_events(cdv_screen *s);

/* Is anything waiting to be looked at (damage, or a band due)? */
int screen_pending(const cdv_screen *s, uint32_t now_ms);

/* Read back what changed, compare, and convert: afterwards s->dirty marks
 * the macroblocks the encoder must code, and the planes hold them.
 * `scan`: also read the next band of the background scan. */
void screen_collect(cdv_screen *s, int scan, uint32_t now_ms);

/* Everything is to be sent again (a keyframe was asked for). */
void screen_mark_all(cdv_screen *s);

/* The frame holding s->dirty was sent: start a new one. */
void screen_sent(cdv_screen *s);

/* Only the macroblocks marked in `sent` were: the rest stay dirty. */
void screen_sent_some(cdv_screen *s, const uint8_t *sent);

/* The screen's commonest colour, as Y, U, V. */
void screen_background(uint8_t *y, uint8_t *u, uint8_t *v);

/* How often the background scan reads its next band, in ms. */
void screen_set_scan(uint32_t ms);

/* Requests trapped and ignored (drawing on pixmaps) since the last call. */
void screen_stats(unsigned long *trapped, unsigned long *ignored);

/* The pointer, for the peer's cursor. */
int screen_pointer(int *x, int *y);

#endif
