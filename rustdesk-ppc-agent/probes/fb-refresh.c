/* What makes the framebuffer mapping show fresh content *within one process*?
 *
 * Established by fb-shot: a newly exec'd process reads the current desktop, but
 * a process that reads twice sees byte-identical content forever, even across a
 * Dock restart. So the read is not "frozen hardware" — the mapping this process
 * holds is a snapshot. This probe forces a screen change and then tries, in
 * order, the things that might refresh it:
 *
 *   1. plain re-read                  (control: expected to be stale)
 *   2. re-read the base address       (in case the pointer itself moves)
 *   3. dcbf the range                 (in case it is a cacheable mapping and the
 *                                      CPU is serving stale lines with no
 *                                      coherency against the GPU's writes)
 *   4. capture/release the display    (Apple's docs say the framebuffer pointer
 *                                      is only valid while captured; kCGCaptureNoFill
 *                                      keeps the local user's pixels on screen)
 *   5. a freshly exec'd child         (control: known to work, proves the screen
 *                                      really did change)
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static CGDirectDisplayID d;
static size_t w, h, bpr;

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Full-frame checksum, read the way the agent reads: bulk copy, then hash RAM. */
static unsigned long shot(const char *tag, unsigned char *base) {
    unsigned char *buf = malloc(bpr * h);
    double t0 = now_ms();
    memcpy(buf, base, bpr * h);
    double copy_ms = now_ms() - t0;
    unsigned long sum = 0;
    size_t i;
    for (i = 0; i < bpr * h; i += 4)
        sum = sum * 31u + buf[i + 1] + buf[i + 2] + buf[i + 3];
    printf("  %-22s sum=%-12lu copy=%6.1f ms  base=%p\n", tag, sum, copy_ms, base);
    fflush(stdout);
    free(buf);
    return sum;
}

/* PowerPC: push any cached lines for the range back out, so a following read
 * has to go to memory. Harmless (a no-op) if the mapping is uncached. */
static void flush_range(void *p, size_t n) {
    char *q = (char *)p;
    size_t i;
    for (i = 0; i < n; i += 32)
        __asm__ volatile("dcbf 0,%0" : : "r"(q + i) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

int main(void) {
    d = CGMainDisplayID();
    w = CGDisplayPixelsWide(d);
    h = CGDisplayPixelsHigh(d);
    bpr = CGDisplayBytesPerRow(d);
    unsigned char *base = (unsigned char *)CGDisplayBaseAddress(d);
    printf("display %ux%u bpr=%u base=%p\n", (unsigned)w, (unsigned)h, (unsigned)bpr, base);

    unsigned long a = shot("1. before change", base);

    printf("-- killall Dock, wait 8s --\n");
    fflush(stdout);
    system("killall Dock 2>/dev/null");
    sleep(8);

    unsigned long b = shot("1. plain re-read", base);

    unsigned char *base2 = (unsigned char *)CGDisplayBaseAddress(d);
    unsigned long c = shot("2. re-read base", base2);

    flush_range(base2, bpr * h);
    unsigned long e = shot("3. after dcbf", base2);

    CGDisplayCaptureWithOptions(d, kCGCaptureNoFill);
    usleep(300000);
    unsigned char *base3 = (unsigned char *)CGDisplayBaseAddress(d);
    unsigned long f = shot("4. while captured", base3);
    CGDisplayRelease(d);
    usleep(300000);
    unsigned char *base4 = (unsigned char *)CGDisplayBaseAddress(d);
    unsigned long g = shot("4. after release", base4);

    printf("-- freshly exec'd child (control) --\n");
    fflush(stdout);
    system("~/ppc-probes/fb-shot");

    printf("\nbefore=%lu  plain=%lu  rebase=%lu  dcbf=%lu  captured=%lu  released=%lu\n",
           a, b, c, e, f, g);
    printf("Anything differing from `before` refreshed the mapping.\n");
    return 0;
}
