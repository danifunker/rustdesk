#include "input.h"
#include "traps.h"

#include <Multiverse.h>
#include <string.h>

/* Low-memory globals (Inside Macintosh, Macintosh Toolbox Essentials). */
#define LM_MBTICKS   (*(volatile unsigned long *)0x016E)
#define LM_MBSTATE   (*(volatile unsigned char *)0x0172)
#define LM_KEYMAP    ((volatile unsigned char *)0x0174)
#define LM_MTEMP     (*(volatile long *)0x0828)
#define LM_RAWMOUSE  (*(volatile long *)0x082C)
#define LM_CRSRNEW   (*(volatile unsigned char *)0x08CE)
#define LM_CRSRCOUPLE (*(volatile unsigned char *)0x08CF)

/* Modifier bits in an EventRecord. */
enum { M_CMD = 0x0100, M_SHIFT = 0x0200, M_CAPS = 0x0400, M_OPTION = 0x0800, M_CONTROL = 0x1000 };

static int scr_w, scr_h;
static int buttons;       /* RustDesk button bits currently held */
static int mods;          /* EventRecord modifier bits currently held */
static int right_ctrl;    /* a right click is a control-click; this is its Control */
static unsigned char held[16]; /* our own KeyMap contribution, to release it */
static unsigned char legacy[256][2]; /* char -> keycode+1, [0] plain, [1] shifted */
static int legacy_ready;

static void keymap_set(int code, int down)
{
    unsigned char bit = (unsigned char)(1 << (code & 7));
    if (code < 0 || code > 127)
        return;
    if (down) {
        LM_KEYMAP[code >> 3] |= bit;
        held[code >> 3] |= bit;
    } else {
        LM_KEYMAP[code >> 3] &= (unsigned char)~bit;
        held[code >> 3] &= (unsigned char)~bit;
    }
}

static int modifier_for(int code)
{
    switch (code) {
    case 55: case 54: return M_CMD;
    case 56: case 60: return M_SHIFT;
    case 57: return M_CAPS;
    case 58: case 61: return M_OPTION;
    case 59: case 62: return M_CONTROL;
    default: return 0;
    }
}

/* The current keyboard layout. Looked up by the main loop, because the
 * Script Manager is not to be called at interrupt time; KeyTranslate is --
 * the keyboard driver itself uses it there. */
static Ptr kchr_ptr;

void input_set_kchr(Ptr p)
{
    if (p != kchr_ptr) {
        kchr_ptr = p;
        legacy_ready = 0;
    }
}

static Ptr kchr(void)
{
    return kchr_ptr;
}

static void build_legacy(void)
{
    Ptr map = kchr();
    int code, shifted;
    memset(legacy, 0, sizeof legacy);
    if (!map)
        return;
    for (shifted = 1; shifted >= 0; shifted--)
        for (code = 0; code < 128; code++) {
            uint32_t state = 0;
            unsigned c = (unsigned)(KeyTranslate(map, (uint16_t)(code | (shifted ? M_SHIFT : 0)),
                                                 &state) & 0xFF);
            if (c && !legacy[c][shifted])
                legacy[c][shifted] = (unsigned char)(code + 1);
        }
    legacy_ready = 1;
}

void input_init(int w, int h)
{
    scr_w = w;
    scr_h = h;
    buttons = mods = right_ctrl = 0;
    memset(held, 0, sizeof held);
}

static void move_to(int x, int y)
{
    union {
        Point p;
        long l;
    } u;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= scr_w)
        x = scr_w - 1;
    if (y >= scr_h)
        y = scr_h - 1;
    u.p.h = (short)x;
    u.p.v = (short)y;
    LM_MTEMP = u.l;
    LM_RAWMOUSE = u.l;
    LM_CRSRNEW = LM_CRSRCOUPLE;
}

static void post(short what, long message)
{
    EvQElPtr q = NULL;
    if (cdv_ppost_event(what, message, &q) == noErr && q)
        q->evtQModifiers = (INTEGER)(mods | (right_ctrl ? M_CONTROL : 0) |
                                     (buttons ? 0 : 0x0080 /* btnState: up */));
}

void input_mouse(int mask, int x, int y)
{
    int kind = mask & 7, button = mask >> 3;
    switch (kind) {
    case 0: /* move, or drag with a button held */
        move_to(x, y);
        if (buttons)
            LM_MBTICKS = TickCount() + 600;
        break;
    case 1: /* down; no coordinates means where the pointer already is */
    case 2: {
        int down = kind == 1, was = buttons;
        if (x || y)
            move_to(x, y);
        if (button != 1 && button != 2 && button != 4)
            break;
        if (down)
            buttons |= button;
        else
            buttons &= ~button;
        if (button == 2) { /* one button: right is Control-click */
            if (down) {
                right_ctrl = 1;
                keymap_set(59, 1);
            }
        }
        if (!was && buttons) {
            LM_MBTICKS = TickCount() + 600;
            LM_MBSTATE = 0x00;
            post(mouseDown, 0);
        } else if (was && !buttons) {
            LM_MBTICKS = TickCount();
            LM_MBSTATE = 0x80;
            post(mouseUp, 0);
            if (right_ctrl) {
                right_ctrl = 0;
                if (!(mods & M_CONTROL))
                    keymap_set(59, 0);
            }
        }
        break;
    }
    default:
        break; /* wheel, trackpad: no scroll events on this system */
    }
}

/* Mac virtual keycodes for RustDesk's named keys. */
static int control_keycode(uint32_t ck)
{
    switch (ck) {
    case CK_RETURN: return 36;
    case CK_NUMPAD_ENTER: return 76;
    case CK_TAB: return 48;
    case CK_SPACE: return 49;
    case CK_BACKSPACE: return 51;
    case CK_ESCAPE: return 53;
    case CK_DELETE: return 117;
    case CK_HOME: return 115;
    case CK_END: return 119;
    case CK_PAGEUP: return 116;
    case CK_PAGEDOWN: return 121;
    case CK_LEFT: return 123;
    case CK_RIGHT: return 124;
    case CK_DOWN: return 125;
    case CK_UP: return 126;
    case CK_SHIFT: return 56;
    case CK_RSHIFT: return 60;
    case CK_CONTROL: return 59;
    case CK_RCONTROL: return 62;
    case CK_ALT: case CK_OPTION: return 58;
    case CK_RALT: return 61;
    case CK_META: return 55;
    case CK_RWIN: return 54;
    case CK_CAPSLOCK: return 57;
    case CK_HELP: return 114;
    case CK_CLEAR: return 71;
    case CK_F1: return 122; case CK_F2: return 120; case CK_F3: return 99;
    case CK_F4: return 118; case CK_F5: return 96; case CK_F6: return 97;
    case CK_F7: return 98; case CK_F8: return 100; case CK_F9: return 101;
    case CK_F10: return 109; case CK_F11: return 103; case CK_F12: return 111;
    case CK_MULTIPLY: return 67;
    case CK_ADD: return 69;
    case CK_SUBTRACT: return 78;
    case CK_DECIMAL: return 65;
    case CK_DIVIDE: return 75;
    case CK_EQUALS: return 81;
    default:
        if (ck >= CK_NUMPAD0 && ck <= CK_NUMPAD9) {
            static const unsigned char pad[10] = { 82, 83, 84, 85, 86, 87, 88, 89, 91, 92 };
            return pad[ck - CK_NUMPAD0];
        }
        return -1;
    }
}

static void keycode(int code, int down)
{
    int m = modifier_for(code);
    keymap_set(code, down);
    if (m) {
        /* Modifiers change state; they post nothing. */
        if (down)
            mods |= m;
        else
            mods &= ~m;
        return;
    }
    {
        Ptr map = kchr();
        uint32_t state = 0;
        unsigned c = map ? (unsigned)(KeyTranslate(map, (uint16_t)(code | (mods & 0xFF00)),
                                                   &state) & 0xFF)
                         : 0;
        post(down ? keyDown : keyUp, ((long)code << 8) | c);
    }
}

void input_key(const cdv_key *k)
{
    int code = -1, extra_shift = 0;
    switch (k->kind) {
    case KEY_CONTROL:
        code = control_keycode(k->value);
        break;
    case KEY_CHR:
        if (k->mode == KMODE_MAP || k->mode == KMODE_TRANSLATE) {
            code = k->value < 128 ? (int)k->value : -1;
        } else {
            unsigned c = k->value;
            if (!legacy_ready)
                build_legacy();
            if (c == '\r')
                code = 36;
            else if (c < 256 && legacy[c][0])
                code = legacy[c][0] - 1;
            else if (c < 256 && legacy[c][1]) {
                code = legacy[c][1] - 1;
                extra_shift = !(mods & M_SHIFT);
            }
        }
        break;
    default:
        return; /* unicode and sequences: later, via the clipboard path */
    }
    if (code < 0)
        return;
    if (extra_shift)
        keycode(56, 1);
    if (k->press) {
        keycode(code, 1);
        keycode(code, 0);
    } else {
        keycode(code, k->down);
    }
    if (extra_shift)
        keycode(56, 0);
}

void input_release_all(void)
{
    int i;
    for (i = 0; i < 16; i++)
        LM_KEYMAP[i] &= (unsigned char)~held[i];
    memset(held, 0, sizeof held);
    mods = right_ctrl = 0;
    if (buttons) {
        buttons = 0;
        LM_MBTICKS = TickCount();
        LM_MBSTATE = 0x80;
        post(mouseUp, 0);
    }
}
