/* C-Desk-Vint in the Control Strip.
 *
 * The strip module (src/cstrip/) is a separate code resource, loaded by the
 * Control Strip in whatever application is in front; the agent is its own
 * process. They meet in a small block in the system heap, found through the
 * Gestalt selector 'CDVs': the app fills in its state, the module draws it
 * and leaves commands. The block outlives the app -- a module may ask after
 * it long after it quit -- so the app makes it once and never frees it, and
 * says when it is gone.
 *
 * The block begins with the Gestalt function itself (68k code the app
 * writes: it answers with the block's address), so there is nothing in the
 * application heap for the Gestalt Manager to call after the app has quit.
 */
#ifndef CDV_STRIP_H
#define CDV_STRIP_H

#include <Multiverse.h>
#include <stdint.h>

#define STRIP_SELECTOR 'CDVs'
#define STRIP_MAGIC 0x43445631UL /* "CDV1": this layout */

enum { STRIP_CMD_NONE, STRIP_CMD_START, STRIP_CMD_STOP, STRIP_CMD_SHOW };

#if defined(__powerpc__) || defined(__ppc__)
#pragma pack(push, 2)
#endif

typedef struct {
    uint16_t code[12];            /* the Gestalt function (see strip.c) */
    uint32_t magic;
    volatile uint8_t app_running; /* the agent is open */
    volatile uint8_t sharing;     /* ...and on the network */
    volatile uint8_t live;        /* ...with a peer connected */
    volatile uint8_t registered;  /* ...and known to the ID server */
    volatile uint8_t listed;      /* ...and to the console */
    volatile uint8_t cmd;         /* STRIP_CMD_*, from the module; the app clears it */
    volatile uint16_t changes;    /* bumped whenever any of the above changes */
    char id[16];
    ProcessSerialNumber psn;      /* to bring the agent to the front */
    FSSpec app;                   /* to open it again after it has quit */
} cdv_strip;

#if defined(__powerpc__) || defined(__ppc__)
#pragma pack(pop)
#endif

/* The app's side. */
void strip_publish(void);                   /* at start: make or find the block */
void strip_update(int sharing, int live, int registered, int listed, const char *id);
int strip_command(void);                    /* STRIP_CMD_*, taken */
void strip_quit(void);                      /* the app is going */

#endif
