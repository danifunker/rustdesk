/* Inject an event, then photograph the screen, so injection can be checked
 * without anyone standing at the machine.
 *
 * The screenshot is taken by exec'ing fb-shot rather than reading here: a fresh
 * process always gets current pixels, which sidesteps the snapshot behaviour
 * described in capture.rs and keeps this probe independent of the fix for it.
 *
 *   inject-shot click <x> <y> <out.ppm>
 *   inject-shot key   <keycode> <out.ppm>      (53 = Escape)
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: inject-shot click <x> <y> <out.ppm> | key <keycode> <out.ppm>\n");
        return 2;
    }
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    const char *out;

    if (!strcmp(argv[1], "click")) {
        double x = atof(argv[2]), y = atof(argv[3]);
        out = argv[4];
        CGPoint pt = CGPointMake(x, y);
        CGEventRef mv = CGEventCreateMouseEvent(src, kCGEventMouseMoved, pt, 0);
        CGEventPost(kCGHIDEventTap, mv);
        CFRelease(mv);
        usleep(300000);
        CGEventRef dn = CGEventCreateMouseEvent(src, kCGEventLeftMouseDown, pt, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, dn);
        CFRelease(dn);
        usleep(120000);
        CGEventRef up = CGEventCreateMouseEvent(src, kCGEventLeftMouseUp, pt, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
        printf("clicked at %.0f,%.0f\n", x, y);
    } else {
        int code = atoi(argv[2]);
        out = argv[3];
        CGEventRef dn = CGEventCreateKeyboardEvent(src, (CGKeyCode)code, true);
        CGEventPost(kCGHIDEventTap, dn);
        CFRelease(dn);
        usleep(80000);
        CGEventRef up = CGEventCreateKeyboardEvent(src, (CGKeyCode)code, false);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
        printf("pressed keycode %d\n", code);
    }

    sleep(2); /* let the window server repaint */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "~/ppc-probes/fb-shot %s", out);
    return system(cmd) == 0 ? 0 : 1;
}
