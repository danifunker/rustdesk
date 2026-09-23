#include "screen.h"

#include <string.h>

/* Low-memory byte: nonzero when the machine is running in 32-bit mode. */
#define LM_MMU32BIT (*(volatile char *)0x0CB2)

static PixMapHandle main_pixmap(void)
{
    GDHandle gd = GetMainDevice();
    return (**gd).gdPMap;
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
            rgb[i][0] = (uint8_t)(c->red >> 8);
            rgb[i][1] = (uint8_t)(c->green >> 8);
            rgb[i][2] = (uint8_t)(c->blue >> 8);
        }
        s->ctseed = (**ct).ctSeed;
    }
    yuv_clut_build(dst, (const uint8_t(*)[3])rgb, n);
}

OSErr screen_open(cdv_screen *s)
{
    PixMapHandle pm = main_pixmap();
    long planes, luma;
    memset(s, 0, sizeof *s);
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

    s->shadow = (uint8_t *)NewPtrClear((long)s->rowbytes * s->height);
    s->Y = (uint8_t *)NewPtrClear(planes);
    s->dirty = (uint8_t *)NewPtrClear((long)s->mbw * s->mbh * 2);
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
    return noErr;
}

void screen_close(cdv_screen *s)
{
    if (s->shadow)
        DisposePtr((Ptr)s->shadow);
    if (s->Y)
        DisposePtr((Ptr)s->Y);
    if (s->dirty)
        DisposePtr((Ptr)s->dirty);
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

    /* VRAM may sit above the 24-bit address space. */
    if (!LM_MMU32BIT) {
        SwapMMUMode((Byte *)&mode);
        swapped = 1;
    }
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
    if (swapped)
        SwapMMUMode((Byte *)&mode);
    return count;
}
