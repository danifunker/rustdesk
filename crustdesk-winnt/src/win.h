/* <windows.h>, less what clashes with the shared core: winuser.h's hot-key
 * modifiers MOD_SHIFT and MOD_CONTROL, which session.h names too (with other
 * values) for a key's modifiers. Every file of the NT side includes this
 * instead of <windows.h>. */
#ifndef CDV_NT_WIN_H
#define CDV_NT_WIN_H

#include <windows.h>
#undef MOD_SHIFT
#undef MOD_CONTROL
/* RegisterHotKey's, under names of their own */
#define HOTKEY_ALT 0x0001
#define HOTKEY_CONTROL 0x0002

/* Newer than Visual C++ 4's headers, older than NT 4.0 itself. */
#ifndef ICON_SMALL
#define ICON_SMALL 0
#define ICON_BIG 1
#endif

#endif
