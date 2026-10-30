/* The settings, shared by the agent and the settings window: a text file of
 * key=value lines beside the program, C-Desk-Vint.cfg, readable only by
 * Administrators and SYSTEM where the file system keeps permissions (it
 * holds the password and this machine's private key).
 *
 * The agent writes C-Desk-Vint.sta beside it too: what it is doing, for the
 * settings window to show (prefs_status_*). */
#ifndef CDV_NT_PREFS_H
#define CDV_NT_PREFS_H

#include <stddef.h>
#include <stdint.h>

#ifndef DEFAULT_SERVER
#define DEFAULT_SERVER "rs-ny.rustdesk.com"
#define DEFAULT_KEY "OeVuKk5nlHiXp+APNn0Y3pC1Iwpwn44JGqrQCsWqmBw="
#endif
#ifndef DEFAULT_KEY
#define DEFAULT_KEY ""
#endif
#ifndef DEFAULT_API
#define DEFAULT_API ""
#endif
#define DEFAULT_PORT 21118
#define DEFAULT_Q 24

#define SERVICE_NAME "CDeskVint"
#define SERVICE_DISPLAY "C-Desk-Vint remote desktop"

typedef struct {
    char password[33];
    int port;             /* direct IP access; 0 off */
    int q;                /* VP8 quantiser, 0..127 */
    char id[16];
    uint8_t uuid[16], seed[32];
    int have_uuid, have_seed;
    char server[64];      /* ID server, host[:port]; "" for none */
    char key[64];
    char relay[64];
    char name[64];        /* what the peer sees; "" for the computer name */
    char files[256];      /* what the file manager sees as its root: "/" all drives, "off" none */
    char api[128];        /* a console's URL, for its device list; none by default */
    int lan;              /* answer LAN discovery */
} cdv_prefs;

extern cdv_prefs prefs;

/* The directory the program is in, with a trailing backslash. */
const char *prefs_dir(void);
/* path = prefs_dir() + name */
void prefs_path(char *out, size_t cap, const char *name);

/* Defaults, then the file. With `create`, a first run makes an ID, a
 * password and keys, and writes them; returns 0 if they could not be saved. */
int prefs_load(int create);
int prefs_save(void);
/* A fresh random password (8 characters nobody would guess). */
void prefs_new_password(char out[9]);

/* Entropy from everything the machine offers, into rng.c. */
void prefs_gather_entropy(void);

/* Status for the settings window: lines of key=value. */
void prefs_status_write(const char *text);
int prefs_status_read(char *out, size_t cap);

/* "123 456 789" */
void prefs_format_id(const char *id, char *out, size_t cap);

#endif
