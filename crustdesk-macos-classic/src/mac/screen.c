#include "screen.h"

#include <string.h>

/* Low-memory byte: nonzero when the machine is running in 32-bit mode. */
#define LM_MMU32BIT (*(volatile char *)0x0CB2)

static PixMapHandle main_pixmap(void)
{
    GDHandle gd = GetMainDevice();
    return (**gd).gdPMap;
}

static void read_palette(cdv_screen *s)
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
    yuv_clut_build(&s->clut, (const uint8_t(*)[3])rgb, n);
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
    s->dirty = (uint8_t *)NewPtrClear((long)s->mbw * s->mbh);
    if (!s->shadow || !s->Y || !s->dirty) {
        screen_close(s);
        return memFullErr;
    }
    s->U = s->Y + luma;
    s->V = s->U + (long)s->uvstride * s->mbh * 8;
    read_palette(s);

    s->fb.base = s->base;
    s->fb.rowbytes = s->rowbytes;
    s->fb.depth = s->depth;
    s->fb.width = s->width;
    s->fb.height = s->height;
    s->fb.clut = &s->clut;
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
    s->shadow = s->Y = s->dirty = NULL;
}

int screen_geometry_changed(const cdv_screen *s)
{
    PixMapHandle pm = main_pixmap();
    return (**pm).pixelSize != s->depth ||
           (**pm).bounds.right - (**pm).bounds.left != s->width ||
           (**pm).bounds.bottom - (**pm).bounds.top != s->height ||
           (uint8_t *)(**pm).baseAddr != s->base;
}

int screen_palette_changed(cdv_screen *s)
{
    PixMapHandle pm = main_pixmap();
    CTabHandle ct = (**pm).pmTable;
    if (s->depth > 8 || !ct || (**ct).ctSeed == s->ctseed)
        return 0;
    read_palette(s);
    return 1;
}

int screen_scan(cdv_screen *s, int refresh, int all)
{
    int bytes = s->depth * 16 / 8; /* one macroblock's width in bytes */
    int mx, my, count = 0;
    char mode = 1; /* true32b */
    int swapped = 0;

    /* VRAM may sit above the 24-bit address space. */
    if (!LM_MMU32BIT) {
        SwapMMUMode((Byte *)&mode);
        swapped = 1;
    }

    for (my = 0; my < s->mbh; my++) {
        int y0 = my * 16, rows = s->height - y0 < 16 ? s->height - y0 : 16;
        int band = refresh && my == s->band;
        for (mx = 0; mx < s->mbw; mx++) {
            long off = (long)y0 * s->rowbytes + (long)mx * bytes;
            int n = mx == s->mbw - 1 ? s->rowbytes - mx * bytes : bytes, y, d = all || band;
            if (n > bytes)
                n = bytes;
            for (y = 0; y < rows && !d; y++)
                d = memcmp(s->base + off + (long)y * s->rowbytes,
                           s->shadow + off + (long)y * s->rowbytes, (size_t)n) != 0;
            s->dirty[my * s->mbw + mx] = (uint8_t)d;
            if (d) {
                for (y = 0; y < rows; y++)
                    memcpy(s->shadow + off + (long)y * s->rowbytes,
                           s->base + off + (long)y * s->rowbytes, (size_t)n);
                count++;
            }
        }
    }
    if (count)
        yuv_convert(&s->fb, s->Y, s->U, s->V, s->ystride, s->uvstride, s->dirty);
    if (swapped)
        SwapMMUMode((Byte *)&mode);
    if (refresh)
        s->band = (s->band + 1) % s->mbh;
    return count;
}
