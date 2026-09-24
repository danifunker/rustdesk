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

/* Why the main loop asks for the session to end. */
enum { DROP_NONE, DROP_SCREEN, DROP_USER };

/* What the ID server made of us, for the window. */
enum { RS_OFF, RS_RESOLVING, RS_NO_DNS, RS_REGISTERING, RS_REGISTERED, RS_REFUSED };

typedef struct {
    /* main -> engine */
    volatile int suspend_req;   /* stop touching the screen and encoder */
    volatile int drop_req;      /* end the session: DROP_SCREEN or DROP_USER */
    volatile int announce;      /* tell the peer the display changed */
    /* engine -> main */
    volatile int suspended;
    volatile int live;          /* a peer is logged in */
    volatile int secure;        /* ...and the session is encrypted */
    volatile int rdv_state;     /* RS_* */
    volatile int rdv_refused;   /* RegisterPkResponse result when refused */
    volatile unsigned long server_ip;
    volatile int restart_req;   /* the peer asked for a restart */
    volatile unsigned long frames, bytes, ticks;
} engine_flags;

extern engine_flags eng;

/* Everything the engine works on. Set up by the main loop before start. */
typedef struct {
    cdv_tcp *direct, *direct2; /* listen on the direct port: unencrypted sessions */
    cdv_tcp *local, *local2; /* listen for a peer hbbs sent our local address to */
    cdv_tcp *relay, *relay2; /* join hbbr */
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
    /* Each of the two session slots: its control queue and its input
     * buffer (big enough for a client's 128 KB file block). */
    uint8_t *slot_ctl[2], *slot_in[2];
    size_t slot_ctlcap, slot_incap;
    /* A file manager's message, for the main loop (see engine_slot). */
    void (*file_hook)(int slot, int field, const uint8_t *d, size_t n);
    const cdv_hooks *hooks;
    const cdv_ident *ident;
    vp8e *enc;
    uint8_t *outq;           /* the video frame, for whichever slot is the desktop */
    size_t outcap;
    int q;
    int q_base;              /* q as set up; a peer's settings last one session */
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
    volatile int compressed; /* clip_in: a zstd frame, for the main loop to open */
} engine_clip;
extern engine_clip clip_out; /* main -> peer */
extern engine_clip clip_in;  /* peer -> main */

/* Chat, a line at a time each way; one writer each, like the clipboard. */
#define CHAT_MAX 512
typedef struct {
    char text[CHAT_MAX];
    volatile size_t len;
    volatile int ready;
} engine_chat;
extern engine_chat chat_out; /* main -> peer */
extern engine_chat chat_in;  /* peer -> main */

/* A screenshot. The peer asks (WANTED); the engine lends the main loop the
 * video frame's buffer once no frame is in it (LENT: buf, cap); the main loop
 * writes a PNG there (DONE: len, or err) and the engine sends it. Video waits
 * meanwhile, so the buffer has one user at a time. */
enum { SHOT_NONE, SHOT_WANTED, SHOT_LENT, SHOT_DONE };
typedef struct {
    volatile int state;
    uint8_t *buf;
    size_t cap;
    volatile size_t len;
    char err[64];
    unsigned long conn;         /* the session it is for */
} engine_shot;
extern engine_shot shot;

/* From the session's hooks (deferred-task time). */
void engine_screenshot_request(void);
void engine_set_view(int quality, int fps);

/* A name for the main loop to look up with the Mac's own resolver: the
 * engine asks, the main loop answers (ans 1 with ip, or -1). If the main loop
 * is held up -- a menu is open -- the engine falls back on its own DNS. */
typedef struct {
    volatile int req, ans;
    char name[64];
    volatile unsigned long ip;
} engine_name;
extern engine_name name_q;

/* Two sessions at once: a desktop and a file manager (a client opens the
 * latter on a connection of its own). The engine accepts every connection
 * into a free slot and runs it until it logs in; a desktop stays with the
 * engine, a file manager is handed to the main loop (owner O_MAIN), which
 * may use the File Manager. When the main loop is finished with one it sets
 * O_DONE and the engine frees the slot. */
enum { O_ENGINE, O_MAIN, O_DONE };
enum { K_PENDING, K_DESKTOP, K_FILE };
typedef struct {
    cdv_tcp *t;              /* NULL: free */
    cdv_session sess;
    unsigned long since;
    volatile int kind, owner;
    cdv_hooks hooks;         /* the caller's, with this slot as user */
} engine_slot_t;
engine_slot_t *engine_slot(int i);
/* The desktop session's peer, for the window (NULL if none). */
const char *engine_peer_name(void);

/* Log lines the engine produced, for the main loop to show. Returns NULL
 * when there are none. */
const char *engine_next_log(void);
void engine_log(const char *msg);

#endif
