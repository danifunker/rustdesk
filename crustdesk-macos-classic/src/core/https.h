/* A small HTTP/1.1 client, over TLS (BearSSL) or not, with no I/O of its own.
 *
 * The platform owns the connection and moves bytes: whatever https_out()
 * offers goes to the socket, whatever arrives goes into https_in(). The client
 * sends one request at a time and keeps the connection for the next -- on a
 * 68040 a TLS handshake is seconds of arithmetic, so a console heartbeat every
 * fifteen seconds must not pay for one each time.
 *
 * Replies: a status line, headers (Content-Length, chunked transfer, and
 * Connection: close are understood), and a body kept up to HTTPS_BODY_MAX
 * bytes. Redirects are not followed.
 */
#ifndef CDV_HTTPS_H
#define CDV_HTTPS_H

#include <bearssl.h>
#include <stddef.h>
#include <stdint.h>

#define HTTPS_BODY_MAX 2048
#define HTTPS_IOBUF (BR_SSL_BUFSIZE_BIDI) /* what a TLS connection needs */

enum { HS_IDLE, HS_BUSY, HS_DONE, HS_FAILED };

/* Why a request failed. */
enum { HE_NONE, HE_TLS, HE_CLOSED, HE_PROTOCOL, HE_TOO_BIG };

typedef struct {
    int secure;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    uint8_t *iobuf;
    size_t iolen;
    int open;        /* a connection is up (it may still be handshaking) */
    int reusable;    /* the server did not ask to close after the last reply */
    int state;       /* HS_* */
    int err;         /* HE_*, and for HE_TLS the BearSSL error in tls_err */
    int tls_err;
    /* the request, as bytes still to hand over */
    char req[1024];
    size_t req_len, req_off;
    /* the reply */
    int phase;       /* inside the reply: head, body, chunk... */
    char head[1024];
    size_t head_len;
    int status;
    long clen;       /* Content-Length, or -1 */
    int chunked, close_after;
    long chunk_left;
    char line[24];   /* a chunk-size line being read */
    size_t line_len;
    char body[HTTPS_BODY_MAX + 1];
    size_t body_len;
    int truncated;
    char location[128];
    /* plain http: where arriving bytes go */
    size_t plain_off;
    uint8_t pin[512];
} cdv_https;

/* A new connection has been made: start TLS on it (secure), or not. `iobuf`
 * (HTTPS_IOBUF bytes) belongs to the client while the connection lasts. The
 * certificate check needs today's date: days since 1 January of year 0 and
 * seconds into the day, UTC. `seed` is 32 random bytes. */
void https_connected(cdv_https *h, int secure, const char *host, uint8_t *iobuf, size_t iolen,
                     uint32_t days, uint32_t secs, const uint8_t seed[32]);

/* Queue a POST of a JSON body. Only when there is no request in flight.
 * Returns 0 if the request does not fit. */
int https_post(cdv_https *h, const char *host, const char *path, const char *json);

/* Bytes for the transport. https_out_done() says how many it took. */
size_t https_out(cdv_https *h, const uint8_t **p);
void https_out_done(cdv_https *h, size_t n);

/* Bytes from the transport: https_in() gives room, https_in_done() fills it.
 * https_eof(): the server closed the connection. */
size_t https_in(cdv_https *h, uint8_t **p);
void https_in_done(cdv_https *h, size_t n);
void https_eof(cdv_https *h);

/* Where the request stands: HS_BUSY until a reply is complete (HS_DONE:
 * status, body) or it failed (HS_FAILED: err). */
int https_poll(cdv_https *h);

/* The connection is finished with, or has to be (after HS_FAILED, or a reply
 * that asked for it): the transport should close it and connect afresh. */
int https_needs_reconnect(const cdv_https *h);
void https_disconnected(cdv_https *h);

/* Days since 1 January of year 0 (proleptic Gregorian) for a date. */
uint32_t https_days(int year, int month, int day);

#endif
