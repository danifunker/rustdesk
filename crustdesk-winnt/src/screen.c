#include "screen.h"
#include "hooks.h"
#include "log.h"
#include "plat.h"

#include "win.h"
#include <stdlib.h>
#include <string.h>

/* How many macroblock rows one BitBlt reads. */
#define BAND_ROWS 4

static HDC mem;
static HBITMAP dib, old_bm;
static uint8_t *bits;      /* the DIB section: top-down BGRX, or RGBX (red_at) */
static int red_at = 2;     /* the byte of a pixel that is red: 2, or 0 on a screen
                            * of red-first pixels (blue is at 2 - red_at) */
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
    /* A screen whose 32-bit pixels have red in the low byte (PINT's, on a
     * real Pi) into the usual DIB, blue there, has GDI translate every pixel
     * through a function: half of a read's time there, and all of it once a
     * display driver keeps the screen in ordinary memory. A DIB section with
     * the screen's own masks is copied as it is; the planes are made from
     * either order. The masks: a compatible bitmap asked for its format
     * twice (the header, then the masks). */
    red_at = 2;
    dib = NULL;
    {
        struct {
            BITMAPINFOHEADER h;
            DWORD m[256];
        } own;
        HBITMAP cb = CreateCompatibleBitmap(sdc, 1, 1);
        memset(&own, 0, sizeof own);
        own.h.biSize = sizeof own.h;
        if (cb) {
            GetDIBits(sdc, cb, 0, 0, NULL, (BITMAPINFO *)&own, DIB_RGB_COLORS);
            GetDIBits(sdc, cb, 0, 0, NULL, (BITMAPINFO *)&own, DIB_RGB_COLORS);
            DeleteObject(cb);
            if (own.h.biBitCount == 32 && own.h.biCompression == BI_BITFIELDS &&
                own.m[0] == 0x000000ffUL && own.m[1] == 0x0000ff00UL && own.m[2] == 0x00ff0000UL) {
                own.h.biWidth = s->width;
                own.h.biHeight = -s->height;
                own.h.biPlanes = 1;
                own.h.biSizeImage = 0;
                dib = CreateDIBSection(sdc, (BITMAPINFO *)&own, DIB_RGB_COLORS, (void **)&bits, NULL,
                                       0);
                if (dib && bits) {
                    red_at = 0;
                    cdv_log("screen: red-first pixels, read as they are");
                } else if (dib) {
                    DeleteObject(dib);
                    dib = NULL;
                }
            }
        }
    }
    if (!dib)
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
    HDESK in;
    char nm[64];
    DWORD len;
    if (name)
        *name = cur_name[0] ? cur_name : "Default";
    if (!plat_nt())
        return 0; /* 9x has one desktop, the logon dialog on it */
    in = OpenInputDesktop(0, FALSE, DESK_RIGHTS);
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

/* One macroblock of BGRX (or RGBX: red_at) at (mx, my), into the planes; the screen's edge
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
                int b = p[i][2 - red_at], g = p[i][1], r = p[i][red_at];
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

/* Compare macroblock rows [r0, r1), columns [c0, c1], with what was there;
 * convert and keep what changed. `all`: everything, changed or not. Most
 * scanlines of a desktop are as they were, so each is compared whole first
 * (the span of it), and only one that differs is looked at macroblock by
 * macroblock. */
static void compare_span(cdv_screen *s, int r0, int r1, int c0, int c1, int all)
{
    static uint8_t *changed;
    static int changed_n;
    int my, mx, y;
    int px0 = c0 * 16, px1 = (c1 + 1) * 16 < s->width ? (c1 + 1) * 16 : s->width;
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
            if (all || !differ(a + px0, b + px0, px1 - px0))
                continue;
            for (mx = c0; mx <= c1; mx++) {
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
        for (mx = c0; mx <= c1; mx++) {
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

static void compare_rows(cdv_screen *s, int r0, int r1, int all)
{
    compare_span(s, r0, r1, 0, s->mbw - 1, all);
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

/* ---- with the screen hooks: read what was drawn on ---------------------------
 *
 * hooks.h says where programs drew. A look reads that; around the pointer
 * when it moved (a hover); every console window (csrss draws those, which
 * no hook sees); and, at most ROUND_MS apart, ROUND_ROWS rows of a slow
 * round of the whole screen for anything else the hooks miss (a caret, a
 * program drawing outside WM_PAINT). Each run of wanted macroblocks is read
 * as a rectangle of its own and compared over its own columns: a look costs
 * what was drawn, not the width of the screen. */

#define ROUND_ROWS 2
#define ROUND_MS 500
/* Console windows have no hook, so they are read blind, and a console is a
 * large read: 210,000 pixels for 80x25, over half the machine at three
 * looks a second on the PowerPC and the Alpha. So the one in front is read
 * CONSOLE_MS apart only while the peer is typing or clicking (for
 * CONSOLE_TYPING_MS after it), and any console CONSOLES_MS apart otherwise. */
#define CONSOLE_MS 300
#define CONSOLE_TYPING_MS 3000
#define CONSOLES_MS 2000
/* The tallest rectangle one BitBlt reads: a big mark (a window that was
 * clicked) is read in pieces, the look stopping when its time is up and the
 * rest kept for the next, so the peer's clicks and keys get in between --
 * a double-click must not have a long read between its two clicks. */
#define SPAN_ROWS 4

/* The hooks mark a square when a program is told to draw there, before it
 * has: a read then can find the old picture, and nothing would look at that
 * square again until the round came by, seconds later. So every marked
 * square is read a second time, ECHO_MS after the marks stop. */
#define ECHO_MS 300

static uint8_t *want; /* mbw*mbh: still to read, kept from look to look */
static uint8_t *fresh, *echo; /* marked in this look; to read once they settle */
static int want_n, want_left, scan_row, echo_left;
static uint32_t echo_at;
static uint32_t round_at, console_at, consoles_at, input_at;
static POINT last_pt;
static int all_consoles;  /* this look reads the consoles behind too */

static void want_rect(const cdv_screen *s, int l, int t, int r, int b)
{
    int x, y, x0, x1, y0, y1;
    if (l < 0)
        l = 0;
    if (t < 0)
        t = 0;
    if (r > s->width)
        r = s->width;
    if (b > s->height)
        b = s->height;
    if (r <= l || b <= t)
        return;
    x0 = l / 16;
    x1 = (r - 1) / 16;
    y0 = t / 16;
    y1 = (b - 1) / 16;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            want[y * s->mbw + x] = 1;
}

static BOOL CALLBACK console_window(HWND h, LPARAM lp)
{
    char cls[24];
    RECT r;
    if ((all_consoles || h == GetForegroundWindow()) && IsWindowVisible(h) && !IsIconic(h) &&
        GetClassName(h, cls, sizeof cls) &&
        !strcmp(cls, "ConsoleWindowClass") && GetWindowRect(h, &r))
        want_rect((const cdv_screen *)lp, r.left, r.top, r.right, r.bottom);
    return TRUE;
}

/* Read rows [r0, r1), macroblock columns [c0, c1], and compare them. */
static void read_span(cdv_screen *s, HDC sdc, int r0, int r1, int c0, int c1, uint32_t *tr,
                      uint32_t *tc)
{
    int x0 = c0 * 16, x1 = (c1 + 1) * 16 < s->width ? (c1 + 1) * 16 : s->width;
    int y0 = r0 * 16, y1 = r1 * 16 < s->height ? r1 * 16 : s->height;
    uint32_t t = cdv_ms();
    if (x1 <= x0 || y1 <= y0)
        return;
    BitBlt(mem, x0, y0, x1 - x0, y1 - y0, sdc, x0, y0, SRCCOPY);
    GdiFlush();
    *tr += cdv_ms() - t;
    s->rows_read += r1 - r0;
    s->rects_read++;
    s->px_read += (long)(x1 - x0) * (y1 - y0);
    t = cdv_ms();
    compare_span(s, r0, r1, c0, c1, 0);
    *tc += cdv_ms() - t;
}

static int collect_hinted(cdv_screen *s, HDC sdc, uint32_t budget_ms, uint32_t *tr,
                          uint32_t *tc)
{
    int n = s->mbw * s->mbh, w = s->mbw, i;
    uint32_t t0 = cdv_ms();
    POINT pt;
    if (want_n != n) {
        free(want);
        free(fresh);
        free(echo);
        want = (uint8_t *)calloc((size_t)n, 1);
        fresh = (uint8_t *)calloc((size_t)n, 1);
        echo = (uint8_t *)calloc((size_t)n, 1);
        want_n = want && fresh && echo ? n : 0;
        scan_row = 0;
        echo_left = 0;
        if (!want_n)
            return 0;
    }
    if (hooks_take(fresh, s->mbw, s->mbh)) {
        for (i = 0; i < n; i++)
            if (fresh[i]) {
                want[i] = echo[i] = 1;
                fresh[i] = 0;
            }
        echo_left = 1;
        echo_at = cdv_ms();
    } else if (echo_left && cdv_ms() - echo_at >= ECHO_MS) {
        for (i = 0; i < n; i++)
            if (echo[i]) {
                want[i] = 1;
                echo[i] = 0;
            }
        echo_left = 0;
    }
    if (GetCursorPos(&pt) && (pt.x != last_pt.x || pt.y != last_pt.y)) {
        want_rect(s, pt.x - 64, pt.y - 32, pt.x + 64, pt.y + 48);
        last_pt = pt;
    }
    all_consoles = cdv_ms() - consoles_at >= CONSOLES_MS;
    if (all_consoles)
        consoles_at = cdv_ms();
    if (all_consoles ||
        (cdv_ms() - input_at < CONSOLE_TYPING_MS && cdv_ms() - console_at >= CONSOLE_MS)) {
        console_at = cdv_ms();
        EnumWindows(console_window, (LPARAM)s);
    }

    /* the round: a band of whole rows, now and then */
    if (cdv_ms() - round_at >= ROUND_MS) {
        int e = next_row + ROUND_ROWS < s->mbh ? next_row + ROUND_ROWS : s->mbh;
        round_at = cdv_ms();
        read_span(s, sdc, next_row, e, 0, s->mbw - 1, tr, tc);
        memset(want + next_row * w, 0, (size_t)((e - next_row) * w)); /* read already */
        next_row = e < s->mbh ? e : 0;
    }

    /* each run of wanted macroblocks in a row (a gap of one is read through:
     * a BitBlt costs more than 16 pixels), grown down while the rows below
     * want all of the same columns, SPAN_ROWS at most; from where the last
     * look stopped, until the time is up */
    for (i = 0; i < s->mbh; i++) {
        int my = (scan_row + i) % s->mbh, x = 0;
        uint8_t *row = want + my * w;
        while (x < w) {
            int a, b, r1, k;
            if (!row[x]) {
                x++;
                continue;
            }
            if (cdv_ms() - t0 >= budget_ms) {
                scan_row = my;
                want_left = 1;
                return 1;
            }
            a = x;
            while (x < w && (row[x] || (x + 1 < w && row[x + 1])))
                x++;
            b = x - 1;
            memset(row + a, 0, (size_t)(b - a + 1));
            for (r1 = my + 1; r1 < s->mbh && r1 < my + SPAN_ROWS; r1++) {
                uint8_t *below = want + r1 * w;
                for (k = a; k <= b && below[k]; k++)
                    ;
                if (k <= b)
                    break;
                memset(below + a, 0, (size_t)(b - a + 1));
            }
            read_span(s, sdc, my, r1, a, b, tr, tc);
        }
    }
    scan_row = 0;
    want_left = 0;
    return 1;
}

void screen_collect(cdv_screen *s, uint32_t budget_ms)
{
    HDC sdc = GetDC(NULL);
    uint32_t t0 = cdv_ms(), tr = 0, tc = 0, t;
    POINT pt;
    s->rows_read = 0;
    s->rects_read = 0;
    s->px_read = 0;
    if (!sdc)
        return;
    /* The hooks are on the Default desktop: the logon and locked screens
     * (Winlogon) are read the old way. */
    if (hooks_active() && (!cur_name[0] || !strcmp(cur_name, "Default")) &&
        collect_hinted(s, sdc, budget_ms, &tr, &tc)) {
        ReleaseDC(NULL, sdc);
        s->read_ms = tr;
        s->convert_ms = tc;
        return;
    }
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
        *out++ = p[red_at];
        *out++ = p[1];
        *out++ = p[2 - red_at];
    }
}

void screen_note_input(void)
{
    input_at = cdv_ms();
}

int screen_settling(void)
{
    return hooks_active() && echo_left;
}

int screen_pending(const cdv_screen *s)
{
    (void)s;
    return hooks_active() ? want_left : next_row != 0;
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
/* The pointer's shape. GetCursor answers for this thread's input queue, so
 * the old way is to attach to the foreground thread's for a moment -- but
 * Windows keeps the double-click state in the queue, and attaching wipes it:
 * done every 100 ms, it broke double-clicks for everyone at the machine, the
 * real mouse too, for as long as a session lasted. So: GetCursorInfo, which
 * touches no queue, where there is one (NT 4.0 SP3, 98 and later); else
 * attach only when the pointer has moved, as it does not between the two
 * clicks of a double-click, and keep the last shape otherwise (an hourglass
 * then shows when the pointer next moves). */
typedef struct {
    DWORD cbSize, flags;
    HCURSOR hCursor;
    POINT ptScreenPos;
} cdv_cursorinfo;
typedef BOOL(WINAPI *GetCursorInfo_t)(cdv_cursorinfo *);

static HCURSOR current_cursor(void)
{
    static GetCursorInfo_t info;
    static int looked;
    static POINT was;
    static HCURSOR shape;
    POINT pt;
    HWND fg;
    DWORD me, them;
    if (!looked) {
        info = (GetCursorInfo_t)GetProcAddress(GetModuleHandle("user32.dll"), "GetCursorInfo");
        looked = 1;
    }
    if (info) {
        cdv_cursorinfo ci;
        ci.cbSize = sizeof ci;
        if (info(&ci))
            return ci.hCursor;
    }
    if (!GetCursorPos(&pt))
        return shape;
    if (shape && pt.x == was.x && pt.y == was.y)
        return shape;
    was = pt;
    fg = GetForegroundWindow();
    me = GetCurrentThreadId();
    them = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (them && them != me && AttachThreadInput(me, them, TRUE)) {
        shape = GetCursor();
        AttachThreadInput(me, them, FALSE);
    } else {
        shape = GetCursor();
    }
    return shape;
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
