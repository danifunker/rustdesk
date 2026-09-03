/*
 * input_shim.c -- keyboard and pointer injection for Solaris 10, over XTEST.
 *
 * Taken from the IRIX port's shim, which is the same X11 code, with one
 * difference: the XTEST requests go through libXtst here. IRIX ships that
 * extension only as a static archive its linker cannot consume, so that port
 * writes the protocol out by hand; Solaris has a perfectly good
 * /usr/openwin/lib/sparcv9/libXtst.so.1, and hand-rolled requests would be a
 * workaround for someone else's problem.
 *
 * Presents exactly the C interface `input.rs` already calls on the Mac, so the
 * Rust side needs no Solaris branch: all the decisions -- which button, whether
 * a move is a drag, when a key needs shift -- stay in `decide_mouse`/`decide_key`,
 * which are pure and already tested on the host. This file is the arm, not the
 * brain.
 *
 * Two translations happen here because they are properties of the platform
 * rather than of the protocol:
 *
 *   Mac virtual keycodes -> X keysyms -> X keycodes.  `input.rs` speaks Mac
 *   keycodes because that is what it was written against; converting here keeps
 *   one table in one place, and going via *keysyms* rather than raw keycodes
 *   means the person's actual keyboard layout on the Solaris side is respected.
 *
 *   Mac button order -> X button order.  Mac calls them 0=left, 1=right,
 *   2=middle; X calls them 1=left, 2=middle, 3=right. Getting this wrong swaps
 *   right-click and middle-click, which is the kind of thing that gets blamed
 *   on the network.
 *
 * A character with no keycode in the current layout is typed by temporarily
 * remapping a spare keycode to its keysym, pressing it, and putting the mapping
 * back -- the same trick xdotool uses, and the only way to type a character the
 * layout cannot otherwise produce.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
/* The standard X11/evdev keymap, shared by every X11 port here: a Map-mode
 * keycode is in the keyspace of the platform we told the peer we are. */
#include "../../rustdesk-ppc-agent/src/linux_keycodes.h"
#include <X11/extensions/XTest.h>

/* Mac modifier masks, as `input.rs` sends them. */
#define MAC_SHIFT   0x00020000u
#define MAC_CONTROL 0x00040000u
#define MAC_ALT     0x00080000u
#define MAC_COMMAND 0x00100000u

static Display *g_dpy;
static int      g_screen;
static int      g_have_xtest;
static int      g_nbuttons;      /* buttons this server's pointer actually has */
static int      g_btn[8];        /* which X buttons this shim believes are down */
static unsigned g_held;          /* modifiers this shim pressed and must release */

/* Xlib's default protocol-error handler calls exit(). One rejected injection
 * would then take the agent down mid-session, which is the wrong trade for an
 * input event: report it and carry on. Measured on this machine: faking button
 * 5 on a three-button pointer is a BadValue, and killed the process. */
static int on_x_error(Display *d, XErrorEvent *e)
{
    char buf[96];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "input: X error %d (%s), request %d.%d\n",
            e->error_code, buf, e->request_code, e->minor_code);
    return 0;
}

/* Open the display once and keep it. Injection is bursty -- a drag is dozens of
 * events -- and a connection per event would cost more than the events do. */
static Display *dpy(void)
{
    int first_event, first_error, major, minor;
    if (g_dpy)
        return g_dpy;
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "input: XOpenDisplay failed; no input will be injected\n");
        return NULL;
    }
    g_screen = DefaultScreen(g_dpy);
    XSetErrorHandler(on_x_error);
    g_have_xtest = XTestQueryExtension(g_dpy, &first_event, &first_error,
                                       &major, &minor) ? 1 : 0;
    if (!g_have_xtest)
        fprintf(stderr, "input: XTEST absent; no input will be injected\n");
    {
        /* How many buttons the pointer has, which is not always five. A wheel
         * is buttons 4 and 5 in X, and this machine's server has a three-button
         * pointer: faking button 5 there is a BadValue, not a no-op. */
        unsigned char map[32];
        g_nbuttons = XGetPointerMapping(g_dpy, map, (int)sizeof map);
        if (g_nbuttons < 1) g_nbuttons = 3;
    }
    return g_dpy;
}

static void fake_key(int keycode, int down)
{
    if (!g_dpy || !g_have_xtest) return;
    XTestFakeKeyEvent(g_dpy, (unsigned int)keycode, down ? True : False, CurrentTime);
}

static void fake_button(int button, int down)
{
    if (!g_dpy || !g_have_xtest) return;
    if (button > g_nbuttons) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "input: pointer has %d buttons; button %d "
                            "(the wheel, most likely) cannot be injected here\n",
                    g_nbuttons, button);
        }
        return;
    }
    XTestFakeButtonEvent(g_dpy, (unsigned int)button, down ? True : False, CurrentTime);
}

static void fake_motion(int x, int y)
{
    if (!g_dpy || !g_have_xtest) return;
    XTestFakeMotionEvent(g_dpy, g_screen, x, y, CurrentTime);
}

static int ready(void)
{
    return dpy() != NULL && g_have_xtest;
}

/* --- keysym tables -------------------------------------------------------- */

/* Mac virtual keycode -> X keysym, for the keys `control_key_to_keycode` in
 * input.rs can produce. Anything absent falls through to unicode entry. */
static const struct { int mac; KeySym sym; } MACKEY[] = {
    {  36, XK_Return },     {  48, XK_Tab },        {  49, XK_space },
    {  51, XK_BackSpace },  {  53, XK_Escape },     { 117, XK_Delete },
    { 115, XK_Home },       { 119, XK_End },        { 116, XK_Prior },
    { 121, XK_Next },       { 123, XK_Left },       { 124, XK_Right },
    { 125, XK_Down },       { 126, XK_Up },
    {  56, XK_Shift_L },    {  60, XK_Shift_R },
    {  59, XK_Control_L },  {  62, XK_Control_R },
    {  58, XK_Alt_L },      {  61, XK_Alt_R },
    /* Mac Command has no Solaris equivalent. Super is the literal match; a client
     * that wants Command to act as Control should send Control. */
    {  55, XK_Super_L },    {  54, XK_Super_R },
    {  57, XK_Caps_Lock },
    { 122, XK_F1 },  { 120, XK_F2 },  {  99, XK_F3 },  { 118, XK_F4 },
    {  96, XK_F5 },  {  97, XK_F6 },  {  98, XK_F7 },  { 100, XK_F8 },
    { 101, XK_F9 },  { 109, XK_F10 }, { 103, XK_F11 }, { 111, XK_F12 },
};
#define NMACKEY ((int)(sizeof MACKEY / sizeof MACKEY[0]))

static KeySym mac_to_keysym(int mac)
{
    int i;
    for (i = 0; i < NMACKEY; i++)
        if (MACKEY[i].mac == mac)
            return MACKEY[i].sym;
    return NoSymbol;
}

/* A Unicode code point as an X keysym. Latin-1 is its own keysym; everything
 * else uses the 0x01000000 encoding every modern X server understands. */
static KeySym unicode_to_keysym(unsigned int cp)
{
    if (cp == 0)
        return NoSymbol;
    if (cp < 0x80 || (cp >= 0xa0 && cp <= 0xff))
        return (KeySym)cp;
    return (KeySym)(cp | 0x01000000u);
}

/* --- pressing keys -------------------------------------------------------- */

/* Press or release a keysym, borrowing a spare keycode if the layout has no key
 * for it. Returns 0 on success.
 *
 * The borrow has to be put back, and it has to be put back *after* the server
 * has processed the press -- hence the XSync before restoring. Leaving a
 * remapped keycode behind would silently corrupt the person's own keyboard. */
static int press_keysym(KeySym sym, int down)
{
    KeyCode kc;
    static KeyCode scratch;
    static int scratch_taken;

    if (sym == NoSymbol || !ready())
        return -1;

    kc = XKeysymToKeycode(g_dpy, sym);
    if (kc != 0) {
        fake_key((int)kc, down);
        XFlush(g_dpy);
        return 0;
    }

    /* No key produces this symbol. Borrow one.
     *
     * Only on the press: the release has to use the same borrowed keycode, so
     * the mapping is held until then rather than restored in between. */
    if (down) {
        int min_kc, max_kc, n, i;
        KeySym *map;
        XDisplayKeycodes(g_dpy, &min_kc, &max_kc);
        map = XGetKeyboardMapping(g_dpy, min_kc, max_kc - min_kc + 1, &n);
        if (!map)
            return -1;
        scratch = 0;
        for (i = max_kc - min_kc; i >= 0; i--) {
            int j, empty = 1;
            for (j = 0; j < n; j++)
                if (map[i * n + j] != NoSymbol) { empty = 0; break; }
            if (empty) { scratch = (KeyCode)(min_kc + i); break; }
        }
        XFree(map);
        if (!scratch) {
            fprintf(stderr, "input: no spare keycode to type keysym 0x%lx\n",
                    (unsigned long)sym);
            return -1;
        }
        {
            KeySym two[2];
            two[0] = sym;
            two[1] = sym;
            XChangeKeyboardMapping(g_dpy, scratch, 2, two, 1);
            XSync(g_dpy, False);
        }
        scratch_taken = 1;
        fake_key((int)scratch, 1);
        XFlush(g_dpy);
        return 0;
    }

    if (scratch_taken) {
        KeySym none[2];
        fake_key((int)scratch, 0);
        XSync(g_dpy, False);
        none[0] = NoSymbol;
        none[1] = NoSymbol;
        XChangeKeyboardMapping(g_dpy, scratch, 2, none, 1);
        XSync(g_dpy, False);
        scratch_taken = 0;
        return 0;
    }
    return -1;
}

/* Hold down the modifiers a keystroke needs, remembering which so they can be
 * let go afterwards. */
static void mods_down(unsigned int flags)
{
    if ((flags & MAC_SHIFT)   && !(g_held & MAC_SHIFT))   { press_keysym(XK_Shift_L, 1);   g_held |= MAC_SHIFT; }
    if ((flags & MAC_CONTROL) && !(g_held & MAC_CONTROL)) { press_keysym(XK_Control_L, 1); g_held |= MAC_CONTROL; }
    if ((flags & MAC_ALT)     && !(g_held & MAC_ALT))     { press_keysym(XK_Alt_L, 1);     g_held |= MAC_ALT; }
    if ((flags & MAC_COMMAND) && !(g_held & MAC_COMMAND)) { press_keysym(XK_Super_L, 1);   g_held |= MAC_COMMAND; }
}

static void mods_up(void)
{
    if (g_held & MAC_SHIFT)   press_keysym(XK_Shift_L, 0);
    if (g_held & MAC_CONTROL) press_keysym(XK_Control_L, 0);
    if (g_held & MAC_ALT)     press_keysym(XK_Alt_L, 0);
    if (g_held & MAC_COMMAND) press_keysym(XK_Super_L, 0);
    g_held = 0;
}

/* --- the interface input.rs calls ----------------------------------------- */

/* Let go of every modifier, whoever pressed it.
 *
 * Worth doing when a session starts: a modifier left held by a client that
 * disconnected mid-shortcut corrupts everything typed afterwards, and on this
 * side the person at the machine has no way to tell why. */
void rd_release_modifiers(void)
{
    static const KeySym all[] = {
        XK_Shift_L, XK_Shift_R, XK_Control_L, XK_Control_R,
        XK_Alt_L, XK_Alt_R, XK_Super_L, XK_Super_R, XK_Meta_L, XK_Meta_R,
    };
    int i;
    if (!ready())
        return;
    for (i = 0; i < (int)(sizeof all / sizeof all[0]); i++)
        press_keysym(all[i], 0);
    g_held = 0;
    XFlush(g_dpy);
}

/* Mac button numbering to X's. */
static int x_button(int mac)
{
    switch (mac) {
    case 0:  return 1;   /* left */
    case 1:  return 3;   /* right */
    case 2:  return 2;   /* middle */
    default: return 1;
    }
}

static void post_mouse(int type, int x, int y, int button, int have_xy)
{
    int b;
    if (!ready())
        return;
    if (have_xy)
        fake_motion(x, y);
    b = x_button(button);
    if (type == 1 && !g_btn[b]) {
        fake_button(b, 1);
        g_btn[b] = 1;
    } else if (type == 2 && g_btn[b]) {
        fake_button(b, 0);
        g_btn[b] = 0;
    }
    /* Types 0, 3 and 4 are moves; a move with a button already held *is* a
     * drag as far as X is concerned, so there is nothing extra to do. */
    XFlush(g_dpy);
}

void rd_mouse(int type, double x, double y, int button)
{
    post_mouse(type, (int)x, (int)y, button, 1);
}

/* A button event from the client carries no coordinates -- proto3 omits zero
 * fields, so a press arrives as nothing but `mask`. Taking those absent
 * coordinates literally clicks the top-left corner every time. */
void rd_mouse_here(int type, int button)
{
    post_mouse(type, 0, 0, button, 0);
}

/* Wheel notches. X has no scroll axis: it has buttons 4/5 for vertical and
 * 6/7 for horizontal, one press-release per notch. `pixels` says the client
 * sent a distance rather than notches, so scale it down to something sane
 * instead of scrolling a page per twitch. */
void rd_scroll(int dy, int dx, int pixels)
{
    int i, n;
    if (!ready())
        return;
    if (dy) {
        int up = dy > 0;
        n = dy > 0 ? dy : -dy;
        if (pixels) n = (n + 19) / 20;
        if (n > 20) n = 20;
        for (i = 0; i < n; i++) {
            fake_button(up ? 4 : 5, 1);
            fake_button(up ? 4 : 5, 0);
        }
    }
    if (dx) {
        int left = dx > 0;
        n = dx > 0 ? dx : -dx;
        if (pixels) n = (n + 19) / 20;
        if (n > 20) n = 20;
        for (i = 0; i < n; i++) {
            fake_button(left ? 6 : 7, 1);
            fake_button(left ? 6 : 7, 0);
        }
    }
    XFlush(g_dpy);
}

void rd_key(int keycode, int down)
{
    KeySym sym = mac_to_keysym(keycode);
    if (sym == NoSymbol) {
        fprintf(stderr, "input: no keysym for Mac keycode %d\n", keycode);
        return;
    }
    press_keysym(sym, down);
}

/* A keycode from the platform the agent told the peer it was -- Linux -- rather
 * than a Mac virtual keycode. See LINUXKEY. */
void rd_key_platform(int keycode, int down)
{
    KeySym sym = rd_linux_to_keysym(keycode);
    if (sym == NoSymbol) {
        fprintf(stderr, "input: no keysym for X11 keycode %d\n", keycode);
        return;
    }
    press_keysym(sym, down);
}

void rd_key_platform_with_flags(int keycode, int down, unsigned int flags)
{
    if (down) {
        if (flags)
            mods_down(flags);
        rd_key_platform(keycode, 1);
    } else {
        rd_key_platform(keycode, 0);
        mods_up();
    }
}

void rd_key_with_flags(int keycode, int down, unsigned int flags)
{
    if (down) {
        if (flags)
            mods_down(flags);
        rd_key(keycode, 1);
    } else {
        rd_key(keycode, 0);
        mods_up();
    }
}

/* Type a character. The layout is consulted first -- pressing the key that
 * actually produces the character is better than borrowing a keycode, because
 * applications that read the keyboard directly see a real key. */
void rd_key_char(unsigned int cp, int down, unsigned int flags)
{
    KeySym sym = unicode_to_keysym(cp);
    if (down) {
        if (flags)
            mods_down(flags);
        press_keysym(sym, 1);
    } else {
        press_keysym(sym, 0);
        mods_up();
    }
}

void rd_key_unicode(unsigned int cp, int down)
{
    press_keysym(unicode_to_keysym(cp), down);
}

/* Which keycode would produce this character, and does it need shift?
 * Diagnostic only, for --probe-keys. */
int rd_keycode_for_char(unsigned int cp, int *needs_shift)
{
    KeySym sym = unicode_to_keysym(cp);
    KeyCode kc;
    int min_kc, max_kc, n, i;
    KeySym *map;

    if (needs_shift)
        *needs_shift = 0;
    if (!ready() || sym == NoSymbol)
        return -1;
    kc = XKeysymToKeycode(g_dpy, sym);
    if (kc == 0)
        return -1;

    XDisplayKeycodes(g_dpy, &min_kc, &max_kc);
    map = XGetKeyboardMapping(g_dpy, kc, 1, &n);
    if (map) {
        for (i = 0; i < n; i++) {
            if (map[i] == sym) {
                if (needs_shift)
                    *needs_shift = (i % 2) ? 1 : 0;
                break;
            }
        }
        XFree(map);
    }
    return (int)kc;
}

void rd_cursor_pos(double *x, double *y)
{
    Window root_ret, child_ret;
    int rx = 0, ry = 0, wx, wy;
    unsigned int mask;

    if (x) *x = -1.0;
    if (y) *y = -1.0;
    if (!dpy())
        return;
    if (!XQueryPointer(g_dpy, RootWindow(g_dpy, g_screen), &root_ret, &child_ret,
                       &rx, &ry, &wx, &wy, &mask))
        return;   /* pointer is on another screen; -1 tells the caller to skip */
    if (x) *x = (double)rx;
    if (y) *y = (double)ry;
}
