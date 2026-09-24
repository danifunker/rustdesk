/* Mac files on a disk that knows only bytes: MacBinary II (the header, which
 * is all there is to it -- the forks follow, each padded to 128 bytes) and
 * BinHex 4.0 (decoding only, streamed).
 *
 * File transfer uses them so a file with a resource fork survives a trip
 * through a modern computer: it leaves the Mac as Name.bin, and a .bin or
 * .hqx that comes back becomes a Mac file again.
 */
#ifndef CDV_MACBIN_H
#define CDV_MACBIN_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[64];            /* Mac Roman */
    uint32_t type, creator;
    uint16_t flags;           /* Finder flags */
    uint32_t dlen, rlen;      /* fork lengths */
    uint32_t created, modified; /* Mac seconds (since 1904) */
} macbin_info;

/* Bytes of the MacBinary file for these fork lengths. */
uint32_t macbin_size(uint32_t dlen, uint32_t rlen);
/* A fork's length rounded up to MacBinary's 128. */
uint32_t macbin_pad(uint32_t n);

void macbin_encode_header(uint8_t h[128], const macbin_info *i);
/* 1 if h is a MacBinary (I or II) header, with *i filled; 0 if not. */
int macbin_decode_header(const uint8_t h[128], macbin_info *i);

/* BinHex 4.0, fed in any pieces; out() gets the header once, then each
 * fork's bytes (fork 0 data, 1 resource). */
typedef struct {
    int state;
    uint32_t bits;
    int nbits;
    int rle_prev, rle_pending;
    uint8_t hdr[64 + 22];
    int hdr_len, hdr_need;
    uint32_t left;            /* bytes left in the current part */
    int part;                 /* 0 header, 1 data, 2 data CRC, 3 rsrc, 4 rsrc CRC, 5 done */
    uint16_t crc, want;
    int crc_bytes;
    int error;                /* a CRC or format mismatch */
    macbin_info info;
    void (*header)(void *u, const macbin_info *i);
    void (*out)(void *u, int fork, const uint8_t *d, size_t n);
    void *u;
    const char *marker_at;    /* matching "(This file must be converted..." */
} binhex_dec;

void binhex_init(binhex_dec *b, void (*header)(void *, const macbin_info *),
                 void (*out)(void *, int, const uint8_t *, size_t), void *u);
void binhex_feed(binhex_dec *b, const uint8_t *d, size_t n);
/* 1 once both forks have arrived with good CRCs; -1 on a bad one. */
int binhex_done(const binhex_dec *b);

uint16_t macbin_crc(uint16_t crc, const uint8_t *d, size_t n);

#endif
