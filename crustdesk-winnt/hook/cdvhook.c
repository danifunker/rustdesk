/* cdvhook.dll: the screen hooks of C-Desk-Vint (src/hook.h).
 *
 * Windows loads it into every program with windows on the desktop the agent
 * hooked, so it is kept small and careful: it opens the agent's shared block
 * when loaded, marks the squares a message draws on, and calls the next hook.
 * Nothing here may fail loudly -- without the block, or with a block of
 * another version, it does nothing at all.
 *
 * Two hooks, as WinVNC's VNCHooks had: WH_GETMESSAGE sees what a program
 * takes from its queue (WM_PAINT, with the region still to paint; keys and
 * clicks, which draw at once in the window they go to), WH_CALLWNDPROC what
 * is sent to a window (its frame being painted, a move, a scroll, an
 * owner-drawn item, a menu changing). */
#include "hook.h"

static cdv_hook_grid *g;

static int live(void)
{
    return g && g->version == HOOK_VERSION && g->on;
}

/* A rectangle of the screen was drawn on. */
static void mark(LONG l, LONG t, LONG r, LONG b)
{
    LONG x, y, x0, x1, y0, y1;
    if (l < 0)
        l = 0;
    if (t < 0)
        t = 0;
    if (r > g->width)
        r = g->width;
    if (b > g->height)
        b = g->height;
    if (r <= l || b <= t)
        return;
    x0 = l >> 4;
    x1 = (r - 1) >> 4;
    y0 = t >> 4;
    y1 = (b - 1) >> 4;
    if (x1 >= HOOK_GW)
        x1 = HOOK_GW - 1;
    if (y1 >= HOOK_GH)
        y1 = HOOK_GH - 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            LONG *c = &g->cell[y * HOOK_GW + x];
            if (!*c)
                InterlockedExchange(c, 1);
        }
    if (!g->pending)
        InterlockedExchange(&g->pending, 1);
}

static void mark_window(HWND h)
{
    RECT r;
    if (h && IsWindowVisible(h) && GetWindowRect(h, &r))
        mark(r.left, r.top, r.right, r.bottom);
}

/* A rectangle of a window's client area. */
static void mark_client(HWND h, const RECT *r)
{
    POINT a, b;
    a.x = r->left;
    a.y = r->top;
    b.x = r->right;
    b.y = r->bottom;
    if (h && ClientToScreen(h, &a) && ClientToScreen(h, &b))
        mark(a.x, a.y, b.x, b.y);
}

/* A key: what it draws is at the text cursor (an edit control draws the
 * line at once, no WM_PAINT), and for one that can move the lines (Enter,
 * Backspace, Delete) all of the window below it. The caret is this
 * thread's -- the hook runs in the program's own thread -- and is where it
 * was before the key; a band of a few lines round it covers the line it
 * ends on. Without a caret in that window, all of it. */
static void mark_key(HWND h, UINT msg, WPARAM key)
{
    POINT c;
    RECT r;
    int lines_move = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
                     (key == VK_RETURN || key == VK_BACK || key == VK_DELETE);
    if (!h || !GetClientRect(h, &r) || GetFocus() != h || !GetCaretPos(&c) ||
        c.x < r.left || c.x > r.right || c.y < r.top || c.y > r.bottom) {
        mark_window(h);
        return;
    }
    r.top = c.y - 48 > r.top ? c.y - 48 : r.top;
    if (!lines_move && c.y + 64 < r.bottom)
        r.bottom = c.y + 64;
    mark_client(h, &r);
}

/* Every menu that is up: its highlight moves without a WM_PAINT. */
static BOOL CALLBACK menu_window(HWND h, LPARAM unused)
{
    char cls[16];
    (void)unused;
    if (GetClassName(h, cls, sizeof cls) && !lstrcmp(cls, "#32768"))
        mark_window(h);
    return TRUE;
}

LRESULT CALLBACK CdvGetMsgProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp == PM_REMOVE && live()) {
        MSG *m = (MSG *)lp;
        RECT r;
        switch (m->message) {
        case WM_PAINT:
            if (GetUpdateRect(m->hwnd, &r, FALSE))
                mark_client(m->hwnd, &r);
            break;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            mark_key(m->hwnd, m->message, m->wParam);
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            mark_window(m->hwnd);
            break;
        case WM_MOUSEMOVE:
            if (m->wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))
                mark_window(m->hwnd); /* a drag: a selection, a slider */
            break;
        }
    }
    return CallNextHookEx(g ? g->msg_hook : NULL, code, wp, lp);
}

LRESULT CALLBACK CdvCallWndProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && live()) {
        CWPSTRUCT *c = (CWPSTRUCT *)lp;
        switch (c->message) {
        case WM_NCPAINT:
        case WM_NCACTIVATE:
        case WM_SETTEXT:
        case WM_ENABLE:
        case WM_SHOWWINDOW:
        case WM_DESTROY:
        case WM_WINDOWPOSCHANGING: /* where it was */
        case WM_WINDOWPOSCHANGED:  /* and where it is */
        case WM_HSCROLL:
        case WM_VSCROLL:
            mark_window(c->hwnd);
            break;
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)c->lParam;
            if (d)
                mark_client(d->hwndItem, &d->rcItem);
            break;
        }
        case WM_MENUSELECT:
        case WM_INITMENUPOPUP:
        case WM_EXITMENULOOP:
            EnumWindows(menu_window, 0);
            break;
        }
    }
    return CallNextHookEx(g ? g->wnd_hook : NULL, code, wp, lp);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID unused)
{
    (void)unused;
    if (why == DLL_PROCESS_ATTACH) {
        HANDLE m;
        DisableThreadLibraryCalls(inst);
        m = OpenFileMapping(FILE_MAP_WRITE, FALSE, HOOK_MAP);
        if (m) {
            g = (cdv_hook_grid *)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0);
            CloseHandle(m);
        }
    } else if (why == DLL_PROCESS_DETACH && g) {
        UnmapViewOfFile(g);
        g = NULL;
    }
    return TRUE;
}
