/* Does the framebuffer mapping survive display sleep?
 *
 * Backlog item 1d: an agent that had read the display fine at startup had
 * CGDisplayBaseAddress return NULL eleven minutes later, and every peer that
 * connected afterwards got a working mouse and no picture. A freshly exec'd
 * process read the framebuffer without trouble at the same moment, so whatever
 * goes stale belongs to the long-lived process rather than to the display.
 *
 * The suspect is display sleep, and the arithmetic is suggestive: this G5's
 * `pmset -g` reports `displaysleep 10`, and the failure was at eleven minutes.
 * That is a correlation and not yet a cause, which is what this is for.
 *
 * Holds a mapping and reports, every tick: whether the display is asleep, what
 * the base address is now against what it was at startup, whether the geometry
 * still agrees, and whether reading through each pointer still yields the same
 * bytes. Runs until killed, so it can be left overnight across as many sleep
 * cycles as the night contains.
 *
 * Two things it must survive rather than die of, since both are findings:
 *
 *   - the held pointer faulting, if the mapping is torn down under us. SIGBUS
 *     and SIGSEGV are caught and reported, and the tick carries on with a
 *     freshly queried address.
 *   - a NULL from CGDisplayBaseAddress, which is the reported symptom itself.
 *
 * Every line is prefixed with a wall-clock time, and anything that is not
 * business as usual also prints a line beginning "CHANGE", so a whole night can
 * be read with grep.
 *
 * **Polling this in a loop prevents the thing it is looking for.** Left running
 * at a 15-second period it kept the display awake for 33 minutes against a
 * `displaysleep 10`, and `ioreg -c IOHIDSystem` explained why: HIDIdleTime was
 * resetting to zero every tick, so the idle timer never reached ten minutes.
 * Something in these CoreGraphics calls counts as user activity. So the loop
 * mode is for watching a session that is already awake, and the *sleep*
 * question is answered by `fb-sleepwatch.sh`, which waits with `ioreg` alone --
 * touching nothing -- and runs this once, in one-shot mode, at the moment the
 * display has actually gone down.
 *
 *   ~/ppc-probes/fb-vigil 0     # one tick and exit -- what the watcher uses
 *   ~/ppc-probes/fb-vigil 15    # loop; keeps the display awake, see above
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

/* Sampled over rows so a tick is cheap even at 1920x1080: this runs for hours
 * and must not itself be the load that keeps the machine awake. Reads R, G and
 * B and never the alpha byte -- sampling only alpha is the bug that made every
 * screen look identical for a week, see docs/BACKLOG.md item 1. */
static unsigned long sample(const unsigned char *base, size_t bpr, size_t h)
{
    unsigned long sum = 1469598103u;
    size_t y;
    for (y = 0; y < h; y += 16) {
        const unsigned char *row = base + y * bpr;
        size_t x;
        for (x = 0; x + 4 <= bpr; x += 256) {
            sum = (sum ^ row[x + 1]) * 16777619u;
            sum = (sum ^ row[x + 2]) * 16777619u;
            sum = (sum ^ row[x + 3]) * 16777619u;
        }
    }
    return sum;
}

/* Read through `p`, or report that reading it faults. */
static int try_sample(const unsigned char *p, size_t bpr, size_t h, unsigned long *out)
{
    if (!p)
        return 0;
    fault_sig = 0;
    if (sigsetjmp(fault_jump, 1) != 0)
        return -1;
    *out = sample(p, bpr, h);
    return 1;
}

static const char *stamp(void)
{
    static char buf[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, sizeof buf, "%H:%M:%S", tm);
    return buf;
}

int main(int argc, char **argv)
{
    int period = argc > 1 ? atoi(argv[1]) : 15;
    CGDirectDisplayID d = CGMainDisplayID();
    unsigned char *held = (unsigned char *)CGDisplayBaseAddress(d);
    size_t bpr0 = CGDisplayBytesPerRow(d);
    size_t w0 = CGDisplayPixelsWide(d), h0 = CGDisplayPixelsHigh(d);
    int prev_asleep = -1, prev_null = -1, prev_fault = 0;
    unsigned long tick = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGBUS, on_fault);
    signal(SIGSEGV, on_fault);

    printf("fb-vigil: held=%p geom=%lux%lu/%lu period=%ds\n",
           (void *)held, (unsigned long)w0, (unsigned long)h0,
           (unsigned long)bpr0, period);
    printf("time      asleep held        fresh       geom            held_sum   fresh_sum\n");

    for (;;) {
        unsigned char *fresh = (unsigned char *)CGDisplayBaseAddress(d);
        int asleep = CGDisplayIsAsleep(d) ? 1 : 0;
        size_t bpr = CGDisplayBytesPerRow(d);
        size_t w = CGDisplayPixelsWide(d), h = CGDisplayPixelsHigh(d);
        unsigned long hs = 0, fs = 0;
        int hr, fr;

        /* Never read more than the geometry we mapped with: if the mode shrank,
         * the old extent runs off the end of the mapping. */
        size_t rb = bpr < bpr0 ? bpr : bpr0;
        size_t rh = h < h0 ? h : h0;

        hr = try_sample(held, rb, rh, &hs);
        fr = try_sample(fresh, rb, rh, &fs);

        printf("%s %6d %-11p %-11p %4lux%-4lu/%-5lu %-10s %-10s%s\n",
               stamp(), asleep, (void *)held, (void *)fresh,
               (unsigned long)w, (unsigned long)h, (unsigned long)bpr,
               hr == 1 ? "ok" : (hr == 0 ? "NULL" : "FAULT"),
               fr == 1 ? "ok" : (fr == 0 ? "NULL" : "FAULT"),
               (hr == 1 && fr == 1 && hs != fs) ? "  DIFFER" : "");

        /* The lines that matter, called out so a night can be grepped. */
        if (prev_asleep >= 0 && asleep != prev_asleep)
            printf("CHANGE %s display %s\n", stamp(), asleep ? "went to SLEEP" : "WOKE");
        if ((fresh == NULL) != (prev_null == 1))
            printf("CHANGE %s CGDisplayBaseAddress %s\n", stamp(),
                   fresh == NULL ? "returned NULL  <-- the reported symptom" : "is non-NULL again");
        if (fresh != NULL && fresh != held)
            printf("CHANGE %s base address moved: %p -> %p\n", stamp(), (void *)held, (void *)fresh);
        if (hr == -1 && !prev_fault)
            printf("CHANGE %s reading the HELD mapping faults (signal %d)  <-- it was torn down\n",
                   stamp(), (int)fault_sig);
        if (w != w0 || h != h0 || bpr != bpr0)
            printf("CHANGE %s geometry moved: %lux%lu/%lu -> %lux%lu/%lu\n", stamp(),
                   (unsigned long)w0, (unsigned long)h0, (unsigned long)bpr0,
                   (unsigned long)w, (unsigned long)h, (unsigned long)bpr);

        prev_asleep = asleep;
        prev_null = (fresh == NULL);
        prev_fault = (hr == -1);
        /* One-shot: the caller is a watcher that has already decided this is an
         * interesting moment, and looping here would end the moment. */
        if (period <= 0)
            break;
        if (++tick % 40 == 0)
            printf("-- %s still running, %lu ticks --\n", stamp(), tick);
        sleep(period);
    }
    return 0;
}
