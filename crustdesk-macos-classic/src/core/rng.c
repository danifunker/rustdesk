#include "rng.h"

#include "sodium/crypto_hash_sha512.h"
#include "sodium/crypto_stream_xsalsa20.h"

#include <string.h>

static uint8_t pool[64];
static uint64_t counter;

void rng_add(const void *data, size_t n)
{
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, pool, sizeof pool);
    crypto_hash_sha512_update(&st, (const unsigned char *)data, n);
    crypto_hash_sha512_final(&st, pool);
}

void rng_bytes(void *out, size_t n)
{
    uint8_t nonce[24] = { 0 }, next[32];
    int i;
    counter++;
    for (i = 0; i < 8; i++)
        nonce[i] = (uint8_t)(counter >> (8 * i));
    memset(out, 0, n);
    crypto_stream_xsalsa20_xor((unsigned char *)out, (const unsigned char *)out, n, nonce, pool);
    /* Rekey: the next state from the keystream past what was handed out. */
    nonce[23] = 1;
    memset(next, 0, sizeof next);
    crypto_stream_xsalsa20_xor(next, next, sizeof next, nonce, pool);
    rng_add(next, sizeof next);
}

uint32_t rng_u32(void)
{
    uint8_t b[4];
    rng_bytes(b, 4);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
