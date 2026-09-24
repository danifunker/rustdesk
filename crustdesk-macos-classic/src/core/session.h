/* One RustDesk peer session, as a state machine with no I/O of its own.
 *
 * Classic Mac OS has no threads and no blocking sockets worth the name, so the
 * session never waits: the platform feeds it whatever bytes arrived, drains
 * whatever it queued, and asks it for a video buffer when the line is free.
 * The same file runs under a POSIX test harness on Linux.
 *
 * Two modes, chosen by how the peer arrived. Direct IP: no key exchange and
 * no encryption, the same as upstream's direct server. Through the ID server
 * (relay or local-address): signed_id, the peer's public_key, then every
 * message sealed in a secretbox -- rustdesk-ppc-agent's crypto.rs, in C over
 * a libsodium subset (third_party/libsodium-min).
 *
 * Buffers are the caller's and are never reallocated. `out` is split in two:
 * a small queue for control messages at the front, and room for one video
 * frame behind it. A frame can be encoded -- a few rows at a time, on the Mac
 * -- while keepalives and replies keep flowing, and messages never interleave
 * on the wire: whichever queue starts sending finishes before the other goes.
 * Bytes handed to the platform stay where they are until they are consumed,
 * which is what lets MacTCP send straight out of them.
 */
#ifndef SESSION_H
#define SESSION_H

#include <stddef.h>
#include <stdint.h>

/* What we tell the peer we are -- and the console, which shows it. A version is
 * a capability claim, not a label: the client decides which messages to send
 * from it. 1.4.5 matches the other vintage agents, whose handling of what it
 * implies has been checked against real clients (see
 * rustdesk-ppc-agent/src/session.rs, REPORTED_VERSION). */
#define REPORTED_VERSION "1.4.5"

/* RustDesk's ControlKey values the platform needs to name. */
enum {
    CK_ALT = 1, CK_BACKSPACE = 2, CK_CAPSLOCK = 3, CK_CONTROL = 4, CK_DELETE = 5,
    CK_DOWN = 6, CK_END = 7, CK_ESCAPE = 8, CK_F1 = 9, CK_F10 = 10, CK_F11 = 11,
    CK_F12 = 12, CK_F2 = 13, CK_F3 = 14, CK_F4 = 15, CK_F5 = 16, CK_F6 = 17,
    CK_F7 = 18, CK_F8 = 19, CK_F9 = 20, CK_HOME = 21, CK_LEFT = 22, CK_META = 23,
    CK_OPTION = 24, CK_PAGEDOWN = 25, CK_PAGEUP = 26, CK_RETURN = 27, CK_RIGHT = 28,
    CK_SHIFT = 29, CK_SPACE = 30, CK_TAB = 31, CK_UP = 32, CK_NUMPAD0 = 33,
    CK_NUMPAD9 = 42, CK_CLEAR = 44, CK_HELP = 59, CK_RWIN = 64, CK_MULTIPLY = 66,
    CK_ADD = 67, CK_SUBTRACT = 68, CK_DECIMAL = 69, CK_DIVIDE = 70, CK_EQUALS = 71,
    CK_NUMPAD_ENTER = 72, CK_RSHIFT = 73, CK_RCONTROL = 74, CK_RALT = 75
};

/* Modifier bits in cdv_key.mods. */
enum { MOD_SHIFT = 1, MOD_CONTROL = 2, MOD_OPTION = 4, MOD_COMMAND = 8, MOD_CAPS = 16 };

/* What a KeyEvent carried. */
enum { KEY_NONE, KEY_CONTROL, KEY_CHR, KEY_UNICODE, KEY_SEQ };
/* KeyboardMode: in MAP and TRANSLATE a KEY_CHR is a Mac virtual keycode,
 * because we tell the peer we are "Mac OS"; in LEGACY it is a character. */
enum { KMODE_LEGACY = 0, KMODE_MAP = 1, KMODE_TRANSLATE = 2, KMODE_AUTO = 3 };

typedef struct {
    int down, press;      /* press: a down and an up together */
    int kind;             /* KEY_* */
    uint32_t value;       /* ControlKey, chr, or unicode */
    const char *seq;      /* KEY_SEQ, UTF-8, not terminated */
    size_t seqlen;
    int mods;             /* MOD_* */
    int mode;             /* KMODE_* */
} cdv_key;

typedef struct {
    /* mask: low 3 bits 0 move, 1 down, 2 up, 3 wheel, 4 trackpad, 5 relative;
     * above them the button, 1 left, 2 right, 4 middle. */
    void (*mouse)(void *user, int mask, int x, int y);
    void (*key)(void *user, const cdv_key *k);
    void (*clipboard)(void *user, const char *utf8, size_t n);
    void (*log)(void *user, const char *msg);
    void *user;
} cdv_hooks;

typedef struct {
    const char *id;         /* this machine's RustDesk ID */
    const uint8_t *sign_sk; /* Ed25519 secret key (64 bytes), for signed_id */
    const char *password;   /* empty: refuse every login */
    const char *salt;
    const char *hostname;
    int width, height;
    int cursor_embedded;    /* the pointer is drawn into the captured picture */
} cdv_ident;

enum { CDV_WAIT_PK, CDV_WAIT_LOGIN, CDV_LIVE, CDV_CLOSED };

typedef struct {
    int state;
    const cdv_hooks *hooks;
    const cdv_ident *id;
    char challenge[8];
    int attempts;
    uint32_t rng;

    uint8_t *out;            /* control queue: out[0 .. ctlcap) */
    size_t ctlcap, ooff, olen;
    uint8_t *vid;            /* one video frame */
    size_t vidcap, voff, vlen;
    int vstate;              /* VID_FREE, VID_ENCODING, VID_READY */
    int sending;             /* which queue is mid-message: 0 none, 1 control, 2 video */

    uint8_t *in;
    size_t incap, ilen, skip;

    uint32_t now;             /* ms, as last told by cdv_tick */
    uint32_t delay_sent;
    int delay_outstanding;
    int refresh;              /* the peer asked for a keyframe */
    uint32_t pts;

    /* encryption, once the key exchange has happened */
    int secure, enc;
    uint8_t box_pk[32], box_sk[32], key[32];
    uint64_t send_seq, recv_seq;

    char peer_name[64];
    char peer_version[16];
    char peer_platform[16];
} cdv_session;

void cdv_init(cdv_session *s, uint8_t *out, size_t outcap, uint8_t *in, size_t incap,
              const cdv_hooks *hooks, const cdv_ident *id, uint32_t seed);

/* Queue the opening message. Call once the connection is up. `secure` for a
 * peer that came through the ID server: it will expect the key exchange. */
void cdv_start(cdv_session *s, uint32_t now_ms, int secure);

/* Bytes from the peer. Returns -1 once the session should be closed. */
int cdv_feed(cdv_session *s, const uint8_t *data, size_t n);

/* Timers: keepalive. Call every pass of the main loop. */
void cdv_tick(cdv_session *s, uint32_t now_ms);

/* The bytes waiting to go out, and how many of them the platform sent.
 * Always consume what was peeked before peeking something new. */
const uint8_t *cdv_out_peek(cdv_session *s, size_t *n);
void cdv_out_consume(cdv_session *s, size_t n);

/* Video. One frame at a time: a new one can begin once the last has been
 * sent, so a slow line paces the encoder instead of frames piling up. vp8
 * data is written at the returned pointer; commit or abort it. */
enum { VID_FREE, VID_ENCODING, VID_READY };
uint8_t *cdv_video_begin(cdv_session *s, size_t *cap);
void cdv_video_commit(cdv_session *s, size_t len, int key);
void cdv_video_abort(cdv_session *s);

/* The peer's refresh button: nonzero once, then cleared. */
int cdv_take_refresh(cdv_session *s);

/* Send a SwitchDisplay: the screen changed size or depth. */
void cdv_send_display(cdv_session *s, int w, int h);

/* Text the Mac's user copied, as UTF-8. Sent uncompressed, in whichever
 * message the peer's version reads. */
void cdv_send_clipboard(cdv_session *s, const char *utf8, size_t n);

/* The pointer, for when it is not part of the picture: its shape as RGBA
 * (w x h, at most 64 x 64) and where it is. */
void cdv_send_cursor(cdv_session *s, uint32_t id, int hotx, int hoty, int w, int h,
                     const uint8_t *rgba);
void cdv_send_cursor_pos(cdv_session *s, int x, int y);

/* Close with a reason the client displays. */
void cdv_close(cdv_session *s, const char *reason);

#endif
