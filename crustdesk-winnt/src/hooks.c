/* The agent's side of the screen hooks: see hooks.h and hook.h. */
#include "hooks.h"
#include "hook.h"
#include "log.h"
#include "plat.h"
#include "prefs.h"
#include "resource.h"

#include "win.h"
#include <stdlib.h>
#include <string.h>

static HANDLE map, thread, stop_ev;
static cdv_hook_grid *g;
static HMODULE dll;
static volatile LONG installed;

/* ---- the DLL ---------------------------------------------------------------- */

int hooks_unpack(void)
{
    char path[MAX_PATH];
    HRSRC r;
    HGLOBAL h;
    const char *p;
    DWORD n, got;
    HANDLE f;
    char *have;
    int same = 0;
    prefs_path(path, sizeof path, HOOK_DLL);
    r = FindResource(NULL, MAKEINTRESOURCE(IDR_HOOKDLL), RT_RCDATA);
    if (!r || (h = LoadResource(NULL, r)) == NULL || (p = (const char *)LockResource(h)) == NULL)
        return GetFileAttributes(path) != 0xffffffff; /* none inside: one put beside it? */
    n = SizeofResource(NULL, r);
    f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (GetFileSize(f, NULL) == n && (have = (char *)malloc(n)) != NULL) {
            same = ReadFile(f, have, n, &got, NULL) && got == n && !memcmp(have, p, n);
            free(have);
        }
        CloseHandle(f);
        if (same)
            return 1;
    }
    f = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        /* a program still has the last one loaded: NT will not overwrite a
         * file in use, but it will rename it -- aside, and write the new one */
        char old[MAX_PATH + 8];
        strcpy(old, path);
        strcat(old, ".old");
        DeleteFile(old);
        if (MoveFile(path, old))
            f = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    }
    if (f == INVALID_HANDLE_VALUE) {
        /* in use: programs still hold the last one; it says its version */
        cdv_log("screen hooks: cannot write %s (error %lu); using the one there", path,
                GetLastError());
        return GetFileAttributes(path) != 0xffffffff;
    }
    if (!WriteFile(f, p, n, &got, NULL) || got != n) {
        CloseHandle(f);
        DeleteFile(path);
        return 0;
    }
    CloseHandle(f);
    cdv_log("screen hooks: wrote %s", path);
    return 1;
}

/* ---- the shared block ------------------------------------------------------- */

typedef BOOL(WINAPI *InitializeSecurityDescriptor_t)(PSECURITY_DESCRIPTOR, DWORD);
typedef BOOL(WINAPI *SetSecurityDescriptorDacl_t)(PSECURITY_DESCRIPTOR, BOOL, PACL, BOOL);

/* Every program on the desktop, whoever runs it, opens the block to mark it:
 * no DACL. The worst a program can do with it is have the agent read more
 * of the screen than it needed to, or less until the round comes by. */
static SECURITY_ATTRIBUTES *open_to_all(void)
{
    static SECURITY_DESCRIPTOR sd;
    static SECURITY_ATTRIBUTES sa;
    HMODULE a;
    InitializeSecurityDescriptor_t isd;
    SetSecurityDescriptorDacl_t ssd;
    if (!plat_nt() || (a = LoadLibrary("advapi32.dll")) == NULL)
        return NULL;
    isd = (InitializeSecurityDescriptor_t)GetProcAddress(a, "InitializeSecurityDescriptor");
    ssd = (SetSecurityDescriptorDacl_t)GetProcAddress(a, "SetSecurityDescriptorDacl");
    if (!isd || !ssd || !isd(&sd, SECURITY_DESCRIPTOR_REVISION) || !ssd(&sd, TRUE, NULL, FALSE))
        return NULL;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    return &sa;
}

/* ---- the hooks -------------------------------------------------------------- */

/* Hooks set by a thread hook the desktop it is on, so a thread of its own on
 * Default, where programs run (the main loop follows the input desktop to
 * Winlogon and back). It only holds them: hooks of these kinds need no
 * message loop in the thread that set them. When the agent stops, or dies,
 * Windows takes them out. */
static DWORD WINAPI hook_thread(LPVOID unused)
{
    HOOKPROC getmsg = (HOOKPROC)GetProcAddress(dll, "CdvGetMsgProc");
    HOOKPROC callwnd = (HOOKPROC)GetProcAddress(dll, "CdvCallWndProc");
    HHOOK hm = NULL, hw = NULL;
    (void)unused;
    if (!getmsg || !callwnd) {
        cdv_log("screen hooks: %s is not the hook DLL", HOOK_DLL);
        return 0;
    }
    if (plat_nt()) {
        HDESK d = OpenDesktop("Default", 0, FALSE,
                              DESKTOP_HOOKCONTROL | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                  DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE);
        if (!d || !SetThreadDesktop(d)) {
            cdv_log("screen hooks: cannot reach the Default desktop (error %lu)", GetLastError());
            return 0;
        }
    }
    hm = SetWindowsHookEx(WH_GETMESSAGE, getmsg, dll, 0);
    hw = SetWindowsHookEx(WH_CALLWNDPROC, callwnd, dll, 0);
    if (!hm || !hw) {
        cdv_log("screen hooks: cannot set them (error %lu)", GetLastError());
        if (hm)
            UnhookWindowsHookEx(hm);
        if (hw)
            UnhookWindowsHookEx(hw);
        return 0;
    }
    g->msg_hook = hm;
    g->wnd_hook = hw;
    InterlockedExchange(&g->on, 1);
    InterlockedExchange((LONG *)&installed, 1);
    cdv_log("screen hooks: on (%s)", HOOK_DLL);
    WaitForSingleObject(stop_ev, INFINITE);
    InterlockedExchange((LONG *)&installed, 0);
    InterlockedExchange(&g->on, 0);
    UnhookWindowsHookEx(hw);
    UnhookWindowsHookEx(hm);
    cdv_log("screen hooks: off");
    return 0;
}

int hooks_start(int width, int height)
{
    char path[MAX_PATH];
    if (thread)
        return 1;
    if (!prefs.hook) {
        cdv_log("screen hooks: off in the settings; reading the whole screen");
        return 0;
    }
    if (!hooks_unpack()) {
        cdv_log("screen hooks: no %s beside the program; reading the whole screen", HOOK_DLL);
        return 0;
    }
    if (!map) {
        map = CreateFileMapping((HANDLE)0xffffffff, open_to_all(), PAGE_READWRITE, 0,
                                sizeof(cdv_hook_grid), HOOK_MAP);
        if (!map) {
            cdv_log("screen hooks: no shared memory (error %lu)", GetLastError());
            return 0;
        }
        g = (cdv_hook_grid *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0);
        if (!g) {
            CloseHandle(map);
            map = NULL;
            return 0;
        }
        memset(g, 0, sizeof *g);
    }
    g->version = HOOK_VERSION;
    hooks_set_screen(width, height);
    if (!dll) {
        /* by its full path: never one found elsewhere on the search path */
        prefs_path(path, sizeof path, HOOK_DLL);
        dll = LoadLibrary(path);
        if (!dll) {
            cdv_log("screen hooks: cannot load %s (error %lu)", path, GetLastError());
            return 0;
        }
    }
    stop_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!stop_ev)
        return 0;
    thread = plat_thread(hook_thread, NULL);
    return thread != NULL;
}

void hooks_stop(void)
{
    if (!thread)
        return;
    SetEvent(stop_ev);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    CloseHandle(stop_ev);
    thread = stop_ev = NULL;
}

int hooks_active(void)
{
    return installed != 0;
}

void hooks_set_screen(int width, int height)
{
    if (!g)
        return;
    g->width = width;
    g->height = height;
}

int hooks_take(uint8_t *want, int mbw, int mbh)
{
    int x, y, n = 0, cols = mbw < HOOK_GW ? mbw : HOOK_GW;
    if (!installed || !InterlockedExchange(&g->pending, 0))
        return 0;
    for (y = 0; y < mbh && y < HOOK_GH; y++) {
        LONG *c = &g->cell[y * HOOK_GW];
        for (x = 0; x < cols; x++)
            if (c[x] && InterlockedExchange(&c[x], 0)) {
                want[y * mbw + x] = 1;
                n++;
            }
    }
    return n;
}
