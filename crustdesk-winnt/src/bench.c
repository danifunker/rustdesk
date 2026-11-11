/* -bench: how fast this machine's screen can be read, and why.
 *
 * The agent reads the screen with BitBlt from the screen into a 32-bit DIB
 * section, as every look does (screen.c). On PINT that read crawls (about
 * 7 MB/s in a session's log), and the pointer flickers. This times the
 * same read beside what it would cost from ordinary memory, so it says how
 * much a display driver that keeps the screen in cached memory (a shadow
 * frame buffer) could win:
 *
 *   RAM copy            memcpy, cached memory to cached memory
 *   DIB -> DIB          BitBlt between two DIB sections: GDI's own cost
 *                       over cached memory
 *   screen -> DIB       BitBlt from the screen, the whole of it in one call
 *   screen, pieces      the same in 256x64 pieces, as a look reads marks
 *   screen, at pointer  128x80 round the pointer (GDI takes a pointer it
 *                       draws itself off the screen for that), and the same
 *                       size far from it
 *
 * Each runs for a second or more. MB/s is of the 32-bit pixels delivered,
 * whatever the screen's depth. The pointer flickers while it runs. */
#include "bench.h"

#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUN_MS 1000

static HDC sdc, dc_a, dc_b;
static int sw, sh;
static uint8_t *ram_a, *ram_b;
static int px, py; /* the rectangle of the pointer tests */

static char report[4000];

static void line(const char *name, uint32_t ms, long n, double bytes, long calls)
{
    char b[200];
    double per = (double)ms / (double)n;
    double mbs = ms ? bytes * (double)n / 1048576.0 / ((double)ms / 1000.0) : 0.0;
    sprintf(b, "%-20s %8.2f ms each, %7.1f MB/s  (%ld runs", name, per, mbs, n);
    if (calls > 1)
        sprintf(b + strlen(b), ", %ld BitBlts each", calls);
    strcat(b, ")\r\n");
    strcat(report, b);
}

static void ram_copy(void)
{
    memcpy(ram_a, ram_b, (size_t)sw * sh * 4);
}

static void dib_to_dib(void)
{
    BitBlt(dc_a, 0, 0, sw, sh, dc_b, 0, 0, SRCCOPY);
    GdiFlush();
}

static void screen_whole(void)
{
    BitBlt(dc_a, 0, 0, sw, sh, sdc, 0, 0, SRCCOPY);
    GdiFlush();
}

static long pieces;

static void screen_pieces(void)
{
    int x, y, w, h;
    pieces = 0;
    for (y = 0; y < sh; y += 64)
        for (x = 0; x < sw; x += 256) {
            w = sw - x < 256 ? sw - x : 256;
            h = sh - y < 64 ? sh - y : 64;
            BitBlt(dc_a, x, y, w, h, sdc, x, y, SRCCOPY);
            pieces++;
        }
    GdiFlush();
}

static void screen_rect(void)
{
    BitBlt(dc_a, px, py, 128, 80, sdc, px, py, SRCCOPY);
    GdiFlush();
}

/* Run f for RUN_MS or more, three times at least; the ms it took and how
 * many times it ran. */
static uint32_t timed(void (*f)(void), long *n)
{
    uint32_t t0;
    f(); /* once untimed: page in, warm up */
    t0 = cdv_ms();
    *n = 0;
    do {
        f();
        (*n)++;
    } while (cdv_ms() - t0 < RUN_MS || *n < 3);
    return cdv_ms() - t0;
}

static HBITMAP dib(int w, int h)
{
    BITMAPINFO bi;
    void *bits;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
}

int bench(void (*out)(const char *))
{
    HBITMAP bm_a, bm_b, old_a, old_b;
    SYSTEM_INFO si;
    POINT pt;
    long n;
    uint32_t ms;
    double whole;
    char b[300];

    report[0] = 0;
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    whole = (double)sw * sh * 4;
    sdc = GetDC(NULL);
    dc_a = CreateCompatibleDC(sdc);
    dc_b = CreateCompatibleDC(sdc);
    bm_a = dib(sw, sh);
    bm_b = dib(sw, sh);
    ram_a = (uint8_t *)malloc((size_t)sw * sh * 4);
    ram_b = (uint8_t *)malloc((size_t)sw * sh * 4);
    if (!sdc || !dc_a || !dc_b || !bm_a || !bm_b || !ram_a || !ram_b) {
        sprintf(b, "bench: no memory for %dx%d (error %lu)\r\n", sw, sh, GetLastError());
        out(b);
        return 1;
    }
    memset(ram_b, 0x5a, (size_t)sw * sh * 4);
    old_a = (HBITMAP)SelectObject(dc_a, bm_a);
    old_b = (HBITMAP)SelectObject(dc_b, bm_b);
    PatBlt(dc_b, 0, 0, sw, sh, WHITENESS);

    GetSystemInfo(&si);
    sprintf(b, "C-Desk-Vint -bench: screen %dx%d, %d-bit; %lu processor(s)\r\n", sw, sh,
            GetDeviceCaps(sdc, BITSPIXEL) * GetDeviceCaps(sdc, PLANES),
            (unsigned long)si.dwNumberOfProcessors);
    strcat(report, b);

    ms = timed(ram_copy, &n);
    line("RAM copy", ms, n, whole, 1);
    ms = timed(dib_to_dib, &n);
    line("DIB -> DIB", ms, n, whole, 1);
    ms = timed(screen_whole, &n);
    line("screen -> DIB", ms, n, whole, 1);
    ms = timed(screen_pieces, &n);
    line("screen, pieces", ms, n, whole, pieces);

    if (GetCursorPos(&pt)) {
        px = pt.x - 64;
        py = pt.y - 32;
        if (px < 0)
            px = 0;
        if (py < 0)
            py = 0;
        if (px > sw - 128)
            px = sw - 128;
        if (py > sh - 80)
            py = sh - 80;
        ms = timed(screen_rect, &n);
        line("screen, at pointer", ms, n, 128.0 * 80 * 4, 1);
        /* the same size in the corner farthest from it */
        px = pt.x < sw / 2 ? sw - 128 : 0;
        py = pt.y < sh / 2 ? sh - 80 : 0;
        ms = timed(screen_rect, &n);
        line("screen, no pointer", ms, n, 128.0 * 80 * 4, 1);
    }

    SelectObject(dc_a, old_a);
    SelectObject(dc_b, old_b);
    DeleteObject(bm_a);
    DeleteObject(bm_b);
    DeleteDC(dc_a);
    DeleteDC(dc_b);
    ReleaseDC(NULL, sdc);
    free(ram_a);
    free(ram_b);
    out(report);
    return 0;
}
