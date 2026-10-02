/* Encoder benchmark on the target, without X in the way.
 *
 *   encbench grab FILE            dump the screen (8-bit + colormap) to FILE
 *   encbench run FILE [q] [n]     keyframe, then n frames of the xterm
 *                                 region scrolling up a 13-pixel line
 *
 * Link with cc -p and run under prof(1) to see where the encoder's time goes
 * on this CPU. The region is the scrolling test's xterm: 80x30 at +150+120. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "vp8enc.h"
#include "yuv.h"

static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec / 1e6; }

#define W 1024
#define H 768

static int grab(const char *path)
{
    Display *d = XOpenDisplay(":0");
    XImage *im;
    XColor xc[256];
    uint8_t rgb[256][3];
    FILE *f;
    int i;
    if (!d)
        return 1;
    im = XGetImage(d, DefaultRootWindow(d), 0, 0, W, H, AllPlanes, ZPixmap);
    for (i = 0; i < 256; i++)
        xc[i].pixel = i;
    XQueryColors(d, DefaultColormap(d, 0), xc, 256);
    for (i = 0; i < 256; i++) {
        rgb[i][0] = xc[i].red >> 8;
        rgb[i][1] = xc[i].green >> 8;
        rgb[i][2] = xc[i].blue >> 8;
    }
    f = fopen(path, "wb");
    for (i = 0; i < H; i++)
        fwrite(im->data + i * im->bytes_per_line, 1, W, f);
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t fb[W * H], rgb[256][3];
    static uint8_t Y[W * H], U[W * H / 4], V[W * H / 4], out[1 << 20], dirty[64 * 48];
    yuv_clut clut;
    vp8e *e;
    void *mem;
    FILE *f;
    int q, n, i, x0 = 160, y0 = 150, x1 = 660, y1 = 560, mx, my, nd = 0;
    double t0, t1, tot = 0;
    size_t len, bytes = 0;
    if (argc >= 3 && !strcmp(argv[1], "grab"))
        return grab(argv[2]);
    if (argc < 3)
        return 2;
    q = argc > 3 ? atoi(argv[3]) : 24;
    n = argc > 4 ? atoi(argv[4]) : 8;
    f = fopen(argv[2], "rb");
    if (!f || fread(fb, 1, sizeof fb, f) != sizeof fb || fread(rgb, 1, sizeof rgb, f) != sizeof rgb)
        return 1;
    fclose(f);
    yuv_clut_build(&clut, (const uint8_t (*)[3])rgb, 256);
    yuv_from_indexed(&clut, fb, W, W, H, Y, U, V, W, W / 2, NULL);
    mem = malloc(vp8e_mem_size(W, H) + 16);
    e = vp8e_init((void *)(((unsigned long)mem + 15) & ~15ul), W, H, q);
    if (!getenv("NOHINTS"))
        vp8e_set_ref(e, malloc(vp8e_ref_size(W, H)));
    t0 = now();
    len = vp8e_encode(e, &(vp8e_src){ Y, U, V, W, W / 2 }, NULL, 1, out, sizeof out);
    t1 = now();
    printf("keyframe q=%d: %.0f ms, %lu bytes\n", q, (t1 - t0) * 1000, (unsigned long)len);
    for (my = y0 / 16; my < (y1 + 15) / 16; my++)
        for (mx = x0 / 16; mx < (x1 + 15) / 16; mx++) {
            dirty[my * 64 + mx] = 1;
            nd++;
        }
    for (i = 0; i < n; i++) {
        int y;
        vp8e_stats st;
        static vp8e_hint hints[64 * 48];
        /* scroll the region up by one 13-pixel text line; the bottom line
         * comes round from the top, so there is always text */
        static uint8_t line[13 * W];
        for (y = 0; y < 13; y++)
            memcpy(line + y * W, fb + (y0 + y) * W, W);
        for (y = y0; y < y1 - 13; y++)
            memcpy(fb + y * W + x0, fb + (y + 13) * W + x0, x1 - x0);
        for (y = 0; y < 13; y++)
            memcpy(fb + (y1 - 13 + y) * W + x0, line + y * W + x0, x1 - x0);
        yuv_from_indexed(&clut, fb, W, W, H, Y, U, V, W, W / 2, dirty);
        for (my = 0; my < 48; my++)
            for (mx = 0; mx < 64; mx++)
                if (mx * 16 >= x0 && mx * 16 + 16 <= x1 && my * 16 >= y0 && my * 16 + 16 <= y1)
                    hints[my * 64 + mx].dy = -13;
        vp8e_set_hints(e, hints);
        t0 = now();
        len = vp8e_encode(e, &(vp8e_src){ Y, U, V, W, W / 2 }, dirty, 0, out, sizeof out);
        t1 = now();
        vp8e_last_stats(e, &st);
        tot += t1 - t0;
        bytes += len;
        printf("scroll %d: %d mbs dirty, %d intra, %d moved: %.0f ms, %lu bytes\n", i, nd, st.intra, st.moved,
               (t1 - t0) * 1000, (unsigned long)len);
    }
    printf("mean %.0f ms per frame, %.2f ms per macroblock, %lu bytes per frame\n",
           tot * 1000 / n, tot * 1000 / n / nd, (unsigned long)(bytes / n));
    return 0;
}
