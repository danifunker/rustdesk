/*
 * TLS for the console API, over mbedTLS.
 *
 * WHY C RATHER THAN A RUST TLS STACK. rustls needs `ring` or `aws-lc-rs`, and
 * neither supports 32-bit big-endian PowerPC; its pure-Rust provider is forty
 * crates of const generics, against a build whose whole premise is a dependency
 * list mrustc can compile. mbedTLS is portable C99 with no dependencies, is
 * explicitly big-endian clean, and is one MacPorts line away. The same trade as
 * every other shim in this directory.
 *
 * WHY NOT THE SYSTEM. Leopard's Secure Transport tops out at TLS 1.0 and its
 * OpenSSL is 0.9.7. Neither can complete a handshake with a server configured
 * this decade, so "use what the OS has" is not an option that reaches a modern
 * console.
 *
 * WHY NOT mbedtls_net_*. This takes an fd Rust already opened and configured,
 * so the socket keeps the connect and I/O timeouts `http` set on it. The BIO
 * callbacks below are plain read()/write(), which also avoids depending on
 * net_sockets.c compiling cleanly on Darwin 8.
 *
 * A SOCKET TIMEOUT IS A HARD ERROR HERE, deliberately. read() on a socket with
 * SO_RCVTIMEO returns EAGAIN, and the usual mapping of EAGAIN to WANT_READ
 * would turn a wedged console into a spin: the retry loop has no deadline of
 * its own, so it would call read() forever without ever blocking. Only EINTR
 * retries; everything else fails and lets the caller log it.
 *
 * ASCII ONLY, C89-COMPATIBLE DECLARATIONS. The G4/G5 toolchain here is
 * gcc10-bootstrap but the rest of this directory is built by Apple's gcc 4.0.1,
 * and matching that costs nothing.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
/* Only for MBEDTLS_ERR_NET_{SEND,RECV}_FAILED, which are what a BIO callback
 * is expected to return. No function from it is called, so this adds a
 * header dependency and not a link one. */
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

struct rd_tls {
    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      conf;
    mbedtls_x509_crt        ca;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    int fd;
    /* Set when read() reported end of file. mbedTLS reports a zero-length read
     * as a receive failure, and only this flag separates "the server finished"
     * from "the socket broke". */
    int eof;
};

/* Declared ahead of rd_tls_connect, which unwinds through it on failure. */
void rd_tls_free(struct rd_tls *t);

static void say(char *err, size_t errlen, const char *text)
{
    if (err && errlen) {
        strncpy(err, text, errlen - 1);
        err[errlen - 1] = '\0';
    }
}

/* An mbedTLS return code as text. `mbedtls_strerror` needs MBEDTLS_ERROR_C,
 * which is on in the default config; the numeric fallback keeps this honest if
 * someone builds mbedTLS without it. */
static void say_ret(char *err, size_t errlen, const char *what, int ret)
{
    char detail[160];
    detail[0] = '\0';
    mbedtls_strerror(ret, detail, sizeof detail);
    if (detail[0] == '\0') {
        snprintf(detail, sizeof detail, "error %d", ret);
    }
    if (err && errlen) {
        snprintf(err, errlen, "%s: %s", what, detail);
    }
}

static int rd_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct rd_tls *t = (struct rd_tls *)ctx;
    ssize_t n = write(t->fd, buf, len);
    if (n >= 0) return (int)n;
    if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int rd_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct rd_tls *t = (struct rd_tls *)ctx;
    ssize_t n = read(t->fd, buf, len);
    if (n > 0) return (int)n;
    if (n == 0) {
        /* The peer closed the connection without a close_notify. Recorded
         * rather than reported as success, so rd_tls_read can tell the caller
         * this was an end and not a fault. */
        t->eof = 1;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/*
 * Wrap an already-connected socket in TLS, verifying the peer against
 * `ca_pem`.
 *
 * `ca_pem` must be PEM and `ca_len` must COUNT ITS TRAILING NUL: that is
 * mbedTLS's calling convention for PEM input, and a length that omits the NUL
 * fails to parse for no visible reason.
 *
 * Returns NULL on failure, with `err` describing why in terms someone can act
 * on -- a certificate problem in particular, since "handshake failed" does not
 * distinguish an expired certificate from a private CA nobody told us about.
 */
struct rd_tls *rd_tls_connect(int fd, const char *hostname,
                              const unsigned char *ca_pem, size_t ca_len,
                              char *err, size_t errlen)
{
    static const char pers[] = "rustdesk-ppc-agent";
    struct rd_tls *t;
    int ret;

    t = (struct rd_tls *)calloc(1, sizeof *t);
    if (!t) {
        say(err, errlen, "out of memory");
        return NULL;
    }
    t->fd = fd;
    t->eof = 0;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);

    ret = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
                                (const unsigned char *)pers, sizeof pers - 1);
    if (ret != 0) {
        say_ret(err, errlen, "could not seed the random generator", ret);
        goto fail;
    }

    /* A positive return is the number of certificates that failed to parse
     * while others succeeded, which is normal for a bundle carrying a format
     * this build does not enable. Only a total failure is fatal. */
    ret = mbedtls_x509_crt_parse(&t->ca, ca_pem, ca_len);
    if (ret < 0) {
        say_ret(err, errlen, "could not read the CA bundle", ret);
        goto fail;
    }
    if (t->ca.version == 0) {
        say(err, errlen, "the CA bundle contained no usable certificates");
        goto fail;
    }

    ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        say_ret(err, errlen, "could not configure TLS", ret);
        goto fail;
    }
    /* Verification is required, not optional. An agent that reported in over a
     * connection it could not authenticate would be worse than one that did
     * not report in at all: it would look like it was working. */
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    /* TLS 1.0 and 1.1 are the versions this machine's own system libraries
     * would offer, and are exactly what must not be negotiated. */
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);

    ret = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (ret != 0) {
        say_ret(err, errlen, "could not set up the TLS session", ret);
        goto fail;
    }
    /* Both SNI and the name checked against the certificate. Without it a
     * shared-hosting console answers with the wrong certificate and every
     * connection fails for a reason that looks like ours. */
    ret = mbedtls_ssl_set_hostname(&t->ssl, hostname);
    if (ret != 0) {
        say_ret(err, errlen, "could not set the server name", ret);
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, rd_send, rd_recv, NULL);

    for (;;) {
        ret = mbedtls_ssl_handshake(&t->ssl);
        if (ret == 0) break;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            /* The one failure worth explaining rather than numbering: this is
             * where an expired certificate, a private CA and a hostname
             * mismatch all land, and they need different fixes. */
            char why[320];
            const char *hint;
            uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
            why[0] = '\0';
            mbedtls_x509_crt_verify_info(why, sizeof why, "", flags);
            /* The hint has to match the actual fault. A generic "check your CA
             * bundle" on a clock problem sends someone looking in the wrong
             * place, and on this hardware the clock is the likelier cause: a
             * G4 or G5 with a dead PRAM battery boots in 1970, and every
             * certificate on earth is then not yet valid. */
            if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
                hint = " -- a private CA needs --ca-bundle";
            } else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
                hint = " -- the host in --api-server must match the certificate";
            } else if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) {
                hint = " -- check this Mac's clock before the certificate; a dead"
                       " PRAM battery makes every certificate look wrong";
            } else {
                hint = "";
            }
            if (err && errlen) {
                char *nl = strchr(why, '\n');
                if (nl) *nl = '\0';
                snprintf(err, errlen, "the console's certificate was rejected: %s%s",
                         why[0] ? why : "unknown reason", hint);
            }
            goto fail;
        }
        say_ret(err, errlen, "TLS handshake failed", ret);
        goto fail;
    }
    return t;

fail:
    rd_tls_free(t);
    return NULL;
}

/* >0 bytes read, 0 at end of stream, <0 on error. */
int rd_tls_read(struct rd_tls *t, unsigned char *buf, size_t len)
{
    int ret;
    for (;;) {
        ret = mbedtls_ssl_read(&t->ssl, buf, len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        /* A close with no close_notify. Accepted as an end rather than an
         * error because the caller checks Content-Length against what arrived,
         * so a body cut short is caught there instead of here. */
        if (ret < 0 && t->eof) return 0;
        return ret;
    }
}

/* The number of bytes written, or <0 on error. */
int rd_tls_write(struct rd_tls *t, const unsigned char *buf, size_t len)
{
    int ret;
    for (;;) {
        ret = mbedtls_ssl_write(&t->ssl, buf, len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return ret;
    }
}

/* What was actually negotiated, for the log. Diagnosing a TLS problem starts
 * with knowing whether it got as far as agreeing on anything. */
const char *rd_tls_version(struct rd_tls *t)
{
    const char *v = mbedtls_ssl_get_version(&t->ssl);
    return v ? v : "unknown";
}

const char *rd_tls_ciphersuite(struct rd_tls *t)
{
    const char *c = mbedtls_ssl_get_ciphersuite(&t->ssl);
    return c ? c : "unknown";
}

void rd_tls_free(struct rd_tls *t)
{
    if (!t) return;
    /* Best effort: the socket is closed by Rust either way, and a peer that
     * has already gone will not read this. */
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t);
}

/* An mbedTLS return code as text, for the Rust side's io::Error. Without this
 * a read failure reaches the log as a bare negative number. */
void rd_tls_strerror(int ret, char *buf, size_t len)
{
    if (!buf || !len) return;
    buf[0] = '\0';
    mbedtls_strerror(ret, buf, len);
    if (buf[0] == '\0') snprintf(buf, len, "mbedTLS error %d", ret);
}
