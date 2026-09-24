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

/* What the ID server made of us, for the window. */
enum { RS_OFF, RS_RESOLVING, RS_NO_DNS, RS_REGISTERING, RS_REGISTERED, RS_REFUSED };

typedef struct {
    /* main -> engine */
    volatile int suspend_req;   /* stop touching the screen and encoder */
    volatile int drop_req;      /* end the session (the screen outgrew the queue) */
    volatile int announce;      /* tell the peer the display changed */
    /* engine -> main */
    volatile int suspended;
    volatile int live;          /* a peer is logged in */
    volatile int secure;        /* ...and the session is encrypted */
    volatile int rdv_state;     /* RS_* */
    volatile int rdv_refused;   /* RegisterPkResponse result when refused */
    volatile unsigned long server_ip;
    volatile unsigned long frames, bytes, ticks;
} engine_flags;

extern engine_flags eng;

/* Everything the engine works on. Set up by the main loop before start. */
typedef struct {
    cdv_tcp *direct;         /* listens on the direct port: unencrypted sessions */
    cdv_tcp *local;          /* listens for a peer hbbs sent our local address to */
    cdv_tcp *relay;          /* joins hbbr */
    cdv_tcp *helper;         /* short messages to hbbs */
    cdv_udp *rdv;            /* registration and DNS */
    cdv_udp *lan;            /* discovery broadcasts, port 21119 */
    ip_addr my_ip;
    unsigned short direct_port, local_port;
    const char *server;      /* "host[:port]", or "" for no ID server */
    const char *relay_host;  /* override for the relay hbbs names, or "" */
    const char *key;         /* the server's key, for a relay started with -k */
    const uint8_t *uuid, *pk;
    ip_addr dns[3];
    int ndns;
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

/* The peer moved the pointer: hold back position reports for a moment, so
 * they do not fight the peer's own. */
void engine_peer_moved_pointer(void);

/* The clipboard, across the two contexts: the main loop owns the Scrap
 * Manager, the engine owns the session. Each buffer has one writer. */
#define CLIP_MAX 16384
typedef struct {
    char text[CLIP_MAX];
    volatile size_t len;
    volatile int ready;
} engine_clip;
extern engine_clip clip_out; /* main -> peer */
extern engine_clip clip_in;  /* peer -> main */

/* A name for the main loop to look up with the Mac's own resolver: the
 * engine asks, the main loop answers (ans 1 with ip, or -1). If the main loop
 * is held up -- a menu is open -- the engine falls back on its own DNS. */
typedef struct {
    volatile int req, ans;
    char name[64];
    volatile unsigned long ip;
} engine_name;
extern engine_name name_q;

/* Log lines the engine produced, for the main loop to show. Returns NULL
 * when there are none. */
const char *engine_next_log(void);
void engine_log(const char *msg);

#endif
