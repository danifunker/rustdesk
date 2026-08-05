/* How long after CGDisplayRelease does fresh content actually appear?
 *
 * fb-refresh proved a capture/release cycle republishes the mapping, but it had
 * 300 ms sleeps in it, and the agent -- which reads immediately after release --
 * still sees stale pixels. If the window server's repaint is asynchronous then
 * the cycle is not the whole story and the read has to wait for it.
 *
 * Forces a change, runs the cycle, then samples every 25 ms until the content
 * moves, so the required settle time is measured rather than guessed.
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

/* Cheap sampled hash, like the agent's dirty-band probe: every 16th row. */
static unsigned long sample(unsigned char *base, size_t bpr, size_t h) {
    unsigned long s = 0;
    size_t y, x;
    for (y = 0; y < h; y += 16)
        for (x = 0; x < bpr; x += 64)
            s = s * 31u + base[y * bpr + x];
    return s;
}

int main(void) {
    CGDirectDisplayID d = CGMainDisplayID();
    size_t h = CGDisplayPixelsHigh(d), bpr = CGDisplayBytesPerRow(d);
    unsigned char *base = (unsigned char *)CGDisplayBaseAddress(d);

    unsigned long before = sample(base, bpr, h);
    printf("before change: %lu\n", before);

    printf("-- killall Dock, wait 6s --\n");
    fflush(stdout);
    system("killall Dock 2>/dev/null");
    sleep(6);

    printf("stale read:    %lu %s\n", sample(base, bpr, h),
           sample(base, bpr, h) == before ? "(unchanged, as expected)" : "(CHANGED without a cycle!)");

    double t0 = now_ms();
    CGError cap = CGDisplayCaptureWithOptions(d, kCGCaptureNoFill);
    double tcap = now_ms() - t0;
    double t1 = now_ms();
    CGError rel = CGDisplayRelease(d);
    double trel = now_ms() - t1;
    printf("capture=%d (%.1f ms)  release=%d (%.1f ms)\n", (int)cap, tcap, (int)rel, trel);

    double t2 = now_ms();
    int i;
    for (i = 0; i < 60; i++) {
        base = (unsigned char *)CGDisplayBaseAddress(d);
        unsigned long s = sample(base, bpr, h);
        if (s != before) {
            printf("content changed %.0f ms after release (sample %d) -> %lu\n",
                   now_ms() - t2, i, s);
            return 0;
        }
        usleep(25000);
    }
    printf("content NEVER changed within %.0f ms after release\n", now_ms() - t2);
    return 1;
}
