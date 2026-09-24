#include "rdv.h"
#include "pb.h"
#include "rng.h"

#include <string.h>

#define REPORTED_VERSION "1.4.5"
#define REG_INTERVAL 15000 /* ms between registrations once answered */
#define REG_RETRY 3000     /* ms before asking again when unanswered */
#define DEDUP_MS 100       /* hbbs repeats a request; act on the first */

/* RendezvousMessage fields, from rendezvous.proto. */
enum {
    R_REGISTER_PEER = 6, R_REGISTER_PEER_RESPONSE = 7, R_PUNCH_HOLE = 9,
    R_FETCH_LOCAL_ADDR = 12, R_LOCAL_ADDR = 13, R_REGISTER_PK = 15,
    R_REGISTER_PK_RESPONSE = 16, R_REQUEST_RELAY = 18, R_RELAY_RESPONSE = 19,
    R_PEER_DISCOVERY = 22
};

void rdv_init(cdv_rdv *r)
{
    r->sent_at = r->answered_at = 0;
    r->awaiting = r->registered = r->refused = 0;
    r->dedup_ip = r->dedup_at = 0;
    r->dedup_port = 0;
}

/* ---- the address mangling (rendezvous.rs) ----------------------------------- */

/* The IP goes in as the little-endian reading of its four octets, as Rust's
 * u32::from_le_bytes(octets) makes it; ours is the usual a<<24|b<<16|c<<8|d. */
static uint32_t swap32(uint32_t v)
{
    return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
}

size_t rdv_mangle(uint32_t ip, uint16_t port, uint32_t tm, uint8_t out[16])
{
    uint64_t a = (uint64_t)swap32(ip) + tm;
    uint64_t p = (uint64_t)port + (tm & 0xFFFF);
    uint64_t lo = p | ((uint64_t)tm << 17) | (a << 49);
    uint64_t hi = a >> 15;
    size_t n = 16;
    int i;
    for (i = 0; i < 8; i++) {
        out[i] = (uint8_t)(lo >> (8 * i));
        out[8 + i] = (uint8_t)(hi >> (8 * i));
    }
    while (n && !out[n - 1])
        n--;
    return n;
}

void rdv_unmangle(const uint8_t *b, size_t n, uint32_t *ip, uint16_t *port)
{
    uint8_t pad[16] = { 0 };
    uint64_t lo = 0, hi = 0, tm, a;
    int i;
    *ip = 0;
    *port = 0;
    if (!n || n > 16)
        return;
    memcpy(pad, b, n);
    for (i = 0; i < 8; i++) {
        lo |= (uint64_t)pad[i] << (8 * i);
        hi |= (uint64_t)pad[8 + i] << (8 * i);
    }
    tm = (lo >> 17) & 0xFFFFFFFFu;
    a = (lo >> 49) | (hi << 15);
    *ip = swap32((uint32_t)(a - tm));
    *port = (uint16_t)((lo & 0xFFFFFF) - (tm & 0xFFFF));
}

/* ---- helpers ------------------------------------------------------------- */

void rdv_uuid4(char out[37])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t b[16];
    int i, o = 0;
    rng_bytes(b, sizeof b);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[o++] = '-';
        out[o++] = hex[b[i] >> 4];
        out[o++] = hex[b[i] & 15];
    }
    out[o] = 0;
}

void rdv_split_host(const char *s, char *host, size_t cap, uint16_t *port, uint16_t def)
{
    const char *colon = strrchr(s, ':');
    size_t n = colon ? (size_t)(colon - s) : strlen(s);
    if (n >= cap)
        n = cap - 1;
    memcpy(host, s, n);
    host[n] = 0;
    *port = def;
    if (colon) {
        unsigned v = 0;
        const char *p = colon + 1;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (unsigned)(*p++ - '0');
        if (v && v < 65536)
            *port = (uint16_t)v;
    }
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* BytesCodec framing, as for the session. */
static size_t frame(uint8_t *out, size_t cap, const uint8_t *body, size_t n)
{
    uint32_t v = (uint32_t)n << 2;
    size_t hl = n <= 0x3F ? 1 : n <= 0x3FFF ? 2 : n <= 0x3FFFFF ? 3 : 4, i;
    if (hl + n > cap)
        return 0;
    v |= (uint32_t)(hl - 1);
    memmove(out + hl, body, n);
    for (i = 0; i < hl; i++)
        out[i] = (uint8_t)(v >> (8 * i));
    return hl + n;
}

/* ---- registration ---------------------------------------------------------- */

size_t rdv_tick(cdv_rdv *r, uint32_t now, uint8_t *out, size_t cap)
{
    int due;
    pbw w;
    if (r->awaiting)
        due = now - r->sent_at >= REG_RETRY;
    else if (r->answered_at)
        due = now - r->answered_at >= REG_INTERVAL;
    else
        due = 1;
    if (!due || !r->id[0])
        return 0;
    pbw_init(&w, out, cap);
    pbw_begin(&w, R_REGISTER_PEER);
    pbw_string(&w, 1, r->id);
    pbw_end(&w);
    r->sent_at = now;
    r->awaiting = 1;
    return w.overflow ? 0 : pbw_len(&w, out);
}

static int duplicate(cdv_rdv *r, const rdv_action *a, uint32_t now)
{
    int dup = a->peer_ip == r->dedup_ip && a->peer_port == r->dedup_port &&
              now - r->dedup_at < DEDUP_MS;
    r->dedup_ip = a->peer_ip;
    r->dedup_port = a->peer_port;
    r->dedup_at = now;
    return dup;
}

static void choose_relay(const cdv_rdv *r, rdv_action *a, const uint8_t *adv, size_t n)
{
    if (r->relay[0])
        strncpy(a->relay, r->relay, sizeof a->relay - 1);
    else
        copy_str(a->relay, sizeof a->relay, adv, n);
}

int rdv_input(cdv_rdv *r, const uint8_t *d, size_t n, uint32_t now, rdv_action *a,
              uint8_t *reply, size_t cap, size_t *replylen)
{
    pbr top, m;
    *replylen = 0;
    memset(a, 0, sizeof *a);
    pbr_init(&top, d, n);
    while (pbr_next(&top)) {
        if (top.wire != PB_LEN)
            continue;
        pbr_sub(&top, &m);
        switch (top.field) {
        case R_REGISTER_PEER_RESPONSE: {
            int request_pk = 0;
            while (pbr_next(&m))
                if (m.field == 2)
                    request_pk = m.v != 0;
            r->awaiting = 0;
            r->answered_at = now;
            if (request_pk) {
                pbw w;
                pbw_init(&w, reply, cap);
                pbw_begin(&w, R_REGISTER_PK);
                pbw_string(&w, 1, r->id);
                pbw_bytes(&w, 2, r->uuid, 16);
                pbw_bytes(&w, 3, r->pk, 32);
                pbw_end(&w);
                if (!w.overflow)
                    *replylen = pbw_len(&w, reply);
                return RDV_NONE;
            }
            if (!r->registered) {
                r->registered = 1;
                return RDV_REGISTERED;
            }
            return RDV_NONE;
        }
        case R_REGISTER_PK_RESPONSE: {
            int result = 0;
            while (pbr_next(&m))
                if (m.field == 1)
                    result = (int)m.v;
            r->awaiting = 0;
            r->answered_at = now;
            /* OK (1), or 0 from a server that leaves the default off */
            if (result == 1 || result == 0) {
                if (!r->registered) {
                    r->registered = 1;
                    return RDV_REGISTERED;
                }
                return RDV_NONE;
            }
            r->registered = 0;
            a->kind = RDV_REFUSED;
            a->refuse_code = result;
            return RDV_REFUSED;
        }
        case R_PUNCH_HOLE:
        case R_REQUEST_RELAY:
        case R_FETCH_LOCAL_ADDR: {
            int f = top.field;
            const uint8_t *adv = NULL;
            size_t advn = 0;
            while (pbr_next(&m)) {
                if (m.wire == PB_LEN) {
                    int sa = (f == R_REQUEST_RELAY) ? 3 : 1;
                    int rs = (f == R_REQUEST_RELAY) ? 4 : 2;
                    if (m.field == sa && m.len <= 16) {
                        memcpy(a->socket_addr, m.data, m.len);
                        a->socket_addr_len = m.len;
                    } else if (m.field == rs) {
                        adv = m.data;
                        advn = m.len;
                    } else if (f == R_REQUEST_RELAY && m.field == 2) {
                        copy_str(a->uuid, sizeof a->uuid, m.data, m.len);
                    }
                } else if (f == R_REQUEST_RELAY && m.field == 5) {
                    a->secure = m.v != 0;
                }
            }
            rdv_unmangle(a->socket_addr, a->socket_addr_len, &a->peer_ip, &a->peer_port);
            if (duplicate(r, a, now))
                return RDV_NONE;
            choose_relay(r, a, adv, advn);
            if (f == R_FETCH_LOCAL_ADDR) {
                a->kind = RDV_LOCAL;
            } else {
                a->kind = RDV_RELAY;
                if (f == R_PUNCH_HOLE) {
                    a->initiate = 1;
                    a->secure = 1;
                    rdv_uuid4(a->uuid);
                } else {
                    /* The session is encrypted either way: the peer takes part
                     * in the key exchange whenever it came through the server. */
                    a->secure = 1;
                }
            }
            return a->kind;
        }
        default:
            break;
        }
    }
    return RDV_NONE;
}

size_t rdv_relay_response(cdv_rdv *r, const rdv_action *a, uint8_t *out, size_t cap)
{
    pbw w;
    pbw_init(&w, out, cap);
    pbw_begin(&w, R_RELAY_RESPONSE);
    pbw_bytes(&w, 1, a->socket_addr, a->socket_addr_len);
    if (a->initiate) {
        pbw_string(&w, 2, a->uuid);
        pbw_string(&w, 3, a->relay);
        pbw_string(&w, 4, r->id);
    }
    pbw_string(&w, 7, REPORTED_VERSION);
    pbw_end(&w);
    return w.overflow ? 0 : frame(out, cap, out, pbw_len(&w, out));
}

size_t rdv_request_relay(const cdv_rdv *r, const rdv_action *a, uint8_t *out, size_t cap)
{
    pbw w;
    pbw_init(&w, out, cap);
    pbw_begin(&w, R_REQUEST_RELAY);
    pbw_string(&w, 2, a->uuid);
    pbw_string(&w, 6, r->key); /* licence_key: only a relay started with -k checks it */
    pbw_end(&w);
    return w.overflow ? 0 : frame(out, cap, out, pbw_len(&w, out));
}

size_t rdv_local_addr(cdv_rdv *r, const rdv_action *a, uint32_t my_ip, uint16_t my_port,
                      uint8_t *out, size_t cap)
{
    uint8_t mine[16];
    size_t mn = rdv_mangle(my_ip, my_port, r->mangle_clock += 7919, mine);
    pbw w;
    pbw_init(&w, out, cap);
    pbw_begin(&w, R_LOCAL_ADDR);
    pbw_bytes(&w, 1, a->socket_addr, a->socket_addr_len);
    pbw_bytes(&w, 2, mine, mn);
    pbw_string(&w, 3, a->relay);
    pbw_string(&w, 4, r->id);
    pbw_string(&w, 5, REPORTED_VERSION);
    pbw_end(&w);
    return w.overflow ? 0 : frame(out, cap, out, pbw_len(&w, out));
}

/* ---- LAN discovery (lan.rs) ------------------------------------------------- */

size_t lan_answer(const uint8_t *d, size_t n, const char *my_id, const char *advertise,
                  const char *hostname, uint8_t *out, size_t cap)
{
    pbr top, m;
    int ping = 0;
    char id[32] = "";
    pbw w;
    pbr_init(&top, d, n);
    while (pbr_next(&top)) {
        if (top.field != R_PEER_DISCOVERY || top.wire != PB_LEN)
            continue;
        pbr_sub(&top, &m);
        while (pbr_next(&m)) {
            if (m.field == 1 && m.wire == PB_LEN)
                ping = m.len == 4 && !memcmp(m.data, "ping", 4);
            else if (m.field == 3 && m.wire == PB_LEN)
                copy_str(id, sizeof id, m.data, m.len);
        }
    }
    if (!ping || !strcmp(id, my_id))
        return 0;
    pbw_init(&w, out, cap);
    pbw_begin(&w, R_PEER_DISCOVERY);
    pbw_string(&w, 1, "pong");
    pbw_string(&w, 3, advertise);
    pbw_string(&w, 5, hostname);
    pbw_string(&w, 6, "Mac OS");
    pbw_end(&w);
    return w.overflow ? 0 : pbw_len(&w, out);
}
