#include "session.h"
#include "pb.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

/* What we tell the peer we are. A version is a capability claim, not a label:
 * the client decides which messages to send from it. 1.4.5 matches the other
 * vintage agents, whose handling of what it implies has been checked against
 * real clients (see rustdesk-ppc-agent/src/session.rs, REPORTED_VERSION). */
#define REPORTED_VERSION "1.4.5"

/* Message fields, from message.proto. */
enum {
    M_TEST_DELAY = 5, M_VIDEO_FRAME = 6, M_LOGIN_REQUEST = 7, M_LOGIN_RESPONSE = 8,
    M_HASH = 9, M_MOUSE_EVENT = 10, M_CURSOR_DATA = 12, M_CURSOR_POSITION = 13,
    M_KEY_EVENT = 15, M_CLIPBOARD = 16, M_MISC = 19, M_SCREENSHOT_REQUEST = 29,
    M_SCREENSHOT_RESPONSE = 30
};

#define KEEPALIVE_MS 3000
#define KEEPALIVE_STALE_MS 10000
#define MAX_LOGIN_ATTEMPTS 10
#define VIDEO_PREFIX 32 /* room for the headers written in front of a frame */
#define VIDEO_SUFFIX 16 /* key and pts, written after it */

static void say(cdv_session *s, const char *msg)
{
    if (s->hooks->log)
        s->hooks->log(s->hooks->user, msg);
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
    s->out = out;
    s->outcap = outcap;
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
    if (s->state == CDV_CLOSED || s->outcap - s->olen < 8)
        return 0;
    pbw_init(w, s->out + s->olen + 4, s->outcap - s->olen - 4);
    return 1;
}

static int msg_end(cdv_session *s, pbw *w)
{
    uint8_t *body = s->out + s->olen + 4, h[4];
    size_t n = pbw_len(w, body), hl;
    if (w->overflow) {
        say(s, "outbound queue full; message dropped");
        return 0;
    }
    hl = frame_header(h, n);
    if (hl < 4)
        memmove(s->out + s->olen + hl, body, n);
    memcpy(s->out + s->olen, h, hl);
    s->olen += hl + n;
    return 1;
}

const uint8_t *cdv_out_peek(const cdv_session *s, size_t *n)
{
    *n = s->olen - s->ooff;
    return s->out + s->ooff;
}

void cdv_out_consume(cdv_session *s, size_t n)
{
    s->ooff += n;
    if (s->ooff >= s->olen)
        s->ooff = s->olen = 0;
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

void cdv_start(cdv_session *s, uint32_t now_ms)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    pbw w;
    int i;
    s->now = s->delay_sent = now_ms;
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

/* ---- video ----------------------------------------------------------------- */

uint8_t *cdv_video_begin(cdv_session *s, size_t *cap)
{
    if (s->state != CDV_LIVE || s->olen != 0 || s->outcap < VIDEO_PREFIX + VIDEO_SUFFIX + 64)
        return NULL;
    *cap = s->outcap - VIDEO_PREFIX - VIDEO_SUFFIX;
    return s->out + VIDEO_PREFIX;
}

/* Message{video_frame: VideoFrame{vp8s: VP9s{frames: [VP9{data, key, pts}]}}},
 * with every header computed from the data's length and written in front of
 * it, so a 100 KB frame is never copied. The gap left before the headers is
 * skipped by starting the queue past it. */
void cdv_video_commit(cdv_session *s, size_t len, int key)
{
    uint8_t *data = s->out + VIDEO_PREFIX, *p, pre[VIDEO_PREFIX];
    size_t suffix, l3, l2, l1, l0, n = 0;
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

    n += frame_header(pre + n, l0);
    pre[n++] = (M_VIDEO_FRAME << 3) | PB_LEN;
    n += pb_put_varint(pre + n, l1);
    pre[n++] = (12 << 3) | PB_LEN; /* vp8s: field 12, not 6, which is VP9 */
    n += pb_put_varint(pre + n, l2);
    pre[n++] = (1 << 3) | PB_LEN; /* frames */
    n += pb_put_varint(pre + n, l3);
    pre[n++] = (1 << 3) | PB_LEN; /* data */
    n += pb_put_varint(pre + n, len);

    memcpy(data - n, pre, n);
    s->ooff = VIDEO_PREFIX - n;
    s->olen = (size_t)(p - s->out);
}

int cdv_take_refresh(cdv_session *s)
{
    int r = s->refresh;
    s->refresh = 0;
    return r;
}

/* ---- inbound ---------------------------------------------------------------- */

static void login(cdv_session *s, const uint8_t *b, size_t n)
{
    pbr r;
    const uint8_t *pw = NULL;
    size_t pwlen = 0;
    char line[160];

    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if (r.wire != PB_LEN)
            continue;
        if (r.field == 2) {
            pw = r.data;
            pwlen = r.len;
        } else if (r.field == 5 || r.field == 11) {
            char *dst = r.field == 5 ? s->peer_name : s->peer_version;
            size_t cap = r.field == 5 ? sizeof s->peer_name : sizeof s->peer_version;
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
            snprintf(line, sizeof line, "wrong password from '%s'", s->peer_name);
            say(s, line);
            send_login_error(s, "Wrong Password");
            return;
        }
    }
    send_peer_info(s);
    s->state = CDV_LIVE;
    s->refresh = 1;
    snprintf(line, sizeof line, "'%s' logged in (client %s)", s->peer_name,
             s->peer_version[0] ? s->peer_version : "unknown");
    say(s, line);
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

static void misc(cdv_session *s, const uint8_t *b, size_t n)
{
    pbr r;
    pbr_init(&r, b, n);
    while (pbr_next(&r)) {
        if ((r.field == 10 && r.v) || r.field == 31)
            s->refresh = 1;
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
     * for; a client compresses only above a size threshold. */
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
            dispatch(s, s->in + hl, len);
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
