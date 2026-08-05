/* Is the framebuffer mapping live in a long-lived process, with nothing forced?
 *
 * Every earlier answer to this was confounded. The sampled hashes read only
 * alpha bytes (constant 0xff, so nothing ever looked different), and the runs
 * that forced a change with `killall Dock` are unreliable because launchd
 * throttles respawns to at least ten seconds -- the repaint can easily land
 * next to whichever call was being credited with refreshing the mapping.
 *
 * So: force nothing, touch nothing, and use the menu-bar clock as the clock it
 * is. Over two minutes it must tick at least twice. Each in-process read is
 * compared against a freshly exec'd fb-shot, which is known to see current
 * pixels; if the two agree the whole way through a couple of minute boundaries,
 * the mapping is live and no refresh call is needed at all.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 130;
    CGDirectDisplayID d = CGMainDisplayID();
    size_t h = CGDisplayPixelsHigh(d), bpr = CGDisplayBytesPerRow(d);
    unsigned char *buf = malloc(bpr * h);

    unsigned long seen[64];
    int nseen = 0, mismatches = 0, i;

    printf("t     in-process   fresh-process  agree?\n");
    for (i = 0; i * 5 < secs; i++) {
        unsigned char *base = (unsigned char *)CGDisplayBaseAddress(d);
        memcpy(buf, base, bpr * h);
        unsigned long mine = 0;
        size_t k;
        for (k = 0; k < bpr * h; k += 4)
            mine = mine * 31u + buf[k + 1] + buf[k + 2] + buf[k + 3];

        FILE *p = popen("~/ppc-probes/fb-shot | sed 's/.*sum=\\([0-9]*\\) .*/\\1/'", "r");
        unsigned long truth = 0;
        if (p) {
            if (fscanf(p, "%lu", &truth) != 1)
                truth = 0;
            pclose(p);
        }

        int j, known = 0;
        for (j = 0; j < nseen; j++)
            if (seen[j] == mine)
                known = 1;
        if (!known && nseen < 64)
            seen[nseen++] = mine;
        if (mine != truth)
            mismatches++;

        printf("%3ds  %-12lu %-14lu %s\n", i * 5, mine, truth, mine == truth ? "yes" : "NO");
        fflush(stdout);
        sleep(5);
    }

    printf("\ndistinct images seen in-process: %d\n", nseen);
    printf("disagreements with a fresh process: %d\n", mismatches);
    if (nseen < 2)
        printf("=> INCONCLUSIVE: the screen never changed, so nothing was tested\n");
    else if (mismatches == 0)
        printf("=> mapping is LIVE; the capture/release refresh is unnecessary\n");
    else
        printf("=> mapping goes STALE; a refresh really is needed\n");
    free(buf);
    return 0;
}
