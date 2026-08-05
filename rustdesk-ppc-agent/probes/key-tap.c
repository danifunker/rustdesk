/* What does rd_key_char actually put on the event stream?
 *
 * Typing into a window proves nothing unless you know which window had focus,
 * and chasing that around a live desktop disturbs whoever is using the machine.
 * An event tap is passive: it reports the keycode, flags and resulting character
 * of every key event posted, so the shim can be checked without any application
 * being involved at all.
 *
 * Links against the real src/input_shim.c -- this tests the shipped code path,
 * not a copy of it.
 *
 *   gcc -O2 key-tap.c ../src/input_shim.c -framework Carbon \
 *       -framework ApplicationServices -o key-tap
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void rd_key_char(unsigned int cp, int down, unsigned int flags);
void rd_key(int keycode, int down);

static int seen;

static CGEventRef on_event(CGEventTapProxy proxy, CGEventType type, CGEventRef e, void *ctx) {
    (void)proxy;
    (void)ctx;
    if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
        CGKeyCode kc = (CGKeyCode)CGEventGetIntegerValueField(e, kCGKeyboardEventKeycode);
        CGEventFlags fl = CGEventGetFlags(e);
        UniChar buf[8];
        UniCharCount n = 0;
        CGEventKeyboardGetUnicodeString(e, 8, &n, buf);
        char txt[16];
        size_t i;
        for (i = 0; i < n && i < sizeof txt - 1; i++)
            txt[i] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '?';
        txt[i] = 0;
        printf("  %-7s keycode=%-4u flags=%#010llx text=%s\n",
               type == kCGEventKeyDown ? "DOWN" : "UP", (unsigned)kc,
               (unsigned long long)fl, n ? txt : "(none)");
        fflush(stdout);
        seen++;
    }
    return e;
}

int main(void) {
    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp);
    CFMachPortRef tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                         kCGEventTapOptionListenOnly, mask, on_event, NULL);
    if (!tap) {
        printf("could not create an event tap.\n");
        printf("On Leopard a listen-only tap needs \"Enable access for assistive\n");
        printf("devices\" in System Preferences > Universal Access.\n");
        return 1;
    }
    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    printf("tap installed; posting keys through the shim\n");

    pid_t child = fork();
    if (child == 0) {
        sleep(2);
        /* Exactly what a client sends for "rd": characters, not keycodes. */
        rd_key_char('r', 1, 0);
        usleep(120000);
        rd_key_char('r', 0, 0);
        usleep(200000);
        rd_key_char('d', 1, 0);
        usleep(120000);
        rd_key_char('d', 0, 0);
        usleep(200000);
        /* An uppercase letter, which the layout can only reach with shift. */
        rd_key_char('R', 1, 0);
        usleep(120000);
        rd_key_char('R', 0, 0);
        usleep(200000);
        /* A known-good control key, as a control: Escape is keycode 53. */
        rd_key(53, 1);
        usleep(120000);
        rd_key(53, 0);
        _exit(0);
    }

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 6.0, false);
    printf("\n%d key events seen\n", seen);
    printf("expected: 'r' keycode 15, 'd' keycode 2, 'R' keycode 15 with shift,\n");
    printf("          then Escape keycode 53\n");
    return seen ? 0 : 1;
}
