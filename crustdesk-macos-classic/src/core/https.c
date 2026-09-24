#include "https.h"

#include "anchors.h"

#include <string.h>

enum { P_HEAD, P_BODY, P_CHUNK_SIZE, P_CHUNK_DATA, P_CHUNK_CRLF, P_TRAILER, P_UNTIL_CLOSE,
       P_DONE };

/* TLS 1.2 with ephemeral keys only, the ciphers a 68040 is least slow at
 * first: ChaCha20 before bitsliced AES. */
static const uint16_t suites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
};

static void fail(cdv_https *h, int err)
{
    if (h->state == HS_BUSY) {
        h->state = HS_FAILED;
        h->err = err;
    }
    h->reusable = 0;
}

static void finish(cdv_https *h)
{
    h->phase = P_DONE;
    h->body[h->body_len] = 0;
    h->reusable = !h->close_after;
    if (h->state == HS_BUSY)
        h->state = HS_DONE;
}

/* ---- the reply ---------------------------------------------------------------- */

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Does header line `l` (length n) name `name`? Returns its value, or NULL. */
static const char *header(const char *l, size_t n, const char *name)
{
    size_t k = strlen(name), i;
    if (n <= k || l[k] != ':')
        return NULL;
    for (i = 0; i < k; i++)
        if (lower((unsigned char)l[i]) != name[i])
            return NULL;
    l += k + 1;
    while (*l == ' ' || *l == '\t')
        l++;
    return l;
}

static int has_token(const char *v, const char *end, const char *tok)
{
    size_t k = strlen(tok);
    for (; v + k <= end; v++) {
        size_t i;
        for (i = 0; i < k && lower((unsigned char)v[i]) == tok[i]; i++)
            ;
        if (i == k)
            return 1;
    }
    return 0;
}

static void keep(cdv_https *h, const uint8_t *d, size_t n)
{
    size_t room = HTTPS_BODY_MAX - h->body_len;
    if (n > room) {
        n = room;
        h->truncated = 1;
    }
    memcpy(h->body + h->body_len, d, n);
    h->body_len += n;
}

/* The head is complete (head[0..head_len) ends with the blank line). */
static void parse_head(cdv_https *h)
{
    char *p = h->head, *end = h->head + h->head_len;
    char *eol = memchr(p, '\n', (size_t)(end - p));
    if (h->head_len < 12 || memcmp(p, "HTTP/1.", 7) || !eol) {
        fail(h, HE_PROTOCOL);
        return;
    }
    h->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    h->close_after = p[7] == '0'; /* HTTP/1.0 closes unless it says otherwise */
    h->clen = -1;
    h->chunked = 0;
    h->location[0] = 0;
    for (p = eol + 1; p < end; p = eol + 1) {
        const char *v;
        size_t n;
        eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol)
            break;
        n = (size_t)(eol - p);
        if (n && p[n - 1] == '\r')
            n--;
        if (!n)
            break;
        if ((v = header(p, n, "content-length")) != NULL) {
            long c = 0;
            while (*v >= '0' && *v <= '9')
                c = c * 10 + (*v++ - '0');
            h->clen = c;
        } else if ((v = header(p, n, "transfer-encoding")) != NULL) {
            h->chunked = has_token(v, p + n, "chunked");
        } else if ((v = header(p, n, "connection")) != NULL) {
            if (has_token(v, p + n, "close"))
                h->close_after = 1;
            else if (has_token(v, p + n, "keep-alive"))
                h->close_after = 0;
        } else if ((v = header(p, n, "location")) != NULL) {
            size_t k = (size_t)(p + n - v);
            if (k > sizeof h->location - 1)
                k = sizeof h->location - 1;
            memcpy(h->location, v, k);
            h->location[k] = 0;
        }
    }
    if (h->status >= 100 && h->status < 200) {
        h->head_len = 0; /* 100 Continue and the like: the real reply follows */
        return;
    }
    if (h->chunked) {
        h->phase = P_CHUNK_SIZE;
        h->line_len = 0;
    } else if (h->clen >= 0) {
        h->phase = P_BODY;
        if (h->clen == 0)
            finish(h);
    } else {
        h->phase = P_UNTIL_CLOSE;
        h->close_after = 1;
    }
}

static void parse(cdv_https *h, const uint8_t *d, size_t n)
{
    while (n && h->state == HS_BUSY) {
        switch (h->phase) {
        case P_HEAD: {
            uint8_t c = *d++;
            n--;
            if (h->head_len >= sizeof h->head - 1) {
                fail(h, HE_TOO_BIG);
                return;
            }
            h->head[h->head_len++] = (char)c;
            if (c == '\n' && h->head_len >= 2 &&
                (h->head[h->head_len - 2] == '\n' ||
                 (h->head_len >= 4 && !memcmp(h->head + h->head_len - 4, "\r\n\r\n", 4)))) {
                h->head[h->head_len] = 0;
                parse_head(h);
            }
            break;
        }
        case P_BODY: {
            size_t k = (size_t)h->clen < n ? (size_t)h->clen : n;
            keep(h, d, k);
            d += k;
            n -= k;
            h->clen -= (long)k;
            if (!h->clen)
                finish(h);
            break;
        }
        case P_CHUNK_SIZE:
        case P_TRAILER: {
            uint8_t c = *d++;
            n--;
            if (c != '\n') {
                if (h->line_len < sizeof h->line - 1)
                    h->line[h->line_len++] = (char)c;
                break;
            }
            h->line[h->line_len] = 0;
            if (h->line_len && h->line[h->line_len - 1] == '\r')
                h->line[--h->line_len] = 0;
            if (h->phase == P_TRAILER) {
                if (!h->line_len)
                    finish(h);
            } else {
                long v = 0;
                const char *s = h->line;
                for (;; s++) {
                    int x = lower((unsigned char)*s);
                    if (x >= '0' && x <= '9')
                        v = v * 16 + (x - '0');
                    else if (x >= 'a' && x <= 'f')
                        v = v * 16 + (x - 'a' + 10);
                    else
                        break;
                }
                h->chunk_left = v;
                h->phase = v ? P_CHUNK_DATA : P_TRAILER;
            }
            h->line_len = 0;
            break;
        }
        case P_CHUNK_DATA: {
            size_t k = (size_t)h->chunk_left < n ? (size_t)h->chunk_left : n;
            keep(h, d, k);
            d += k;
            n -= k;
            h->chunk_left -= (long)k;
            if (!h->chunk_left) {
                h->phase = P_CHUNK_CRLF;
                h->line_len = 0;
            }
            break;
        }
        case P_CHUNK_CRLF:
            if (*d == '\n')
                h->phase = P_CHUNK_SIZE;
            d++;
            n--;
            break;
        case P_UNTIL_CLOSE:
            keep(h, d, n);
            n = 0;
            break;
        default:
            return; /* bytes after the reply: nothing asked for them */
        }
    }
}

/* ---- moving bytes --------------------------------------------------------------- */

/* Between the request/reply buffers and the TLS engine. */
static void run(cdv_https *h)
{
    br_ssl_engine_context *e = &h->sc.eng;
    if (!h->secure || !h->open)
        return;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(e);
        int moved = 0;
        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(e);
            h->open = 0;
            if (err) {
                h->tls_err = err;
                fail(h, HE_TLS);
            } else {
                fail(h, HE_CLOSED);
            }
            return;
        }
        if ((st & BR_SSL_SENDAPP) && h->req_off < h->req_len) {
            size_t len, k;
            unsigned char *b = br_ssl_engine_sendapp_buf(e, &len);
            k = h->req_len - h->req_off;
            if (k > len)
                k = len;
            memcpy(b, h->req + h->req_off, k);
            br_ssl_engine_sendapp_ack(e, k);
            h->req_off += k;
            if (h->req_off == h->req_len)
                br_ssl_engine_flush(e, 0);
            moved = 1;
        }
        if (st & BR_SSL_RECVAPP) {
            size_t len;
            unsigned char *b = br_ssl_engine_recvapp_buf(e, &len);
            parse(h, b, len);
            br_ssl_engine_recvapp_ack(e, len);
            moved = 1;
        }
        if (!moved)
            return;
    }
}

void https_connected(cdv_https *h, int secure, const char *host, uint8_t *iobuf, size_t iolen,
                     uint32_t days, uint32_t secs, const uint8_t seed[32])
{
    br_ssl_engine_context *e = &h->sc.eng;
    h->secure = secure;
    h->open = 1;
    h->reusable = 1;
    h->state = HS_IDLE;
    h->err = HE_NONE;
    h->tls_err = 0;
    h->req_len = h->req_off = 0;
    if (!secure)
        return;
    h->iobuf = iobuf;
    h->iolen = iolen;
    /* br_ssl_client_init_full, less everything this client never offers:
     * no RSA key exchange, no CBC, no TLS before 1.2. */
    br_ssl_client_zero(&h->sc);
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(e, suites, sizeof suites / sizeof suites[0]);
    br_x509_minimal_init(&h->xc, &br_sha256_vtable, cdv_anchors, cdv_anchors_count);
    br_ssl_engine_set_hash(e, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(e, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&h->xc, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(&h->xc, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&h->xc, br_sha512_ID, &br_sha512_vtable);
    br_ssl_engine_set_prf_sha256(e, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(e, &br_tls12_sha384_prf);
    br_x509_minimal_set_rsa(&h->xc, br_rsa_pkcs1_vrfy_get_default());
    br_x509_minimal_set_ecdsa(&h->xc, br_ec_get_default(), br_ecdsa_vrfy_asn1_get_default());
    br_ssl_engine_set_default_rsavrfy(e);
    br_ssl_engine_set_default_ecdsa(e);
    br_ssl_engine_set_default_chapol(e);
    br_ssl_engine_set_default_aes_gcm(e);
    br_x509_minimal_set_time(&h->xc, days, secs);
    br_ssl_engine_set_x509(e, &h->xc.vtable);
    br_ssl_engine_set_buffer(e, iobuf, iolen, 1);
    br_ssl_engine_inject_entropy(e, seed, 32);
    if (!br_ssl_client_reset(&h->sc, host, 0)) {
        h->open = 0;
        h->tls_err = br_ssl_engine_last_error(e);
    }
}

int https_post(cdv_https *h, const char *host, const char *path, const char *json)
{
    size_t jl = strlen(json), n;
    char num[12], *p;
    int i = 0;
    unsigned long v = (unsigned long)jl;
    if (h->state == HS_BUSY || !h->open)
        return 0;
    do
        num[i++] = (char)('0' + v % 10);
    while ((v /= 10) != 0);
    /* "POST path HTTP/1.1\r\nHost: host\r\n...Content-Length: n\r\n\r\njson" */
    n = 5 + strlen(path) + 17 + strlen(host) + 60 + 18 + (size_t)i + 4 + jl;
    if (n > sizeof h->req)
        return 0;
    p = h->req;
    memcpy(p, "POST ", 5), p += 5;
    memcpy(p, path, strlen(path)), p += strlen(path);
    memcpy(p, " HTTP/1.1\r\nHost: ", 17), p += 17;
    memcpy(p, host, strlen(host)), p += strlen(host);
    memcpy(p, "\r\nContent-Type: application/json\r\nConnection: keep-alive", 56), p += 56;
    memcpy(p, "\r\nContent-Length: ", 18), p += 18;
    while (i)
        *p++ = num[--i];
    memcpy(p, "\r\n\r\n", 4), p += 4;
    memcpy(p, json, jl), p += jl;
    h->req_len = (size_t)(p - h->req);
    h->req_off = 0;
    h->plain_off = 0;
    h->state = HS_BUSY;
    h->err = HE_NONE;
    h->phase = P_HEAD;
    h->head_len = 0;
    h->body_len = 0;
    h->truncated = 0;
    h->status = 0;
    h->close_after = 0;
    run(h);
    return 1;
}

size_t https_out(cdv_https *h, const uint8_t **p)
{
    size_t len;
    if (!h->open)
        return 0;
    if (!h->secure) {
        *p = (const uint8_t *)h->req + h->req_off;
        return h->state == HS_BUSY ? h->req_len - h->req_off : 0;
    }
    if (!(br_ssl_engine_current_state(&h->sc.eng) & BR_SSL_SENDREC))
        return 0;
    *p = br_ssl_engine_sendrec_buf(&h->sc.eng, &len);
    return len;
}

void https_out_done(cdv_https *h, size_t n)
{
    if (!h->secure) {
        h->req_off += n;
        return;
    }
    br_ssl_engine_sendrec_ack(&h->sc.eng, n);
    run(h);
}

size_t https_in(cdv_https *h, uint8_t **p)
{
    size_t len;
    if (!h->open)
        return 0;
    if (!h->secure) {
        *p = h->pin;
        return sizeof h->pin;
    }
    if (!(br_ssl_engine_current_state(&h->sc.eng) & BR_SSL_RECVREC))
        return 0;
    *p = br_ssl_engine_recvrec_buf(&h->sc.eng, &len);
    return len;
}

void https_in_done(cdv_https *h, size_t n)
{
    if (!h->secure) {
        parse(h, h->pin, n);
        return;
    }
    br_ssl_engine_recvrec_ack(&h->sc.eng, n);
    run(h);
}

void https_eof(cdv_https *h)
{
    if (h->state == HS_BUSY && h->phase == P_UNTIL_CLOSE)
        finish(h);
    h->open = 0;
    h->reusable = 0;
    fail(h, HE_CLOSED);
}

int https_poll(cdv_https *h)
{
    run(h);
    return h->state;
}

int https_needs_reconnect(const cdv_https *h)
{
    return !h->open || !h->reusable || h->state == HS_FAILED;
}

void https_disconnected(cdv_https *h)
{
    h->open = 0;
    if (h->state == HS_BUSY)
        fail(h, HE_CLOSED);
}

uint32_t https_days(int y, int m, int d)
{
    /* Days from civil, counted from 1 March of year 0 then shifted to 1 January. */
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe + 60); /* 1 March 0 is day 60 of year 0 */
}
