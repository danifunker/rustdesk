/* Does the cursor position read steady when nothing is moving?
 *
 * The agent sends a CursorPosition whenever the value changes. In a live
 * session it was sending a burst of them with no mouse events arriving at all,
 * which either means the pointer really is moving or the read is unstable. One
 * of those is a bug in the agent; the other is not.
 *
 * Uses the same call the shim does: an event created with a NULL source, whose
 * location is the current pointer.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    double lastx = -1, lasty = -1;
    int changes = 0, i;
    printf("polling the pointer 40 times over 4s, nothing should be moving it\n");
    for (i = 0; i < 40; i++) {
        CGEventRef e = CGEventCreate(NULL);
        CGPoint p = e ? CGEventGetLocation(e) : CGPointMake(-1, -1);
        if (e)
            CFRelease(e);
        if (p.x != lastx || p.y != lasty) {
            changes++;
            printf("  %2d: %.4f, %.4f   <-- changed\n", i, p.x, p.y);
        }
        lastx = p.x;
        lasty = p.y;
        usleep(100000);
    }
    printf("\n%d changes in 40 polls with no input\n", changes);
    printf("more than 1 means the read is unstable, and the agent will flood\n");
    return 0;
}
