/* Motion vectors against the real decoder.
 *
 * An indexed desktop (the shape of an 8-bit screen), converted with the
 * agent's own converter, then scrolled, dragged and shifted by odd and even
 * amounts in both directions, with hints saying so -- and some hints that
 * lie. libvpx must decode every frame to exactly the encoder's
 * reconstruction (a vector the encoder and the decoder disagree about would
 * smear the peer's screen for ever), and scrolls must actually use vectors.
 */
#include "../src/core/vp8enc.h"
#include "../src/core/yuv.h"

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 480
#define CW (W / 2)
#define CH (H / 2)
#define MBW (W / 16)
#define MBH (H / 16)

static uint8_t fb[W * H], Y[W * H], U[CW * CH], V[CW * CH];
static uint32_t rng = 777;
static uint32_t rnd(void) { return (rng = rng * 1103515245u + 12345u) >> 8; }

enum { WHITE, BLACK, BLUE, GREY, LIGHT, RED };

static void rect(int x0, int y0, int w, int h, int c)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if (x >= 0 && y >= 0 && x < W && y < H)
                fb[y * W + x] = (uint8_t)c;
}

static void text(int x0, int y0, int w, int lines)
{
    int l, x, k;
    for (l = 0; l < lines; l++)
        for (x = x0; x < x0 + w; x += 1 + (int)(rnd() % 3)) {
            int hgt = 3 + (int)(rnd() % 7);
            if (rnd() % 5 == 0) {
                x += 4;
                continue;
            }
            for (k = 0; k < hgt; k++)
                rect(x, y0 + l * 13 + 10 - k, 1, 1, BLACK);
        }
}

/* Move the rectangle's contents by (dx, dy) within the screen, as CopyArea. */
static void copy_area(int x, int y, int w, int h, int dx, int dy)
{
    static uint8_t tmp[W * H];
    int j;
    for (j = 0; j < h; j++)
        memcpy(tmp + j * w, fb + (y + j) * W + x, (size_t)w);
    for (j = 0; j < h; j++)
        if (y + dy + j >= 0 && y + dy + j < H)
            memcpy(fb + (y + dy + j) * W + x + dx, tmp + j * w, (size_t)w);
}

static int compare_plane(const char *name, int f, const uint8_t *dec, int ds, const uint8_t *rec,
                         int rs, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (dec[y * ds + x] != rec[y * rs + x]) {
                printf("  frame %d: MISMATCH %s at (%d,%d): decoder %d, encoder %d\n", f, name, x,
                       y, dec[y * ds + x], rec[y * rs + x]);
                return 1;
            }
    return 0;
}

int main(int argc, char **argv)
{
    static const uint8_t pal[6][3] = { { 255, 255, 255 }, { 0, 0, 0 }, { 40, 60, 200 },
                                       { 128, 128, 128 }, { 160, 190, 230 }, { 220, 30, 30 } };
    uint8_t rgb[256][3];
    yuv_clut clut;
    int q = argc > 1 ? atoi(argv[1]) : 24, f, fails = 0, use_hints = !getenv("NOHINTS");
    size_t cap = 4 << 20, total = 0;
    void *mem = malloc(vp8e_mem_size(W, H)), *ref = malloc(vp8e_ref_size(W, H));
    uint8_t *out = malloc(cap), dirty[MBW * MBH];
    vp8e_hint hints[MBW * MBH];
    vp8e *e = vp8e_init(mem, W, H, q);
    vpx_codec_ctx_t dec;
    vp8e_src src = { Y, U, V, W, CW };
    int moved_total = 0;

    memset(rgb, 0, sizeof rgb);
    memcpy(rgb, pal, sizeof pal);
    yuv_clut_build(&clut, (const uint8_t (*)[3])rgb, 6);
    vp8e_set_ref(e, ref);
    if (vpx_codec_dec_init(&dec, vpx_codec_vp8_dx(), NULL, 0))
        return 2;
    rect(0, 0, W, H, LIGHT);
    rect(40, 40, 400, 330, GREY);
    rect(44, 64, 392, 302, WHITE);
    text(50, 70, 370, 22);
    rect(470, 100, 140, 200, BLUE);
    text(480, 110, 110, 14);

    for (f = 0; f < 16; f++) {
        /* what moved this frame: a rectangle, and by how much */
        int x = 0, y = 0, w = 0, h = 0, dx = 0, dy = 0, lie = 0, i;
        size_t n;
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img;
        const uint8_t *ry, *ru, *rv;
        int rys, ruvs;
        vp8e_stats st;
        memset(dirty, 0, sizeof dirty);
        memset(hints, 0, sizeof hints);
        switch (f % 8) {
        case 1: case 2: /* a terminal scrolls up a 13-pixel line */
            x = 44; y = 64; w = 392; h = 302; dy = -13;
            copy_area(x, y + 13, w, h - 13, 0, -13);
            rect(x, y + h - 13, w, 13, WHITE);
            text(50, y + h - 13, 370, 1);
            break;
        case 3: /* and back down 12 */
            x = 44; y = 64; w = 392; h = 302; dy = 12;
            copy_area(x, y, w, h - 12, 0, 12);
            rect(x, y, w, 12, WHITE);
            text(50, y, 370, 1);
            break;
        case 4: /* a list scrolls a whole 16 */
            x = 470; y = 100; w = 140; h = 200; dy = -16;
            copy_area(x, y + 16, w, h - 16, 0, -16);
            rect(x, y + h - 16, w, 16, BLUE);
            break;
        case 5: /* sideways by an odd amount */
            x = 44; y = 64; w = 392; h = 302; dx = -7;
            copy_area(x + 7, y, w - 7, h, -7, 0);
            rect(x + w - 7, y, 7, h, WHITE);
            break;
        case 6: /* a window dragged diagonally */
            x = 470; y = 100; w = 140; h = 200; dx = -23; dy = 9;
            copy_area(x, y, w, h, dx, dy);
            rect(x + w + dx, y, -dx, h, LIGHT); /* what it uncovered */
            rect(x, y, w, dy, LIGHT);
            x += dx; y += dy;
            break;
        case 7: /* a hint that lies: nothing moved, a few lines changed */
            x = 44; y = 64; w = 392; h = 120; dy = -13; lie = 1;
            rect(50, 90, 300, 26, WHITE);
            text(50, 90, 300, 2);
            break;
        default: /* nothing moves, the clock ticks */
            rect(470, 20, 100, 16, WHITE);
            text(472, 22, 90, 1);
            x = 470; y = 20; w = 100; h = 16;
            break;
        }
        for (i = 0; i < MBW * MBH; i++) {
            int mx = (i % MBW) * 16, my = (i / MBW) * 16;
            int x0 = x - (dx < 0 ? 0 : dx) - 16, x1 = x + w + (dx > 0 ? dx : -dx) + 16;
            int y0 = y - 16 - (dy > 0 ? dy : -dy), y1 = y + h + 16 + (dy > 0 ? dy : -dy);
            if (mx + 16 > x0 && mx < x1 && my + 16 > y0 && my < y1)
                dirty[i] = 1;
            /* hints for macroblocks inside the moved area (and, with `lie`,
             * a wrong one) -- including some at the edges, whose source is
             * partly outside: the encoder must refuse those */
            if ((dx || dy) && mx >= x - 8 && mx + 16 <= x + w + 8 && my >= y - 8 &&
                my + 16 <= y + h + 8) {
                hints[i].dx = (short)dx;
                hints[i].dy = (short)dy;
            }
        }
        (void)lie;
        yuv_from_indexed(&clut, fb, W, W, H, Y, U, V, W, CW, f ? dirty : NULL);
        if (use_hints)
            vp8e_set_hints(e, hints);
        n = vp8e_encode(e, &src, f ? dirty : NULL, f == 0, out, cap);
        if (!n) {
            printf("frame %d: overflow\n", f);
            return 1;
        }
        total += n;
        vp8e_last_stats(e, &st);
        moved_total += st.moved;
        if (vpx_codec_decode(&dec, out, (unsigned)n, NULL, 0)) {
            printf("frame %d: libvpx rejected it: %s\n", f, vpx_codec_error(&dec));
            return 1;
        }
        img = vpx_codec_get_frame(&dec, &it);
        vp8e_recon(e, &ry, &ru, &rv, &rys, &ruvs);
        fails += compare_plane("Y", f, img->planes[0], img->stride[0], ry, rys, W, H);
        fails += compare_plane("U", f, img->planes[1], img->stride[1], ru, ruvs, CW, CH);
        fails += compare_plane("V", f, img->planes[2], img->stride[2], rv, ruvs, CW, CH);
        printf("frame %2d %6zu bytes  intra %3d  moved %3d  (dx %d dy %d)\n", f, n, st.intra,
               st.moved, dx, dy);
    }
    vpx_codec_destroy(&dec);
    if (use_hints && moved_total < 100) {
        printf("FAIL: only %d macroblocks used a vector\n", moved_total);
        fails++;
    }
    printf("%s: %d frames, %zu bytes, %d moved, q=%d\n", fails ? "FAIL" : "PASS", f, total,
           moved_total, q);
    return fails ? 1 : 0;
}
