/* Can a character -> keycode map be built with CoreGraphics alone?
 *
 * The client sends printable keys as `chr`, an ASCII/unicode codepoint, not a
 * Mac virtual keycode. Typing them needs the reverse of the usual mapping, and
 * the obvious tool for that (UCKeyTranslate) lives in Carbon, which this build
 * does not link.
 *
 * CGEventKeyboardGetUnicodeString reads back the character a synthesized key
 * event *would* produce under the current layout, so walking every keycode with
 * and without shift builds the reverse map using only CoreGraphics -- and it
 * follows whatever layout the machine is actually set to rather than assuming a
 * US one.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    int lower[128], shifted[128];
    memset(lower, 0, sizeof lower);
    memset(shifted, 0, sizeof shifted);

    int k, sh;
    for (k = 0; k < 128; k++) {
        for (sh = 0; sh < 2; sh++) {
            CGEventRef e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)k, true);
            if (!e)
                continue;
            if (sh)
                CGEventSetFlags(e, kCGEventFlagMaskShift);
            UniChar buf[8];
            UniCharCount n = 0;
            CGEventKeyboardGetUnicodeString(e, 8, &n, buf);
            if (n == 1 && buf[0] < 128) {
                int *t = sh ? shifted : lower;
                if (!t[buf[0]])
                    t[buf[0]] = k + 1; /* +1: keycode 0 is a real key ('a') */
            }
            CFRelease(e);
        }
    }

    const char *probe = "testf aAzZ0.9/-cqQ";
    size_t i;
    printf("char -> keycode (0 = not found)\n");
    for (i = 0; i < strlen(probe); i++) {
        unsigned char c = (unsigned char)probe[i];
        printf("  '%c' (%3d)  plain=%-4d shift=%-4d\n", c, c,
               lower[c] ? lower[c] - 1 : 0, shifted[c] ? shifted[c] - 1 : 0);
    }
    int found = 0;
    for (i = 32; i < 127; i++)
        if (lower[i] || shifted[i])
            found++;
    printf("printable ASCII covered: %d/95\n", found);
    return found > 50 ? 0 : 1;
}
