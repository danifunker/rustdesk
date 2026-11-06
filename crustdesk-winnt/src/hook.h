/* The screen hooks: cdvhook.dll (hook/cdvhook.c) in every program with
 * windows on the desktop, telling the agent where the screen is being drawn
 * so a look at the screen reads that and not all of it -- reading through
 * GDI is most of what a session costs (RESUME.md, Speed).
 *
 * The agent makes a named shared memory block, this struct, and installs the
 * DLL's two hooks from a thread of its own on the Default desktop; each copy
 * of the DLL opens the block and marks the 16-pixel squares (macroblocks)
 * that a WM_PAINT, a move, a key or a click touches. Marks are set and taken
 * with InterlockedExchange, so none is lost between the two sides.
 *
 * What no hook sees -- console windows, which csrss draws; a blinking caret;
 * a program that draws outside WM_PAINT -- the agent finds by reading the
 * console windows and a slow round of the rest (screen.c).
 *
 * Shared by the agent and the DLL: change HOOK_VERSION with the layout. */
#ifndef CDV_NT_HOOK_H
#define CDV_NT_HOOK_H

#include "win.h"

#define HOOK_DLL "cdvhook.dll"
#define HOOK_MAP "C-Desk-Vint screen hooks"
#define HOOK_VERSION 1

/* the grid: 16-pixel squares, up to 2560x2048 */
#define HOOK_GW 160
#define HOOK_GH 128

typedef struct {
    LONG version;       /* HOOK_VERSION: a DLL of another layout does nothing */
    LONG on;            /* 0: mark nothing (the hooks are going) */
    LONG width, height; /* the screen */
    LONG pending;       /* a square was marked since the agent last took them */
    HHOOK msg_hook;     /* for CallNextHookEx, which 9x needs */
    HHOOK wnd_hook;
    LONG cell[HOOK_GH * HOOK_GW]; /* 1: drawn since taken */
} cdv_hook_grid;

#endif
