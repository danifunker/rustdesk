/* SHA-256 (FIPS 180-4), byte-oriented so it is the same on either endianness.
 * The login check is sha256(sha256(password || salt) || challenge). */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint32_t nbuf;
    uint64_t total;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

#endif
