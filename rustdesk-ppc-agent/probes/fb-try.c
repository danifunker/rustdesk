/* Which call republishes the framebuffer snapshot WITHOUT seizing the display?
 *
 * CGDisplayCaptureWithOptions works, but it is far too heavy-handed to run a few
 * times a second: it takes exclusive control of the display and registers the
 * caller as an application (CGSRegisterProcessAsApp), which steals focus and
 * dismisses whatever menu the person at the machine has open. Sitting at the G5
 * while a client was connected was described, accurately, as the agent
 * "stealing the activity".
 *
 * One method per run, because the first one to work hides the rest:
 *
 *   fb-try none        control: expect stale
 *   fb-try release     CGDisplayRelease without capturing first
 *   fb-try cursor      hide/show the cursor
 *   fb-try fade        acquire a fade reservation and release it
 *   fb-try winlist     CGWindowListCreateImage (composites everything, no capture)
 *   fb-try capture     control: the current, harmful method
 *
 * A change is forced with `killall Dock` (launchd respawns it), and the verdict
 * compares against a freshly exec'd fb-shot, which always sees current pixels.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static CGDirectDisplayID d;
static size_t h, bpr;

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Full-frame hash of the colour bytes; alpha is constant and must be skipped. */
static unsigned long shot(void) {
    unsigned char *base = (unsigned char *)CGDisplayBaseAddress(d);
    unsigned char *buf = malloc(bpr * h);
    memcpy(buf, base, bpr * h);
    unsigned long s = 0;
    size_t i;
    for (i = 0; i < bpr * h; i += 4)
        s = s * 31u + buf[i + 1] + buf[i + 2] + buf[i + 3];
    free(buf);
    return s;
}

static unsigned long child_truth(void) {
    FILE *p = popen("~/ppc-probes/fb-shot | sed 's/.*sum=\\([0-9]*\\) .*/\\1/'", "r");
    unsigned long v = 0;
    if (p) {
        if (fscanf(p, "%lu", &v) != 1)
            v = 0;
        pclose(p);
    }
    return v;
}

int main(int argc, char **argv) {
    const char *method = argc > 1 ? argv[1] : "none";
    d = CGMainDisplayID();
    h = CGDisplayPixelsHigh(d);
    bpr = CGDisplayBytesPerRow(d);

    unsigned long before = shot();
    printf("method=%-8s before=%lu\n", method, before);
    fflush(stdout);

    printf("  forcing a change (killall Dock)\n");
    fflush(stdout);
    system("killall Dock 2>/dev/null");
    sleep(6);

    unsigned long stale = shot();
    printf("  without any refresh: %lu %s\n", stale,
           stale == before ? "(stale, as expected)" : "(already moved -- inconclusive)");

    double t0 = now_ms();
    if (!strcmp(method, "release")) {
        CGDisplayRelease(d);
    } else if (!strcmp(method, "cursor")) {
        CGDisplayHideCursor(d);
        CGDisplayShowCursor(d);
    } else if (!strcmp(method, "fade")) {
        CGDisplayFadeReservationToken tok;
        if (CGAcquireDisplayFadeReservation(1, &tok) == kCGErrorSuccess)
            CGReleaseDisplayFadeReservation(tok);
    } else if (!strcmp(method, "winlist")) {
        CGImageRef img = CGWindowListCreateImage(CGRectInfinite, kCGWindowListOptionOnScreenOnly,
                                                 kCGNullWindowID, kCGWindowImageDefault);
        printf("  winlist image: %s\n", img ? "created" : "NULL");
        if (img)
            CGImageRelease(img);
    } else if (!strcmp(method, "capture")) {
        CGDisplayCaptureWithOptions(d, kCGCaptureNoFill);
        CGDisplayRelease(d);
    }
    double ms = now_ms() - t0;

    unsigned long after = shot();
    unsigned long truth = child_truth();
    printf("  after %-8s: %lu  (%.1f ms)\n", method, after, ms);
    printf("  ground truth  : %lu\n", truth);
    printf("  => %s\n",
           after != stale && after == truth ? "REFRESHED (and matches ground truth)"
           : after != stale                 ? "changed, but does not match ground truth"
                                            : "no refresh");
    return 0;
}
