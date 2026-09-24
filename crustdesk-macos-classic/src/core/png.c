#include "png.h"

#include <string.h>

#define WSIZE 32768u        /* deflate's window */
#define HBITS 13
#define MAXMATCH 258u

struct png_writer {
    uint8_t *out;
    size_t cap, n;
    int over;
    size_t idat_at;          /* where the IDAT chunk's length goes */
    int width, height, bpp, rows;
    uint32_t bits;           /* deflate output, LSB first */
    int nbits;
    uint32_t a, b;           /* Adler-32 */
    size_t have, pos;        /* bytes in buf; the next one to encode */
    int32_t head[1 << HBITS];
    uint8_t buf[2 * WSIZE];
};

size_t png_writer_size(void)
{
    return sizeof(png_writer);
}

/* ---- bytes out -------------------------------------------------------------- */

static void put(png_writer *p, uint8_t c)
{
    if (p->n < p->cap)
        p->out[p->n] = c;
    else
        p->over = 1;
    p->n++;
}

static void put32(png_writer *p, uint32_t v)
{
    put(p, (uint8_t)(v >> 24));
    put(p, (uint8_t)(v >> 16));
    put(p, (uint8_t)(v >> 8));
    put(p, (uint8_t)v);
}

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *d, size_t n)
{
    size_t i;
    if (!crc_table[1]) {
        uint32_t k;
        int j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[k] = v;
        }
    }
    for (i = 0; i < n; i++)
        c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

/* A chunk whose data is already in out[at + 8 .. n): length, then CRC. */
static void chunk_end(png_writer *p, size_t at)
{
    size_t len = p->n - at - 8;
    uint32_t c;
    if (p->over)
        return;
    p->out[at] = (uint8_t)(len >> 24);
    p->out[at + 1] = (uint8_t)(len >> 16);
    p->out[at + 2] = (uint8_t)(len >> 8);
    p->out[at + 3] = (uint8_t)len;
    c = crc(0xFFFFFFFFu, p->out + at + 4, len + 4) ^ 0xFFFFFFFFu;
    put32(p, c);
}

static size_t chunk_begin(png_writer *p, const char *type)
{
    size_t at = p->n;
    put32(p, 0);
    put(p, (uint8_t)type[0]);
    put(p, (uint8_t)type[1]);
    put(p, (uint8_t)type[2]);
    put(p, (uint8_t)type[3]);
    return at;
}

/* ---- deflate: fixed Huffman ------------------------------------------------ */

static void bits(png_writer *p, uint32_t v, int n)
{
    p->bits |= v << p->nbits;
    p->nbits += n;
    while (p->nbits >= 8) {
        put(p, (uint8_t)p->bits);
        p->bits >>= 8;
        p->nbits -= 8;
    }
}

/* Huffman codes go most significant bit first. */
static void code(png_writer *p, uint32_t c, int n)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < n; i++)
        r |= ((c >> i) & 1) << (n - 1 - i);
    bits(p, r, n);
}

static void litlen(png_writer *p, unsigned v)
{
    if (v < 144)
        code(p, 0x30 + v, 8);
    else if (v < 256)
        code(p, 0x190 + (v - 144), 9);
    else if (v < 280)
        code(p, v - 256, 7);
    else
        code(p, 0xC0 + (v - 280), 8);
}

static const uint16_t len_base[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                       15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                       67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1,    2,    3,    4,    5,    7,     9,     13,
                                        17,   25,   33,   49,   65,   97,    129,   193,
                                        257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                        4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void match(png_writer *p, unsigned len, unsigned dist)
{
    int i = 28, d = 29;
    while (len_base[i] > len)
        i--;
    litlen(p, 257 + i);
    bits(p, len - len_base[i], len_extra[i]);
    while (dist_base[d] > dist)
        d--;
    code(p, (uint32_t)d, 5);
    bits(p, dist - dist_base[d], dist_extra[d]);
}

static unsigned hash3(const uint8_t *s)
{
    return ((unsigned)s[0] << 10 ^ (unsigned)s[1] << 5 ^ s[2]) & ((1u << HBITS) - 1);
}

static void insert(png_writer *p, size_t at)
{
    if (at + 3 <= p->have)
        p->head[hash3(p->buf + at)] = (int32_t)at;
}

/* Encode what is buffered, keeping MAXMATCH bytes back unless `all`, so a
 * match is never cut short by a row that has not arrived yet. */
static void encode(png_writer *p, int all)
{
    size_t stop = all ? p->have : (p->have > MAXMATCH ? p->have - MAXMATCH : 0);
    while (p->pos < stop) {
        const uint8_t *s = p->buf + p->pos;
        size_t avail = p->have - p->pos, dist = 0;
        unsigned best = 0;
        if (avail >= 3) {
            unsigned h = hash3(s);
            int32_t cand = p->head[h];
            if (cand >= 0 && p->pos - (size_t)cand <= WSIZE) {
                const uint8_t *c = p->buf + cand;
                unsigned lim = avail < MAXMATCH ? (unsigned)avail : MAXMATCH;
                while (best < lim && c[best] == s[best])
                    best++;
                dist = p->pos - (size_t)cand;
            }
            p->head[h] = (int32_t)p->pos;
        }
        if (best >= 3) {
            size_t k;
            match(p, best, (unsigned)dist);
            for (k = 1; k < best; k++)
                insert(p, p->pos + k);
            p->pos += best;
        } else {
            litlen(p, *s);
            p->pos++;
        }
    }
}

/* Keep the window in the first half of buf once the second half fills. */
static void slide(png_writer *p)
{
    int i;
    memmove(p->buf, p->buf + WSIZE, p->have - WSIZE);
    p->have -= WSIZE;
    p->pos -= WSIZE;
    for (i = 0; i < (1 << HBITS); i++)
        p->head[i] = p->head[i] >= (int32_t)WSIZE ? p->head[i] - (int32_t)WSIZE : -1;
}

static void feed(png_writer *p, const uint8_t *d, size_t n)
{
    while (n) {
        size_t k;
        if (p->have == sizeof p->buf) {
            encode(p, 0);
            slide(p);
        }
        k = sizeof p->buf - p->have;
        if (k > n)
            k = n;
        memcpy(p->buf + p->have, d, k);
        {
            /* Adler-32, reduced every 5552 bytes rather than every byte: the
             * most that cannot overflow 32 bits (zlib's NMAX). */
            size_t i = 0;
            while (i < k) {
                size_t end = k - i > 5552 ? i + 5552 : k;
                for (; i < end; i++) {
                    p->a += d[i];
                    p->b += p->a;
                }
                p->a %= 65521u;
                p->b %= 65521u;
            }
        }
        p->have += k;
        d += k;
        n -= k;
        if (p->have - p->pos > MAXMATCH + 4096)
            encode(p, 0);
    }
}

/* ---- PNG --------------------------------------------------------------------- */

void png_begin(png_writer *p, uint8_t *out, size_t cap, int width, int height,
               const uint8_t (*palette)[3], int npal)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    size_t at;
    int i;
    p->out = out;
    p->cap = cap;
    p->n = 0;
    p->over = 0;
    p->width = width;
    p->height = height;
    p->bpp = palette ? 1 : 3;
    p->rows = 0;
    p->bits = 0;
    p->nbits = 0;
    p->a = 1;
    p->b = 0;
    p->have = p->pos = 0;
    for (i = 0; i < (1 << HBITS); i++)
        p->head[i] = -1;
    for (i = 0; i < 8; i++)
        put(p, sig[i]);
    at = chunk_begin(p, "IHDR");
    put32(p, (uint32_t)width);
    put32(p, (uint32_t)height);
    put(p, 8);                   /* bits per sample (or index) */
    put(p, palette ? 3 : 2);     /* indexed, or truecolour */
    put(p, 0);                   /* deflate */
    put(p, 0);                   /* adaptive filtering */
    put(p, 0);                   /* not interlaced */
    chunk_end(p, at);
    if (palette) {
        at = chunk_begin(p, "PLTE");
        for (i = 0; i < npal; i++) {
            put(p, palette[i][0]);
            put(p, palette[i][1]);
            put(p, palette[i][2]);
        }
        chunk_end(p, at);
    }
    p->idat_at = chunk_begin(p, "IDAT");
    put(p, 0x78);  /* zlib: deflate, 32 KB window */
    put(p, 0x01);
    bits(p, 1, 1); /* the last block */
    bits(p, 1, 2); /* fixed Huffman codes */
}

void png_row(png_writer *p, const uint8_t *row)
{
    static const uint8_t none = 0; /* filter type 0 for every row */
    if (p->rows >= p->height)
        return;
    feed(p, &none, 1);
    feed(p, row, (size_t)p->width * (size_t)p->bpp);
    p->rows++;
}

size_t png_end(png_writer *p)
{
    size_t at;
    encode(p, 1);
    litlen(p, 256); /* end of block */
    if (p->nbits)
        bits(p, 0, 8 - p->nbits);
    put32(p, p->b << 16 | p->a);
    chunk_end(p, p->idat_at);
    at = chunk_begin(p, "IEND");
    chunk_end(p, at);
    return p->over || p->rows != p->height ? 0 : p->n;
}
