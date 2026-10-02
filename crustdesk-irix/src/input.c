/* Keyboard and pointer from the peer, injected through XTEST.
 *
 * The decisions follow rustdesk-irix65-agent/src/input_shim.c, which has
 * been run against real clients on an O2: keys go through keysyms, so the
 * Indigo's own keyboard layout decides which key is pressed; a character the
 * layout cannot type borrows a spare keycode for the moment; X numbers its
 * buttons 1 left, 2 middle, 3 right, 4/5 the wheel. Single-threaded here, so
 * none of that file's locking is needed, and SGI's libXtst links fine with
 * SGI's ld.
 *
 * We tell the peer we are "Linux", so in Map mode a key arrives as an X11
 * keycode of the standard evdev keymap (linux_keycodes.h, shared with the
 * other X11 ports).
 */
#include "input.h"
#include "log.h"

#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include "../../rustdesk-ppc-agent/src/linux_keycodes.h"

static Display *dpy;
static int have_xtest;
static int btn_down[8];
static unsigned held;          /* modifiers we pressed for a Legacy keystroke */
static KeyCode scratch;        /* a borrowed keycode, while one is down */
static int px, py;             /* where we last put the pointer */

void input_init(Display *d)
{
    int ev, er, maj, min;
    dpy = d;
    have_xtest = XTestQueryExtension(dpy, &ev, &er, &maj, &min);
    if (!have_xtest)
        cdv_log("XTEST is missing: the peer cannot type or click");
    memset(btn_down, 0, sizeof btn_down);
    held = 0;
    scratch = 0;
}

/* ---- keys ------------------------------------------------------------------- */

static void fake_key(KeyCode kc, int down)
{
    XTestFakeKeyEvent(dpy, kc, down ? True : False, CurrentTime);
}

/* Is `sym` the shifted symbol of its key (so shift must be held to type it)? */
static int needs_shift(KeyCode kc, KeySym sym)
{
    return XKeycodeToKeysym(dpy, kc, 0) != sym && XKeycodeToKeysym(dpy, kc, 1) == sym;
}

/* Press or release a keysym; a character no key produces borrows the
 * highest keycode with nothing on it until its release. `shift_ok`: add
 * shift for a shifted symbol (typing characters, not replaying keys). */
static void press_keysym(KeySym sym, int down, int shift_ok)
{
    KeyCode kc;
    if (sym == NoSymbol || !have_xtest)
        return;
    kc = XKeysymToKeycode(dpy, sym);
    if (kc) {
        int shift = shift_ok && needs_shift(kc, sym) && !(held & 1);
        KeyCode sk = shift ? XKeysymToKeycode(dpy, XK_Shift_L) : 0;
        if (down && sk)
            fake_key(sk, 1);
        fake_key(kc, down);
        if (down && sk)
            fake_key(sk, 0);
        return;
    }
    if (down) {
        int lo, hi, n, i, j;
        KeySym *map, two[2];
        XDisplayKeycodes(dpy, &lo, &hi);
        map = XGetKeyboardMapping(dpy, (KeyCode)lo, hi - lo + 1, &n);
        if (!map)
            return;
        scratch = 0;
        for (i = hi - lo; i >= 0 && !scratch; i--) {
            int empty = 1;
            for (j = 0; j < n; j++)
                if (map[i * n + j] != NoSymbol)
                    empty = 0;
            if (empty)
                scratch = (KeyCode)(lo + i);
        }
        XFree(map);
        if (!scratch) {
            cdv_log("no spare keycode to type keysym 0x%lx", (unsigned long)sym);
            return;
        }
        two[0] = two[1] = sym;
        XChangeKeyboardMapping(dpy, scratch, 2, two, 1);
        XSync(dpy, False);
        fake_key(scratch, 1);
    } else if (scratch) {
        KeySym none[2];
        fake_key(scratch, 0);
        XSync(dpy, False);
        none[0] = none[1] = NoSymbol;
        XChangeKeyboardMapping(dpy, scratch, 2, none, 1);
        scratch = 0;
    }
}

static KeySym control_keysym(unsigned ck)
{
    static const struct { unsigned char ck; KeySym sym; } t[] = {
        { CK_ALT, XK_Alt_L }, { CK_BACKSPACE, XK_BackSpace }, { CK_CAPSLOCK, XK_Caps_Lock },
        { CK_CONTROL, XK_Control_L }, { CK_DELETE, XK_Delete }, { CK_DOWN, XK_Down },
        { CK_END, XK_End }, { CK_ESCAPE, XK_Escape }, { CK_F1, XK_F1 }, { CK_F2, XK_F2 },
        { CK_F3, XK_F3 }, { CK_F4, XK_F4 }, { CK_F5, XK_F5 }, { CK_F6, XK_F6 },
        { CK_F7, XK_F7 }, { CK_F8, XK_F8 }, { CK_F9, XK_F9 }, { CK_F10, XK_F10 },
        { CK_F11, XK_F11 }, { CK_F12, XK_F12 }, { CK_HOME, XK_Home }, { CK_LEFT, XK_Left },
        /* SGI keyboards have no Windows key; Meta is the nearest. */
        { CK_META, XK_Alt_L }, { CK_OPTION, XK_Alt_L }, { CK_PAGEDOWN, XK_Next },
        { CK_PAGEUP, XK_Prior }, { CK_RETURN, XK_Return }, { CK_RIGHT, XK_Right },
        { CK_SHIFT, XK_Shift_L }, { CK_SPACE, XK_space }, { CK_TAB, XK_Tab },
        { CK_UP, XK_Up }, { CK_CLEAR, XK_Clear }, { CK_HELP, XK_Help },
        { 58, XK_Insert }, { 46, XK_Pause }, { 55, XK_Print }, { 57, XK_Print },
        { 62, XK_Scroll_Lock }, { 63, XK_Num_Lock }, { 65, XK_Menu },
        { CK_RWIN, XK_Alt_R }, { CK_MULTIPLY, XK_KP_Multiply }, { CK_ADD, XK_KP_Add },
        { CK_SUBTRACT, XK_KP_Subtract }, { CK_DECIMAL, XK_KP_Decimal },
        { CK_DIVIDE, XK_KP_Divide }, { CK_EQUALS, XK_KP_Equal },
        { CK_NUMPAD_ENTER, XK_KP_Enter }, { CK_RSHIFT, XK_Shift_R },
        { CK_RCONTROL, XK_Control_R }, { CK_RALT, XK_Alt_R },
    };
    size_t i;
    if (ck >= CK_NUMPAD0 && ck <= CK_NUMPAD9)
        return XK_KP_0 + (ck - CK_NUMPAD0);
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].ck == ck)
            return t[i].sym;
    return NoSymbol;
}

static KeySym unicode_keysym(uint32_t cp)
{
    if (cp == '\r' || cp == '\n')
        return XK_Return;
    if (cp == '\t')
        return XK_Tab;
    if (cp == 8)
        return XK_BackSpace;
    if (cp == 27)
        return XK_Escape;
    if (cp < 0x20)
        return NoSymbol;
    if (cp < 0x7f || (cp >= 0xa0 && cp <= 0xff))
        return (KeySym)cp;
    /* IRIX 5.3's X predates the 0x01000000 Unicode keysyms; the few it might
     * map are tried, and a missing one borrows a keycode like anything else. */
    return (KeySym)(cp | 0x01000000u);
}

static void mods_down(int mods)
{
    static const struct { int bit; KeySym sym; } m[] = {
        { MOD_SHIFT, XK_Shift_L }, { MOD_CONTROL, XK_Control_L },
        { MOD_OPTION, XK_Alt_L }, { MOD_COMMAND, XK_Alt_L },
    };
    size_t i;
    for (i = 0; i < sizeof m / sizeof m[0]; i++)
        if ((mods & m[i].bit) && !(held & (unsigned)m[i].bit)) {
            press_keysym(m[i].sym, 1, 0);
            held |= (unsigned)m[i].bit;
        }
}

static void mods_up(void)
{
    if (held & MOD_SHIFT)
        press_keysym(XK_Shift_L, 0, 0);
    if (held & MOD_CONTROL)
        press_keysym(XK_Control_L, 0, 0);
    if (held & (MOD_OPTION | MOD_COMMAND))
        press_keysym(XK_Alt_L, 0, 0);
    held = 0;
}

/* One UTF-8 character from s[0..n); returns its length (0 at the end). */
static size_t utf8_next(const char *s, size_t n, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!n)
        return 0;
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xe0) == 0xc0 && n >= 2) {
        *cp = (uint32_t)(p[0] & 0x1f) << 6 | (p[1] & 0x3f);
        return 2;
    }
    if ((p[0] & 0xf0) == 0xe0 && n >= 3) {
        *cp = (uint32_t)(p[0] & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
        return 3;
    }
    if ((p[0] & 0xf8) == 0xf0 && n >= 4) {
        *cp = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3f) << 12 |
              (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
        return 4;
    }
    *cp = '?';
    return 1;
}

void input_key(const cdv_key *k)
{
    KeySym sym = NoSymbol;
    int typed = 0; /* a character, typed with whatever shift it needs */
    if (!have_xtest)
        return;
    switch (k->kind) {
    case KEY_CHR:
        if (k->mode == KMODE_LEGACY) {
            sym = unicode_keysym(k->value);
            typed = 1;
        } else {
            sym = rd_linux_to_keysym((int)k->value);
            if (sym == NoSymbol)
                cdv_debug("no keysym for X11 keycode %u", k->value);
        }
        break;
    case KEY_CONTROL:
        sym = control_keysym(k->value);
        break;
    case KEY_UNICODE:
        sym = unicode_keysym(k->value);
        typed = 1;
        break;
    case KEY_SEQ: {
        size_t o = 0, l;
        uint32_t cp;
        if (!k->down && !k->press)
            return;
        while ((l = utf8_next(k->seq + o, k->seqlen - o, &cp)) != 0) {
            KeySym s = unicode_keysym(cp);
            press_keysym(s, 1, 1);
            press_keysym(s, 0, 1);
            o += l;
        }
        XFlush(dpy);
        return;
    }
    default:
        return;
    }
    if (sym == NoSymbol)
        return;
    /* Map mode replays the peer's keyboard key by key, modifiers included;
     * the other modes say which modifiers go with a key. */
    if (k->press || k->down) {
        if (k->mode != KMODE_MAP)
            mods_down(k->mods);
        press_keysym(sym, 1, typed);
    }
    if (k->press || !k->down) {
        press_keysym(sym, 0, typed);
        if (k->mode != KMODE_MAP)
            mods_up();
    }
    XFlush(dpy);
}

/* ---- pointer -------------------------------------------------------------- */

static void button(int b, int down)
{
    if (b < 1 || b > 7 || btn_down[b] == down)
        return;
    XTestFakeButtonEvent(dpy, (unsigned)b, down ? True : False, CurrentTime);
    btn_down[b] = down;
}

void input_mouse(int mask, int x, int y)
{
    int kind = mask & 7, b = mask >> 3, xb;
    if (!have_xtest)
        return;
    /* A press or release carries no position unless the pointer moved with
     * it (proto3 leaves zeros out): (0, 0) there means "where it is". */
    if (kind == 0 || ((kind == 1 || kind == 2) && (x || y))) {
        if (x != px || y != py) {
            XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
            px = x;
            py = y;
        }
    }
    xb = b == 1 ? 1 : b == 2 ? 3 : b == 4 ? 2 : 0;
    if (kind == 1 && xb)
        button(xb, 1);
    else if (kind == 2 && xb)
        button(xb, 0);
    else if (kind == 3) {
        /* The wheel: y > 0 is up (button 4), one press per notch; x sideways
         * (6/7) for servers that know them. */
        int n, i, up = y > 0;
        n = y < 0 ? -y : y;
        if (n > 10)
            n = (n + 59) / 60; /* pixels, not notches */
        for (i = 0; i < n && i < 10; i++) {
            XTestFakeButtonEvent(dpy, up ? 4 : 5, True, CurrentTime);
            XTestFakeButtonEvent(dpy, up ? 4 : 5, False, CurrentTime);
        }
        n = x < 0 ? -x : x;
        if (n > 10)
            n = (n + 59) / 60;
        for (i = 0; i < n && i < 10; i++) {
            XTestFakeButtonEvent(dpy, x > 0 ? 6 : 7, True, CurrentTime);
            XTestFakeButtonEvent(dpy, x > 0 ? 6 : 7, False, CurrentTime);
        }
    }
    XFlush(dpy);
}

/* A session ended or began: nothing the peer pressed may stay down. */
void input_release_all(void)
{
    static const KeySym mods[] = { XK_Shift_L, XK_Shift_R, XK_Control_L, XK_Control_R,
                                   XK_Alt_L, XK_Alt_R, XK_Meta_L, XK_Meta_R };
    size_t i;
    int b;
    if (!dpy || !have_xtest)
        return;
    for (b = 1; b < 8; b++)
        button(b, 0);
    for (i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        KeyCode kc = XKeysymToKeycode(dpy, mods[i]);
        if (kc)
            fake_key(kc, 0);
    }
    if (scratch) {
        KeySym none[2];
        fake_key(scratch, 0);
        XSync(dpy, False);
        none[0] = none[1] = NoSymbol;
        XChangeKeyboardMapping(dpy, scratch, 2, none, 1);
        scratch = 0;
    }
    held = 0;
    XFlush(dpy);
}
