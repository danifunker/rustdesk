/* The main screen, read straight out of video memory.
 *
 * Classic Mac OS lets an application read the framebuffer of the main
 * GDevice directly. Changes are found by comparing it, a macroblock at a time,
 * against a copy of what was last encoded -- exact, where MiniVNC's row and
 * column checksums are cheaper but blind to some changes. A VBL-time checksum
 * pass can replace it later; the rest of the pipeline only sees the dirty map.
 *
 * Two contexts touch this. The main loop opens it, and watches the GDevice for
 * a new size, depth or colour table -- the only parts that read Toolbox
 * handles. The engine, at deferred-task time, scans and converts using only
 * what was cached here. A new colour table is built into the spare of two and
 * then published by bumping clut_seq, so the engine never sees half a table.
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
    yuv_clut clut[2];
    volatile int clut_cur;    /* which of the two is published */
    volatile long clut_seq;   /* bumped on every publish */
    yuv_fb fb;

    uint8_t *shadow;          /* rowbytes * height: the pixels last encoded */
    uint8_t *Y, *U, *V;       /* the whole frame, kept current */
    int ystride, uvstride;
    uint8_t *dirty;           /* one byte per macroblock */
    uint8_t *stale;           /* changed since its refinement pass */
    int mbw, mbh;
} cdv_screen;

/* Main loop only. */
OSErr screen_open(cdv_screen *s);
void screen_close(cdv_screen *s);
int screen_geometry_changed(const cdv_screen *s);
/* Rebuild and publish the colour table if it changed. */
void screen_check_palette(cdv_screen *s);

/* Engine. Compare macroblock rows [my0, my1) and mark what changed, or all of
 * them. In the one row `band`, also mark what changed since that row was last
 * refined and is not already exact: coding it again against the peer's
 * picture heals most of the rounding the first pass left, once. Marked
 * macroblocks are copied into the shadow and converted. Returns how many. */
int screen_scan_rows(cdv_screen *s, int my0, int my1, int band, int all, const uint8_t *exact);

#endif
