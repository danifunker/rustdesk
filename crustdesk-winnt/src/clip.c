#include "clip.h"
#include "log.h"
#include "plat.h"
#include "text.h"

#include "win.h"
#include <stdlib.h>
#include <string.h>

#define CLIP_MAX (256 * 1024)
#define WM_CDV_SET (WM_APP + 1)

static CRITICAL_SECTION lock;
static HWND wnd, next_viewer;
static char *incoming;       /* from the peer, waiting for the thread to set it */
static size_t incoming_len;
static char *outgoing;       /* copied here, waiting for the agent to take it */
static long outgoing_len = -1;
static int setting;          /* the change being reported is our own */

/* \n -> \r\n, the way Windows text is; *out is malloc'd. */
static WCHAR *to_windows_text(const char *utf8, size_t n)
{
    WCHAR *w = (WCHAR *)malloc((n + 1) * sizeof(WCHAR)), *crlf;
    size_t len = utf8_to_utf16(utf8, n, w, n + 1), i, o = 0;
    if (!w)
        return NULL;
    crlf = (WCHAR *)malloc((2 * len + 1) * sizeof(WCHAR));
    if (!crlf) {
        free(w);
        return NULL;
    }
    for (i = 0; i < len; i++) {
        if (w[i] == '\n' && (i == 0 || w[i - 1] != '\r'))
            crlf[o++] = '\r';
        crlf[o++] = w[i];
    }
    crlf[o] = 0;
    free(w);
    return crlf;
}

static void set_clipboard(void)
{
    char *text;
    size_t n;
    WCHAR *w;
    HGLOBAL h;
    EnterCriticalSection(&lock);
    text = incoming;
    n = incoming_len;
    incoming = NULL;
    LeaveCriticalSection(&lock);
    if (!text)
        return;
    w = to_windows_text(text, n);
    free(text);
    if (!w)
        return;
    if (plat_nt()) {
        h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (wcslen(w) + 1) * sizeof(WCHAR));
        if (h) {
            memcpy(GlobalLock(h), w, (wcslen(w) + 1) * sizeof(WCHAR));
            GlobalUnlock(h);
        }
    } else {
        /* 95 and 98 keep text in the code page; what it cannot spell is '?' */
        int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
        h = n > 0 ? GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (DWORD)n) : NULL;
        if (h) {
            WideCharToMultiByte(CP_ACP, 0, w, -1, (char *)GlobalLock(h), n, NULL, NULL);
            GlobalUnlock(h);
        }
    }
    if (h) {
        if (OpenClipboard(wnd)) {
            setting = 1;
            EmptyClipboard();
            if (!SetClipboardData(plat_nt() ? CF_UNICODETEXT : CF_TEXT, h))
                GlobalFree(h);
            CloseClipboard();
            setting = 0;
        } else {
            GlobalFree(h);
        }
    }
    free(w);
}

static void read_clipboard(void)
{
    HANDLE h;
    const WCHAR *w;
    WCHAR *conv = NULL;
    char *u;
    size_t n;
    UINT fmt = plat_nt() ? CF_UNICODETEXT : CF_TEXT;
    if (!IsClipboardFormatAvailable(fmt) || !OpenClipboard(wnd))
        return;
    h = GetClipboardData(fmt);
    w = h ? (const WCHAR *)GlobalLock(h) : NULL;
    if (w && fmt == CF_TEXT) {
        /* the code page's text, as UTF-16 like NT's */
        const char *a = (const char *)w;
        int wn = MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);
        conv = wn > 0 ? (WCHAR *)malloc((size_t)wn * sizeof(WCHAR)) : NULL;
        if (conv)
            MultiByteToWideChar(CP_ACP, 0, a, -1, conv, wn);
        w = conv;
    }
    if (w) {
        n = wcslen(w);
        u = (char *)malloc(n * 3 + 1);
        if (u) {
            long len = (long)utf16_to_utf8(w, n, u, n * 3 + 1);
            if (len <= CLIP_MAX) {
                EnterCriticalSection(&lock);
                free(outgoing);
                outgoing = u;
                outgoing_len = len;
                LeaveCriticalSection(&lock);
                u = NULL;
            }
            free(u);
        }
    }
    if (h)
        GlobalUnlock(h);
    free(conv);
    CloseClipboard();
}

static LRESULT CALLBACK clip_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_DRAWCLIPBOARD:
        if (!setting)
            read_clipboard();
        if (next_viewer)
            SendMessage(next_viewer, m, wp, lp);
        return 0;
    case WM_CHANGECBCHAIN:
        if ((HWND)wp == next_viewer)
            next_viewer = (HWND)lp;
        else if (next_viewer)
            SendMessage(next_viewer, m, wp, lp);
        return 0;
    case WM_CDV_SET:
        set_clipboard();
        return 0;
    case WM_DESTROY:
        ChangeClipboardChain(h, next_viewer);
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

static DWORD WINAPI clip_thread(LPVOID started)
{
    WNDCLASS wc;
    MSG msg;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = clip_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "C-Desk-Vint clipboard";
    RegisterClass(&wc);
    wnd = CreateWindow(wc.lpszClassName, "", 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (wnd) {
        setting = 1; /* what is there already is not news */
        next_viewer = SetClipboardViewer(wnd);
        setting = 0;
    } else {
        cdv_log("clipboard: no window (error %lu); the clipboard is not shared", GetLastError());
    }
    SetEvent((HANDLE)started);
    if (!wnd)
        return 0;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

void clip_start(void)
{
    HANDLE ev = CreateEvent(NULL, TRUE, FALSE, NULL), t;
    InitializeCriticalSection(&lock);
    t = plat_thread(clip_thread, ev);
    if (t) {
        WaitForSingleObject(ev, 10000);
        CloseHandle(t);
    }
    CloseHandle(ev);
}

void clip_from_peer(const char *utf8, size_t n)
{
    char *copy;
    if (!wnd || n > CLIP_MAX)
        return;
    copy = (char *)malloc(n + 1);
    if (!copy)
        return;
    memcpy(copy, utf8, n);
    EnterCriticalSection(&lock);
    free(incoming);
    incoming = copy;
    incoming_len = n;
    LeaveCriticalSection(&lock);
    PostMessage(wnd, WM_CDV_SET, 0, 0);
}

long clip_take(char *buf, size_t cap)
{
    long n = -1;
    if (!wnd)
        return -1;
    EnterCriticalSection(&lock);
    if (outgoing && outgoing_len >= 0 && (size_t)outgoing_len < cap) {
        memcpy(buf, outgoing, (size_t)outgoing_len);
        n = outgoing_len;
    }
    free(outgoing);
    outgoing = NULL;
    outgoing_len = -1;
    LeaveCriticalSection(&lock);
    return n;
}

void clip_reset(void)
{
    char none[1];
    clip_take(none, 0);
}
