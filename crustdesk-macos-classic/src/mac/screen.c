#include "screen.h"
#include "mem.h"

#include <string.h>

/* Low-memory byte: nonzero when the machine is running in 32-bit mode. */
#define LM_MMU32BIT (*(volatile char *)0x0CB2)

static PixMapHandle main_pixmap(void)
{
    GDHandle gd = GetMainDevice();
    return (**gd).gdPMap;
}

/* The driver's current gamma table (cscGetGamma), as three 8-bit tables.
 * Identity if the driver will not say. */
static void read_gamma(cdv_screen *s)
{
    struct {
        short gVersion, gType, gFormulaSize, gChanCnt, gDataCnt, gDataWidth;
    } *g;
    CntrlParam pb;
    Ptr rec = NULL; /* VDGammaRecord: the driver writes csGTable here */
    int ch, i;
    for (ch = 0; ch < 3; ch++)
        for (i = 0; i < 256; i++)
            s->gamma[ch][i] = (uint8_t)i;
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = s->refnum;
    pb.csCode = 8; /* cscGetGamma: csParam points at a VDGammaRecord */
    {
        Ptr *recp = &rec;
        memcpy(pb.csParam, &recp, sizeof recp);
    }
    s->gamma_err = PBStatusSync((ParmBlkPtr)&pb);
    if (s->gamma_err != noErr)
        return;
    g = (void *)rec;
    if (g) {
        s->gamma_info[0] = g->gChanCnt;
        s->gamma_info[1] = g->gDataCnt;
        s->gamma_info[2] = g->gDataWidth;
        s->gamma_info[3] = g->gFormulaSize;
    }
    if (!g || g->gDataCnt != 256 || g->gDataWidth != 8 || (g->gChanCnt != 1 && g->gChanCnt != 3))
        return;
    {
        const uint8_t *data = (const uint8_t *)(g + 1) + g->gFormulaSize;
        for (ch = 0; ch < 3; ch++)
            memcpy(s->gamma[ch], data + (g->gChanCnt == 3 ? ch * 256 : 0), 256);
    }
}

static void build_palette(cdv_screen *s, yuv_clut *dst)
{
    PixMapHandle pm = main_pixmap();
    CTabHandle ct = (**pm).pmTable;
    static uint8_t rgb[256][3];
    int i, n = 0;
    if (ct && s->depth <= 8) {
        n = (**ct).ctSize + 1;
        if (n > 256)
            n = 256;
        for (i = 0; i < n; i++) {
            /* On a device's table, entry i is pixel value i. */
            const RGBColor *c = &(**ct).ctTable[i].rgb;
            rgb[i][0] = s->gamma[0][c->red >> 8];
            rgb[i][1] = s->gamma[1][c->green >> 8];
            rgb[i][2] = s->gamma[2][c->blue >> 8];
        }
        s->ctseed = (**ct).ctSeed;
    }
    yuv_clut_build(dst, (const uint8_t(*)[3])rgb, n);
}

/* ---- for screenshots (main loop) ----------------------------------------------- */

int screen_palette(cdv_screen *s, uint8_t pal[256][3])
{
    PixMapHandle pm = main_pixmap();
    CTabHandle ct = (**pm).pmTable;
    int i, n = 0;
    if (s->depth > 8 || !ct)
        return 0;
    n = (**ct).ctSize + 1;
    if (n > 256)
        n = 256;
    for (i = 0; i < n; i++) {
        const RGBColor *c = &(**ct).ctTable[i].rgb;
        pal[i][0] = s->gamma[0][c->red >> 8];
        pal[i][1] = s->gamma[1][c->green >> 8];
        pal[i][2] = s->gamma[2][c->blue >> 8];
    }
    return n;
}

void screen_read_row(cdv_screen *s, int y, uint8_t *dst)
{
    const uint8_t *src = s->base + (long)y * s->rowbytes;
    int x, w = s->width;
#if !(defined(__powerpc__) || defined(__ppc__))
    char mode = 1; /* true32b */
    int swapped = 0;
    if (!LM_MMU32BIT) {
        SwapMMUMode((Byte *)&mode);
        swapped = 1;
    }
#endif
    switch (s->depth) {
    case 1: case 2: case 4: {
        int d = s->depth, per = 8 / d, mask = (1 << d) - 1;
        for (x = 0; x < w; x++)
            dst[x] = (uint8_t)((src[x / per] >> ((per - 1 - x % per) * d)) & mask);
        break;
    }
    case 8:
        memcpy(dst, src, (size_t)w);
        break;
    case 16:
        for (x = 0; x < w; x++) {
            unsigned v = (unsigned)src[2 * x] << 8 | src[2 * x + 1];
            unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
            dst[3 * x] = s->gamma[0][(r << 3) | (r >> 2)];
            dst[3 * x + 1] = s->gamma[1][(g << 3) | (g >> 2)];
            dst[3 * x + 2] = s->gamma[2][(b << 3) | (b >> 2)];
        }
        break;
    default: /* 32: xRGB */
        for (x = 0; x < w; x++) {
            dst[3 * x] = s->gamma[0][src[4 * x + 1]];
            dst[3 * x + 1] = s->gamma[1][src[4 * x + 2]];
            dst[3 * x + 2] = s->gamma[2][src[4 * x + 3]];
        }
        break;
    }
#if !(defined(__powerpc__) || defined(__ppc__))
    if (swapped)
        SwapMMUMode((Byte *)&mode);
#endif
}

OSErr screen_open(cdv_screen *s, int use_gamma)
{
    PixMapHandle pm = main_pixmap();
    long planes, luma;
    memset(s, 0, sizeof *s);
    s->refnum = (**GetMainDevice()).gdRefNum;
    read_gamma(s);
    if (!use_gamma) {
        int ch, i;
        for (ch = 0; ch < 3; ch++)
            for (i = 0; i < 256; i++)
                s->gamma[ch][i] = (uint8_t)i;
    }
    s->width = (**pm).bounds.right - (**pm).bounds.left;
    s->height = (**pm).bounds.bottom - (**pm).bounds.top;
    s->depth = (**pm).pixelSize;
    s->rowbytes = (**pm).rowBytes & 0x3FFF;
    s->base = (uint8_t *)(**pm).baseAddr;
    s->mbw = (s->width + 15) / 16;
    s->mbh = (s->height + 15) / 16;
    s->ystride = s->mbw * 16;
    s->uvstride = s->mbw * 8;
    luma = (long)s->ystride * s->mbh * 16;
    planes = luma + 2L * s->uvstride * s->mbh * 8;

    s->shadow = (uint8_t *)big_alloc((long)s->rowbytes * s->height);
    s->Y = (uint8_t *)big_alloc(planes);
    s->dirty = (uint8_t *)big_alloc((long)s->mbw * s->mbh * 2);
    if (!s->shadow || !s->Y || !s->dirty) {
        screen_close(s);
        return memFullErr;
    }
    s->stale = s->dirty + (long)s->mbw * s->mbh;
    s->U = s->Y + luma;
    s->V = s->U + (long)s->uvstride * s->mbh * 8;
    build_palette(s, &s->clut[0]);
    s->clut_cur = 0;

    s->fb.base = s->base;
    s->fb.rowbytes = s->rowbytes;
    s->fb.depth = s->depth;
    s->fb.width = s->width;
    s->fb.height = s->height;
    s->fb.clut = &s->clut[0];
    s->fb.gamma = s->depth > 8 ? &s->gamma[0][0] : NULL;
    return noErr;
}

void screen_close(cdv_screen *s)
{
    if (s->shadow)
        big_free(s->shadow);
    if (s->Y)
        big_free(s->Y);
    if (s->dirty)
        big_free(s->dirty);
    s->shadow = s->Y = s->dirty = s->stale = NULL;
}

int screen_geometry_changed(const cdv_screen *s)
{
    PixMapHandle pm = main_pixmap();
    return (**pm).pixelSize != s->depth ||
           (**pm).bounds.right - (**pm).bounds.left != s->width ||
           (**pm).bounds.bottom - (**pm).bounds.top != s->height ||
           (uint8_t *)(**pm).baseAddr != s->base;
}

void screen_check_palette(cdv_screen *s)
{
    PixMapHandle pm = main_pixmap();
    CTabHandle ct = (**pm).pmTable;
    int spare;
    if (s->depth > 8 || !ct || (**ct).ctSeed == s->ctseed)
        return;
    spare = !s->clut_cur;
    build_palette(s, &s->clut[spare]);
    s->clut_cur = spare;
    s->clut_seq++;
}

/* Which macroblock columns of one scanline differ from the shadow, as a
 * bit per column in `diff`. Longwords, compared in place: a memcmp call per
 * 16-pixel run was most of the cost of a scan on a 68040. */
static void diff_line(const uint32_t *a, const uint32_t *b, int longs, int longs_per_mb,
                      uint8_t *diff)
{
    int i = 0, mx = 0;
    while (i < longs) {
        int end = i + longs_per_mb;
        if (end > longs)
            end = longs;
        if (!diff[mx]) {
            for (; i < end; i++)
                if (a[i] != b[i]) {
                    diff[mx] = 1;
                    break;
                }
        }
        i = end;
        mx++;
    }
}

int screen_scan_rows(cdv_screen *s, int my0, int my1, int band, int all, const uint8_t *exact)
{
    int bytes = s->depth * 16 / 8; /* one macroblock's width in bytes */
    int mx, my, count = 0;
    char mode = 1; /* true32b */
    int swapped = 0;
    /* A macroblock is 2 bytes wide at 1 bit, 4 at 2: compare those depths a
     * longword (several macroblocks) at a time and mark each one it covers. */
    int longs = s->rowbytes / 4, lpm = bytes >= 4 ? bytes / 4 : 1;
    int mbs_per_long = bytes >= 4 ? 1 : 4 / bytes;

#if !(defined(__powerpc__) || defined(__ppc__))
    /* VRAM may sit above the 24-bit address space. */
    if (!LM_MMU32BIT) {
        SwapMMUMode((Byte *)&mode);
        swapped = 1;
    }
#endif
    s->fb.clut = &s->clut[s->clut_cur];

    for (my = my0; my < my1; my++) {
        int y0 = my * 16, rows = s->height - y0 < 16 ? s->height - y0 : 16;
        int n0 = count, y;
        uint8_t *d = s->dirty + my * s->mbw, *st = s->stale + my * s->mbw;
        static uint8_t cols[1024];

        if (all) {
            memset(d, 1, (size_t)s->mbw);
            memset(st, 1, (size_t)s->mbw);
        } else {
            memset(cols, 0, (size_t)(longs / lpm + 1));
            for (y = 0; y < rows; y++) {
                long off = (long)(y0 + y) * s->rowbytes;
                diff_line((const uint32_t *)(s->base + off), (const uint32_t *)(s->shadow + off),
                          longs, lpm, cols);
            }
            for (mx = 0; mx < s->mbw; mx++) {
                int changed = cols[mx / mbs_per_long];
                int refine = my == band && st[mx] && !exact[my * s->mbw + mx];
                d[mx] = (uint8_t)(changed || refine);
                st[mx] = (uint8_t)(changed || (st[mx] && my != band));
            }
        }
        for (mx = 0; mx < s->mbw; mx++) {
            if (!d[mx])
                continue;
            {
                long off = (long)y0 * s->rowbytes + (long)mx * bytes;
                int n = mx == s->mbw - 1 ? s->rowbytes - mx * bytes : bytes;
                if (n > bytes)
                    n = bytes;
                for (y = 0; y < rows; y++)
                    memcpy(s->shadow + off + (long)y * s->rowbytes,
                           s->base + off + (long)y * s->rowbytes, (size_t)n);
            }
            count++;
        }
        if (count > n0)
            yuv_convert_rows(&s->fb, s->Y, s->U, s->V, s->ystride, s->uvstride, s->dirty, my,
                             my + 1);
    }
#if !(defined(__powerpc__) || defined(__ppc__))
    if (swapped)
        SwapMMUMode((Byte *)&mode);
#else
    (void)mode;
    (void)swapped;
#endif
    return count;
}
