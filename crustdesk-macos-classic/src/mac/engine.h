/* The agent proper: network, session, capture, encoder -- run from a
 * deferred task, not from the event loop.
 *
 * On System 7 an application only gets time when the frontmost one lets it.
 * A Menu Manager drag, a modal dialog or a busy program in front stops every
 * background application dead, and an agent living in its event loop would
 * stop with it -- with the remote button held down, since the release is one
 * of the things it would never get to. Interrupts keep running regardless, so
 * a Time Manager task fires every few milliseconds and queues a deferred task,
 * which runs with interrupts enabled once the interrupt has been dealt with,
 * whatever the foreground application is doing.
 *
 * Rules for everything reachable from engine_tick: no Memory Manager, no
 * QuickDraw, no File Manager, no synchronous driver calls, no printf; only
 * what the main loop cached. Work is done in slices -- a few macroblock rows
 * at a time -- so no single run holds the machine for long.
 *
 * The main loop does what cannot happen here (allocation, resetting the
 * stream, reading the colour table, drawing the window) and talks to the
 * engine through the flags below.
 */
#ifndef CDV_ENGINE_H
#define CDV_ENGINE_H

#include "net.h"
#include "screen.h"
#include "../core/session.h"
#include "../core/vp8enc.h"

typedef struct {
    /* main -> engine */
    volatile int suspend_req;   /* stop touching the screen and encoder */
    volatile int announce;      /* tell the peer the display changed */
    /* engine -> main */
    volatile int suspended;
    volatile int need_reset;    /* the connection is over; reset the stream */
    volatile int live;          /* a peer is logged in */
    volatile unsigned long frames, bytes, ticks;
} engine_flags;

extern engine_flags eng;

/* Everything the engine works on. Set up by the main loop before start. */
typedef struct {
    cdv_net *net;
    cdv_screen *scr;
    cdv_session *sess;
    const cdv_hooks *hooks;
    const cdv_ident *ident;
    vp8e *enc;
    uint8_t *outq, *inq;
    size_t outcap, incap;
    int q;
} engine_ctx;

void engine_setup(const engine_ctx *ctx);
/* Swap in a new screen and encoder (after a suspend). */
void engine_replace_video(vp8e *enc, uint8_t *outq, size_t outcap);

/* Start and stop the Time Manager heartbeat. */
OSErr engine_start(void);
void engine_stop(void);

/* One slice of work. Deferred-task time. */
void engine_tick(void);

/* Log lines the engine produced, for the main loop to show. Returns NULL
 * when there are none. */
const char *engine_next_log(void);
void engine_log(const char *msg);

#endif
