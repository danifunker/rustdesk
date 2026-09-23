#include "yuv.h"

#include <string.h>

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static uint8_t to_y(int r, int g, int b) { return clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }
static uint8_t to_u(int r, int g, int b) { return clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); }
static uint8_t to_v(int r, int g, int b) { return clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128); }

void yuv_clut_build(yuv_clut *c, const uint8_t (*rgb)[3], int n)
{
    int i;
    for (i = 0; i < 256; i++) {
        int r = 0, g = 0, b = 0;
        if (i < n) {
            r = rgb[i][0];
            g = rgb[i][1];
            b = rgb[i][2];
        }
        c->y[i] = to_y(r, g, b);
        c->u[i] = to_u(r, g, b);
        c->v[i] = to_v(r, g, b);
    }
}

/* One row segment [x0, x1) as luma, and chroma per pixel for averaging. */
static void row_yuv(const yuv_fb *fb, int y, int x0, int x1, uint8_t *yo, uint8_t *uo,
                    uint8_t *vo)
{
    const uint8_t *row = fb->base + (long)y * fb->rowbytes;
    int x;
    switch (fb->depth) {
    case 8:
        for (x = x0; x < x1; x++) {
            int i = row[x];
            *yo++ = fb->clut->y[i];
            *uo++ = fb->clut->u[i];
            *vo++ = fb->clut->v[i];
        }
        break;
    case 1:
    case 2:
    case 4: {
        int d = fb->depth, per = 8 / d, mask = (1 << d) - 1;
        for (x = x0; x < x1; x++) {
            int i = (row[x / per] >> ((per - 1 - x % per) * d)) & mask;
            *yo++ = fb->clut->y[i];
            *uo++ = fb->clut->u[i];
            *vo++ = fb->clut->v[i];
        }
        break;
    }
    case 16:
        for (x = x0; x < x1; x++) {
            int p = row[2 * x] << 8 | row[2 * x + 1];
            int r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
            r = r << 3 | r >> 2;
            g = g << 3 | g >> 2;
            b = b << 3 | b >> 2;
            *yo++ = to_y(r, g, b);
            *uo++ = to_u(r, g, b);
            *vo++ = to_v(r, g, b);
        }
        break;
    default: /* 32 */
        for (x = x0; x < x1; x++) {
            const uint8_t *p = row + 4 * x;
            *yo++ = to_y(p[1], p[2], p[3]);
            *uo++ = to_u(p[1], p[2], p[3]);
            *vo++ = to_v(p[1], p[2], p[3]);
        }
        break;
    }
}

void yuv_convert(const yuv_fb *fb, uint8_t *Y, uint8_t *U, uint8_t *V, int ys, int uvs,
                 const uint8_t *dirty)
{
    yuv_convert_rows(fb, Y, U, V, ys, uvs, dirty, 0, (fb->height + 15) / 16);
}

void yuv_convert_rows(const yuv_fb *fb, uint8_t *Y, uint8_t *U, uint8_t *V, int ys, int uvs,
                      const uint8_t *dirty, int my0, int my1)
{
    int mbw = (fb->width + 15) / 16, mx, my;
    uint8_t u0[16], v0[16], u1[16], v1[16];
    for (my = my0; my < my1; my++)
        for (mx = 0; mx < mbw; mx++) {
            int x0 = mx * 16, y0 = my * 16, x1 = x0 + 16, y1 = y0 + 16, y;
            if (dirty && !dirty[my * mbw + mx])
                continue;
            if (x1 > fb->width)
                x1 = fb->width;
            if (y1 > fb->height)
                y1 = fb->height;
            for (y = y0; y < y1; y += 2) {
                int n = x1 - x0, x, has2 = y + 1 < y1;
                row_yuv(fb, y, x0, x1, Y + (long)y * ys + x0, u0, v0);
                if (has2)
                    row_yuv(fb, y + 1, x0, x1, Y + (long)(y + 1) * ys + x0, u1, v1);
                else {
                    memcpy(u1, u0, (size_t)n);
                    memcpy(v1, v0, (size_t)n);
                }
                for (x = 0; x < n; x += 2) {
                    int x2 = x + 1 < n ? x + 1 : x;
                    long o = (long)(y / 2) * uvs + (x0 + x) / 2;
                    U[o] = (uint8_t)((u0[x] + u0[x2] + u1[x] + u1[x2] + 2) >> 2);
                    V[o] = (uint8_t)((v0[x] + v0[x2] + v1[x] + v1[x2] + 2) >> 2);
                }
            }
        }
}

void yuv_from_indexed(const yuv_clut *c, const uint8_t *fbp, int rowbytes, int w, int h,
                      uint8_t *Y, uint8_t *U, uint8_t *V, int ys, int uvs, const uint8_t *dirty)
{
    yuv_fb fb;
    fb.base = fbp;
    fb.rowbytes = rowbytes;
    fb.depth = 8;
    fb.width = w;
    fb.height = h;
    fb.clut = c;
    yuv_convert(&fb, Y, U, V, ys, uvs, dirty);
}
