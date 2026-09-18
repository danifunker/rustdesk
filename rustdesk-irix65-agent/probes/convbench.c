/*
 * convbench.c -- how fast the capture shim turns the canvas into I420, and
 * whether a variant can do it faster with the same bytes out.
 *
 * The fused ABGR->I420 kernel (capture_shim.c, rd_abgr_to_i420_rect) runs at
 * roughly 90 ns a source pixel on the O2's R10000 at 1/2 scale -- about
 * eighteen cycles for three byte loads and some adds -- so it waits on memory,
 * not arithmetic. This measures, on the machine:
 *
 *   read      the bandwidth ceiling: one 32-bit load per pixel and a sum
 *   shipped   rd_abgr_to_i420_rect as the agent calls it, factors 1 and 2
 *   words     factor 2 with one 32-bit load per source pixel, the channels
 *             summed two to a register (B and R in 16-bit lanes, G alone)
 *   words+pf  the same, prefetching each source row ahead with MIPS IV's
 *             `pref` (inline asm: clang drops __builtin_prefetch on MIPS). A
 *             MIPS III build has no pref and runs these rows without it.
 *
 * Every variant is checked against the shipped kernel's planes byte for byte.
 *
 *     convbench [frames]
 *
 * Build it both ways -- the MIPS IV compiler is toolchain.sh's sgug-mips4:
 *
 *     irix-cc -O2 -o convbench probes/convbench.c src/capture_shim.c -lXext -lX11 -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sgidefs.h>

#if defined(_MIPS_ISA) && _MIPS_ISA == _MIPS_ISA_MIPS4
#define PREFETCH(p) __asm__ volatile("pref 0, 0(%0)" : : "r"(p))
#else
#define PREFETCH(p) ((void)(p))
#endif

int rd_abgr_to_i420_rect(const unsigned char *src, size_t src_len, int src_stride,
                         unsigned char *yp, unsigned char *up, unsigned char *vp,
                         int dst_w, int dst_h, int chroma_stride, int factor,
                         int dx0, int dy0, int dx1, int dy1);

#define W 1280
#define H 1024
#define STRIDE (W * 4)

static unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
#define Y_OF(r, g, b, sh) \
    clamp8(((((66 * (r) + 129 * (g) + 25 * (b)) >> (sh)) + 128) >> 8) + 16)
#define U_OF(r, g, b, sh) \
    clamp8(((((-38 * (r) - 74 * (g) + 112 * (b)) >> (sh)) + 128) >> 8) + 128)
#define V_OF(r, g, b, sh) \
    clamp8(((((112 * (r) - 94 * (g) - 18 * (b)) >> (sh)) + 128) >> 8) + 128)

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

#define BR(w) ((w) & 0x00ff00ffu)
#define GG(w) (((w) >> 8) & 0xffu)

/* Factor 2, whole frame, one load per source pixel. Big-endian ABGR words are
 * A<<24 | B<<16 | G<<8 | R, so w & 0x00ff00ff holds B and R in separate 16-bit
 * lanes: four of them sum without carrying (4 x 255), and so do sixteen. */
static void f2_words(const unsigned char *src, unsigned char *yp, unsigned char *up,
                     unsigned char *vp, int dst_w, int dst_h, int pf)
{
    int by, bx;
    const int cs = dst_w / 2;
    for (by = 0; by < dst_h / 2; by++) {
        const unsigned int *s0 = (const unsigned int *)(src + (size_t)(by * 4) * STRIDE);
        const unsigned int *s1 = s0 + STRIDE / 4;
        const unsigned int *s2 = s1 + STRIDE / 4;
        const unsigned int *s3 = s2 + STRIDE / 4;
        unsigned char *ya = yp + (size_t)(by * 2) * dst_w;
        unsigned char *yb = ya + dst_w;
        unsigned char *ua = up + (size_t)by * cs;
        unsigned char *va = vp + (size_t)by * cs;
        for (bx = 0; bx < dst_w / 2; bx++) {
            const int o = bx * 4;
            unsigned int br0, br1, br2, br3, g0, g1, g2, g3;
            if (pf && (bx & 3) == 0) {
                /* 64 bytes a row every fourth block, `pf` bytes ahead. */
                PREFETCH((const char *)(s0 + o) + pf);
                PREFETCH((const char *)(s1 + o) + pf);
                PREFETCH((const char *)(s2 + o) + pf);
                PREFETCH((const char *)(s3 + o) + pf);
            }
            {
                unsigned int a = s0[o], b = s0[o + 1], c = s1[o], d = s1[o + 1];
                br0 = BR(a) + BR(b) + BR(c) + BR(d);
                g0 = GG(a) + GG(b) + GG(c) + GG(d);
            }
            {
                unsigned int a = s0[o + 2], b = s0[o + 3], c = s1[o + 2], d = s1[o + 3];
                br1 = BR(a) + BR(b) + BR(c) + BR(d);
                g1 = GG(a) + GG(b) + GG(c) + GG(d);
            }
            {
                unsigned int a = s2[o], b = s2[o + 1], c = s3[o], d = s3[o + 1];
                br2 = BR(a) + BR(b) + BR(c) + BR(d);
                g2 = GG(a) + GG(b) + GG(c) + GG(d);
            }
            {
                unsigned int a = s2[o + 2], b = s2[o + 3], c = s3[o + 2], d = s3[o + 3];
                br3 = BR(a) + BR(b) + BR(c) + BR(d);
                g3 = GG(a) + GG(b) + GG(c) + GG(d);
            }
            ya[bx * 2]     = Y_OF((int)(br0 & 0xffff), (int)g0, (int)(br0 >> 16), 2);
            ya[bx * 2 + 1] = Y_OF((int)(br1 & 0xffff), (int)g1, (int)(br1 >> 16), 2);
            yb[bx * 2]     = Y_OF((int)(br2 & 0xffff), (int)g2, (int)(br2 >> 16), 2);
            yb[bx * 2 + 1] = Y_OF((int)(br3 & 0xffff), (int)g3, (int)(br3 >> 16), 2);
            {
                unsigned int br = br0 + br1 + br2 + br3, g = g0 + g1 + g2 + g3;
                int r = (int)(br & 0xffff), bl = (int)(br >> 16);
                ua[bx] = U_OF(r, (int)g, bl, 4);
                va[bx] = V_OF(r, (int)g, bl, 4);
            }
        }
    }
}

/* Factor 1, whole frame: each 2x2 source block is four Y and one U and V. */
static void f1_words(const unsigned char *src, unsigned char *yp, unsigned char *up,
                     unsigned char *vp, int pf)
{
    int by, bx;
    for (by = 0; by < H / 2; by++) {
        const unsigned int *s0 = (const unsigned int *)(src + (size_t)(by * 2) * STRIDE);
        const unsigned int *s1 = s0 + STRIDE / 4;
        unsigned char *ya = yp + (size_t)(by * 2) * W;
        unsigned char *yb = ya + W;
        unsigned char *ua = up + (size_t)by * (W / 2);
        unsigned char *va = vp + (size_t)by * (W / 2);
        for (bx = 0; bx < W / 2; bx++) {
            const int o = bx * 2;
            unsigned int a, b, c, d, br, g;
            if (pf && (bx & 7) == 0) {
                PREFETCH((const char *)(s0 + o) + pf);
                PREFETCH((const char *)(s1 + o) + pf);
            }
            a = s0[o]; b = s0[o + 1]; c = s1[o]; d = s1[o + 1];
            ya[o]     = Y_OF((int)(a & 0xff), (int)((a >> 8) & 0xff), (int)((a >> 16) & 0xff), 0);
            ya[o + 1] = Y_OF((int)(b & 0xff), (int)((b >> 8) & 0xff), (int)((b >> 16) & 0xff), 0);
            yb[o]     = Y_OF((int)(c & 0xff), (int)((c >> 8) & 0xff), (int)((c >> 16) & 0xff), 0);
            yb[o + 1] = Y_OF((int)(d & 0xff), (int)((d >> 8) & 0xff), (int)((d >> 16) & 0xff), 0);
            br = BR(a) + BR(b) + BR(c) + BR(d);
            g = GG(a) + GG(b) + GG(c) + GG(d);
            ua[bx] = U_OF((int)(br & 0xffff), (int)g, (int)(br >> 16), 2);
            va[bx] = V_OF((int)(br & 0xffff), (int)g, (int)(br >> 16), 2);
        }
    }
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 8;
    unsigned char *src = malloc((size_t)STRIDE * H);
    size_t ysz2 = (size_t)(W / 2) * (H / 2), csz2 = ysz2 / 4;
    size_t ysz1 = (size_t)W * H, csz1 = ysz1 / 4;
    unsigned char *ref = malloc(ysz2 + 2 * csz2), *out = malloc(ysz2 + 2 * csz2);
    unsigned char *full = malloc(ysz1 + 2 * csz1);
    unsigned int seed = 1, sum = 0;
    size_t i;
    int f, pfs[] = { 0, 128, 256, 512 }, k;
    double t, best;

    if (!src || !ref || !out || !full) { fprintf(stderr, "no memory\n"); return 1; }
    /* A desktop's worth of structure: flat runs, edges, some noise. */
    for (i = 0; i < (size_t)STRIDE * H; i += 4) {
        size_t px = i / 4, x = px % W, y = px / W;
        seed = seed * 1103515245u + 12345u;
        src[i] = 0xff;
        src[i + 1] = (unsigned char)((x / 40 + y / 30) % 2 ? 200 : (seed >> 16));
        src[i + 2] = (unsigned char)((x ^ y) & 0xff);
        src[i + 3] = (unsigned char)((x * 3 + y) & 0xff);
    }

#define TIME(label, stmt) do {                                           \
        best = 1e9;                                                       \
        for (f = 0; f < frames; f++) {                                    \
            t = now_ms(); stmt; t = now_ms() - t; if (t < best) best = t; \
        }                                                                 \
        printf("%-22s %7.1f ms   %5.1f ns/source pixel\n", label, best,    \
               best * 1e6 / ((double)W * H));                             \
    } while (0)

    TIME("read (bandwidth)", {
        const unsigned int *p = (const unsigned int *)src;
        for (i = 0; i < (size_t)W * H; i++) sum += p[i];
    });
    TIME("shipped, factor 1", rd_abgr_to_i420_rect(src, (size_t)STRIDE * H, STRIDE,
        full, full + ysz1, full + ysz1 + csz1, W, H, W / 2, 1, 0, 0, W, H));
    TIME("shipped, factor 2", rd_abgr_to_i420_rect(src, (size_t)STRIDE * H, STRIDE,
        ref, ref + ysz2, ref + ysz2 + csz2, W / 2, H / 2, W / 4, 2, 0, 0, W / 2, H / 2));
    for (k = 0; k < 4; k++) {
        char label[32];
        if (pfs[k] == 0)
            strcpy(label, "words, factor 2");
        else
            sprintf(label, "words+pf %d", pfs[k]);
        memset(out, 0, ysz2 + 2 * csz2);
        TIME(label, f2_words(src, out, out + ysz2, out + ysz2 + csz2, W / 2, H / 2, pfs[k]));
        if (memcmp(out, ref, ysz2 + 2 * csz2) != 0) {
            printf("   ^ DIFFERS from the shipped kernel\n");
            return 1;
        }
    }
    {
        unsigned char *f1 = malloc(ysz1 + 2 * csz1);
        int pf1[] = { 0, 256, 512 };
        if (!f1) return 1;
        for (k = 0; k < 3; k++) {
            char label[32];
            sprintf(label, pf1[k] ? "f1 words+pf %d" : "f1 words", pf1[k]);
            memset(f1, 0, ysz1 + 2 * csz1);
            TIME(label, f1_words(src, f1, f1 + ysz1, f1 + ysz1 + csz1, pf1[k]));
            if (memcmp(f1, full, ysz1 + 2 * csz1) != 0) {
                printf("   ^ DIFFERS from the shipped kernel\n");
                return 1;
            }
        }
    }
    /* Rectangles, the way the agent converts damage: change random regions of
     * the source, convert only those through the shim, and the planes must
     * equal a full reference conversion of the changed source -- which checks
     * the shim's offsets for dx0/dy0, not just a whole frame. */
    {
        int fac, n;
        unsigned char *want = malloc(ysz1 + 2 * csz1), *have = malloc(ysz1 + 2 * csz1);
        if (!want || !have) return 1;
        for (fac = 1; fac <= 2; fac++) {
            int dw = W / fac, dh = H / fac;
            size_t ys = (size_t)dw * dh, cs = ys / 4;
            if (fac == 2) f2_words(src, have, have + ys, have + ys + cs, dw, dh, 0);
            else f1_words(src, have, have + ys, have + ys + cs, 0);
            for (n = 0; n < 200; n++) {
                int x, y, rw, rh, xx, yy;
                seed = seed * 1103515245u + 12345u; x = (int)((seed >> 8) % (W - 64));
                seed = seed * 1103515245u + 12345u; y = (int)((seed >> 8) % (H - 64));
                seed = seed * 1103515245u + 12345u; rw = 1 + (int)((seed >> 8) % 63);
                seed = seed * 1103515245u + 12345u; rh = 1 + (int)((seed >> 8) % 63);
                for (yy = y; yy < y + rh; yy++)
                    for (xx = x; xx < x + rw; xx++) {
                        unsigned char *px = src + (size_t)yy * STRIDE + (size_t)xx * 4;
                        px[1] ^= (unsigned char)(n * 7); px[2] += 13; px[3] ^= 0x5a;
                    }
                /* The session's rounding: framebuffer box to destination pixels, outward. */
                rd_abgr_to_i420_rect(src, (size_t)STRIDE * H, STRIDE, have, have + ys, have + ys + cs,
                    dw, dh, dw / 2, fac, x / fac, y / fac,
                    (x + rw + fac - 1) / fac, (y + rh + fac - 1) / fac);
            }
            if (fac == 2) f2_words(src, want, want + ys, want + ys + cs, dw, dh, 0);
            else f1_words(src, want, want + ys, want + ys + cs, 0);
            if (memcmp(want, have, ys + 2 * cs) != 0) {
                printf("rectangles, factor %d: DIFFER from a full conversion\n", fac);
                return 1;
            }
            printf("rectangles, factor %d: 200 converted, identical to a full conversion\n", fac);
        }
    }
    printf("all variants byte-identical to the shipped kernel (sum %u)\n", sum);
    return 0;
}
