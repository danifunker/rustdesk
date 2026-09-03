/* linux_keycodes.h -- the standard X11/evdev keymap, as a keycode -> keysym
 * table.
 *
 * WHY A TABLE AND NOT A LOOKUP
 *
 * A client in Map mode sends a keycode it has already translated for the
 * platform the agent *reported*, and every X11 port here reports "Linux" --
 * deliberately, because clients match that string against a known set and Linux
 * is the entry whose key handling suits an X11 desktop. So what arrives is a
 * keycode from the standard X11/evdev keymap.
 *
 * That half cannot be discovered locally. Nothing on Solaris or IRIX knows what
 * Linux keycode 38 means; it is a fact about the machine at the other end of the
 * wire, fixed by the contract we entered by calling ourselves Linux. X11
 * keycodes are positional and standardised, so a constant is the honest
 * representation of it rather than a guess.
 *
 * The half that CAN be discovered locally already is: `press_keysym` puts these
 * keysyms through XKeysymToKeycode against the live display, so whatever
 * keyboard and layout the local server has is what gets pressed. Sun and SGI
 * both number their keys differently from a PC and neither needs to be known
 * here. Shifted characters arrive as separate modifiers, so only base symbols
 * belong in this table.
 *
 * Shared by every X11 port because it describes the client, not the host. The
 * Mac port does not include it: there the reported platform really is the Mac,
 * and a Map-mode keycode really is a Mac virtual keycode.
 */
#ifndef RD_LINUX_KEYCODES_H
#define RD_LINUX_KEYCODES_H

/* Xlib.h for KeySym itself; keysym.h only defines the XK_ names. Both, so this
 * header stands on its own rather than relying on the includer's order. */
#include <X11/Xlib.h>
#include <X11/keysym.h>

static const struct { int code; KeySym sym; } RD_LINUXKEY[] = {
    {   9, XK_Escape },
    {  10, XK_1 }, {  11, XK_2 }, {  12, XK_3 }, {  13, XK_4 }, {  14, XK_5 },
    {  15, XK_6 }, {  16, XK_7 }, {  17, XK_8 }, {  18, XK_9 }, {  19, XK_0 },
    {  20, XK_minus }, {  21, XK_equal }, {  22, XK_BackSpace }, {  23, XK_Tab },
    {  24, XK_q }, {  25, XK_w }, {  26, XK_e }, {  27, XK_r }, {  28, XK_t },
    {  29, XK_y }, {  30, XK_u }, {  31, XK_i }, {  32, XK_o }, {  33, XK_p },
    {  34, XK_bracketleft }, {  35, XK_bracketright },
    {  36, XK_Return }, {  37, XK_Control_L },
    {  38, XK_a }, {  39, XK_s }, {  40, XK_d }, {  41, XK_f }, {  42, XK_g },
    {  43, XK_h }, {  44, XK_j }, {  45, XK_k }, {  46, XK_l },
    {  47, XK_semicolon }, {  48, XK_apostrophe }, {  49, XK_grave },
    {  50, XK_Shift_L }, {  51, XK_backslash },
    {  52, XK_z }, {  53, XK_x }, {  54, XK_c }, {  55, XK_v }, {  56, XK_b },
    {  57, XK_n }, {  58, XK_m },
    {  59, XK_comma }, {  60, XK_period }, {  61, XK_slash },
    {  62, XK_Shift_R }, {  63, XK_KP_Multiply }, {  64, XK_Alt_L },
    {  65, XK_space }, {  66, XK_Caps_Lock },
    {  67, XK_F1 }, {  68, XK_F2 }, {  69, XK_F3 }, {  70, XK_F4 },
    {  71, XK_F5 }, {  72, XK_F6 }, {  73, XK_F7 }, {  74, XK_F8 },
    {  75, XK_F9 }, {  76, XK_F10 },
    {  77, XK_Num_Lock }, {  78, XK_Scroll_Lock },
    {  79, XK_KP_7 }, {  80, XK_KP_8 }, {  81, XK_KP_9 }, {  82, XK_KP_Subtract },
    {  83, XK_KP_4 }, {  84, XK_KP_5 }, {  85, XK_KP_6 }, {  86, XK_KP_Add },
    {  87, XK_KP_1 }, {  88, XK_KP_2 }, {  89, XK_KP_3 }, {  90, XK_KP_0 },
    {  91, XK_KP_Decimal },
    {  94, XK_less }, {  95, XK_F11 }, {  96, XK_F12 },
    { 104, XK_KP_Enter }, { 105, XK_Control_R }, { 106, XK_KP_Divide },
    { 107, XK_Print }, { 108, XK_Alt_R },
    { 110, XK_Home }, { 111, XK_Up }, { 112, XK_Prior }, { 113, XK_Left },
    { 114, XK_Right }, { 115, XK_End }, { 116, XK_Down }, { 117, XK_Next },
    { 118, XK_Insert }, { 119, XK_Delete },
    { 127, XK_Pause },
    { 133, XK_Super_L }, { 134, XK_Super_R }, { 135, XK_Menu },
};
#define RD_NLINUXKEY ((int)(sizeof RD_LINUXKEY / sizeof RD_LINUXKEY[0]))

static KeySym rd_linux_to_keysym(int code)
{
    int i;
    for (i = 0; i < RD_NLINUXKEY; i++)
        if (RD_LINUXKEY[i].code == code)
            return RD_LINUXKEY[i].sym;
    return NoSymbol;
}

#endif /* RD_LINUX_KEYCODES_H */
