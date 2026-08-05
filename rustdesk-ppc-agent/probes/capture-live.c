/* Is CGDisplayBaseAddress actually frozen, and does CGWindowListCreateImage work?
 *
 * The earlier "framebuffer is frozen" finding was taken on an idle desktop with
 * nobody at the machine, so a static image proves nothing on its own. This probe
 * *causes* a large repaint (launch TextEdit, then quit it) and checksums both
 * capture routes around it. Only a route that misses a forced repaint is dead.
 *
 * CGWindowListCreateImage is 10.5+, so it is available here and is the obvious
 * replacement if the direct framebuffer really is stale under Quartz Extreme.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Sampled checksum: touch few bytes, VRAM reads are expensive. */
static unsigned long sum_sampled(const unsigned char *p, size_t bpr, size_t h,
                                 size_t rowstep, size_t colstep) {
    unsigned long s = 0;
    size_t y, x;
    for (y = 0; y < h; y += rowstep)
        for (x = 0; x < bpr; x += colstep)
            s = s * 31u + p[y * bpr + x];
    return s;
}

static CGDirectDisplayID g_d;

static unsigned long probe_base(double *ms) {
    double t0 = now_ms();
    unsigned char *p = (unsigned char *)CGDisplayBaseAddress(g_d);
    unsigned long s = 0;
    if (p)
        s = sum_sampled(p, CGDisplayBytesPerRow(g_d), CGDisplayPixelsHigh(g_d), 16, 64);
    *ms = now_ms() - t0;
    return s;
}

static unsigned long probe_winlist(double *ms, size_t *w, size_t *h, size_t *bpr) {
    double t0 = now_ms();
    CGImageRef img = CGWindowListCreateImage(CGRectInfinite,
                                             kCGWindowListOptionOnScreenOnly,
                                             kCGNullWindowID,
                                             kCGWindowImageDefault);
    unsigned long s = 0;
    *w = *h = *bpr = 0;
    if (img) {
        CGDataProviderRef dp = CGImageGetDataProvider(img);
        CFDataRef data = CGDataProviderCopyData(dp);
        if (data) {
            const unsigned char *p = CFDataGetBytePtr(data);
            *w = CGImageGetWidth(img);
            *h = CGImageGetHeight(img);
            *bpr = CGImageGetBytesPerRow(img);
            s = sum_sampled(p, *bpr, *h, 16, 64);
            CFRelease(data);
        }
        CGImageRelease(img);
    }
    *ms = now_ms() - t0;
    return s;
}

static void snapshot(const char *tag) {
    double mb, mw;
    size_t w, h, bpr;
    unsigned long sb = probe_base(&mb);
    unsigned long sw = probe_winlist(&mw, &w, &h, &bpr);
    printf("%-14s base=%-20lu (%6.1f ms)   winlist=%-20lu (%7.1f ms) %ux%u bpr=%u\n",
           tag, sb, mb, sw, mw, (unsigned)w, (unsigned)h, (unsigned)bpr);
    fflush(stdout);
}

int main(void) {
    g_d = CGMainDisplayID();
    printf("display %ux%u bpp=%u  base=%p\n",
           (unsigned)CGDisplayPixelsWide(g_d), (unsigned)CGDisplayPixelsHigh(g_d),
           (unsigned)CGDisplayBitsPerPixel(g_d), CGDisplayBaseAddress(g_d));

    snapshot("idle-1");
    sleep(1);
    snapshot("idle-2");

    /* `open -a` needs LaunchServices and so needs a GUI session; killing Dock and
     * Finder does not — launchd respawns them *inside* the console session, which
     * repaints the dock, the menu bar and the desktop. This is the forced repaint
     * that an ssh-only process can actually cause. */
    printf("-- killall Dock --\n");
    fflush(stdout);
    system("killall Dock 2>/dev/null");
    sleep(6);
    snapshot("dock-restart");

    printf("-- killall Finder --\n");
    fflush(stdout);
    system("killall Finder 2>/dev/null");
    sleep(7);
    snapshot("finder-restart");

    /* Cursor motion: usually a hardware overlay, so a *static* result here is
     * not evidence of a dead framebuffer. Included to see whether the
     * WindowServer accepts our calls at all. */
    printf("-- warp cursor --\n");
    fflush(stdout);
    CGWarpMouseCursorPosition(CGPointMake(400.0, 300.0));
    sleep(2);
    snapshot("cursor-400x300");
    CGWarpMouseCursorPosition(CGPointMake(1200.0, 800.0));
    sleep(2);
    snapshot("cursor-1200x800");

    printf("\nA route is LIVE if its checksum moved at dock-restart or finder-restart.\n");
    return 0;
}
