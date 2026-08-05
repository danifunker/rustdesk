/* Is the display in a sane state, and is anyone holding it captured?
 *
 * The agent's capture/release refresh spends most of its time *inside* the
 * captured window, so a kill -9 at the wrong moment can leave the display
 * captured by a process that no longer exists. This reports enough to tell that
 * apart from a display that has simply gone to sleep.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>

int main(void) {
    CGDirectDisplayID d = CGMainDisplayID();
    printf("display   %ux%u  bpp=%u bpr=%u\n",
           (unsigned)CGDisplayPixelsWide(d), (unsigned)CGDisplayPixelsHigh(d),
           (unsigned)CGDisplayBitsPerPixel(d), (unsigned)CGDisplayBytesPerRow(d));
    printf("captured  %s\n", CGDisplayIsCaptured(d) ? "YES (by someone)" : "no");
    printf("base      %p\n", CGDisplayBaseAddress(d));
    printf("asleep    %s\n", CGDisplayIsAsleep(d) ? "YES" : "no");
    printf("online    %s  active %s\n",
           CGDisplayIsOnline(d) ? "yes" : "NO", CGDisplayIsActive(d) ? "yes" : "NO");
    return 0;
}
