/* The main screen, read straight out of video memory.
 *
 * Classic Mac OS lets an application read the framebuffer of the main
 * GDevice directly. Changes are found by comparing it, a macroblock at a time,
 * against a copy of what was last encoded -- exact, where MiniVNC's row and
 * column checksums are cheaper but blind to some changes. A VBL-time checksum
 * pass can replace it later; the rest of the pipeline only sees the dirty map.
 *
 * The pointer on most Mac video hardware is drawn into VRAM by software, so it
 * is part of the picture (DisplayInfo.cursor_embedded).
 */
#ifndef CDV_SCREEN_H
#define CDV_SCREEN_H

#include "../core/yuv.h"

#include <Multiverse.h>
#include <stdint.h>

typedef struct {
    int width, height, depth, rowbytes;
    uint8_t *base;
    long ctseed;
    yuv_clut clut;
    yuv_fb fb;

    uint8_t *shadow;          /* rowbytes * height: the pixels last encoded */
    uint8_t *Y, *U, *V;       /* the whole frame, kept current */
    int ystride, uvstride;
    uint8_t *dirty;           /* one byte per macroblock */
    int mbw, mbh;
    int band;                 /* next macroblock row for the rolling refresh */
} cdv_screen;

/* Size up the main screen and allocate for it. */
OSErr screen_open(cdv_screen *s);
void screen_close(cdv_screen *s);

/* Nonzero if the main screen's size or depth is no longer what was opened. */
int screen_geometry_changed(const cdv_screen *s);

/* Nonzero if the colour table changed; the table is rebuilt, and every
 * macroblock is marked for conversion. */
int screen_palette_changed(cdv_screen *s);

/* Compare, mark, and convert what changed. With `refresh` one extra row of
 * macroblocks is converted and marked whether or not it changed, which heals
 * rounding left behind by earlier frames. `all` marks everything (keyframes).
 * Returns how many macroblocks are marked. */
int screen_scan(cdv_screen *s, int refresh, int all);

#endif
