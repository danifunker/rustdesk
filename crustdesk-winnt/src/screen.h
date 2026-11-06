/* The NT screen, as I420 for the encoder, through GDI.
 *
 * Every look copies the screen into a 32-bit DIB section (GDI converts from
 * whatever depth the display runs at, palettes included), compares it with
 * the last look one macroblock at a time, and converts only what changed.
 * Large screens are read a band at a time between frames when the look
 * would be slow (screen_collect), so the peer's input keeps flowing.
 *
 * The agent runs as a service on the interactive window station, and the
 * screen it can see is its thread's desktop: Default while someone is
 * logged in, Winlogon at the logon and locked screens. screen_follow()
 * moves the calling thread to whichever desktop takes input now, which
 * only works for a thread that owns no windows -- the agent's main thread.
 */
#ifndef CDV_NT_SCREEN_H
#define CDV_NT_SCREEN_H

#include <stdint.h>

typedef struct {
    int width, height, mbw, mbh;
    uint8_t *y, *u, *v;   /* I420 of the whole screen, current where sent */
    int ystride, uvstride;
    uint8_t *dirty;       /* mbw*mbh: changed since the last frame */
    int ndirty;
    uint8_t *flat;        /* mbw*mbh: one colour (cheap to code), as last converted */
    /* the last screen_collect, for the log */
    int rows_read;
    int rects_read;       /* with the screen hooks: rectangles, and pixels, read */
    long px_read;
    uint32_t read_ms, convert_ms;
} cdv_screen;

/* Size everything for the screen as it is now; 0 if out of memory. */
int screen_open(cdv_screen *s);
void screen_close(cdv_screen *s);

/* Has the screen changed size since screen_open? */
int screen_resized(const cdv_screen *s);

/* Move this thread to the input desktop if that changed (a logon, a lock,
 * the logon screen, a screen saver). 1 if it moved: everything must be sent
 * again. *name is the desktop's name ("Default", "Winlogon"...). */
int screen_follow(const char **name);

/* A session started: read the whole screen, everything dirty. */
void screen_start(cdv_screen *s);

/* Read back (some of) the screen, compare, convert: afterwards s->dirty
 * marks what the encoder must code. `budget_ms`: stop reading bands after
 * about this long (the rest is read next time). */
void screen_collect(cdv_screen *s, uint32_t budget_ms);

/* The peer pressed a key or a button: its console will want watching. */
void screen_note_input(void);

/* Marked squares are waiting for their second read: keep looking. */
int screen_settling(void);

/* Is a band of the screen still to be read in this pass? */
int screen_pending(const cdv_screen *s);

void screen_mark_all(cdv_screen *s);
/* Only the macroblocks marked in `sent` were sent: the rest stay dirty. */
void screen_sent_some(cdv_screen *s, const uint8_t *sent);

/* The screen's commonest colour, as Y, U, V. */
void screen_background(const cdv_screen *s, uint8_t *y, uint8_t *u, uint8_t *v);

/* A screenshot: read the whole screen afresh (not touching what the
 * encoder is given), then each row as R G B. */
void screen_grab_all(cdv_screen *s);
void screen_rgb_row(const cdv_screen *s, int y, uint8_t *out);

/* The pointer: where it is; and its shape, when it changed (returns 1 and
 * fills rgba, at most 64x64, with its size and hot spot). */
int screen_pointer(int *x, int *y);
int screen_cursor_shape(uint32_t *id, int *hotx, int *hoty, int *w, int *h, uint8_t *rgba);

#endif
