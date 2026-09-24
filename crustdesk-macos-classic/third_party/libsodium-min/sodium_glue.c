/* What the vendored libsodium subset expects of the rest of libsodium:
 * randomness (from src/core/rng.c), zeroing, and a misuse handler. */
#include "rng.h"

#include <stdlib.h>
#include <string.h>

void randombytes_buf(void *const buf, const size_t size)
{
    rng_bytes(buf, size);
}

void sodium_memzero(void *const pnt, const size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)pnt;
    size_t i;
    for (i = 0; i < len; i++)
        p[i] = 0;
}

void sodium_misuse(void)
{
    abort();
}

int sodium_is_zero(const unsigned char *n, const size_t nlen)
{
    size_t i;
    volatile unsigned char d = 0U;
    for (i = 0U; i < nlen; i++)
        d |= n[i];
    return 1 & ((d - 1) >> 8);
}

int sodium_memcmp(const void *const b1_, const void *const b2_, size_t len)
{
    const volatile unsigned char *b1 = (const volatile unsigned char *)b1_;
    const volatile unsigned char *b2 = (const volatile unsigned char *)b2_;
    size_t i;
    volatile unsigned char d = 0U;
    for (i = 0U; i < len; i++)
        d |= b1[i] ^ b2[i];
    return (1 & ((d - 1) >> 8)) - 1;
}
