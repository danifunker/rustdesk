/* Quartz event injection, with no struct crossing the FFI boundary.
 *
 * CGPoint is two doubles (16 bytes), passed and returned *by value* by the
 * CoreGraphics API. On 32-bit PowerPC that is precisely where the calling
 * convention stops matching a naive `extern "C"` declaration: a direct Rust FFI
 * of CGEventCreateMouseEvent delivered the x correctly and left y as denormal
 * garbage, and CGEventGetLocation returned nonsense for the same reason (a
 * 16-byte return uses a hidden pointer here).
 *
 * Measured before this shim existed:
 *     asked for     : 640, 360
 *     cursor after  : 640, 0.0000...805
 *
 * So the struct is built and consumed on the C side, and only scalars cross.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>   /* UCKeyTranslate; see rd_key_char */

static CGEventSourceRef src(void)
{
    static CGEventSourceRef s;
    static int tried;
    if (!tried) {
        tried = 1;
        s = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    }
    return s;
}

/* type: 0 move, 1 down, 2 up, 3 dragged-left, 4 dragged-right
 * button: 0 left, 1 right, 2 centre */
static void post_mouse(int type, CGPoint pt, int button)
{
    CGEventType et;
    CGMouseButton b = (CGMouseButton)button;

    switch (type) {
    case 1:
        et = (button == 1) ? kCGEventRightMouseDown
           : (button == 2) ? kCGEventOtherMouseDown : kCGEventLeftMouseDown;
        break;
    case 2:
        et = (button == 1) ? kCGEventRightMouseUp
           : (button == 2) ? kCGEventOtherMouseUp : kCGEventLeftMouseUp;
        break;
    case 3: et = kCGEventLeftMouseDragged;  break;
    case 4: et = kCGEventRightMouseDragged; break;
    default: et = kCGEventMouseMoved;       break;
    }

    CGEventRef e = CGEventCreateMouseEvent(src(), et, pt, b);
    if (e) {
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }
}

void rd_mouse(int type, double x, double y, int button)
{
    post_mouse(type, CGPointMake(x, y), button);
}

/* Post at wherever the cursor actually is.
 *
 * A button event from the client carries no coordinates: proto3 omits zero
 * fields, so a press arrives as a 4-byte message that is nothing but `mask`.
 * Taking those absent coordinates literally clicks the top-left corner every
 * time. Upstream's macOS path has the same shape — `input_service.rs` passes no
 * position to enigo's `mouse_down`, and enigo reads the current location itself.
 */
void rd_mouse_here(int type, int button)
{
    CGEventRef cur = CGEventCreate(NULL);
    CGPoint pt = CGPointMake(0.0, 0.0);
    if (cur) {
        pt = CGEventGetLocation(cur);
        CFRelease(cur);
    }
    post_mouse(type, pt, button);
}

/* Both axes. wheel1 is vertical, wheel2 horizontal; a client's two-finger
 * sideways swipe arrives as x and was previously dropped on the floor. */
void rd_scroll(int dy, int dx)
{
    CGEventRef e = CGEventCreateScrollWheelEvent(src(), kCGScrollEventUnitLine, 2, dy, dx);
    if (e) {
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }
}

void rd_key(int keycode, int down)
{
    CGEventRef e = CGEventCreateKeyboardEvent(src(), (CGKeyCode)keycode, down ? true : false);
    if (e) {
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }
}

/* Type a single unicode scalar without needing a keycode for it. */
void rd_key_unicode(unsigned int cp, int down)
{
    CGEventRef e = CGEventCreateKeyboardEvent(src(), 0, down ? true : false);
    if (!e)
        return;
    UniChar u = (UniChar)cp;
    CGEventKeyboardSetUnicodeString(e, 1, &u);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

/* --- modifiers ------------------------------------------------------------ */

/* Modifier flags and the keycodes that produce them. */
static const struct { unsigned int flag; int keycode; } MODS[] = {
    { kCGEventFlagMaskShift,     56 },
    { kCGEventFlagMaskControl,   59 },
    { kCGEventFlagMaskAlternate, 58 },
    { kCGEventFlagMaskCommand,   55 },
};
#define NMODS ((int)(sizeof MODS / sizeof MODS[0]))

/* Modifiers this shim pressed itself, and must therefore release. */
static unsigned int g_held;

static void post_key(int keycode, int down)
{
    CGEventRef e = CGEventCreateKeyboardEvent(src(), (CGKeyCode)keycode, down ? true : false);
    if (e) {
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }
}

/* Hold down the modifiers a keystroke needs.
 *
 * Setting CGEventFlags on the keystroke itself looks equivalent and is not: the
 * window server takes the flags on a posted event as the new modifier state, so
 * a synthetic ctrl-C leaves Control held **for every event afterwards**, with
 * nothing to release it. On this platform a Control-click is a right-click, so
 * the visible symptom is that left-clicking starts opening context menus, with
 * nothing wrong in the mouse path at all.
 *
 * Pressing the real keys keeps the state consistent and self-clearing. Anything
 * the peer is already holding is left alone, so this composes with the explicit
 * modifier key events a client also sends.
 */
static void mods_down(unsigned int flags)
{
    unsigned int cur = (unsigned int)CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
    int i;
    for (i = 0; i < NMODS; i++) {
        if ((flags & MODS[i].flag) && !(cur & MODS[i].flag)) {
            post_key(MODS[i].keycode, 1);
            g_held |= MODS[i].flag;
        }
    }
}

/* Release only what mods_down pressed. */
static void mods_up(void)
{
    int i;
    for (i = 0; i < NMODS; i++)
        if (g_held & MODS[i].flag)
            post_key(MODS[i].keycode, 0);
    g_held = 0;
}

/* Release every modifier, both sides of the keyboard.
 *
 * Called when a session starts, so a modifier left stuck by an earlier run --
 * or by a client that disconnected mid-shortcut -- does not silently corrupt
 * every click and keystroke that follows.
 */
void rd_release_modifiers(void)
{
    static const int all[] = { 56, 60, 59, 62, 58, 61, 55, 54 };
    int i;
    for (i = 0; i < (int)(sizeof all / sizeof all[0]); i++) {
        CGEventRef e = CGEventCreateKeyboardEvent(src(), (CGKeyCode)all[i], false);
        if (e) {
            CGEventSetFlags(e, 0);
            CGEventPost(kCGHIDEventTap, e);
            CFRelease(e);
        }
    }
    g_held = 0;
}

/* --- typing a character -------------------------------------------------- */

/* Reverse map of the *current* keyboard layout: character -> virtual keycode.
 * Entries are keycode+1 so that 0 means "absent" (keycode 0 is a real key, 'a').
 */
static int kc_plain[128], kc_shift[128], kc_ready;

/* Built with UCKeyTranslate, which is why this file needs Carbon.
 *
 * The obvious CoreGraphics-only alternative -- synthesize an event per keycode
 * and read back CGEventKeyboardGetUnicodeString -- returns nothing at all on
 * Leopard, measured: 0 of 95 printable ASCII characters mapped. UCKeyTranslate
 * maps all 95, and follows whatever layout the machine is set to instead of
 * assuming a US one.
 */
static void build_keymap(void)
{
    kc_ready = 1;
    TISInputSourceRef s = TISCopyCurrentKeyboardLayoutInputSource();
    if (!s)
        return;
    CFDataRef data = (CFDataRef)TISGetInputSourceProperty(s, kTISPropertyUnicodeKeyLayoutData);
    if (!data) {
        CFRelease(s);
        return;
    }
    const UCKeyboardLayout *layout = (const UCKeyboardLayout *)CFDataGetBytePtr(data);
    UInt32 kbd = LMGetKbdType();

    int k, sh;
    for (k = 0; k < 128; k++) {
        for (sh = 0; sh < 2; sh++) {
            UInt32 dead = 0;
            UniChar chars[8];
            UniCharCount len = 0;
            UInt32 mods = sh ? (shiftKey >> 8) : 0;   /* Carbon modifiers >> 8 */
            if (UCKeyTranslate(layout, (UInt16)k, kUCKeyActionDown, mods, kbd,
                               kUCKeyTranslateNoDeadKeysBit, &dead, 8, &len, chars) == noErr
                && len == 1 && chars[0] < 128) {
                int *t = sh ? kc_shift : kc_plain;
                if (!t[chars[0]])
                    t[chars[0]] = k + 1;
            }
        }
    }
    CFRelease(s);
}

/* Diagnostic: which keycode would this character be typed as, and does it need
 * shift? Returns -1 if the layout cannot produce it, in which case rd_key_char
 * falls back to unicode entry. Exists because the alternative ways to check the
 * mapping on this OS are all unavailable: a listen-only event tap needs
 * "Enable access for assistive devices", and typing into a window only proves
 * something if you already know which window has focus.
 */
int rd_keycode_for_char(unsigned int cp, int *needs_shift)
{
    if (!kc_ready)
        build_keymap();
    *needs_shift = 0;
    if (cp >= 128)
        return -1;
    if (kc_plain[cp])
        return kc_plain[cp] - 1;
    if (kc_shift[cp]) {
        *needs_shift = 1;
        return kc_shift[cp] - 1;
    }
    return -1;
}

/* Type a character the client sent as `chr`.
 *
 * `chr` carries a *character*, not a keycode -- typing "test" arrives as
 * 116,101,115,116. Posting those as virtual keycodes types PageUp, F9, Home,
 * PageUp, which is why keyboard input appeared to do nothing at all.
 *
 * Going through a keycode rather than just attaching a unicode string matters
 * for shortcuts: Cmd+C has to arrive as the 'c' *key* with the command flag set,
 * or the target application never sees a shortcut.
 */
void rd_key_char(unsigned int cp, int down, unsigned int flags)
{
    if (!kc_ready)
        build_keymap();

    if (cp < 128) {
        int kc = -1;
        unsigned int f = flags;
        if (kc_plain[cp])
            kc = kc_plain[cp] - 1;
        else if (kc_shift[cp]) {
            kc = kc_shift[cp] - 1;
            f |= kCGEventFlagMaskShift;
        }
        if (kc >= 0) {
            /* Hold the modifiers as keys rather than stamping flags on this
             * event; see mods_down. The event inherits the resulting state. */
            if (down) {
                if (f)
                    mods_down(f);
                post_key(kc, 1);
            } else {
                post_key(kc, 0);
                mods_up();
            }
            return;
        }
    }
    /* Anything this layout cannot produce still gets typed, just without a
     * keycode -- no shortcut, but the character appears. */
    rd_key_unicode(cp, down);
}

/* Modifier flags applied to subsequent synthetic events. */
void rd_key_with_flags(int keycode, int down, unsigned int flags)
{
    if (down) {
        if (flags)
            mods_down(flags);
        post_key(keycode, 1);
    } else {
        post_key(keycode, 0);
        mods_up();
    }
}

/* Out-params rather than a returned struct, for the same ABI reason. */
void rd_cursor_pos(double *x, double *y)
{
    CGEventRef e = CGEventCreate(NULL);
    if (!e) {
        *x = -1;
        *y = -1;
        return;
    }
    CGPoint p = CGEventGetLocation(e);
    *x = p.x;
    *y = p.y;
    CFRelease(e);
}
