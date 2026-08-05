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
void rd_mouse(int type, double x, double y, int button)
{
    CGPoint pt = CGPointMake(x, y);
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

void rd_scroll(int dy)
{
    CGEventRef e = CGEventCreateScrollWheelEvent(src(), kCGScrollEventUnitLine, 1, dy);
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

/* Modifier flags applied to subsequent synthetic events. */
void rd_key_with_flags(int keycode, int down, unsigned int flags)
{
    CGEventRef e = CGEventCreateKeyboardEvent(src(), (CGKeyCode)keycode, down ? true : false);
    if (!e)
        return;
    if (flags)
        CGEventSetFlags(e, (CGEventFlags)flags);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
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
