/* Can the capture/release refresh be done every frame, and what does it cost?
 *
 * fb-refresh established that a capture/release cycle is what makes this
 * process's framebuffer mapping show current pixels. That is only useful if it
 * is cheap enough to run per frame and keeps working when repeated — and if it
 * does not visibly disturb whoever is sitting at the machine (kCGCaptureNoFill
 * leaves their pixels on screen; the display is only "captured" for the
 * duration of the cycle).
 *
 * The Dock is killed part way through, so the checksum must move at that point.
 * A run where the checksum never moves means the refresh does not survive
 * repetition.
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

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 12;
    CGDirectDisplayID d = CGMainDisplayID();
    size_t h = CGDisplayPixelsHigh(d), bpr = CGDisplayBytesPerRow(d);
    unsigned char *buf = malloc(bpr * h);
    int i;

    printf("iter  refresh_ms  copy_ms  sum\n");
    for (i = 0; i < iters; i++) {
        double t0 = now_ms();
        CGDisplayCaptureWithOptions(d, kCGCaptureNoFill);
        CGDisplayRelease(d);
        double refresh_ms = now_ms() - t0;

        unsigned char *base = (unsigned char *)CGDisplayBaseAddress(d);
        double t1 = now_ms();
        memcpy(buf, base, bpr * h);
        double copy_ms = now_ms() - t1;

        unsigned long sum = 0;
        size_t k;
        for (k = 0; k < bpr * h; k += 4)
            sum = sum * 31u + buf[k + 1] + buf[k + 2] + buf[k + 3];

        /* Ground truth: a freshly exec'd process always sees current pixels.
         * If the in-process refreshed sum tracks the child's, the refresh is
         * genuinely working rather than just changing once. */
        printf("%4d  %10.1f  %7.1f  %-12lu child: ", i, refresh_ms, copy_ms, sum);
        fflush(stdout);
        system("~/ppc-probes/fb-shot | sed 's/.*sum=\\([0-9]*\\).*/\\1/'");
        sleep(1);
    }
    free(buf);
    return 0;
}
