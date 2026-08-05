/* Are any modifier flags stuck down?
 *
 * On macOS a Control-click is a right-click, so one stuck Control flag turns
 * every left click into a context menu -- with nothing wrong in the button
 * decoding at all. Synthetic mouse events built from an HID-state event source
 * inherit whatever the system currently believes is held, so a modifier left
 * set by earlier synthetic key events would do it.
 *
 * Prints the flags the system reports, and what a freshly created mouse event
 * would therefore carry.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>

static void show(const char *what, CGEventFlags f) {
    printf("%-22s %#010llx  ", what, (unsigned long long)f);
    if (f & kCGEventFlagMaskShift)      printf("SHIFT ");
    if (f & kCGEventFlagMaskControl)    printf("CONTROL ");
    if (f & kCGEventFlagMaskAlternate)  printf("ALT ");
    if (f & kCGEventFlagMaskCommand)    printf("COMMAND ");
    if (f & kCGEventFlagMaskAlphaShift) printf("CAPSLOCK ");
    if (f & kCGEventFlagMaskSecondaryFn) printf("FN ");
    if (!(f & (kCGEventFlagMaskShift | kCGEventFlagMaskControl | kCGEventFlagMaskAlternate |
               kCGEventFlagMaskCommand | kCGEventFlagMaskAlphaShift)))
        printf("(none)");
    printf("\n");
}

int main(void) {
    show("HID system state", CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState));
    show("combined session", CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState));

    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CGEventRef e = CGEventCreateMouseEvent(src, kCGEventLeftMouseDown, CGPointMake(10, 10),
                                           kCGMouseButtonLeft);
    if (e) {
        show("a new left-click", CGEventGetFlags(e));
        CFRelease(e);
    }
    CGEventRef q = CGEventCreate(NULL);
    if (q) {
        show("a bare event", CGEventGetFlags(q));
        CFRelease(q);
    }
    printf("\nCONTROL on a left-click is a right-click on this platform.\n");
    return 0;
}
