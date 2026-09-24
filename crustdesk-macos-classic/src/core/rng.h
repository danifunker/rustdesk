/* Randomness for keys, nonces, IDs and challenges.
 *
 * A classic Mac has no /dev/random. The platform feeds entropy in -- timer
 * jitter, the pointer, the screen, arrival times of packets -- and bytes come
 * out of an XSalsa20 keystream keyed by SHA-512 of everything fed so far,
 * rekeyed after every request so an old state cannot be walked back.
 */
#ifndef RNG_H
#define RNG_H

#include <stddef.h>
#include <stdint.h>

void rng_add(const void *data, size_t n);
void rng_bytes(void *out, size_t n);
uint32_t rng_u32(void);

#endif
