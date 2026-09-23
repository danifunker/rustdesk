/* Mac framebuffer pixels to I420, BT.601 studio range -- the conversion the
 * other vintage agents use (rustdesk-ppc-agent/src/convert.rs).
 *
 * Every depth a Mac Monitors panel offers:
 *   1, 2, 4, 8 bits  indexed through the device's colour table, MSB-first
 *   16 bits          xRRRRRGG GGGBBBBB, big-endian
 *   32 bits          xRGB, big-endian
 * Indexed depths go through a 256-entry lookup built whenever the colour table
 * changes, so a palette screen costs three table reads a pixel.
 *
 * Pixels are read as bytes, so the same code is right on the Mac and on the
 * little-endian machines the tests run on.
 */
#ifndef YUV_H
#define YUV_H

#include <stdint.h>

typedef struct {
    uint8_t y[256], u[256], v[256];
} yuv_clut;

/* rgb: n entries of 8-bit R, G, B. Missing entries become black. */
void yuv_clut_build(yuv_clut *c, const uint8_t (*rgb)[3], int n);

typedef struct {
    const uint8_t *base;   /* the framebuffer's first row */
    int rowbytes;
    int depth;             /* 1, 2, 4, 8, 16 or 32 */
    int width, height;
    const yuv_clut *clut;  /* indexed depths only */
} yuv_fb;

/* Convert the macroblocks marked in `dirty` (all of them if NULL) into the
 * planes. Planes are at least width x height (chroma half, rounded up). */
void yuv_convert(const yuv_fb *fb, uint8_t *Y, uint8_t *U, uint8_t *V, int ystride,
                 int uvstride, const uint8_t *dirty);

/* The same, for macroblock rows [my0, my1) only. */
void yuv_convert_rows(const yuv_fb *fb, uint8_t *Y, uint8_t *U, uint8_t *V, int ystride,
                      int uvstride, const uint8_t *dirty, int my0, int my1);

/* 8-bit shorthand used by the host tests. */
void yuv_from_indexed(const yuv_clut *c, const uint8_t *fb, int rowbytes, int w, int h,
                      uint8_t *Y, uint8_t *U, uint8_t *V, int ystride, int uvstride,
                      const uint8_t *dirty);

#endif
