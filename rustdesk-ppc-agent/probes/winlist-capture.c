/* Can the screen be read faster than 23 MB/s?
 *
 * The agent captures with a memcpy out of CGDisplayBaseAddress, which measures
 * ~350 ms for a 1920x1080 frame -- 23 MB/s, and by far the largest single cost
 * in the pipeline. It is that slow because it is a CPU-driven read of uncached
 * VRAM, which cannot burst.
 *
 * CGWindowListCreateImage is 10.5+ and takes a different route: the window
 * server composites and hands back a CGImage whose bytes live in RAM. If that
 * readback is a GPU DMA it could be several times faster; if it recomposites
 * the whole screen per call it will be worse. Nobody has timed it -- it has
 * only ever been checksummed for liveness (see capture-live.c, fb-try.c).
 *
 * Three things are asked here, in order of how much they would change:
 *
 *   1. full screen, both routes            -- is the readback faster at all?
 *   2. one band (1/16th), both routes      -- the agent reads bands, not
 *                                             frames, so a route that only
 *                                             wins full-screen wins little
 *   3. pixel format and geometry           -- a faster route that hands back
 *                                             BGRA or a different stride needs
 *                                             conversion work doing anyway
 *
 * Every result is accumulated into a volatile sink: an earlier probe in this
 * project reported 0.0 ms for copying megabytes because gcc -O2 had eliminated
 * memcpys whose destination was never read.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static volatile unsigned long sink;

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void observe(const unsigned char *p, size_t n) {
    unsigned long s = 0;
    size_t i;
    for (i = 0; i < n; i += 997)   /* prime stride: touch it, cheaply */
        s += p[i];
    sink += s;
}

/* Median of three, like the agent's own sweeps: one sample on a machine with a
 * window server on it is noise. */
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static double median3(double *v) {
    qsort(v, 3, sizeof(double), cmp_d);
    return v[1];
}

static CGDirectDisplayID d;

/* Route 1: what the agent does today. */
static double time_memcpy(size_t y0, size_t rows, unsigned char *dst) {
    size_t bpr = CGDisplayBytesPerRow(d);
    double t[3];
    int i;
    for (i = 0; i < 3; i++) {
        const unsigned char *base = (const unsigned char *)CGDisplayBaseAddress(d);
        double t0 = now_ms();
        memcpy(dst, base + y0 * bpr, rows * bpr);
        t[i] = now_ms() - t0;
        observe(dst, rows * bpr);
    }
    return median3(t);
}

/* Route 2: composite through the window server and copy the result out. */
static double time_winlist(size_t y0, size_t rows, size_t *w, size_t *h,
                           size_t *bpr, size_t *bpp, int *ok) {
    size_t sw = CGDisplayPixelsWide(d);
    double t[3];
    int i;
    *ok = 0;
    for (i = 0; i < 3; i++) {
        CGRect r = CGRectMake(0.0, (double)y0, (double)sw, (double)rows);
        double t0 = now_ms();
        CGImageRef img = CGWindowListCreateImage(r, kCGWindowListOptionOnScreenOnly,
                                                 kCGNullWindowID, kCGWindowImageDefault);
        if (!img) { t[i] = -1.0; continue; }
        CGDataProviderRef dp = CGImageGetDataProvider(img);
        CFDataRef data = CGDataProviderCopyData(dp);
        if (data) {
            const unsigned char *p = CFDataGetBytePtr(data);
            size_t n = (size_t)CFDataGetLength(data);
            observe(p, n);
            *w = CGImageGetWidth(img);
            *h = CGImageGetHeight(img);
            *bpr = CGImageGetBytesPerRow(img);
            *bpp = CGImageGetBitsPerPixel(img);
            *ok = 1;
            CFRelease(data);
        }
        CGImageRelease(img);
        t[i] = now_ms() - t0;
    }
    if (t[0] < 0 || t[1] < 0 || t[2] < 0) return -1.0;
    return median3(t);
}

int main(void) {
    d = CGMainDisplayID();
    size_t sw = CGDisplayPixelsWide(d), sh = CGDisplayPixelsHigh(d);
    size_t bpr = CGDisplayBytesPerRow(d);
    printf("display %ux%u  bpr %u  bpp %u\n", (unsigned)sw, (unsigned)sh,
           (unsigned)bpr, (unsigned)CGDisplayBitsPerPixel(d));

    unsigned char *dst = malloc(bpr * sh);
    if (!dst) { printf("out of memory\n"); return 1; }

    size_t band = (sh + 15) / 16;   /* the agent's BANDS = 16 */

    struct { const char *name; size_t y0, rows; } cases[] = {
        { "full screen", 0, sh },
        { "one band",    0, band },
    };

    printf("\n%-12s %10s %10s %10s %10s\n", "region", "memcpy", "MB/s", "winlist", "MB/s");
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t rows = cases[i].rows;
        if (cases[i].y0 + rows > sh) rows = sh - cases[i].y0;
        double mb = (double)(rows * bpr) / (1024.0 * 1024.0);

        double a = time_memcpy(cases[i].y0, rows, dst);
        size_t w = 0, h = 0, ibpr = 0, ibpp = 0;
        int ok = 0;
        double b = time_winlist(cases[i].y0, rows, &w, &h, &ibpr, &ibpp, &ok);

        printf("%-12s %8.1f ms %10.1f ", cases[i].name, a, mb * 1000.0 / a);
        if (ok && b > 0)
            printf("%8.1f ms %10.1f   (%ux%u bpr %u bpp %u)\n", b, mb * 1000.0 / b,
                   (unsigned)w, (unsigned)h, (unsigned)ibpr, (unsigned)ibpp);
        else
            printf("%8s    %10s   (returned nothing)\n", "-", "-");
    }

    /* Byte order matters as much as speed: the agent's converter reads A,R,G,B
     * in memory, and a route handing back B,G,R,A would silently swap red and
     * blue -- the exact bug src/convert.rs exists to avoid. */
    {
        CGImageRef img = CGWindowListCreateImage(CGRectMake(0, 0, 4, 1),
                                                 kCGWindowListOptionOnScreenOnly,
                                                 kCGNullWindowID, kCGWindowImageDefault);
        if (img) {
            CGDataProviderRef dp = CGImageGetDataProvider(img);
            CFDataRef data = CGDataProviderCopyData(dp);
            const unsigned char *base = (const unsigned char *)CGDisplayBaseAddress(d);
            printf("\nfirst 8 bytes, framebuffer : %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   base[0], base[1], base[2], base[3], base[4], base[5], base[6], base[7]);
            if (data && CFDataGetLength(data) >= 8) {
                const unsigned char *p = CFDataGetBytePtr(data);
                printf("first 8 bytes, winlist     : %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                printf("  (they must agree, or the converter needs a different byte order)\n");
            }
            if (data) CFRelease(data);
            CGImageRelease(img);
        }
    }

    free(dst);
    printf("\nsink %lu (ignore; it exists so the copies are not optimised away)\n", sink);
    return 0;
}
