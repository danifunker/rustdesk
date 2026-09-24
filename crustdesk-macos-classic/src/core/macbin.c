#include "macbin.h"

#include <string.h>

/* CRC-16/XMODEM (CCITT, 0x1021, from 0), which both formats use. */
uint16_t macbin_crc(uint16_t crc, const uint8_t *d, size_t n)
{
    size_t i;
    int j;
    for (i = 0; i < n; i++) {
        crc ^= (uint16_t)(d[i] << 8);
        for (j = 0; j < 8; j++)
            crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint32_t macbin_pad(uint32_t n)
{
    return (n + 127) & ~127u;
}

uint32_t macbin_size(uint32_t dlen, uint32_t rlen)
{
    return 128 + macbin_pad(dlen) + macbin_pad(rlen);
}

void macbin_encode_header(uint8_t h[128], const macbin_info *i)
{
    size_t n = strlen(i->name);
    uint16_t crc;
    if (n > 63)
        n = 63;
    memset(h, 0, 128);
    h[1] = (uint8_t)n;
    memcpy(h + 2, i->name, n);
    be32(h + 65, i->type);
    be32(h + 69, i->creator);
    h[73] = (uint8_t)(i->flags >> 8);
    h[101] = (uint8_t)i->flags;
    be32(h + 83, i->dlen);
    be32(h + 87, i->rlen);
    be32(h + 91, i->created);
    be32(h + 95, i->modified);
    h[122] = 129; /* written as MacBinary II */
    h[123] = 129; /* ...and readable as it */
    crc = macbin_crc(0, h, 124);
    h[124] = (uint8_t)(crc >> 8);
    h[125] = (uint8_t)crc;
}

int macbin_decode_header(const uint8_t h[128], macbin_info *i)
{
    uint16_t crc = (uint16_t)(h[124] << 8 | h[125]);
    int n = h[1];
    /* Byte 0 and 74 must be zero; a name of 1..63; forks of sane size. */
    if (h[0] || h[74] || n < 1 || n > 63 || get32(h + 83) > 0x7FFFFF || get32(h + 87) > 0x7FFFFF)
        return 0;
    /* MacBinary II carries a CRC; I does not, and has zeros where II's
     * extras are (byte 82 zero in both). */
    if (h[82])
        return 0;
    if (h[123] >= 129 && crc != macbin_crc(0, h, 124))
        return 0;
    memset(i, 0, sizeof *i);
    memcpy(i->name, h + 2, (size_t)n);
    i->name[n] = 0;
    i->type = get32(h + 65);
    i->creator = get32(h + 69);
    i->flags = (uint16_t)(h[73] << 8 | (h[123] >= 129 ? h[101] : 0));
    i->dlen = get32(h + 83);
    i->rlen = get32(h + 87);
    i->created = get32(h + 91);
    i->modified = get32(h + 95);
    return 1;
}

/* ---- BinHex 4.0 --------------------------------------------------------------------- */

static const char alphabet[] =
    "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
static const char marker[] = "(This file must be converted with BinHex";

enum { BH_MARKER, BH_COLON, BH_DATA, BH_END };

void binhex_init(binhex_dec *b, void (*header)(void *, const macbin_info *),
                 void (*out)(void *, int, const uint8_t *, size_t), void *u)
{
    memset(b, 0, sizeof *b);
    b->header = header;
    b->out = out;
    b->u = u;
    b->rle_prev = -1;
    b->hdr_need = 1; /* the name's length first */
    b->marker_at = marker;
}

/* One decoded (and run-length expanded) byte. */
static void byte(binhex_dec *b, uint8_t c)
{
    switch (b->part) {
    case 0: /* header: len, name, 0, type, creator, flags, dlen, rlen, crc */
        if (b->hdr_len < (int)sizeof b->hdr)
            b->hdr[b->hdr_len] = c;
        b->hdr_len++;
        if (b->hdr_len == 1)
            b->hdr_need = 1 + c + 1 + 4 + 4 + 2 + 4 + 4 + 2;
        if (b->hdr_len == b->hdr_need) {
            int n = b->hdr[0];
            const uint8_t *p = b->hdr + 1 + n + 1;
            uint16_t crc = macbin_crc(0, b->hdr, (size_t)b->hdr_need - 2);
            if (n > 63 || crc != (uint16_t)(p[18] << 8 | p[19])) {
                b->error = 1;
                b->part = 5;
                return;
            }
            memset(&b->info, 0, sizeof b->info);
            memcpy(b->info.name, b->hdr + 1, (size_t)n);
            b->info.type = get32(p);
            b->info.creator = get32(p + 4);
            b->info.flags = (uint16_t)(p[8] << 8 | p[9]);
            b->info.dlen = get32(p + 10);
            b->info.rlen = get32(p + 14);
            if (b->header)
                b->header(b->u, &b->info);
            b->part = 1;
            b->left = b->info.dlen;
            b->crc = 0;
        }
        return;
    case 1: /* data fork */
    case 3: /* resource fork */
        b->crc = macbin_crc(b->crc, &c, 1);
        if (b->out)
            b->out(b->u, b->part == 1 ? 0 : 1, &c, 1);
        b->left--;
        return;
    case 2:
    case 4: /* a fork's CRC. (BinHex's own routine appends two zero bytes to
             * a bit-by-bit CRC; the direct form here gives the same value.) */
        b->want = (uint16_t)(b->want << 8 | c);
        if (++b->crc_bytes == 2) {
            if (b->crc != b->want) {
                b->error = 1;
                b->part = 5;
                return;
            }
            b->crc_bytes = 0;
            b->want = 0;
            b->crc = 0;
            b->part++;
            if (b->part == 3)
                b->left = b->info.rlen;
        }
        return;
    default:
        return;
    }
}

/* Parts with nothing in them go by at once. */
static void settle(binhex_dec *b)
{
    while ((b->part == 1 || b->part == 3) && b->left == 0)
        b->part++;
}

static void decoded(binhex_dec *b, uint8_t c)
{
    /* Run-length: 0x90 n repeats the previous byte to n in all; 0x90 0 is
     * a literal 0x90. */
    if (b->rle_pending) {
        b->rle_pending = 0;
        if (c == 0) {
            b->rle_prev = 0x90;
            settle(b);
            byte(b, 0x90);
        } else {
            int k;
            for (k = 1; k < c && b->rle_prev >= 0; k++) {
                settle(b);
                byte(b, (uint8_t)b->rle_prev);
            }
        }
        return;
    }
    if (c == 0x90) {
        b->rle_pending = 1;
        return;
    }
    b->rle_prev = c;
    settle(b);
    byte(b, c);
    settle(b);
}

void binhex_feed(binhex_dec *b, const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i < n && b->part < 5; i++) {
        uint8_t c = d[i];
        if (b->state == BH_MARKER) {
            /* "(This file must be converted with BinHex 4.0)" first. */
            if (c == (uint8_t)*b->marker_at) {
                if (!*++b->marker_at)
                    b->state = BH_COLON;
            } else {
                b->marker_at = marker + (c == (uint8_t)marker[0]);
            }
        } else if (b->state == BH_COLON) {
            if (c == ':')
                b->state = BH_DATA;
        } else if (b->state == BH_DATA) {
            const char *p;
            if (c == ':') {
                b->state = BH_END;
                break;
            }
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
                continue;
            p = strchr(alphabet, c);
            if (!p || !c) {
                b->error = 1;
                b->part = 5;
                break;
            }
            b->bits = b->bits << 6 | (uint32_t)(p - alphabet);
            b->nbits += 6;
            if (b->nbits >= 8) {
                b->nbits -= 8;
                decoded(b, (uint8_t)(b->bits >> b->nbits));
            }
        }
    }
}

int binhex_done(const binhex_dec *b)
{
    if (b->error)
        return -1;
    return b->part == 5 ? 1 : 0;
}
