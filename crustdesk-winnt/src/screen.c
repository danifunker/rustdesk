#include "screen.h"
#include "log.h"

#include "win.h"
#include <stdlib.h>
#include <string.h>

/* How many macroblock rows one BitBlt reads. */
#define BAND_ROWS 4

static HDC mem;
static HBITMAP dib, old_bm;
static uint8_t *bits;      /* the DIB section: top-down BGRX */
static uint8_t *prev;      /* what the peer was last given the chance to see */
static int stride;         /* bytes per row of both */
static int next_row;       /* the band the round of the screen reads next */
static HDESK cur_desk;
static char cur_name[64];
static HCURSOR last_cursor;

static void free_all(cdv_screen *s)
{
    if (mem) {
        if (old_bm)
            SelectObject(mem, old_bm);
        DeleteDC(mem);
    }
    if (dib)
        DeleteObject(dib);
    mem = NULL;
    dib = old_bm = NULL;
    bits = NULL;
    free(prev);
    free(s->y);
    free(s->u);
    free(s->v);
    free(s->dirty);
    free(s->flat);
    prev = s->y = s->u = s->v = s->dirty = s->flat = NULL;
}

int screen_open(cdv_screen *s)
{
    BITMAPINFO bi;
    HDC sdc;
    int n;
    free_all(s);
    memset(s, 0, sizeof *s);
    s->width = GetSystemMetrics(SM_CXSCREEN);
    s->height = GetSystemMetrics(SM_CYSCREEN);
    s->mbw = (s->width + 15) / 16;
    s->mbh = (s->height + 15) / 16;
    s->ystride = s->mbw * 16;
    s->uvstride = s->mbw * 8;
    n = s->mbw * s->mbh;
    stride = s->width * 4;

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = s->width;
    bi.bmiHeader.biHeight = -s->height; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    mem = CreateCompatibleDC(sdc);
    dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, sdc);
    if (!mem || !dib || !bits) {
        cdv_log("screen: no DIB section for %dx%d (error %lu)", s->width, s->height,
                GetLastError());
        free_all(s);
        return 0;
    }
    old_bm = (HBITMAP)SelectObject(mem, dib);
    prev = (uint8_t *)malloc((size_t)stride * s->height);
    s->y = (uint8_t *)malloc((size_t)s->ystride * s->mbh * 16);
    s->u = (uint8_t *)malloc((size_t)s->uvstride * s->mbh * 8);
    s->v = (uint8_t *)malloc((size_t)s->uvstride * s->mbh * 8);
    s->dirty = (uint8_t *)calloc((size_t)n, 1);
    s->flat = (uint8_t *)calloc((size_t)n, 1);
    if (!prev || !s->y || !s->u || !s->v || !s->dirty || !s->flat) {
        cdv_log("screen: out of memory for %dx%d", s->width, s->height);
        free_all(s);
        return 0;
    }
    memset(prev, 0, (size_t)stride * s->height);
    memset(s->y, 16, (size_t)s->ystride * s->mbh * 16);
    memset(s->u, 128, (size_t)s->uvstride * s->mbh * 8);
    memset(s->v, 128, (size_t)s->uvstride * s->mbh * 8);
    next_row = 0;
    return 1;
}

void screen_close(cdv_screen *s)
{
    free_all(s);
}

int screen_resized(const cdv_screen *s)
{
    return GetSystemMetrics(SM_CXSCREEN) != s->width || GetSystemMetrics(SM_CYSCREEN) != s->height;
}

#define DESK_RIGHTS (DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU |        \
                     DESKTOP_HOOKCONTROL | DESKTOP_JOURNALRECORD | DESKTOP_JOURNALPLAYBACK | \
                     DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP |      \
                     GENERIC_WRITE)

int screen_follow(const char **name)
{
    HDESK in = OpenInputDesktop(0, FALSE, DESK_RIGHTS);
    char nm[64];
    DWORD len;
    if (name)
        *name = cur_name[0] ? cur_name : "Default";
    if (!in)
        return 0;
    nm[0] = 0;
    if (!GetUserObjectInformation(in, UOI_NAME, nm, sizeof nm, &len) ||
        (cur_name[0] && !strcmp(nm, cur_name))) {
        CloseDesktop(in);
        return 0;
    }
    if (!SetThreadDesktop(in)) {
        static DWORD told;
        if (told != GetLastError())
            cdv_log("cannot follow the input desktop to %s (error %lu)", nm, GetLastError());
        told = GetLastError();
        CloseDesktop(in);
        return 0;
    }
    if (cur_desk)
        CloseDesktop(cur_desk);
    cur_desk = in;
    strncpy(cur_name, nm, sizeof cur_name - 1);
    if (name)
        *name = cur_name;
    return 1;
}

/* ---- conversion ---------------------------------------------------------- */

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* One macroblock of BGRX at (mx, my), into the planes; the screen's edge
 * repeats into a macroblock it only partly covers. 1 if it is one colour. */
static int convert_mb(cdv_screen *s, int mx, int my)
{
    int x0 = mx * 16, y0 = my * 16, x, y, flat = 1;
    uint32_t first = *(const uint32_t *)(bits + (size_t)y0 * stride + x0 * 4) & 0xffffff;
    uint8_t *yp = s->y + (size_t)y0 * s->ystride + x0;
    uint8_t *up = s->u + (size_t)(y0 / 2) * s->uvstride + x0 / 2;
    uint8_t *vp = s->v + (size_t)(y0 / 2) * s->uvstride + x0 / 2;
    for (y = 0; y < 16; y += 2) {
        int sy0 = y0 + y < s->height ? y0 + y : s->height - 1;
        int sy1 = y0 + y + 1 < s->height ? y0 + y + 1 : s->height - 1;
        const uint8_t *r0 = bits + (size_t)sy0 * stride, *r1 = bits + (size_t)sy1 * stride;
        for (x = 0; x < 16; x += 2) {
            int sx0 = x0 + x < s->width ? x0 + x : s->width - 1;
            int sx1 = x0 + x + 1 < s->width ? x0 + x + 1 : s->width - 1;
            const uint8_t *p[4];
            int i, R = 0, G = 0, B = 0;
            p[0] = r0 + sx0 * 4;
            p[1] = r0 + sx1 * 4;
            p[2] = r1 + sx0 * 4;
            p[3] = r1 + sx1 * 4;
            for (i = 0; i < 4; i++) {
                int b = p[i][0], g = p[i][1], r = p[i][2];
                uint8_t *dst = yp + (y + (i >> 1)) * s->ystride + x + (i & 1);
                *dst = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                R += r;
                G += g;
                B += b;
                if (flat && (*(const uint32_t *)p[i] & 0xffffff) != first)
                    flat = 0;
            }
            R = (R + 2) >> 2;
            G = (G + 2) >> 2;
            B = (B + 2) >> 2;
            up[(y / 2) * s->uvstride + x / 2] = clamp8(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
            vp[(y / 2) * s->uvstride + x / 2] = clamp8(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
        }
    }
    return flat;
}

/* Do two runs of pixels differ? Visual C++'s memcmp for the PowerPC goes a
 * byte at a time: this is four times faster there (7 ms a 1280x1024 screen
 * under QEMU, against 27). */
static int differ(const uint32_t *a, const uint32_t *b, int n)
{
    int i;
    for (i = 0; i + 4 <= n; i += 4)
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3])
            return 1;
    for (; i < n; i++)
        if (a[i] != b[i])
            return 1;
    return 0;
}

/* Compare macroblock rows [r0, r1) with what was there; convert and keep
 * what changed. `all`: everything, changed or not. Most scanlines of a
 * desktop are as they were, so each is compared whole first, and only one
 * that differs is looked at macroblock by macroblock. */
static void compare_rows(cdv_screen *s, int r0, int r1, int all)
{
    static uint8_t *changed;
    static int changed_n;
    int my, mx, y;
    if (changed_n < s->mbw) {
        free(changed);
        changed = (uint8_t *)malloc((size_t)s->mbw);
        changed_n = changed ? s->mbw : 0;
        if (!changed)
            return;
    }
    for (my = r0; my < r1; my++) {
        int y0 = my * 16, y1 = y0 + 16 < s->height ? y0 + 16 : s->height, any = all;
        memset(changed, all, (size_t)s->mbw);
        for (y = y0; y < y1; y++) {
            const uint32_t *a = (const uint32_t *)(bits + (size_t)y * stride);
            const uint32_t *b = (const uint32_t *)(prev + (size_t)y * stride);
            if (all || !differ(a, b, s->width))
                continue;
            for (mx = 0; mx < s->mbw; mx++) {
                int x, x1 = mx * 16 + 16 < s->width ? mx * 16 + 16 : s->width;
                if (changed[mx])
                    continue;
                for (x = mx * 16; x < x1; x++)
                    if (a[x] != b[x]) {
                        changed[mx] = 1;
                        any = 1;
                        break;
                    }
            }
        }
        if (!any)
            continue;
        for (mx = 0; mx < s->mbw; mx++) {
            int x0 = mx * 16, bytes = ((x0 + 16 < s->width ? x0 + 16 : s->width) - x0) * 4;
            int i = my * s->mbw + mx;
            size_t off = (size_t)y0 * stride + x0 * 4;
            if (!changed[mx])
                continue;
            for (y = y0; y < y1; y++, off += stride)
                memcpy(prev + off, bits + off, (size_t)bytes);
            s->flat[i] = (uint8_t)convert_mb(s, mx, my);
            if (!s->dirty[i]) {
                s->dirty[i] = 1;
                s->ndirty++;
            }
        }
    }
}

static void read_rows(cdv_screen *s, HDC sdc, int r0, int r1)
{
    int y0 = r0 * 16, y1 = r1 * 16 < s->height ? r1 * 16 : s->height;
    if (y1 > y0)
        BitBlt(mem, 0, y0, s->width, y1 - y0, sdc, 0, y0, SRCCOPY);
    GdiFlush();
    s->rows_read += r1 - r0;
}

void screen_start(cdv_screen *s)
{
    HDC sdc = GetDC(NULL);
    uint32_t t0 = cdv_ms();
    s->rows_read = 0;
    read_rows(s, sdc, 0, s->mbh);
    ReleaseDC(NULL, sdc);
    s->read_ms = cdv_ms() - t0;
    t0 = cdv_ms();
    memset(s->dirty, 0, (size_t)(s->mbw * s->mbh));
    s->ndirty = 0;
    compare_rows(s, 0, s->mbh, 1);
    s->convert_ms = cdv_ms() - t0;
    next_row = 0;
    last_cursor = NULL; /* and the pointer's shape, again */
}

void screen_collect(cdv_screen *s, uint32_t budget_ms)
{
    HDC sdc = GetDC(NULL);
    uint32_t t0 = cdv_ms(), tr = 0, tc = 0, t;
    POINT pt;
    s->rows_read = 0;
    if (!sdc)
        return;
    /* First where the pointer is: that is where the person is looking. */
    if (GetCursorPos(&pt)) {
        int r = pt.y / 16 - 2, e = pt.y / 16 + 3;
        if (r < 0)
            r = 0;
        if (e > s->mbh)
            e = s->mbh;
        t = cdv_ms();
        read_rows(s, sdc, r, e);
        tr += cdv_ms() - t;
        t = cdv_ms();
        compare_rows(s, r, e, 0);
        tc += cdv_ms() - t;
    }
    /* Then the round of the whole screen, a band at a time. */
    do {
        int e = next_row + BAND_ROWS < s->mbh ? next_row + BAND_ROWS : s->mbh;
        t = cdv_ms();
        read_rows(s, sdc, next_row, e);
        tr += cdv_ms() - t;
        t = cdv_ms();
        compare_rows(s, next_row, e, 0);
        tc += cdv_ms() - t;
        next_row = e;
        if (next_row >= s->mbh) {
            next_row = 0;
            break;
        }
    } while (cdv_ms() - t0 < budget_ms);
    ReleaseDC(NULL, sdc);
    s->read_ms = tr;
    s->convert_ms = tc;
}

void screen_grab_all(cdv_screen *s)
{
    HDC sdc = GetDC(NULL);
    if (!sdc)
        return;
    BitBlt(mem, 0, 0, s->width, s->height, sdc, 0, 0, SRCCOPY);
    GdiFlush();
    ReleaseDC(NULL, sdc);
    /* what changed is found by comparing with prev, which still holds what
     * the encoder was given: nothing is lost */
}

void screen_rgb_row(const cdv_screen *s, int y, uint8_t *out)
{
    const uint8_t *p = bits + (size_t)y * stride;
    int x;
    for (x = 0; x < s->width; x++, p += 4) {
        *out++ = p[2];
        *out++ = p[1];
        *out++ = p[0];
    }
}

int screen_pending(const cdv_screen *s)
{
    (void)s;
    return next_row != 0;
}

void screen_mark_all(cdv_screen *s)
{
    memset(s->dirty, 1, (size_t)(s->mbw * s->mbh));
    s->ndirty = s->mbw * s->mbh;
}

void screen_sent_some(cdv_screen *s, const uint8_t *sent)
{
    int i, n = s->mbw * s->mbh;
    for (i = 0; i < n; i++)
        if (sent[i] && s->dirty[i]) {
            s->dirty[i] = 0;
            s->ndirty--;
        }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

void screen_background(const cdv_screen *s, uint8_t *y, uint8_t *u, uint8_t *v)
{
    int n = s->mbw * s->mbh, i, k = 0, best = 0, run = 0;
    uint32_t *c = (uint32_t *)malloc((size_t)n * 4), mode = 0x108080;
    if (c) {
        for (i = 0; i < n; i++) {
            int mx = i % s->mbw, my = i / s->mbw;
            if (!s->flat[i])
                continue;
            c[k++] = (uint32_t)s->y[(size_t)my * 16 * s->ystride + mx * 16] << 16 |
                     (uint32_t)s->u[(size_t)my * 8 * s->uvstride + mx * 8] << 8 |
                     s->v[(size_t)my * 8 * s->uvstride + mx * 8];
        }
        qsort(c, (size_t)k, 4, cmp_u32);
        for (i = 0; i < k; i++) {
            run = i && c[i] == c[i - 1] ? run + 1 : 1;
            if (run > best) {
                best = run;
                mode = c[i];
            }
        }
        free(c);
    }
    *y = (uint8_t)(mode >> 16);
    *u = (uint8_t)(mode >> 8);
    *v = (uint8_t)mode;
}

/* ---- the pointer ----------------------------------------------------------- */

int screen_pointer(int *x, int *y)
{
    POINT p;
    if (!GetCursorPos(&p))
        return 0;
    *x = p.x;
    *y = p.y;
    return 1;
}

/* The cursor some other program set is not ours to see with GetCursor(),
 * which answers for the calling thread's input state; joining the input of
 * the foreground window's thread for a moment makes it answer for that. */
static HCURSOR current_cursor(void)
{
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), them = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    HCURSOR c;
    if (them && them != me && AttachThreadInput(me, them, TRUE)) {
        c = GetCursor();
        AttachThreadInput(me, them, FALSE);
    } else {
        c = GetCursor();
    }
    return c;
}

int screen_cursor_shape(uint32_t *id, int *hotx, int *hoty, int *w, int *h, uint8_t *rgba)
{
    HCURSOR c = current_cursor();
    ICONINFO ii;
    BITMAP bm;
    BITMAPINFO bi;
    HDC dc;
    uint32_t *mask = NULL, *color = NULL;
    int x, y, mh, cw, ch, ok = 0;
    if (!c || c == last_cursor)
        return 0;
    if (!GetIconInfo(c, &ii))
        return 0;
    if (!GetObject(ii.hbmMask, sizeof bm, &bm))
        goto out;
    mh = bm.bmHeight;
    cw = bm.bmWidth;
    ch = ii.hbmColor ? mh : mh / 2;
    mask = (uint32_t *)malloc((size_t)cw * mh * 4);
    color = (uint32_t *)malloc((size_t)cw * ch * 4);
    if (!mask || !color)
        goto out;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -mh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = GetDC(NULL);
    if (!GetDIBits(dc, ii.hbmMask, 0, (UINT)mh, mask, &bi, DIB_RGB_COLORS)) {
        ReleaseDC(NULL, dc);
        goto out;
    }
    if (ii.hbmColor) {
        bi.bmiHeader.biHeight = -ch;
        if (!GetDIBits(dc, ii.hbmColor, 0, (UINT)ch, color, &bi, DIB_RGB_COLORS)) {
            ReleaseDC(NULL, dc);
            goto out;
        }
    }
    ReleaseDC(NULL, dc);
    *w = cw < 64 ? cw : 64;
    *h = ch < 64 ? ch : 64;
    for (y = 0; y < *h; y++)
        for (x = 0; x < *w; x++) {
            uint8_t *p = rgba + (y * *w + x) * 4;
            int and_ = (mask[y * cw + x] & 0xffffff) != 0;
            if (ii.hbmColor) {
                uint32_t px = color[y * cw + x];
                p[0] = (uint8_t)(px >> 16);
                p[1] = (uint8_t)(px >> 8);
                p[2] = (uint8_t)px;
                p[3] = (uint8_t)(and_ && !(px & 0xffffff) ? 0 : 255);
            } else {
                int xor_ = (mask[(y + ch) * cw + x] & 0xffffff) != 0;
                /* AND 1, XOR 0: transparent. AND 0: black or white. AND 1,
                 * XOR 1 inverts the screen, which a picture cannot: black,
                 * as the I-beam over a page looks. */
                if (and_ && !xor_) {
                    p[0] = p[1] = p[2] = p[3] = 0;
                } else {
                    p[0] = p[1] = p[2] = (uint8_t)(!and_ && xor_ ? 255 : 0);
                    p[3] = 255;
                }
            }
        }
    *hotx = (int)ii.xHotspot;
    *hoty = (int)ii.yHotspot;
    *id = (uint32_t)c;
    last_cursor = c;
    ok = 1;
out:
    free(mask);
    free(color);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    return ok;
}
