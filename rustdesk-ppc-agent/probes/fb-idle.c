/* Does a process that sits idle lose its access to the framebuffer?
 *
 * Backlog item 1d, second attempt. The first suspect was display sleep, on the
 * strength of the failure being at eleven minutes and `pmset` saying
 * `displaysleep 10`. That is a correlation between two numbers and nothing
 * else, and `fb-vigil` cannot test it: polling CoreGraphics every fifteen
 * seconds resets HIDIdleTime, so the display stayed awake for 33 minutes and
 * the probe never observed the thing it was built for.
 *
 * The reported failure has a sharper shape than "eleven minutes had passed",
 * and it is one the polling probe actively destroys:
 *
 *   - the agent printed its banner at startup, which reads the geometry --
 *     CGDisplayPixelsWide / High -- and nothing else;
 *   - it then made no CoreGraphics call whatsoever while it waited for a peer;
 *   - eleven minutes later a peer connected, Capturer::new called
 *     CGDisplayBaseAddress, and got NULL;
 *   - a freshly exec'd process read the framebuffer fine at that same moment.
 *
 * So the variable to test is **idleness**, not elapsed time: a window-server
 * connection that is never used may be torn down, and a fresh process makes a
 * new one. This reproduces the agent's sequence exactly and does nothing at all
 * in between -- no timer, no polling, one `sleep`.
 *
 * Run several at different waits, in parallel, and compare:
 *
 *   for m in 2 5 10 15 20 30; do
 *     screen -dmS idle$m bash -c "~/ppc-probes/fb-idle $m > ~/fb-idle-$m.log 2>&1"
 *   done
 *
 * `hold` as a second argument maps the framebuffer at startup as well, which
 * separates "the connection went stale" from "an established mapping went
 * stale" -- the agent does the former, and the difference says which to fix.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static sigjmp_buf fault_jump;
static volatile sig_atomic_t fault_sig;

static void on_fault(int sig)
{
    fault_sig = sig;
    siglongjmp(fault_jump, 1);
}

static const char *stamp(void)
{
    static char buf[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, sizeof buf, "%H:%M:%S", tm);
    return buf;
}

/* Can we actually read through this pointer? Catches the case where the address
 * is non-NULL but the mapping underneath it has gone. */
static int readable(const unsigned char *p, size_t bpr, size_t h)
{
    volatile unsigned long acc = 0;
    size_t y;
    if (!p)
        return 0;
    fault_sig = 0;
    if (sigsetjmp(fault_jump, 1) != 0)
        return -1;
    for (y = 0; y < h; y += 64)
        acc += p[y * bpr + 1];
    return 1;
}

int main(int argc, char **argv)
{
    int minutes = argc > 1 ? atoi(argv[1]) : 11;
    int hold = argc > 2 && strcmp(argv[2], "hold") == 0;
    CGDirectDisplayID d = CGMainDisplayID();
    size_t w, h, bpr;
    unsigned char *first = NULL, *after;
    int r;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGBUS, on_fault);
    signal(SIGSEGV, on_fault);

    /* Startup, as the agent's banner does it: geometry only. */
    w = CGDisplayPixelsWide(d);
    h = CGDisplayPixelsHigh(d);
    bpr = CGDisplayBytesPerRow(d);
    printf("%s start: %lux%lu/%lu, waiting %d min%s\n", stamp(),
           (unsigned long)w, (unsigned long)h, (unsigned long)bpr, minutes,
           hold ? ", holding a mapping" : ", no mapping held");

    if (hold) {
        first = (unsigned char *)CGDisplayBaseAddress(d);
        printf("%s held : %p\n", stamp(), (void *)first);
    }

    /* Nothing at all in between. That is the whole experiment. */
    sleep(minutes * 60);

    after = (unsigned char *)CGDisplayBaseAddress(d);
    printf("%s after %d min: CGDisplayBaseAddress = %p %s\n", stamp(), minutes,
           (void *)after,
           after ? "" : " <-- NULL, which is the reported failure");

    if (after) {
        r = readable(after, bpr, h);
        printf("%s        reading it: %s\n", stamp(),
               r == 1 ? "ok" : (r == -1 ? "FAULTS" : "no pointer"));
    }
    if (hold && first) {
        r = readable(first, bpr, h);
        printf("%s        the mapping held since startup: %s%s\n", stamp(),
               r == 1 ? "ok" : (r == -1 ? "FAULTS" : "no pointer"),
               (after && after != first) ? " (and the address moved)" : "");
    }

    /* The other half of the original report: a fresh process was fine at the
     * same moment. If that is still true here, the difference is the process
     * and not the machine -- which is the finding that decides the fix. */
    {
        size_t w2 = CGDisplayPixelsWide(d);
        printf("%s        geometry now reads %lux%lu\n", stamp(),
               (unsigned long)w2, (unsigned long)CGDisplayPixelsHigh(d));
    }
    printf("%s done\n", stamp());
    return 0;
}
