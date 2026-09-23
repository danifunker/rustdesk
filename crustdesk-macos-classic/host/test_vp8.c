/* Check VP8-lite against the real decoder.
 *
 * Every frame is decoded by libvpx, and the picture it produces must equal the
 * encoder's own reconstruction byte for byte. That is the property the whole
 * design rests on: an unchanged macroblock is never re-sent, so the encoder's
 * idea of what the peer shows has to be exactly right, or errors accumulate
 * on screen for ever.
 *
 * The frames are synthetic desktops -- flat fills, windows, text-like strokes,
 * a gradient -- then a run of small edits of the kind a session produces:
 * a menu opening, a caret blinking, a window dragged.
 */
#include "../src/core/vp8enc.h"

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEST_W
#define TEST_W 640
#endif
#ifndef TEST_H
#define TEST_H 480
#endif
#define W TEST_W
#define H TEST_H
#define CW ((W + 1) / 2)
#define CH ((H + 1) / 2)

static uint8_t Y[W * H], U[CW * CH], V[CW * CH];
static uint32_t rng = 12345;

static uint32_t rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

/* Paint an RGB rectangle into the I420 planes, BT.601 studio range -- the same
 * conversion the agent uses. */
static void rect(int x0, int y0, int w, int h, int r, int g, int b)
{
    int x, y;
    int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    int uu = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    int vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    for (y = y0; y < y0 + h && y < H; y++)
        for (x = x0; x < x0 + w && x < W; x++) {
            Y[y * W + x] = (uint8_t)yy;
            if (!(x & 1) && !(y & 1)) {
                U[(y / 2) * CW + x / 2] = (uint8_t)uu;
                V[(y / 2) * CW + x / 2] = (uint8_t)vv;
            }
        }
}

/* Black strokes on white, roughly the density of 9-point Geneva. */
static void text(int x0, int y0, int w, int lines)
{
    int l, x;
    for (l = 0; l < lines; l++)
        for (x = x0; x < x0 + w; x += 1 + (int)(rnd() % 3)) {
            int hgt = 3 + (int)(rnd() % 7), k;
            if (rnd() % 5 == 0) {
                x += 4;
                continue;
            }
            for (k = 0; k < hgt; k++)
                rect(x, y0 + l * 12 + 9 - k, 1, 1, 0, 0, 0);
        }
}

static void window(int x, int y, int w, int h)
{
    rect(x + 2, y + 2, w, h, 0, 0, 0);          /* shadow */
    rect(x, y, w, h, 0, 0, 0);                  /* frame */
    rect(x + 1, y + 1, w - 2, 18, 204, 204, 255); /* title bar */
    rect(x + 1, y + 20, w - 2, h - 21, 255, 255, 255);
    text(x + 6, y + 24, w - 16, (h - 30) / 12);
}

static void desktop(void)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            /* a dithered desktop pattern */
            int on = ((x ^ y) & 3) == 0;
            rect(x, y, 1, 1, on ? 64 : 102, on ? 64 : 102, on ? 160 : 204);
        }
    rect(0, 0, W, 20, 255, 255, 255); /* menu bar */
    text(10, 4, 300, 1);
    window(40, 60, 360, 260);
    window(220, 180, 380, 240);
    for (x = 0; x < 200; x++) /* a gradient, the worst case for flat prediction */
        rect(420 + x / 2, 40, 1, 100, x, 255 - x, 128);
}

static double psnr(const uint8_t *a, int as, const uint8_t *b, int bs, int w, int h)
{
    double se = 0;
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int d = a[y * as + x] - b[y * bs + x];
            se += d * d;
        }
    if (se == 0)
        return 99.0;
    return 10.0 * log10(255.0 * 255.0 * w * h / se);
}

static int compare_plane(const char *name, const uint8_t *dec, int ds, const uint8_t *rec, int rs,
                         int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (dec[y * ds + x] != rec[y * rs + x]) {
                printf("  MISMATCH %s at (%d,%d): decoder %d, encoder %d\n", name, x, y,
                       dec[y * ds + x], rec[y * rs + x]);
                return 1;
            }
    return 0;
}

int main(int argc, char **argv)
{
    int q = argc > 1 ? atoi(argv[1]) : 16;
    size_t memsz = vp8e_mem_size(W, H), cap = 4 * 1024 * 1024;
    void *mem = malloc(memsz);
    uint8_t *out = malloc(cap);
    vp8e *e = vp8e_init(mem, W, H, q);
    vpx_codec_ctx_t dec;
    vp8e_src src = { Y, U, V, W, CW };
    uint8_t dirty[((W + 15) / 16) * ((H + 15) / 16)];
    int f, fails = 0, mbw = vp8e_mb_cols(e), mbh = vp8e_mb_rows(e);
    size_t total = 0;

    if (vpx_codec_dec_init(&dec, vpx_codec_vp8_dx(), NULL, 0)) {
        printf("cannot open libvpx VP8 decoder: %s\n", vpx_codec_error(&dec));
        return 2;
    }
    desktop();

    for (f = 0; f < 40; f++) {
        int key = f == 0 || f == 25, i;
        size_t n;
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img;
        const uint8_t *ry, *ru, *rv;
        int rys, ruvs, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        vp8e_stats st;

        /* This frame's edit, and the macroblocks it touches. */
        memset(dirty, 0, sizeof dirty);
        if (f > 0) {
            switch (f % 5) {
            case 1: x0 = 10; y0 = 20; x1 = 170; y1 = 200; /* a menu drops down */
                rect(x0, y0, x1 - x0, y1 - y0, 255, 255, 255);
                text(x0 + 8, y0 + 4, 140, 14);
                break;
            case 2: x0 = 300; y0 = 250; x1 = 302; y1 = 262; /* the caret blinks */
                rect(x0, y0, 1, 12, (f / 5) & 1 ? 0 : 255, (f / 5) & 1 ? 0 : 255,
                     (f / 5) & 1 ? 0 : 255);
                break;
            case 3: x0 = 220 + f; y0 = 180; x1 = x0 + 382; y1 = 422; /* a window drags */
                if (x1 > W) x1 = W;
                if (x0 >= W) x0 = W - 16;
                window(x0, y0, 380, 240);
                x0 -= 8;
                break;
            case 4: x0 = 60; y0 = 90; x1 = 380; y1 = 300; /* text scrolls */
                rect(x0, y0, x1 - x0, y1 - y0, 255, 255, 255);
                text(x0 + 4, y0 + 4, 300, 16);
                break;
            default: x0 = 0; y0 = 0; x1 = W; y1 = 20; /* the clock in the menu bar */
                rect(560, 2, 70, 16, 255, 255, 255);
                text(565, 4, 60, 1);
                break;
            }
            if (x0 < 0) x0 = 0;
            for (i = 0; i < mbw * mbh; i++) {
                int mx = i % mbw, my = i / mbw;
                if (mx * 16 < x1 && mx * 16 + 16 > x0 && my * 16 < y1 && my * 16 + 16 > y0)
                    dirty[i] = 1;
            }
            /* The rolling refresh the agent will do: a band a frame. */
            for (i = 0; i < mbw; i++)
                dirty[(f % mbh) * mbw + i] = 1;
        }

        if (getenv("SLICE")) { /* the way the Mac drives it: a few rows at a time */
            n = 0;
            if (vp8e_begin(e, &src, f ? dirty : NULL, key, out, cap)) {
                while (!vp8e_rows(e, atoi(getenv("SLICE"))))
                    ;
                n = vp8e_end(e);
            }
        } else {
            n = vp8e_encode(e, &src, f ? dirty : NULL, key, out, cap);
        }
        if (!n) {
            printf("frame %d: encoder overflow\n", f);
            return 1;
        }
        total += n;
        vp8e_last_stats(e, &st);
        if (vpx_codec_decode(&dec, out, (unsigned)n, NULL, 0)) {
            printf("frame %d: libvpx rejected it: %s (%s)\n", f, vpx_codec_error(&dec),
                   vpx_codec_error_detail(&dec) ? vpx_codec_error_detail(&dec) : "");
            return 1;
        }
        img = vpx_codec_get_frame(&dec, &it);
        if (!img) {
            printf("frame %d: no picture\n", f);
            return 1;
        }
        vp8e_recon(e, &ry, &ru, &rv, &rys, &ruvs);
        fails += compare_plane("Y", img->planes[0], img->stride[0], ry, rys, W, H);
        fails += compare_plane("U", img->planes[1], img->stride[1], ru, ruvs, CW, CH);
        fails += compare_plane("V", img->planes[2], img->stride[2], rv, ruvs, CW, CH);
        printf("frame %2d %s %7zu bytes  skip %4d intra %4d  PSNR-Y %.2f dB\n", f,
               key ? "key  " : "inter", n, st.skipped, st.intra,
               psnr(img->planes[0], img->stride[0], Y, W, W, H));
    }
    vpx_codec_destroy(&dec);
    printf("%s: %d frames, %zu bytes, q=%d\n", fails ? "FAIL" : "PASS", f, total, q);
    return fails ? 1 : 0;
}
