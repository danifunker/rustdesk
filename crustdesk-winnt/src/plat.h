/* Which Windows this is. The x86 build runs on Windows 95 and 98 as well
 * as NT: they load the same program (every function it imports is exported
 * there too, NT-only ones as stubs that fail -- scripts/check-imports.py
 * checks), and the code asks this where they differ: services, desktops and
 * security are NT's; Unicode file and clipboard calls are NT's, 9x has the
 * ANSI ones. */
#ifndef CDV_NT_PLAT_H
#define CDV_NT_PLAT_H

/* 1 on the NT family, 0 on Windows 95/98/Me. */
int plat_nt(void);

/* A thread with the C runtime's own per-thread state (_beginthreadex):
 * CreateThread under the multithreaded runtime leaks it. */
#include "win.h"
HANDLE plat_thread(DWORD (WINAPI *fn)(LPVOID), void *arg);

#endif
