#include "plat.h"

#include <process.h>

int plat_nt(void)
{
    static int known = -1;
    if (known < 0) {
        OSVERSIONINFO v;
        v.dwOSVersionInfoSize = sizeof v;
        known = GetVersionEx(&v) && v.dwPlatformId == VER_PLATFORM_WIN32_NT;
    }
    return known;
}

HANDLE plat_thread(DWORD (WINAPI *fn)(LPVOID), void *arg)
{
    unsigned id;
    return (HANDLE)_beginthreadex(NULL, 0, (unsigned (__stdcall *)(void *))fn, arg, 0, &id);
}
