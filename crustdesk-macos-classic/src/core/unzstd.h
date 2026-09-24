/* zstd frames in, bytes out: what a client compresses (file blocks,
 * clipboard text). A thin wrapper over third_party/zstd-decoder. It
 * allocates (a ~100 KB context, once), so on the Mac it is main loop only. */
#ifndef CDV_UNZSTD_H
#define CDV_UNZSTD_H

#include <stddef.h>

/* Decompress src[0..n) into dst[0..cap): the length, or -1 if it is not a
 * zstd frame, is damaged, or does not fit. */
long cdv_unzstd(void *dst, size_t cap, const void *src, size_t n);

#endif
