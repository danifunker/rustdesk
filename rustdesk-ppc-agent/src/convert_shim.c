/* ARGB -> I420, in C so it gets a real optimiser.
 *
 * This is a line-for-line port of `argb_to_i420_rows` in src/convert.rs, and it
 * exists for one reason: mrustc emits C compiled at -O1, with Rust's bounds
 * checks still in the inner loop. The Rust version measures ~185-206 ms for a
 * 1920x1080 frame; hand-written C at -O2 did luma-only from RAM in 14 ms. The
 * same file compiled here gets the C toolchain's own -O2 and -maltivec.
 *
 * The Rust version stays as the reference implementation and is what the host
 * tests exercise. `--probe-display` runs both over a real frame and compares
 * the planes byte for byte, because "faster" is worthless if it is also wrong,
 * and the failure mode here -- swapped red and blue -- is exactly the one
 * src/convert.rs was written to avoid.
 *
 * Memory order is A,R,G,B. Byte 0 is alpha and is never read. BT.601 studio
 * swing, chroma averaged over each 2x2 block:
 *
 *   Y = ( 66R + 129G +  25B + 128) >> 8 +  16
 *   U = (-38R -  74G + 112B + 128) >> 8 + 128
 *   V = (112R -  94G -  18B + 128) >> 8 + 128
 */
#include <stddef.h>

static inline unsigned char clamp_u8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (unsigned char)v;
}

/* Convert rows [y0, y1) only, leaving the rest of the destination alone.
 *
 * src_len is passed so this can refuse rather than read off the end: the
 * caller's geometry can go stale if the display mode changes mid-frame, and a
 * segfault is a worse outcome than a skipped frame.
 */
void rd_argb_to_i420_rows(const unsigned char *src, size_t src_len, int stride,
                          unsigned char *yp, unsigned char *up, unsigned char *vp,
                          int width, int height, int chroma_stride,
                          int y0, int y1)
{
    int by, bx;

    if (!src || !yp || !up || !vp || width < 2 || height < 2 || stride < width * 4)
        return;

    y0 &= ~1;
    y1 = (y1 + 1) & ~1;
    if (y1 > height)
        y1 = height;
    if (y0 >= y1)
        return;
    if (src_len < (size_t)stride * (size_t)y1)
        return;

    for (by = y0 / 2; by < y1 / 2; by++) {
        const unsigned char *row0 = src + (size_t)(by * 2) * stride;
        const unsigned char *row1 = row0 + stride;
        unsigned char *ya = yp + (size_t)(by * 2) * width;
        unsigned char *yb = ya + width;
        unsigned char *ua = up + (size_t)by * chroma_stride;
        unsigned char *va = vp + (size_t)by * chroma_stride;

        for (bx = 0; bx < width / 2; bx++) {
            const unsigned char *p0 = row0 + (size_t)bx * 8;
            const unsigned char *p1 = row1 + (size_t)bx * 8;
            int r0 = p0[1], g0 = p0[2], b0 = p0[3];
            int r1 = p0[5], g1 = p0[6], b1 = p0[7];
            int r2 = p1[1], g2 = p1[2], b2 = p1[3];
            int r3 = p1[5], g3 = p1[6], b3 = p1[7];
            int r, g, b;

            ya[bx * 2]     = clamp_u8(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
            ya[bx * 2 + 1] = clamp_u8(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);
            yb[bx * 2]     = clamp_u8(((66 * r2 + 129 * g2 + 25 * b2 + 128) >> 8) + 16);
            yb[bx * 2 + 1] = clamp_u8(((66 * r3 + 129 * g3 + 25 * b3 + 128) >> 8) + 16);

            /* Integer division by four, matching the Rust exactly -- all four
             * inputs are non-negative, so there is no rounding difference to
             * worry about between the two. */
            r = (r0 + r1 + r2 + r3) / 4;
            g = (g0 + g1 + g2 + g3) / 4;
            b = (b0 + b1 + b2 + b3) / 4;

            ua[bx] = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            va[bx] = clamp_u8((( 112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}
