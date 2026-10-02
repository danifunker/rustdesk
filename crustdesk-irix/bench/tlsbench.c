/* What a TLS handshake costs on this machine: the public-key operations of a
 * client's ECDHE_ECDSA handshake, one at a time, with BearSSL's 15-bit and
 * 31-bit arithmetic. Verification does all its work before comparing, so
 * made-up signatures time the same as real ones.
 *
 *   make -C bench tlsbench; then link on the machine (see bench/Makefile)
 */
#include <bearssl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* A point on each curve: the generators. */
static void point(const br_ec_impl *ec, int curve, unsigned char *q, size_t *qlen)
{
    const unsigned char *g;
    g = ec->generator(curve, qlen);
    memcpy(q, g, *qlen);
}

static void verify(const char *name, const br_ec_impl *ec, br_ecdsa_vrfy vrfy, int curve,
                   size_t hlen, size_t slen)
{
    unsigned char q[200], hash[64], sig[132];
    br_ec_public_key pk;
    size_t qlen;
    double t0;
    int r;
    point(ec, curve, q, &qlen);
    pk.curve = curve;
    pk.q = q;
    pk.qlen = qlen;
    memset(hash, 0x5a, sizeof hash);
    memset(sig, 0x33, sizeof sig);
    t0 = now();
    r = vrfy(ec, hash, hlen, &pk, sig, slen);
    printf("%-28s %6.2f s  (%d)\n", name, now() - t0, r);
}

static void x25519(const char *name, const br_ec_impl *ec)
{
    unsigned char g[32], k[32];
    size_t glen;
    double t0;
    memcpy(g, ec->generator(BR_EC_curve25519, &glen), 32);
    memset(k, 0x47, sizeof k);
    t0 = now();
    ec->mul(g, 32, k, 32, BR_EC_curve25519);
    printf("%-28s %6.2f s\n", name, now() - t0);
}

int main(void)
{
    x25519("x25519 mul, m15", &br_ec_c25519_m15);
    x25519("x25519 mul, m31", &br_ec_c25519_m31);
    x25519("x25519 mul, i15", &br_ec_c25519_i15);
    x25519("x25519 mul, i31", &br_ec_c25519_i31);
    verify("P-256 ecdsa verify, m15", &br_ec_p256_m15, br_ecdsa_i15_vrfy_raw, BR_EC_secp256r1, 32, 64);
    verify("P-256 ecdsa verify, m31", &br_ec_p256_m31, br_ecdsa_i31_vrfy_raw, BR_EC_secp256r1, 32, 64);
    verify("P-384 ecdsa verify, i15", &br_ec_prime_i15, br_ecdsa_i15_vrfy_raw, BR_EC_secp384r1, 48, 96);
    verify("P-384 ecdsa verify, i31", &br_ec_prime_i31, br_ecdsa_i31_vrfy_raw, BR_EC_secp384r1, 48, 96);
    printf("default ec: %s\n", br_ec_get_default() == &br_ec_all_m15 ? "all_m15" :
           br_ec_get_default() == &br_ec_all_m31 ? "all_m31" : "other");
    return 0;
}
