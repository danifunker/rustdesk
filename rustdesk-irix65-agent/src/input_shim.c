/*
 * input_shim.c -- keyboard and pointer injection for IRIX, over XTEST.
 *
 * Presents exactly the C interface `input.rs` already calls on the Mac, so the
 * Rust side needs no IRIX branch: all the decisions -- which button, whether a
 * move is a drag, when a key needs shift -- stay in `decide_mouse`/`decide_key`,
 * which are pure and already tested on the host. This file is the arm, not the
 * brain.
 *
 * Two translations happen here because they are properties of the platform
 * rather than of the protocol:
 *
 *   Mac virtual keycodes -> X keysyms -> X keycodes.  `input.rs` speaks Mac
 *   keycodes because that is what it was written against; converting here keeps
 *   one table in one place, and going via *keysyms* rather than raw keycodes
 *   means the person's actual keyboard layout on the IRIX side is respected.
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
#include <unistd.h>
#include <setjmp.h>
#include <pthread.h>

#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
/* See the header: this port reports "Linux" too, so a Map-mode keycode from a
 * client is an X11/evdev keycode and not a Mac virtual keycode. */
#include "../../rustdesk-ppc-agent/src/linux_keycodes.h"
#include <X11/extensions/XTest.h>

#include "capture_shim.h"   /* rd_x_handlers and the per-thread I/O guard */

/*
 * The XTEST requests are issued by hand rather than through libXtst, because
 * IRIX ships XTEST only as a static archive and LLD cannot consume it:
 *
 *     ld.lld-irix: error: libXtst.a(XTest.o): invalid sh_info in symbol table
 *     ld.lld-irix: error: ...: multiple relocation sections to one section
 *                          are not supported
 *
 * The first is repairable -- tools/fix-sgi-archive.py does it, and the field is
 * genuinely wrong in SGI's objects -- but the second is a real gap in LLD's
 * MIPS support and not something a post-processing pass can paper over. Every X
 * library that *is* shared links fine, so this affects only the extensions SGI
 * shipped static.
 *
 * XTestFakeInput is one request with a fixed 36-byte body, so writing it out is
 * a few lines and removes the dependency entirely. The struct below is the
 * protocol definition from the XTEST spec; SGI ships XTest.h but not the
 * matching xteststr.h.
 */
typedef struct {
    CARD8  reqType;      /* the extension's major opcode, learned at runtime */
    CARD8  xtReqType;    /* X_XTestFakeInput */
    CARD16 length B16;
    CARD8  type;         /* KeyPress, ButtonRelease, MotionNotify, ... */
    CARD8  detail;       /* keycode, or button number */
    CARD16 pad0 B16;
    CARD32 time B32;     /* delay in milliseconds; 0 is CurrentTime */
    CARD32 root B32;     /* for MotionNotify */
    CARD32 pad1 B32;
    CARD32 pad2 B32;
    INT16  rootX B16, rootY B16;
    CARD32 pad3 B32;
    CARD16 pad4 B16;
    CARD8  pad5;
    CARD8  deviceid;
} xXTestFakeInputReq;
#define sz_xXTestFakeInputReq 36

/* Mac modifier masks, as `input.rs` sends them. */
#define MAC_SHIFT   0x00020000u
#define MAC_CONTROL 0x00040000u
#define MAC_ALT     0x00080000u
#define MAC_COMMAND 0x00100000u

static Display *g_dpy;
static int      g_screen;
static int      g_have_xtest;
static int      g_xtest_opcode;
static int      g_btn[8];        /* which X buttons this shim believes are down */
static unsigned g_held;          /* modifiers this shim pressed and must release */
static KeyCode  g_scratch;       /* the keycode press_keysym borrowed, if any */
static int      g_scratch_taken;

/* ONE THREAD AT A TIME ON g_dpy.
 *
 * Every session runs on its own thread and they all share this connection:
 * input from each, and a cursor-position query from each on every pass of its
 * loop, about 25 a second. The agent never calls XInitThreads, so Xlib's own
 * LockDisplay is a no-op -- and IRIX's Xlib, finding another thread mid-write
 * on a connection with no lock, waits for it like this (_XFlushInt, at libX11
 * .text+0xdf3d0 on 6.5.22m; found by tools/pcsample.c):
 *
 *     while (dpy->flags & XlibDisplayWriting)
 *         if (dpy->lock) ConditionWait(dpy, dpy->lock->writers);
 *
 * with both fields read once, into registers. With no lock that is a two-
 * instruction loop that never ends, even after the writer has finished. On the
 * O2, two Macs connected at once froze one session for good and left its
 * thread spinning, one more every time it happened: the agent burned the whole
 * CPU and every other session ran at a third of its speed. probes/xrace.c
 * shows it in under a second.
 *
 * So every entry point takes this lock (INPUT_ENTRY, at the end of the file).
 * Uncontended with one session, which is the usual case, and held for a few
 * milliseconds at most with two. XInitThreads would also do it, but it changes
 * how every Display in the process locks, including the capture connections
 * that are each only ever used by one thread; this is the one that is shared. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Open the display once and keep it. Injection is bursty -- a drag is dozens of
 * events -- and a connection per event would cost more than the events do. */
static Display *dpy(void)
{
    int first_event, first_error;
    if (g_dpy)
        return g_dpy;
    rd_x_handlers();
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "input: XOpenDisplay failed; no input will be injected\n");
        return NULL;
    }
    g_screen = DefaultScreen(g_dpy);
    g_have_xtest = XQueryExtension(g_dpy, XTestExtensionName, &g_xtest_opcode,
                                   &first_event, &first_error) ? 1 : 0;
    if (!g_have_xtest)
        fprintf(stderr, "input: XTEST absent; no input will be injected\n");
    return g_dpy;
}

/* One XTestFakeInput request. `root` and the coordinates matter only for
 * MotionNotify; the server ignores them otherwise. */
static void xtest_fake(int type, int detail, int x, int y, Window root)
{
    /* Xlibint.h's GetReq refers to a variable spelled exactly `dpy`, which is
     * also the name of the accessor above; the local shadows it deliberately. */
    Display *dpy = g_dpy;
    xXTestFakeInputReq *req;
    if (!dpy || !g_have_xtest)
        return;
    LockDisplay(dpy);
    GetReq(XTestFakeInput, req);
    req->reqType   = (CARD8)g_xtest_opcode;
    req->xtReqType = X_XTestFakeInput;
    req->type      = (CARD8)type;
    req->detail    = (CARD8)detail;
    req->time      = 0;            /* no delay */
    req->root      = (CARD32)root;
    req->rootX     = (INT16)x;
    req->rootY     = (INT16)y;
    req->deviceid  = 0;
    UnlockDisplay(dpy);
    SyncHandle();
}

static void fake_key(int keycode, int down)
{
    xtest_fake(down ? KeyPress : KeyRelease, keycode, 0, 0, None);
}

static void fake_button(int button, int down)
{
    xtest_fake(down ? ButtonPress : ButtonRelease, button, 0, 0, None);
}

static void fake_motion(int x, int y)
{
    xtest_fake(MotionNotify, 0, x, y, RootWindow(g_dpy, g_screen));
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
    /* Mac Command has no IRIX equivalent. Super is the literal match; a client
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
        g_scratch = 0;
        for (i = max_kc - min_kc; i >= 0; i--) {
            int j, empty = 1;
            for (j = 0; j < n; j++)
                if (map[i * n + j] != NoSymbol) { empty = 0; break; }
            if (empty) { g_scratch = (KeyCode)(min_kc + i); break; }
        }
        XFree(map);
        if (!g_scratch) {
            fprintf(stderr, "input: no spare keycode to type keysym 0x%lx\n",
                    (unsigned long)sym);
            return -1;
        }
        {
            KeySym two[2];
            two[0] = sym;
            two[1] = sym;
            XChangeKeyboardMapping(g_dpy, g_scratch, 2, two, 1);
            XSync(g_dpy, False);
        }
        g_scratch_taken = 1;
        fake_key((int)g_scratch, 1);
        XFlush(g_dpy);
        return 0;
    }

    if (g_scratch_taken) {
        KeySym none[2];
        fake_key((int)g_scratch, 0);
        XSync(g_dpy, False);
        none[0] = NoSymbol;
        none[1] = NoSymbol;
        XChangeKeyboardMapping(g_dpy, g_scratch, 2, none, 1);
        XSync(g_dpy, False);
        g_scratch_taken = 0;
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

/* --- what the entry points do -------------------------------------------- *
 *
 * Everything from here to INPUT_ENTRY runs with g_lock held and this thread's
 * X I/O guard armed; see the entry points at the end of the file. */

/* Let go of every modifier, whoever pressed it.
 *
 * Worth doing when a session starts: a modifier left held by a client that
 * disconnected mid-shortcut corrupts everything typed afterwards, and on this
 * side the person at the machine has no way to tell why. */
static void release_modifiers(void)
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

/* Wheel notches. X has no scroll axis: it has buttons 4/5 for vertical and
 * 6/7 for horizontal, one press-release per notch. `pixels` says the client
 * sent a distance rather than notches, so scale it down to something sane
 * instead of scrolling a page per twitch. */
static void scroll(int dy, int dx, int pixels)
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

static void key_mac(int keycode, int down)
{
    KeySym sym = mac_to_keysym(keycode);
    if (sym == NoSymbol) {
        fprintf(stderr, "input: no keysym for Mac keycode %d\n", keycode);
        return;
    }
    press_keysym(sym, down);
}

/* A keycode from the platform the agent told the peer it was -- Linux -- rather
 * than a Mac virtual keycode. Decode to a keysym and let press_keysym find
 * whatever keycode this server actually uses for it. */
static void key_platform(int keycode, int down)
{
    KeySym sym = rd_linux_to_keysym(keycode);
    if (sym == NoSymbol) {
        fprintf(stderr, "input: no keysym for X11 keycode %d\n", keycode);
        return;
    }
    press_keysym(sym, down);
}

static void key_platform_with_flags(int keycode, int down, unsigned int flags)
{
    if (down) {
        if (flags)
            mods_down(flags);
        key_platform(keycode, 1);
    } else {
        key_platform(keycode, 0);
        mods_up();
    }
}

static void key_mac_with_flags(int keycode, int down, unsigned int flags)
{
    if (down) {
        if (flags)
            mods_down(flags);
        key_mac(keycode, 1);
    } else {
        key_mac(keycode, 0);
        mods_up();
    }
}

/* Type a character. The layout is consulted first -- pressing the key that
 * actually produces the character is better than borrowing a keycode, because
 * applications that read the keyboard directly see a real key. */
static void key_char(unsigned int cp, int down, unsigned int flags)
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

static void key_unicode(unsigned int cp, int down)
{
    press_keysym(unicode_to_keysym(cp), down);
}

/* Which keycode would produce this character, and does it need shift?
 * Diagnostic only, for --probe-keys. */
static int keycode_for_char(unsigned int cp, int *needs_shift)
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

static void cursor_pos(double *x, double *y)
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

/* The connection died under us. A logout does that -- xdm restarts the server
 * -- and so does a server that wedges and is restarted by hand. Before the
 * guard, the next cursor query after that met Xlib's I/O error with nothing
 * armed and the handler exited the process, so the first session after a
 * logout took the agent down with it.
 *
 * Drop the connection and let the next call open a fresh one. The teardown is
 * rd_capture_close's: give the socket back, keep the Display allocation,
 * because XCloseDisplay has been seen to poison later connections in this
 * process. Whatever this shim believed was held down went with the server. */
static void input_lost(void)
{
    if (g_dpy)
        close(ConnectionNumber(g_dpy));
    g_dpy = NULL;
    g_have_xtest = 0;
    memset(g_btn, 0, sizeof g_btn);
    g_held = 0;
    g_scratch_taken = 0;
    fprintf(stderr, "input: X connection lost; the next event reconnects\n");
}

/* --- the interface input.rs calls ----------------------------------------- *
 *
 * Every entry point runs its body through INPUT_ENTRY: under g_lock, with the
 * calling thread's I/O guard armed. See g_lock for why the lock, and
 * input_lost for what happens when the guard fires. The bodies are the static
 * functions above; they call each other, never these, so nothing takes the
 * lock twice. */

#define INPUT_ENTRY(body)                                   \
    do {                                                    \
        pthread_mutex_lock(&g_lock);                        \
        if (setjmp(*rd_x_guard_jmp()) == 0) {               \
            rd_x_guard_arm();                               \
            body;                                           \
            rd_x_guard_disarm();                            \
        } else {                                            \
            input_lost();                                   \
        }                                                   \
        pthread_mutex_unlock(&g_lock);                      \
    } while (0)

void rd_release_modifiers(void)
{
    INPUT_ENTRY(release_modifiers());
}

void rd_mouse(int type, double x, double y, int button)
{
    INPUT_ENTRY(post_mouse(type, (int)x, (int)y, button, 1));
}

/* A button event from the client carries no coordinates -- proto3 omits zero
 * fields, so a press arrives as nothing but `mask`. Taking those absent
 * coordinates literally clicks the top-left corner every time. */
void rd_mouse_here(int type, int button)
{
    INPUT_ENTRY(post_mouse(type, 0, 0, button, 0));
}

void rd_scroll(int dy, int dx, int pixels)
{
    INPUT_ENTRY(scroll(dy, dx, pixels));
}

void rd_key(int keycode, int down)
{
    INPUT_ENTRY(key_mac(keycode, down));
}

void rd_key_platform(int keycode, int down)
{
    INPUT_ENTRY(key_platform(keycode, down));
}

void rd_key_platform_with_flags(int keycode, int down, unsigned int flags)
{
    INPUT_ENTRY(key_platform_with_flags(keycode, down, flags));
}

void rd_key_with_flags(int keycode, int down, unsigned int flags)
{
    INPUT_ENTRY(key_mac_with_flags(keycode, down, flags));
}

void rd_key_char(unsigned int cp, int down, unsigned int flags)
{
    INPUT_ENTRY(key_char(cp, down, flags));
}

void rd_key_unicode(unsigned int cp, int down)
{
    INPUT_ENTRY(key_unicode(cp, down));
}

int rd_keycode_for_char(unsigned int cp, int *needs_shift)
{
    volatile int kc = -1;   /* volatile: read after a longjmp */
    INPUT_ENTRY(kc = keycode_for_char(cp, needs_shift));
    return kc;
}

void rd_cursor_pos(double *x, double *y)
{
    if (x) *x = -1.0;
    if (y) *y = -1.0;
    INPUT_ENTRY(cursor_pos(x, y));
}
