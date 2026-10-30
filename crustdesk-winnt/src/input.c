/* Keyboard and pointer from the peer.
 *
 * We tell the peer we are "Windows", so in Map mode (a client's default) a
 * key arrives as a PC/AT set-1 scancode, 0xE0nn for the extended keys --
 * what upstream's Windows server gives SendInput. It goes in with the
 * virtual key this machine's layout gives it, so the keyboard layout here
 * decides what is typed, as it would for a real keyboard. Legacy mode and
 * typed text send characters: those go through VkKeyScan, and one the
 * layout has no key for is typed as Alt + its ANSI code on the keypad.
 */
#include "input.h"
#include "log.h"

#include "win.h"
#include <string.h>

#ifndef MOUSEEVENTF_WHEEL
#define MOUSEEVENTF_WHEEL 0x0800
#endif
#ifndef VK_LWIN
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#endif

/* ControlKey values session.h does not name. */
enum { CK_CANCEL = 43, CK_MENU = 45, CK_PAUSE = 46, CK_SELECT = 54, CK_PRINT = 55,
       CK_EXECUTE = 56, CK_SNAPSHOT = 57, CK_INSERT = 58, CK_SLEEP = 60, CK_SEPARATOR = 61,
       CK_SCROLL = 62, CK_NUMLOCK = 63, CK_APPS = 65, CK_CTRLALTDEL = 100, CK_LOCKSCREEN = 101 };

static int scr_w = 640, scr_h = 480;
static uint8_t key_down[256];   /* virtual keys we pressed and have not released */
static int btn_down;            /* MOUSEEVENTF_*DOWN bits of buttons held */
static unsigned held;           /* modifiers pressed for a Legacy keystroke */

void input_init(int width, int height)
{
    scr_w = width > 1 ? width : 2;
    scr_h = height > 1 ? height : 2;
}

/* ---- keys ------------------------------------------------------------------- */

static int is_modifier(int vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN ||
           vk == VK_RWIN || vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_LSHIFT ||
           vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_LMENU;
}

/* Injected keys go through the raw input thread like the keyboard's, but a
 * burst of them -- a typed character with its shift, the peer's own shift
 * and key arriving in one packet -- can have a modifier change overtake the
 * key next to it on a slow machine: "Nt4" for shift-N-T-4. A modifier
 * change waits for the input queue to take what came before it. */
#define MOD_PACE_MS 15

static void vk_event(int vk, int sc, int ext, int down)
{
    DWORD fl = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    int mod;
    if (vk <= 0 || vk > 255)
        return;
    if (!sc)
        sc = (int)MapVirtualKey((UINT)vk, 0);
    mod = is_modifier(vk);
    if (mod)
        Sleep(MOD_PACE_MS);
    keybd_event((BYTE)vk, (BYTE)sc, fl, 0);
    if (mod)
        Sleep(MOD_PACE_MS);
    key_down[vk] = (uint8_t)down;
}

static int is_extended_vk(int vk)
{
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_SNAPSHOT:
        return 1;
    }
    return 0;
}

static void vk_press(int vk, int down)
{
    vk_event(vk, 0, is_extended_vk(vk), down);
}

/* A set-1 scancode (0xE0nn: extended) as this keyboard's virtual key. */
static int scancode_vk(uint32_t code, int *ext)
{
    static const struct { uint8_t sc; uint8_t vk; } e0[] = {
        { 0x1C, VK_RETURN }, { 0x1D, VK_RCONTROL }, { 0x35, VK_DIVIDE }, { 0x37, VK_SNAPSHOT },
        { 0x38, VK_RMENU }, { 0x45, VK_NUMLOCK }, { 0x47, VK_HOME }, { 0x48, VK_UP },
        { 0x49, VK_PRIOR }, { 0x4B, VK_LEFT }, { 0x4D, VK_RIGHT }, { 0x4F, VK_END },
        { 0x50, VK_DOWN }, { 0x51, VK_NEXT }, { 0x52, VK_INSERT }, { 0x53, VK_DELETE },
        { 0x5B, VK_LWIN }, { 0x5C, VK_RWIN }, { 0x5D, VK_APPS },
    };
    /* The keypad without NumLock is the cursor keys; with it, digits. */
    static const struct { uint8_t sc; uint8_t vk; } pad[] = {
        { 0x47, VK_NUMPAD7 }, { 0x48, VK_NUMPAD8 }, { 0x49, VK_NUMPAD9 }, { 0x4B, VK_NUMPAD4 },
        { 0x4C, VK_NUMPAD5 }, { 0x4D, VK_NUMPAD6 }, { 0x4F, VK_NUMPAD1 }, { 0x50, VK_NUMPAD2 },
        { 0x51, VK_NUMPAD3 }, { 0x52, VK_NUMPAD0 }, { 0x53, VK_DECIMAL },
    };
    int sc = (int)(code & 0xff);
    size_t i;
    *ext = (code & 0xff00) == 0xe000;
    if (*ext) {
        for (i = 0; i < sizeof e0 / sizeof e0[0]; i++)
            if (e0[i].sc == sc)
                return e0[i].vk;
        return (int)MapVirtualKey((UINT)sc, 1);
    }
    if (GetKeyState(VK_NUMLOCK) & 1)
        for (i = 0; i < sizeof pad / sizeof pad[0]; i++)
            if (pad[i].sc == sc)
                return pad[i].vk;
    if (sc == 0x45)
        return VK_PAUSE;
    if (sc == 0x36)
        return VK_SHIFT;
    return (int)MapVirtualKey((UINT)sc, 1);
}

static int control_vk(unsigned ck)
{
    static const struct { uint8_t ck; uint8_t vk; } t[] = {
        { CK_ALT, VK_MENU }, { CK_BACKSPACE, VK_BACK }, { CK_CAPSLOCK, VK_CAPITAL },
        { CK_CONTROL, VK_CONTROL }, { CK_DELETE, VK_DELETE }, { CK_DOWN, VK_DOWN },
        { CK_END, VK_END }, { CK_ESCAPE, VK_ESCAPE }, { CK_F1, VK_F1 }, { CK_F2, VK_F2 },
        { CK_F3, VK_F3 }, { CK_F4, VK_F4 }, { CK_F5, VK_F5 }, { CK_F6, VK_F6 },
        { CK_F7, VK_F7 }, { CK_F8, VK_F8 }, { CK_F9, VK_F9 }, { CK_F10, VK_F10 },
        { CK_F11, VK_F11 }, { CK_F12, VK_F12 }, { CK_HOME, VK_HOME }, { CK_LEFT, VK_LEFT },
        { CK_META, VK_LWIN }, { CK_OPTION, VK_MENU }, { CK_PAGEDOWN, VK_NEXT },
        { CK_PAGEUP, VK_PRIOR }, { CK_RETURN, VK_RETURN }, { CK_RIGHT, VK_RIGHT },
        { CK_SHIFT, VK_SHIFT }, { CK_SPACE, VK_SPACE }, { CK_TAB, VK_TAB }, { CK_UP, VK_UP },
        { CK_CANCEL, VK_CANCEL }, { CK_CLEAR, VK_CLEAR }, { CK_MENU, VK_MENU },
        { CK_PAUSE, VK_PAUSE }, { CK_SELECT, VK_SELECT }, { CK_PRINT, VK_PRINT },
        { CK_EXECUTE, VK_EXECUTE }, { CK_SNAPSHOT, VK_SNAPSHOT }, { CK_INSERT, VK_INSERT },
        { CK_HELP, VK_HELP }, { CK_SEPARATOR, VK_SEPARATOR }, { CK_SCROLL, VK_SCROLL },
        { CK_NUMLOCK, VK_NUMLOCK }, { CK_RWIN, VK_RWIN }, { CK_APPS, VK_APPS },
        { CK_MULTIPLY, VK_MULTIPLY }, { CK_ADD, VK_ADD }, { CK_SUBTRACT, VK_SUBTRACT },
        { CK_DECIMAL, VK_DECIMAL }, { CK_DIVIDE, VK_DIVIDE }, { CK_EQUALS, 0xBB },
        { CK_NUMPAD_ENTER, VK_RETURN }, { CK_RSHIFT, VK_SHIFT }, { CK_RCONTROL, VK_RCONTROL },
        { CK_RALT, VK_RMENU },
    };
    size_t i;
    if (ck >= CK_NUMPAD0 && ck <= CK_NUMPAD9)
        return VK_NUMPAD0 + (int)(ck - CK_NUMPAD0);
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].ck == ck)
            return t[i].vk;
    return 0;
}

static void mods_down(int mods)
{
    static const struct { int bit; int vk; } m[] = {
        { MOD_SHIFT, VK_SHIFT }, { MOD_CONTROL, VK_CONTROL },
        { MOD_OPTION, VK_MENU }, { MOD_COMMAND, VK_LWIN },
    };
    size_t i;
    for (i = 0; i < sizeof m / sizeof m[0]; i++)
        if ((mods & m[i].bit) && !(held & (unsigned)m[i].bit)) {
            vk_press(m[i].vk, 1);
            held |= (unsigned)m[i].bit;
        }
}

static void mods_up(void)
{
    if (held & MOD_SHIFT)
        vk_press(VK_SHIFT, 0);
    if (held & MOD_CONTROL)
        vk_press(VK_CONTROL, 0);
    if (held & MOD_OPTION)
        vk_press(VK_MENU, 0);
    if (held & MOD_COMMAND)
        vk_press(VK_LWIN, 0);
    held = 0;
}

/* Type one character: the key that makes it here, with the shift states it
 * needs (those the peer already holds are left alone), or Alt+0nnn. */
static void type_char(uint32_t cp, int down, int up)
{
    SHORT r;
    int vk, need;
    if (cp == '\r' || cp == '\n')
        cp = '\r';
    if (cp > 0xffff)
        return;
    r = VkKeyScanW((WCHAR)cp);
    if (r != -1 && (r & 0xff) != 0xff) {
        vk = r & 0xff;
        need = (r >> 8) & 7;
        if (down) {
            if ((need & 1) && !(held & MOD_SHIFT) && !key_down[VK_SHIFT])
                vk_press(VK_SHIFT, 1);
            else
                need &= ~1;
            if ((need & 2) && !key_down[VK_CONTROL])
                vk_press(VK_CONTROL, 1);
            else
                need &= ~2;
            if ((need & 4) && !key_down[VK_MENU])
                vk_press(VK_MENU, 1);
            else
                need &= ~4;
            vk_press(vk, 1);
            if (up)
                vk_press(vk, 0);
            if (need & 4)
                vk_press(VK_MENU, 0);
            if (need & 2)
                vk_press(VK_CONTROL, 0);
            if (need & 1)
                vk_press(VK_SHIFT, 0);
        } else if (up) {
            vk_press(vk, 0);
        }
        return;
    }
    if (down) {
        /* No key for it: Alt and its code page number on the keypad. */
        WCHAR w = (WCHAR)cp;
        char a[4], digits[8];
        BOOL lost = FALSE;
        int i;
        if (WideCharToMultiByte(CP_ACP, 0, &w, 1, a, sizeof a, NULL, &lost) != 1 || lost)
            return;
        wsprintf(digits, "0%u", (unsigned)(uint8_t)a[0]);
        vk_press(VK_MENU, 1);
        for (i = 0; digits[i]; i++) {
            vk_press(VK_NUMPAD0 + digits[i] - '0', 1);
            vk_press(VK_NUMPAD0 + digits[i] - '0', 0);
            Sleep(MOD_PACE_MS);
        }
        vk_press(VK_MENU, 0);
    }
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
    int vk = 0, ext = 0, sc = 0, down = k->press || k->down, up = k->press || !k->down;
    switch (k->kind) {
    case KEY_CHR:
        if (k->mode == KMODE_LEGACY) {
            if (down)
                mods_down(k->mods);
            type_char(k->value, down, up);
            if (up)
                mods_up();
            return;
        }
        vk = scancode_vk(k->value, &ext);
        sc = (int)(k->value & 0xff);
        if (!vk)
            cdv_debug("no virtual key for scancode 0x%x", (unsigned)k->value);
        break;
    case KEY_CONTROL:
        if (k->value == CK_CTRLALTDEL) {
            if (down)
                input_sas();
            return;
        }
        if (k->value == CK_LOCKSCREEN) {
            cdv_log("the peer asked to lock the screen; NT 4.0 cannot be told to");
            return;
        }
        vk = control_vk(k->value);
        ext = is_extended_vk(vk) || k->value == CK_NUMPAD_ENTER;
        break;
    case KEY_UNICODE:
        if (k->mode != KMODE_MAP && down)
            mods_down(k->mods);
        type_char(k->value, down, up);
        if (up && k->mode != KMODE_MAP)
            mods_up();
        return;
    case KEY_SEQ: {
        size_t o = 0, l;
        uint32_t cp;
        if (!down)
            return;
        while ((l = utf8_next(k->seq + o, k->seqlen - o, &cp)) != 0) {
            type_char(cp, 1, 1);
            o += l;
        }
        return;
    }
    default:
        return;
    }
    if (!vk)
        return;
    /* Map mode replays the peer's keyboard key by key, modifiers included;
     * the other modes say which modifiers go with a key. */
    if (down) {
        if (k->mode != KMODE_MAP)
            mods_down(k->mods);
        vk_event(vk, sc, ext, 1);
    }
    if (up) {
        vk_event(vk, sc, ext, 0);
        if (k->mode != KMODE_MAP)
            mods_up();
    }
}

/* ---- pointer -------------------------------------------------------------- */

static void button(DWORD downflag, DWORD upflag, int down)
{
    int was = (btn_down & (int)downflag) != 0;
    if (was == down)
        return;
    mouse_event(down ? downflag : upflag, 0, 0, 0, 0);
    if (down)
        btn_down |= (int)downflag;
    else
        btn_down &= ~(int)downflag;
}

void input_mouse(int mask, int x, int y)
{
    int kind = mask & 7, b = mask >> 3;
    /* A press or release carries no position unless the pointer moved with
     * it (proto3 leaves zeros out): (0, 0) there means "where it is". */
    if (kind == 0 || ((kind == 1 || kind == 2) && (x || y))) {
        DWORD nx, ny;
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x >= scr_w)
            x = scr_w - 1;
        if (y >= scr_h)
            y = scr_h - 1;
        nx = (DWORD)((x * 65535L + (scr_w - 1) / 2) / (scr_w - 1));
        ny = (DWORD)((y * 65535L + (scr_h - 1) / 2) / (scr_h - 1));
        mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny, 0, 0);
    }
    if (kind == 1 || kind == 2) {
        int down = kind == 1;
        if (b == 1)
            button(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, down);
        else if (b == 2)
            button(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, down);
        else if (b == 4)
            button(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, down);
    } else if (kind == 3 && y) {
        /* The wheel: y > 0 is up; one notch is 120. */
        int n = y < 0 ? -y : y;
        if (n > 10)
            n = (n + 59) / 60; /* pixels, not notches */
        if (n > 10)
            n = 10;
        mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)((y > 0 ? 120 : -120) * n), 0);
    }
}

/* A session ended or began: nothing the peer pressed may stay down. */
void input_release_all(void)
{
    int vk;
    button(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0);
    button(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0);
    button(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0);
    for (vk = 1; vk < 256; vk++)
        if (key_down[vk])
            vk_press(vk, 0);
    held = 0;
}

/* ---- Ctrl-Alt-Del ------------------------------------------------------------- */

/* Winlogon watches for the secure attention sequence with a hot key on a
 * window of its own desktop; posting it that hot key is what the keyboard
 * would have done. Only a thread with no windows may change desktop, so a
 * thread of its own does it. Needs SYSTEM, which the service is. */
static DWORD WINAPI sas_thread(LPVOID unused)
{
    HDESK d = OpenDesktop("Winlogon", 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE |
                                                     DESKTOP_WRITEOBJECTS | GENERIC_WRITE);
    HWND w;
    (void)unused;
    if (!d) {
        cdv_log("Ctrl-Alt-Del: cannot open the Winlogon desktop (error %lu)", GetLastError());
        return 0;
    }
    if (!SetThreadDesktop(d)) {
        cdv_log("Ctrl-Alt-Del: cannot move to the Winlogon desktop (error %lu)", GetLastError());
    } else {
        w = FindWindow("SAS window class", "SAS window");
        if (w)
            PostMessage(w, WM_HOTKEY, 0, MAKELONG(HOTKEY_ALT | HOTKEY_CONTROL, VK_DELETE));
        else
            cdv_log("Ctrl-Alt-Del: Winlogon's window is not there");
    }
    CloseDesktop(d);
    return 0;
}

void input_sas(void)
{
    DWORD tid;
    HANDLE t;
    cdv_log("the peer sent Ctrl-Alt-Del");
    input_release_all();
    t = CreateThread(NULL, 0, sas_thread, NULL, 0, &tid);
    if (t) {
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
}
