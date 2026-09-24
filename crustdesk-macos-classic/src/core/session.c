#include "session.h"
#include "pb.h"
#include "rng.h"
#include "sha256.h"

#include "sodium/crypto_box.h"
#include "sodium/crypto_secretbox.h"
#include "sodium/crypto_sign_ed25519.h"

#include <string.h>

/* Message fields, from message.proto. */
enum {
    M_TEST_DELAY = 5, M_VIDEO_FRAME = 6, M_LOGIN_REQUEST = 7, M_LOGIN_RESPONSE = 8,
    M_SIGNED_ID = 3, M_PUBLIC_KEY = 4, M_HASH = 9, M_MOUSE_EVENT = 10, M_CURSOR_DATA = 12, M_CURSOR_POSITION = 13,
    M_KEY_EVENT = 15, M_CLIPBOARD = 16, M_MISC = 19, M_MULTI_CLIPBOARDS = 28,
    M_SCREENSHOT_REQUEST = 29, M_SCREENSHOT_RESPONSE = 30
};

#define KEEPALIVE_MS 3000
#define KEEPALIVE_STALE_MS 10000
#define MAX_LOGIN_ATTEMPTS 10
#define VIDEO_PREFIX 32 /* room for the headers written in front of a frame */
#define VIDEO_SUFFIX 32 /* key and pts after it, and the MAC sealing adds */
#define MAC 16          /* crypto_secretbox_MACBYTES */

static void say(cdv_session *s, const char *msg)
{
    if (s->hooks->log)
        s->hooks->log(s->hooks->user, msg);
}

/* No printf: on the Mac this runs at deferred-task time, on whatever stack
 * was interrupted, and newlib's formatter wants more of it than is polite. */
static void say3(cdv_session *s, const char *a, const char *b, const char *c)
{
    char line[96];
    size_t n = 0;
    const char *parts[3];
    int i;
    parts[0] = a;
    parts[1] = b;
    parts[2] = c;
    for (i = 0; i < 3; i++) {
        const char *p = parts[i];
        while (p && *p && n < sizeof line - 1)
            line[n++] = *p++;
    }
    line[n] = 0;
    say(s, line);
}

static uint32_t rnd(cdv_session *s)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}

void cdv_init(cdv_session *s, uint8_t *out, size_t outcap, uint8_t *in, size_t incap,
              const cdv_hooks *hooks, const cdv_ident *id, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->hooks = hooks;
    s->id = id;
    /* A sixteenth of the space, at least 4 KB, for control messages. */
    s->ctlcap = outcap / 16 < 4096 ? 4096 : outcap / 16;
    if (s->ctlcap > outcap / 2)
        s->ctlcap = outcap / 2;
    s->out = out;
    s->vid = out + s->ctlcap;
    s->vidcap = outcap - s->ctlcap;
    s->in = in;
    s->incap = incap;
    s->rng = seed ? seed : 0x2545F491u;
    s->state = CDV_WAIT_LOGIN;
}

/* ---- framing (BytesCodec): length << 2 | (header bytes - 1), little-endian -- */

static size_t frame_header(uint8_t *h, size_t n)
{
    uint32_t v = (uint32_t)n << 2;
    size_t hl = n <= 0x3F ? 1 : n <= 0x3FFF ? 2 : n <= 0x3FFFFF ? 3 : 4, i;
    v |= (uint32_t)(hl - 1);
    for (i = 0; i < hl; i++)
        h[i] = (uint8_t)(v >> (8 * i));
    return hl;
}

/* ---- outbound -------------------------------------------------------------- */

/* Messages are built 4 bytes past the tail, then the frame header is slid in
 * front. Returns 0 if there was no room, and queues nothing. */
static int msg_begin(cdv_session *s, pbw *w)
{
    if (s->state == CDV_CLOSED || s->ctlcap - s->olen < 8 + MAC)
        return 0;
    pbw_init(w, s->out + s->olen + 4, s->ctlcap - s->olen - 4 - MAC);
    return 1;
}

/* Section: crypto.rs's SecureChannel. The nonce is the message's sequence
 * number, starting at 1, little-endian in the first 8 of 24 bytes -- whatever
 * the host's byte order. Sealed in place: MAC first, then the ciphertext. */
static void seq_nonce(uint64_t seq, uint8_t n[24])
{
    int i;
    memset(n, 0, 24);
    for (i = 0; i < 8; i++)
        n[i] = (uint8_t)(seq >> (8 * i));
}

static size_t seal(cdv_session *s, uint8_t *p, size_t n)
{
    uint8_t nonce[24];
    seq_nonce(++s->send_seq, nonce);
    crypto_secretbox_easy(p, p, n, nonce, s->key);
    return n + MAC;
}

/* Queued with the frame header it will have once sealed, and room for the
 * MAC after it; sealed by seal_queued() when it is about to go. */
static int msg_end(cdv_session *s, pbw *w)
{
    uint8_t *body = s->out + s->olen + 4, h[4];
    size_t n = pbw_len(w, body), hl, framed;
    if (w->overflow) {
        say(s, "outbound queue full; message dropped");
        return 0;
    }
    framed = s->enc ? n + MAC : n;
    hl = frame_header(h, framed);
    if (hl < 4)
        memmove(s->out + s->olen + hl, body, n);
    memcpy(s->out + s->olen, h, hl);
    s->olen += hl + framed;
    if (!s->enc)
        s->osealed = s->olen; /* nothing to seal: ready as it is */
    return 1;
}

/* Seal the control messages queued since the last call, in queue order. */
static void seal_queued(cdv_session *s)
{
    while (s->osealed < s->olen) {
        uint8_t *m = s->out + s->osealed;
        size_t hl = (size_t)(m[0] & 3) + 1, len = 0, i;
        for (i = 0; i < hl; i++)
            len |= (size_t)m[i] << (8 * i);
        len >>= 2;
        seal(s, m + hl, len - MAC);
        s->osealed += hl + len;
    }
}

/* Peeking claims the queue it returns: bytes handed to the platform must be
 * consumed from the same queue, even if the other fills up meanwhile. */
const uint8_t *cdv_out_peek(cdv_session *s, size_t *n)
{
    int src;
    if (!s->sending)
        s->sending = s->olen > s->ooff ? 1 : s->vstate == VID_READY ? 2 : 0;
    src = s->sending;
    if (src == 1) {
        seal_queued(s);
        *n = s->olen - s->ooff;
        return s->out + s->ooff;
    }
    if (src == 2) {
        if (s->vneedseal) {
            seal(s, s->vid + s->vmsg, s->vmsglen);
            s->vneedseal = 0;
        }
        *n = s->vlen - s->voff;
        return s->vid + s->voff;
    }
    *n = 0;
    return s->out;
}

void cdv_out_consume(cdv_session *s, size_t n)
{
    if (!n || !s->sending)
        return;
    if (s->sending == 1) {
        s->ooff += n;
        if (s->ooff >= s->olen) {
            s->ooff = s->olen = s->osealed = 0;
            s->sending = 0;
        }
    } else {
        s->voff += n;
        if (s->voff >= s->vlen) {
            s->voff = s->vlen = 0;
            s->vstate = VID_FREE;
            s->sending = 0;
        }
    }
}

static void send_login_error(cdv_session *s, const char *err)
{
    pbw w;
    if (!msg_begin(s, &w))
        return;
    pbw_begin(&w, M_LOGIN_RESPONSE);
    pbw_string(&w, 1, err);
    pbw_end(&w);
    msg_end(s, &w);
}

static void send_peer_info(cdv_session *s)
{
    const cdv_ident *id = s->id;
    pbw w;
    if (!msg_begin(s, &w))
        return;
    pbw_begin(&w, M_LOGIN_RESPONSE);
    pbw_begin(&w, 2); /* peer_info */
    pbw_string(&w, 2, id->hostname);
    /* "Mac OS" is true, and it is what makes a client in Map mode send Mac
     * virtual keycodes -- which are the ADB keycodes this machine uses. */
    pbw_string(&w, 3, "Mac OS");
    pbw_begin(&w, 4); /* displays */
    pbw_varint(&w, 3, (uint32_t)id->width);
    pbw_varint(&w, 4, (uint32_t)id->height);
    pbw_string(&w, 5, "Display");
    pbw_bool(&w, 6, 1);
    pbw_bool(&w, 7, id->cursor_embedded);
    pbw_end(&w);
    pbw_string(&w, 7, REPORTED_VERSION);
    pbw_end(&w);
    pbw_end(&w);
    msg_end(s, &w);
}

static void send_test_delay(cdv_session *s, uint64_t t, int from_client)
{
    pbw w;
    if (!msg_begin(s, &w))
        return;
    pbw_begin(&w, M_TEST_DELAY);
    pbw_varint(&w, 1, t);
    pbw_bool(&w, 2, from_client);
    pbw_end(&w);
    msg_end(s, &w);
}

void cdv_send_display(cdv_session *s, int wd, int ht)
{
    pbw w;
    if (s->state != CDV_LIVE || !msg_begin(s, &w))
        return;
    pbw_begin(&w, M_MISC);
    pbw_begin(&w, 5); /* switch_display */
    pbw_varint(&w, 4, (uint32_t)wd);
    pbw_varint(&w, 5, (uint32_t)ht);
    pbw_bool(&w, 6, s->id->cursor_embedded);
    pbw_end(&w);
    pbw_end(&w);
    msg_end(s, &w);
}

/* CursorData.colors is not raw pixels on the wire: the client runs it through
 * zstd. A zstd frame may be made of Raw blocks, which hold literal bytes, so a
 * valid frame needs no compressor (RFC 8878 3.1; rustdesk-ppc-agent's
 * zstd_frame.rs has the story of finding that out). Single segment, content
 * size given, no checksum. */
static size_t zstd_raw(uint8_t *out, const uint8_t *data, size_t n)
{
    size_t o = 0, off = 0;
    out[o++] = 0x28; /* magic 0xFD2FB528, little-endian */
    out[o++] = 0xB5;
    out[o++] = 0x2F;
    out[o++] = 0xFD;
    if (n < 256) {
        out[o++] = 0x20;
        out[o++] = (uint8_t)n;
    } else {
        out[o++] = 0x60; /* two-byte size, stored minus 256 */
        out[o++] = (uint8_t)(n - 256);
        out[o++] = (uint8_t)((n - 256) >> 8);
    }
    do {
        size_t len = n - off > 65536 ? 65536 : n - off;
        uint32_t h = (uint32_t)len << 3 | (off + len == n); /* Raw block, last? */
        out[o++] = (uint8_t)h;
        out[o++] = (uint8_t)(h >> 8);
        out[o++] = (uint8_t)(h >> 16);
        memcpy(out + o, data + off, len);
        o += len;
        off += len;
    } while (off < n);
    return o;
}

void cdv_send_cursor(cdv_session *s, uint32_t id, int hotx, int hoty, int w, int h,
                     const uint8_t *rgba)
{
    static uint8_t z[64 * 64 * 4 + 32];
    size_t zn;
    pbw m;
    if (s->state != CDV_LIVE || w <= 0 || h <= 0 || w > 64 || h > 64)
        return;
    zn = zstd_raw(z, rgba, (size_t)w * h * 4);
    if (!msg_begin(s, &m))
        return;
    pbw_begin(&m, M_CURSOR_DATA);
    pbw_varint(&m, 1, id);
    pbw_sint(&m, 2, hotx);
    pbw_sint(&m, 3, hoty);
    pbw_varint(&m, 4, (uint32_t)w);
    pbw_varint(&m, 5, (uint32_t)h);
    pbw_bytes(&m, 6, z, zn);
    pbw_end(&m);
    msg_end(s, &m);
}

void cdv_send_cursor_pos(cdv_session *s, int x, int y)
{
    pbw m;
    if (s->state != CDV_LIVE || !msg_begin(s, &m))
        return;
    pbw_begin(&m, M_CURSOR_POSITION);
    pbw_sint(&m, 1, x);
    pbw_sint(&m, 2, y);
    pbw_end(&m);
    msg_end(s, &m);
}

/* "1.3.0" -> 10300: enough to compare versions. */
static long version_number(const char *v)
{
    long n = 0, part = 0;
    int parts = 0;
    for (;; v++) {
        if (*v >= '0' && *v <= '9') {
            part = part * 10 + (*v - '0');
        } else {
            n = n * 100 + part;
            part = 0;
            parts++;
            if (*v != '.' || parts == 3)
                break;
        }
    }
    while (parts++ < 3)
        n *= 100;
    return n;
}

void cdv_send_clipboard(cdv_session *s, const char *utf8, size_t n)
{
    /* MultiClipboards from 1.3.0, except to iOS, which never took it (the
     * rule rustdesk-ppc-agent's clipboard.rs arrived at). */
    int multi = version_number(s->peer_version) >= version_number("1.3.0") &&
                s->peer_platform[0] && strcmp(s->peer_platform, "iOS");
    pbw m;
    if (s->state != CDV_LIVE || !n || !msg_begin(s, &m))
        return;
    if (multi) {
        pbw_begin(&m, M_MULTI_CLIPBOARDS);
        pbw_begin(&m, 1);
    } else {
        pbw_begin(&m, M_CLIPBOARD);
    }
    pbw_bytes(&m, 2, utf8, n); /* content; compress false, format Text: defaults */
    pbw_end(&m);
    if (multi)
        pbw_end(&m);
    msg_end(s, &m);
}

void cdv_close(cdv_session *s, const char *reason)
{
    pbw w;
    if (s->state == CDV_CLOSED)
        return;
    if (msg_begin(s, &w)) {
        pbw_begin(&w, M_MISC);
        pbw_string(&w, 9, reason); /* close_reason */
        pbw_end(&w);
        msg_end(s, &w);
    }
    s->state = CDV_CLOSED;
}

static void send_hash(cdv_session *s)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    pbw w;
    int i;
    for (i = 0; i < 6; i++)
        s->challenge[i] = alphabet[rnd(s) % (sizeof alphabet - 1)];
    s->challenge[6] = 0;
    if (!msg_begin(s, &w))
        return;
    pbw_begin(&w, M_HASH);
    pbw_string(&w, 1, s->id->salt);
    pbw_string(&w, 2, s->challenge);
    pbw_end(&w);
    msg_end(s, &w);
}

/* crypto.rs's Handshake::signed_id: IdPk{id, pk: a fresh box public key},
 * signed (combined mode) with the machine's Ed25519 key. */
static void send_signed_id(cdv_session *s)
{
    uint8_t idpk[96], signed_[96 + 64];
    unsigned long long sl;
    pbw w;
    size_t n;
    crypto_box_keypair(s->box_pk, s->box_sk);
    pbw_init(&w, idpk, sizeof idpk);
    pbw_string(&w, 1, s->id->id);
    pbw_bytes(&w, 2, s->box_pk, 32);
    n = pbw_len(&w, idpk);
    crypto_sign_ed25519(signed_, &sl, idpk, n, s->id->sign_sk);
    if (!msg_begin(s, &w))
        return;
    pbw_begin(&w, M_SIGNED_ID);
    pbw_bytes(&w, 1, signed_, (size_t)sl);
    pbw_end(&w);
    msg_end(s, &w);
}

void cdv_start(cdv_session *s, uint32_t now_ms, int secure)
{
    s->now = s->delay_sent = now_ms;
    s->secure = secure && s->id->sign_sk && s->id->id;
    if (s->secure) {
        send_signed_id(s);
        s->state = CDV_WAIT_PK;
    } else {
        send_hash(s);
    }
}

/* The peer's answer to signed_id: public_key{asymmetric_value: its box
 * public key, symmetric_value: the session key, boxed to ours with a zero
 * nonce}. Empty -- or an empty message -- means it declines, and upstream
 * carries on unencrypted; so do we. */
static void public_key(cdv_session *s, const uint8_t *b, size_t n)
{
    const uint8_t *their = NULL, *boxed = NULL;
    size_t tl = 0, bl = 0;
    uint8_t nonce[24] = { 0 };
    pbr r;
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if (r.wire != PB_LEN)
            continue;
        if (r.field == 1) {
            their = r.data;
            tl = r.len;
        } else if (r.field == 2) {
            boxed = r.data;
            bl = r.len;
        }
    }
    if (!tl) {
        say(s, "peer declined encryption; continuing unencrypted");
    } else if (tl != 32 || bl != 32 + MAC ||
               crypto_box_open_easy(s->key, boxed, bl, nonce, their, s->box_sk) != 0) {
        say(s, "key exchange failed");
        s->state = CDV_CLOSED;
        return;
    } else {
        s->enc = 1;
        s->osealed = s->olen; /* what is queued already goes as it is */
        say(s, "session encrypted");
    }
    s->state = CDV_WAIT_LOGIN;
    send_hash(s);
}

/* ---- video ----------------------------------------------------------------- */

uint8_t *cdv_video_begin(cdv_session *s, size_t *cap)
{
    if (s->state != CDV_LIVE || s->vstate != VID_FREE || s->vidcap < VIDEO_PREFIX + VIDEO_SUFFIX + 64)
        return NULL;
    s->vstate = VID_ENCODING;
    *cap = s->vidcap - VIDEO_PREFIX - VIDEO_SUFFIX;
    return s->vid + VIDEO_PREFIX;
}

void cdv_video_abort(cdv_session *s)
{
    if (s->vstate == VID_ENCODING)
        s->vstate = VID_FREE;
}

/* Message{video_frame: VideoFrame{vp8s: VP9s{frames: [VP9{data, key, pts}]}}},
 * with every header computed from the data's length and written in front of
 * it, so a 100 KB frame is never copied. The gap left before the headers is
 * skipped by starting the queue past it. */
void cdv_video_commit(cdv_session *s, size_t len, int key)
{
    uint8_t *data = s->vid + VIDEO_PREFIX, *p, pre[VIDEO_PREFIX], *msg, h[4];
    size_t suffix, l3, l2, l1, l0, n = 0, hl, total;
    p = data + len;
    if (key) {
        *p++ = 0x10; /* key = 2, varint */
        *p++ = 1;
    }
    s->pts += 1;
    *p++ = 0x18; /* pts = 3, varint */
    p += pb_put_varint(p, s->pts);
    suffix = (size_t)(p - (data + len));

    l3 = 1 + pb_varint_size(len) + len + suffix;      /* VP9 */
    l2 = 1 + pb_varint_size(l3) + l3;                 /* VP9s */
    l1 = 1 + pb_varint_size(l2) + l2;                 /* VideoFrame */
    l0 = 1 + pb_varint_size(l1) + l1;                 /* Message */

    pre[n++] = (M_VIDEO_FRAME << 3) | PB_LEN;
    n += pb_put_varint(pre + n, l1);
    pre[n++] = (12 << 3) | PB_LEN; /* vp8s: field 12, not 6, which is VP9 */
    n += pb_put_varint(pre + n, l2);
    pre[n++] = (1 << 3) | PB_LEN; /* frames */
    n += pb_put_varint(pre + n, l3);
    pre[n++] = (1 << 3) | PB_LEN; /* data */
    n += pb_put_varint(pre + n, len);

    msg = data - n;
    memcpy(msg, pre, n);
    /* Sealed when it is claimed for sending (cdv_out_peek), not now. */
    total = s->enc ? l0 + MAC : l0;
    s->vmsg = (size_t)(msg - s->vid);
    s->vmsglen = l0;
    s->vneedseal = s->enc;
    hl = frame_header(h, total);
    memcpy(msg - hl, h, hl);
    s->voff = (size_t)(msg - hl - s->vid);
    s->vlen = (size_t)(msg + total - s->vid);
    s->vstate = VID_READY;
}

int cdv_take_refresh(cdv_session *s)
{
    int r = s->refresh;
    s->refresh = 0;
    return r;
}

/* ---- inbound ---------------------------------------------------------------- */

static void option(cdv_session *s, const uint8_t *b, size_t n);

static void login(cdv_session *s, const uint8_t *b, size_t n)
{
    pbr r;
    const uint8_t *pw = NULL, *login_option = NULL;
    size_t pwlen = 0, login_option_len = 0;

    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if (r.wire != PB_LEN)
            continue;
        if (r.field == 2) {
            pw = r.data;
            pwlen = r.len;
        } else if (r.field == 6) {
            login_option = r.data;
            login_option_len = r.len;
        } else if (r.field == 5 || r.field == 11 || r.field == 13) {
            char *dst = r.field == 5 ? s->peer_name : r.field == 11 ? s->peer_version : s->peer_platform;
            size_t cap = r.field == 5    ? sizeof s->peer_name
                         : r.field == 11 ? sizeof s->peer_version
                                         : sizeof s->peer_platform;
            size_t k = r.len < cap - 1 ? r.len : cap - 1;
            memcpy(dst, r.data, k);
            dst[k] = 0;
        }
    }

    if (!s->id->password[0]) {
        send_login_error(s, "No Password Access");
        s->state = CDV_CLOSED;
        return;
    }
    if (++s->attempts > MAX_LOGIN_ATTEMPTS) {
        say(s, "too many login attempts");
        s->state = CDV_CLOSED;
        return;
    }
    if (!pwlen) {
        /* A client probes with no password to learn whether one is needed;
         * this is what makes it ask. The answer comes on this connection. */
        send_login_error(s, "Empty Password");
        return;
    }
    {
        sha256_ctx c;
        uint8_t h1[32], h2[32], diff = 0;
        size_t i;
        sha256_init(&c);
        sha256_update(&c, s->id->password, strlen(s->id->password));
        sha256_update(&c, s->id->salt, strlen(s->id->salt));
        sha256_final(&c, h1);
        sha256_init(&c);
        sha256_update(&c, h1, 32);
        sha256_update(&c, s->challenge, strlen(s->challenge));
        sha256_final(&c, h2);
        if (pwlen != 32) {
            send_login_error(s, "Wrong Password");
            return;
        }
        for (i = 0; i < 32; i++)
            diff |= (uint8_t)(h2[i] ^ pw[i]);
        if (diff) {
            say3(s, "wrong password from '", s->peer_name, "'");
            send_login_error(s, "Wrong Password");
            return;
        }
    }
    send_peer_info(s);
    s->state = CDV_LIVE;
    s->refresh = 1;
    if (login_option)
        option(s, login_option, login_option_len);
    say3(s, s->peer_name, " logged in, client ",
         s->peer_version[0] ? s->peer_version : "unknown");
}

static int modifier_bit(uint64_t ck)
{
    switch (ck) {
    case CK_SHIFT: case CK_RSHIFT: return MOD_SHIFT;
    case CK_CONTROL: case CK_RCONTROL: return MOD_CONTROL;
    case CK_ALT: case CK_RALT: case CK_OPTION: return MOD_OPTION;
    case CK_META: case CK_RWIN: return MOD_COMMAND;
    case CK_CAPSLOCK: return MOD_CAPS;
    default: return 0;
    }
}

static void key_event(cdv_session *s, const uint8_t *b, size_t n)
{
    cdv_key k;
    pbr r;
    memset(&k, 0, sizeof k);
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        switch (r.field) {
        case 1: k.down = r.v != 0; break;
        case 2: k.press = r.v != 0; break;
        case 3: k.kind = KEY_CONTROL; k.value = (uint32_t)r.v; break;
        case 4: k.kind = KEY_CHR; k.value = (uint32_t)r.v; break;
        case 5: k.kind = KEY_UNICODE; k.value = (uint32_t)r.v; break;
        case 6:
            if (r.wire == PB_LEN) {
                k.kind = KEY_SEQ;
                k.seq = (const char *)r.data;
                k.seqlen = r.len;
            }
            break;
        case 8: /* repeated enum: packed by default, but accept either */
            if (r.wire == PB_LEN) {
                pbr p;
                uint64_t v;
                pbr_init(&p, r.data, r.len);
                /* a packed run is bare varints; read them as field 0 */
                while (p.p < p.end) {
                    const uint8_t *q = p.p;
                    int shift = 0;
                    v = 0;
                    while (q < p.end) {
                        v |= (uint64_t)(*q & 0x7f) << shift;
                        shift += 7;
                        if (!(*q++ & 0x80))
                            break;
                    }
                    p.p = q;
                    k.mods |= modifier_bit(v);
                }
            } else {
                k.mods |= modifier_bit(r.v);
            }
            break;
        case 9: k.mode = (int)r.v; break;
        }
    }
    if (s->hooks->key)
        s->hooks->key(s->hooks->user, &k);
}

static void mouse_event(cdv_session *s, const uint8_t *b, size_t n)
{
    int mask = 0, x = 0, y = 0;
    pbr r;
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if (r.field == 1)
            mask = (int)r.v;
        else if (r.field == 2)
            x = pb_unzigzag32(r.v);
        else if (r.field == 3)
            y = pb_unzigzag32(r.v);
    }
    if (s->hooks->mouse)
        s->hooks->mouse(s->hooks->user, mask, x, y);
}

/* OptionMessage: image_quality (2 Low, 3 Balanced, 4 Best), a custom
 * quality in percent, and frames a second. */
static void option(cdv_session *s, const uint8_t *b, size_t n)
{
    pbr r;
    int quality = -1, fps = 0;
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if (r.field == 1 && r.wire == PB_VARINT) {
            if (r.v == 2)
                quality = 40;
            else if (r.v == 3)
                quality = 16;
            else if (r.v == 4)
                quality = 4;
        } else if (r.field == 6 && r.wire == PB_VARINT) {
            /* Newer clients pack a percentage in the low byte. */
            int pct = (int)(r.v & 0xFF);
            if (pct > 0 && pct <= 100)
                quality = 60 - pct * 58 / 100; /* 10% -> 55, 100% -> 2 */
        } else if (r.field == 11 && r.wire == PB_VARINT && r.v > 0 && r.v <= 120) {
            fps = (int)r.v;
        }
    }
    if ((quality >= 0 || fps) && s->hooks->option)
        s->hooks->option(s->hooks->user, quality, fps);
}

static void misc(cdv_session *s, const uint8_t *b, size_t n)
{
    pbr r;
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if ((r.field == 10 && r.v) || r.field == 31) {
            s->refresh = 1;
        } else if (r.field == 4 && r.wire == PB_LEN) { /* chat_message */
            pbr c;
            pbr_init(&c, r.data, r.len);
            while (pbr_next(&c))
                if (c.field == 1 && c.wire == PB_LEN && s->hooks->chat)
                    s->hooks->chat(s->hooks->user, (const char *)c.data, c.len);
        } else if (r.field == 7 && r.wire == PB_LEN) {
            option(s, r.data, r.len);
        } else if (r.field == 14 && r.wire == PB_VARINT && r.v) { /* restart_remote_device */
            if (s->hooks->restart)
                s->hooks->restart(s->hooks->user);
        }
    }
}

static void test_delay(cdv_session *s, const uint8_t *b, size_t n)
{
    uint64_t t = 0;
    int from_client = 0;
    pbr r;
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if (r.field == 1)
            t = r.v;
        else if (r.field == 2)
            from_client = r.v != 0;
    }
    if (from_client)
        send_test_delay(s, t, 1);
    else
        s->delay_outstanding = 0;
}

static void clipboard(cdv_session *s, const uint8_t *b, size_t n)
{
    const uint8_t *text = NULL;
    size_t len = 0;
    int compressed = 0, format = 0;
    pbr r;
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if (r.field == 1)
            compressed = r.v != 0;
        else if (r.field == 2 && r.wire == PB_LEN) {
            text = r.data;
            len = r.len;
        } else if (r.field == 5)
            format = (int)r.v;
    }
    /* Compressed text is real zstd, which this agent does not carry a decoder
     * for yet; a client compresses whenever that is smaller, which for text
     * is all but the shortest. */
    if (compressed || format != 0 || !text) {
        say(s, "peer clipboard skipped (compressed or not text)");
        return;
    }
    if (s->hooks->clipboard)
        s->hooks->clipboard(s->hooks->user, (const char *)text, len);
}

static void screenshot(cdv_session *s, const uint8_t *b, size_t n)
{
    pbw w;
    pbr r;
    pbr_init(&r, b, n);
    if (s->hooks->screenshot) {
        s->shot_sid_len = 0;
        while (pbr_next(&r))
            if (r.field == 2 && r.wire == PB_LEN && r.len <= sizeof s->shot_sid) {
                memcpy(s->shot_sid, r.data, r.len);
                s->shot_sid_len = r.len;
            }
        s->hooks->screenshot(s->hooks->user);
        return;
    }
    if (!msg_begin(s, &w))
        return;
    pbw_begin(&w, M_SCREENSHOT_RESPONSE);
    while (pbr_next(&r))
        if (r.field == 2 && r.wire == PB_LEN)
            pbw_bytes(&w, 1, r.data, r.len); /* sid comes back as it came */
    pbw_string(&w, 2, "Screenshots are not supported by this agent yet");
    pbw_end(&w);
    msg_end(s, &w);
}

static void dispatch(cdv_session *s, const uint8_t *b, size_t n)
{
    pbr r;
    if (s->state == CDV_WAIT_PK) {
        pbr_init(&r, b, n);
        while (pbr_next(&r))
            if (r.field == M_PUBLIC_KEY && r.wire == PB_LEN) {
                public_key(s, r.data, r.len);
                return;
            }
        public_key(s, NULL, 0); /* an empty message: no key for us */
        return;
    }
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if (r.wire != PB_LEN)
            continue;
        if (s->state == CDV_WAIT_LOGIN) {
            if (r.field == M_LOGIN_REQUEST)
                login(s, r.data, r.len);
            continue; /* nothing else is allowed before login */
        }
        switch (r.field) {
        case M_MOUSE_EVENT: mouse_event(s, r.data, r.len); break;
        case M_KEY_EVENT: key_event(s, r.data, r.len); break;
        case M_TEST_DELAY: test_delay(s, r.data, r.len); break;
        case M_MISC: misc(s, r.data, r.len); break;
        case M_CLIPBOARD: clipboard(s, r.data, r.len); break;
        case M_MULTI_CLIPBOARDS: {
            pbr mc;
            pbr_init(&mc, r.data, r.len);
            while (pbr_next(&mc))
                if (mc.field == 1 && mc.wire == PB_LEN) {
                    clipboard(s, mc.data, mc.len);
                    break;
                }
            break;
        }
        case M_SCREENSHOT_REQUEST: screenshot(s, r.data, r.len); break;
        default: break;
        }
    }
}

int cdv_feed(cdv_session *s, const uint8_t *data, size_t n)
{
    while (n && s->state != CDV_CLOSED) {
        size_t take, hl, len, i;

        if (s->skip) { /* the rest of a frame too big to hold */
            take = n < s->skip ? n : s->skip;
            s->skip -= take;
            data += take;
            n -= take;
            continue;
        }
        take = s->incap - s->ilen;
        if (take > n)
            take = n;
        memcpy(s->in + s->ilen, data, take);
        s->ilen += take;
        data += take;
        n -= take;

        /* every complete frame in the buffer */
        for (;;) {
            uint32_t v = 0;
            if (!s->ilen)
                break;
            hl = (size_t)(s->in[0] & 3) + 1;
            if (s->ilen < hl)
                break;
            for (i = 0; i < hl; i++)
                v |= (uint32_t)s->in[i] << (8 * i);
            len = v >> 2;
            if (hl + len > s->incap) {
                /* Larger than we will ever hold: a big clipboard, say. Drop it. */
                size_t have = s->ilen - hl;
                say(s, "dropping an oversized message");
                s->skip = len - have;
                s->ilen = 0;
                break;
            }
            if (s->ilen < hl + len)
                break;
            if (s->enc) {
                uint8_t nonce[24];
                seq_nonce(++s->recv_seq, nonce);
                if (len < MAC || crypto_secretbox_open_easy(s->in + hl, s->in + hl, len, nonce,
                                                            s->key) != 0) {
                    say(s, "a message failed to decrypt; closing");
                    s->state = CDV_CLOSED;
                    return -1;
                }
                dispatch(s, s->in + hl, len - MAC);
            } else {
                dispatch(s, s->in + hl, len);
            }
            memmove(s->in, s->in + hl + len, s->ilen - hl - len);
            s->ilen -= hl + len;
        }
    }
    return s->state == CDV_CLOSED ? -1 : 0;
}

void cdv_tick(cdv_session *s, uint32_t now_ms)
{
    uint32_t waited;
    s->now = now_ms;
    if (s->state != CDV_LIVE)
        return;
    /* Liveness: without traffic the client gives up on a still screen. */
    waited = now_ms - s->delay_sent;
    if (waited >= KEEPALIVE_MS && (!s->delay_outstanding || waited >= KEEPALIVE_STALE_MS)) {
        send_test_delay(s, now_ms, 0);
        s->delay_sent = now_ms;
        s->delay_outstanding = 1;
    }
}

void cdv_send_chat(cdv_session *s, const char *utf8, size_t n)
{
    pbw w;
    if (s->state != CDV_LIVE || !msg_begin(s, &w))
        return;
    pbw_begin(&w, M_MISC);
    pbw_begin(&w, 4); /* chat_message */
    pbw_bytes(&w, 1, (const uint8_t *)utf8, n);
    pbw_end(&w);
    pbw_end(&w);
    msg_end(s, &w);
}

#define BIG_SUFFIX 160 /* the sid and a message after the data, and the MAC */

uint8_t *cdv_big_begin(cdv_session *s, size_t *cap)
{
    if (s->state != CDV_LIVE || s->vstate != VID_FREE || s->vidcap < VIDEO_PREFIX + BIG_SUFFIX + 64)
        return NULL;
    s->vstate = VID_ENCODING;
    *cap = s->vidcap - VIDEO_PREFIX - BIG_SUFFIX;
    return s->vid + VIDEO_PREFIX;
}

/* Message{screenshot_response(30): {data(3), sid(1), msg(2)}}, built around
 * the data where it lies. Fields may come in any order; data goes first so
 * its headers fit in front of it. */
void cdv_screenshot_commit(cdv_session *s, size_t len, const char *err)
{
    uint8_t *data = s->vid + VIDEO_PREFIX, *p = data + len, pre[VIDEO_PREFIX], *msg, h[4];
    size_t inner, l0, n = 0, hl, total, el = err ? strlen(err) : 0;
    if (el > 96)
        el = 96;
    if (s->shot_sid_len) {
        *p++ = (1 << 3) | PB_LEN;
        p += pb_put_varint(p, s->shot_sid_len);
        memcpy(p, s->shot_sid, s->shot_sid_len);
        p += s->shot_sid_len;
    }
    if (el) {
        *p++ = (2 << 3) | PB_LEN;
        p += pb_put_varint(p, el);
        memcpy(p, err, el);
        p += el;
    }
    inner = (size_t)(p - data) + 1 + pb_varint_size(len);
    pre[n++] = (uint8_t)(((M_SCREENSHOT_RESPONSE << 3) | PB_LEN) | 0x80); /* field 30: */
    pre[n++] = (uint8_t)(((M_SCREENSHOT_RESPONSE << 3) | PB_LEN) >> 7);  /* two bytes */
    n += pb_put_varint(pre + n, inner);
    pre[n++] = (3 << 3) | PB_LEN; /* data */
    n += pb_put_varint(pre + n, len);
    l0 = n + (size_t)(p - data);
    msg = data - n;
    memcpy(msg, pre, n);
    total = s->enc ? l0 + MAC : l0;
    s->vmsg = (size_t)(msg - s->vid);
    s->vmsglen = l0;
    s->vneedseal = s->enc;
    hl = frame_header(h, total);
    memcpy(msg - hl, h, hl);
    s->voff = (size_t)(msg - hl - s->vid);
    s->vlen = (size_t)(msg + total - s->vid);
    s->vstate = VID_READY;
}
