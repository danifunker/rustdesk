/* Character -> keycode via the real layout API.
 *
 * The CoreGraphics-only trick (keymap.c) fails on Leopard: reading a synthesized
 * event's unicode string gives nothing. UCKeyTranslate is the supported way, but
 * it lives in Carbon, so this probe exists to find out whether Carbon compiles
 * and links here before the shim depends on it.
 */
#include <Carbon/Carbon.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    TISInputSourceRef src = TISCopyCurrentKeyboardLayoutInputSource();
    if (!src) {
        printf("TISCopyCurrentKeyboardLayoutInputSource returned NULL\n");
        return 1;
    }
    CFDataRef data = (CFDataRef)TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData);
    if (!data) {
        printf("no kTISPropertyUnicodeKeyLayoutData\n");
        return 1;
    }
    const UCKeyboardLayout *layout = (const UCKeyboardLayout *)CFDataGetBytePtr(data);
    UInt32 kbdType = LMGetKbdType();

    int lower[128], shifted[128];
    memset(lower, 0, sizeof lower);
    memset(shifted, 0, sizeof shifted);

    int k, sh;
    for (k = 0; k < 128; k++) {
        for (sh = 0; sh < 2; sh++) {
            UInt32 dead = 0;
            UniChar chars[8];
            UniCharCount len = 0;
            /* modifier state is the Carbon modifiers shifted right by 8 */
            UInt32 mods = sh ? (shiftKey >> 8) : 0;
            OSStatus st = UCKeyTranslate(layout, (UInt16)k, kUCKeyActionDown, mods, kbdType,
                                         kUCKeyTranslateNoDeadKeysBit, &dead, 8, &len, chars);
            if (st == noErr && len == 1 && chars[0] < 128) {
                int *t = sh ? shifted : lower;
                if (!t[chars[0]])
                    t[chars[0]] = k + 1; /* +1: keycode 0 is 'a' */
            }
        }
    }

    const char *probe = "testf aAzZ0.9/-cqQ";
    size_t i;
    printf("char -> keycode (-1 = not found)\n");
    for (i = 0; i < strlen(probe); i++) {
        unsigned char c = (unsigned char)probe[i];
        printf("  '%c' (%3d)  plain=%-4d shift=%-4d\n", c, c,
               lower[c] - 1, shifted[c] - 1);
    }
    int found = 0;
    for (i = 32; i < 127; i++)
        if (lower[i] || shifted[i])
            found++;
    printf("printable ASCII covered: %d/95\n", found);
    CFRelease(src);
    return found > 50 ? 0 : 1;
}
