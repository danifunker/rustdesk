/* The tray program: "C-Desk-Vint -tray", started at logon (the Run key the
 * service's installer writes) as whoever logs on.
 *
 * An icon by the clock, its tip saying who is connected; its menu: Chat,
 * Disconnect remote sessions, Restart terminal, Settings, Exit. It runs the
 * terminal's shell too (shell.c), so that runs as the user logged on, and it
 * restarts it when it ends. The chat window opens by
 * itself when the peer writes, and what is typed in it goes to the peer.
 * It talks to the agent over the pipe in link.h, and keeps trying while the
 * agent is not there.
 *
 * NT only (the agent serves the pipe on NT alone), but this file is in the
 * x86 program that loads on 95 and 98 too: the Unicode window calls are
 * looked up at run time, and so is CancelIo, which 95 lacks. So is
 * Shell_NotifyIcon: nothing else in the program imports shell32, and PINT's
 * build has no import library for it; without it there is no icon, and the
 * chat window still opens when the peer writes. */
#include "tray.h"
#include "link.h"
#include "plat.h"
#include "service.h"
#include "shell.h"
#include "text.h"
#include "resource.h"

#include "win.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_TRAYICON (WM_APP + 1)
#define WM_LINKMSG (WM_APP + 2)   /* lParam: a tray_msg, ours to free */
#define WM_LINKSTATE (WM_APP + 3) /* wParam: 1 linked to the agent, 0 not */

#define ID_CHAT 100
#define ID_DISCONNECT 101
#define ID_SETTINGS 102
#define ID_EXIT 103
#define ID_RESTART_TERM 104
#define ID_TRANSCRIPT 200
#define ID_INPUT 201

#define TRAY_MUTEX "C-Desk-Vint tray"

/* tray_msg types of the shell's, posted to the window: the pipe is written
 * from one thread only */
#define TRAY_SHELL_OUT 0x100
#define TRAY_SHELL_ENDED 0x101
#define OUT_PENDING_MAX 32

typedef struct {
    int type;
    size_t n;
    char data[1];
} tray_msg;

/* Unicode calls, NT's; the program is ANSI and loads on 9x too */
typedef LRESULT(WINAPI *SendMessageW_t)(HWND, UINT, WPARAM, LPARAM);
typedef int(WINAPI *GetWindowTextW_t)(HWND, LPWSTR, int);
typedef BOOL(WINAPI *CancelIo_t)(HANDLE);
typedef BOOL(WINAPI *Shell_NotifyIconA_t)(DWORD, NOTIFYICONDATA *);
static SendMessageW_t pSendMessageW;
static Shell_NotifyIconA_t pShell_NotifyIcon;
static GetWindowTextW_t pGetWindowTextW;
static CancelIo_t pCancelIo;

static HINSTANCE inst;
static HWND main_wnd, chat_wnd, transcript, input_box, send_btn;
static HICON icon;
static UINT taskbar_created;
static int linked, sessions;
static char peer_names[200];

static CRITICAL_SECTION pipe_lock;
static HANDLE pipe = INVALID_HANDLE_VALUE;
static HANDLE quit_ev, pipe_thr;
static volatile LONG out_pending; /* shell output posted and not yet sent */

/* ---- the pipe --------------------------------------------------------------- */

/* A message to the agent; 0 if there is no agent to take it. */
static int link_write(int type, const char *data, size_t n)
{
    static char buf[LINK_MAX];
    OVERLAPPED ov;
    DWORD put = 0;
    int ok = 0;
    if (n > LINK_MAX - 1)
        n = LINK_MAX - 1;
    buf[0] = (char)type;
    if (n)
        memcpy(buf + 1, data, n);
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent)
        return 0;
    EnterCriticalSection(&pipe_lock);
    if (pipe != INVALID_HANDLE_VALUE) {
        if (WriteFile(pipe, buf, (DWORD)n + 1, &put, &ov) || GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(pipe, &ov, &put, TRUE) && put == (DWORD)n + 1;
    }
    LeaveCriticalSection(&pipe_lock);
    CloseHandle(ov.hEvent);
    return ok;
}

static void post_msg(int type, const char *data, size_t n)
{
    tray_msg *m = (tray_msg *)malloc(sizeof *m + n);
    if (!m)
        return;
    m->type = type;
    m->n = n;
    memcpy(m->data, data, n);
    m->data[n] = 0;
    if (!PostMessage(main_wnd, WM_LINKMSG, 0, (LPARAM)m))
        free(m);
}

/* The shell's output, from its reader's thread: in pieces the pipe takes,
 * and no faster than they leave, so a long listing is not lost. */
static void shell_out(const char *s, size_t n)
{
    while (n) {
        size_t k = n < LINK_MAX - 1 ? n : LINK_MAX - 1;
        while (out_pending >= OUT_PENDING_MAX && WaitForSingleObject(quit_ev, 10) == WAIT_TIMEOUT)
            ;
        InterlockedIncrement((LONG *)&out_pending);
        post_msg(TRAY_SHELL_OUT, s, k);
        s += k;
        n -= k;
    }
}

static void shell_ended(void)
{
    post_msg(TRAY_SHELL_ENDED, "", 0);
}

/* A peer opened the terminal: the shell, up if it was not, and what it
 * showed before. */
static void term_open(void)
{
    static char past[64 * 1024];
    char ready[32];
    unsigned long pid = shell_start();
    size_t n = shell_history(past, sizeof past), o;
    sprintf(ready, "%lu %d", pid, n ? 1 : 0);
    link_write(LINK_TERM_READY, ready, strlen(ready));
    for (o = 0; o < n; o += LINK_MAX - 1)
        link_write(LINK_TERM_OUT, past + o, n - o < LINK_MAX - 1 ? n - o : LINK_MAX - 1);
}

/* Read from the agent until it goes; then try again, while the tray runs. */
static DWORD WINAPI pipe_thread(LPVOID unused)
{
    static char buf[LINK_MAX + 1];
    static OVERLAPPED ov;
    (void)unused;
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    while (ov.hEvent && WaitForSingleObject(quit_ev, 0) != WAIT_OBJECT_0) {
        DWORD mode = PIPE_READMODE_MESSAGE, wait = 3000;
        int refused = 0;
        /* not GENERIC_WRITE: that asks to make pipe instances too, which only
         * the agent may (link.c) */
        HANDLE p = CreateFile(LINK_PIPE, GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0,
                              NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (p == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipe(LINK_PIPE, 2000))
                WaitForSingleObject(quit_ev, wait);
            continue;
        }
        SetNamedPipeHandleState(p, &mode, NULL, NULL);
        EnterCriticalSection(&pipe_lock);
        pipe = p;
        LeaveCriticalSection(&pipe_lock);
        if (link_write(LINK_HELLO, "1", 1)) {
            PostMessage(main_wnd, WM_LINKSTATE, 1, 0);
            for (;;) {
                HANDLE w[2];
                DWORD got = 0;
                ResetEvent(ov.hEvent);
                if (!ReadFile(p, buf, LINK_MAX, &got, &ov) && GetLastError() != ERROR_IO_PENDING)
                    break;
                w[0] = ov.hEvent;
                w[1] = quit_ev;
                if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    if (pCancelIo)
                        pCancelIo(p);
                    break;
                }
                if (!GetOverlappedResult(p, &ov, &got, FALSE) || got < 1)
                    break;
                if (buf[0] == LINK_REFUSED)
                    refused = 1; /* the agent says why before it lets go */
                post_msg((unsigned char)buf[0], buf + 1, got - 1);
            }
        }
        EnterCriticalSection(&pipe_lock);
        pipe = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&pipe_lock);
        CloseHandle(p);
        PostMessage(main_wnd, WM_LINKSTATE, 0, 0);
        if (refused)
            wait = 60000; /* turned away: not the console user, do not pester */
        WaitForSingleObject(quit_ev, wait);
    }
    return 0;
}

/* ---- the chat window -------------------------------------------------------- */

/* A line into the transcript: "who: text", the text UTF-8. */
static void transcript_add(const char *who, const char *utf8, size_t n)
{
    static char line[LINK_MAX * 2 + 64];
    static WCHAR w[LINK_MAX * 2 + 64];
    size_t k = 0, i;
    int len;
    if (!chat_wnd)
        return;
    k = (size_t)sprintf(line, "%s: ", who);
    for (i = 0; i < n && k < sizeof line - 3; i++) {
        if (utf8[i] == '\r')
            continue;
        if (utf8[i] == '\n')
            line[k++] = '\r';
        line[k++] = utf8[i];
    }
    line[k++] = '\r';
    line[k++] = '\n';
    line[k] = 0;
    len = GetWindowTextLength(transcript);
    SendMessage(transcript, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    if (pSendMessageW) {
        utf8_to_utf16(line, k, w, sizeof w / sizeof w[0]);
        pSendMessageW(transcript, EM_REPLACESEL, FALSE, (LPARAM)w);
    } else {
        SendMessage(transcript, EM_REPLACESEL, FALSE, (LPARAM)line);
    }
    SendMessage(transcript, EM_SCROLLCARET, 0, 0);
}

static void chat_title(void)
{
    char t[260];
    if (!chat_wnd)
        return;
    if (!linked)
        strcpy(t, "C-Desk-Vint chat - the agent is not running");
    else if (sessions)
        snprintf(t, sizeof t, "C-Desk-Vint chat with %s", peer_names);
    else
        strcpy(t, "C-Desk-Vint chat - nobody is connected");
    SetWindowText(chat_wnd, t);
}

static void chat_send(void)
{
    static WCHAR w[LINK_MAX];
    static char utf8[LINK_MAX * 3];
    size_t n;
    if (pGetWindowTextW) {
        int wl = pGetWindowTextW(input_box, w, sizeof w / sizeof w[0]);
        n = utf16_to_utf8(w, wl > 0 ? (size_t)wl : 0, utf8, LINK_MAX - 1);
    } else {
        GetWindowText(input_box, utf8, LINK_MAX - 1);
        n = strlen(utf8);
    }
    if (!n)
        return;
    if (!link_write(LINK_CHAT, utf8, n)) {
        static const char gone[] = "(not sent: the agent is not running)";
        transcript_add("C-Desk-Vint", gone, sizeof gone - 1);
        return;
    }
    transcript_add("You", utf8, n);
    SetWindowText(input_box, "");
}

static void chat_layout(int w, int h)
{
    int bw = 64, ih = 24, m = 6;
    MoveWindow(transcript, m, m, w - 2 * m, h - ih - 3 * m, TRUE);
    MoveWindow(input_box, m, h - ih - m, w - bw - 3 * m, ih, TRUE);
    MoveWindow(send_btn, w - bw - m, h - ih - m, bw, ih, TRUE);
}

static LRESULT CALLBACK chat_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        transcript = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                        ES_READONLY | ES_AUTOVSCROLL,
                                    0, 0, 10, 10, h, (HMENU)ID_TRANSCRIPT, inst, NULL);
        input_box = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10,
                                   10, h, (HMENU)ID_INPUT, inst, NULL);
        send_btn = CreateWindow("BUTTON", "&Send",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10,
                                h, (HMENU)IDOK, inst, NULL);
        SendMessage(transcript, WM_SETFONT, (WPARAM)f, FALSE);
        SendMessage(input_box, WM_SETFONT, (WPARAM)f, FALSE);
        SendMessage(send_btn, WM_SETFONT, (WPARAM)f, FALSE);
        SendMessage(input_box, EM_LIMITTEXT, LINK_MAX / 3, 0);
        return 0;
    }
    case WM_SIZE:
        chat_layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_SETFOCUS:
        SetFocus(input_box);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            chat_send();
            SetFocus(input_box);
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE); /* the transcript stays for next time */
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

static void chat_show(int flash)
{
    if (!chat_wnd) {
        chat_wnd = CreateWindowEx(0, "CDeskVintChat", "C-Desk-Vint chat", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 420, 300, NULL, NULL, inst, NULL);
        if (!chat_wnd)
            return;
        chat_title();
    }
    ShowWindow(chat_wnd, IsIconic(chat_wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(chat_wnd);
    if (flash) {
        FlashWindow(chat_wnd, TRUE);
        MessageBeep(MB_ICONASTERISK);
    }
}

/* ---- the icon ------------------------------------------------------------------- */

static void icon_set(DWORD what)
{
    NOTIFYICONDATA nd;
    if (!pShell_NotifyIcon)
        return;
    memset(&nd, 0, sizeof nd);
    nd.cbSize = sizeof nd;
    nd.hWnd = main_wnd;
    nd.uID = 1;
    nd.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nd.uCallbackMessage = WM_TRAYICON;
    nd.hIcon = icon;
    if (!linked)
        strcpy(nd.szTip, "C-Desk-Vint: the agent is not running");
    else if (sessions)
        snprintf(nd.szTip, sizeof nd.szTip, "C-Desk-Vint: %s connected", peer_names);
    else
        strcpy(nd.szTip, "C-Desk-Vint: nobody connected");
    pShell_NotifyIcon(what, &nd);
}

/* The agent's status lines: "session=desktop NAME (how)" for each session. */
static void take_status(const char *text)
{
    const char *p = text;
    sessions = 0;
    peer_names[0] = 0;
    while (p && *p) {
        if (!strncmp(p, "session=", 8)) {
            const char *s = strchr(p, ' '), *e = strstr(p, " (");
            if (!e)
                e = p + strcspn(p, "\r\n");
            if (s && s < e && !strncmp(p + 8, "desktop", 7)) {
                size_t have = strlen(peer_names), n = (size_t)(e - s - 1);
                if (have && have < sizeof peer_names - 3) {
                    strcat(peer_names, ", ");
                    have += 2;
                }
                if (n > sizeof peer_names - have - 1)
                    n = sizeof peer_names - have - 1;
                memcpy(peer_names + have, s + 1, n);
                peer_names[have + n] = 0;
            }
            sessions++;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    if (sessions && !peer_names[0])
        strcpy(peer_names, "a remote session");
}

static void lock_workstation(void)
{
    typedef BOOL(WINAPI * lock_fn)(void);
    lock_fn f = (lock_fn)GetProcAddress(GetModuleHandle("user32.dll"), "LockWorkStation");
    if (f)
        f(); /* Windows 2000 and later; NT 4.0 has no way to */
}

static void on_link_msg(tray_msg *m)
{
    switch (m->type) {
    case LINK_STATUS:
        take_status(m->data);
        icon_set(NIM_MODIFY);
        chat_title();
        break;
    case LINK_PEER_CHAT: {
        const char *nl = (const char *)memchr(m->data, '\n', m->n);
        char who[80];
        size_t wl = nl ? (size_t)(nl - m->data) : 0;
        if (wl >= sizeof who)
            wl = sizeof who - 1;
        memcpy(who, m->data, wl);
        who[wl] = 0;
        chat_show(1);
        transcript_add(wl ? who : "Peer", nl ? nl + 1 : m->data,
                       nl ? m->n - (size_t)(nl + 1 - m->data) : m->n);
        break;
    }
    case LINK_NOTE:
    case LINK_REFUSED:
        if (chat_wnd && IsWindowVisible(chat_wnd))
            transcript_add("C-Desk-Vint", m->data, m->n);
        break;
    case LINK_LOCK:
        lock_workstation();
        break;
    case LINK_TERM_OPEN:
        term_open();
        break;
    case LINK_TERM_IN:
        shell_input(m->data, m->n);
        break;
    case TRAY_SHELL_OUT:
        link_write(LINK_TERM_OUT, m->data, m->n);
        InterlockedDecrement((LONG *)&out_pending);
        break;
    case TRAY_SHELL_ENDED:
        shell_exited();
        break;
    }
}

static void menu(void)
{
    HMENU mu = CreatePopupMenu();
    POINT pt;
    AppendMenu(mu, MF_STRING, ID_CHAT, "&Chat...");
    AppendMenu(mu, MF_STRING | (linked && sessions ? 0 : MF_GRAYED), ID_DISCONNECT,
               "&Disconnect remote sessions");
    AppendMenu(mu, MF_STRING | (shell_running() ? 0 : MF_GRAYED), ID_RESTART_TERM,
               "Restart &terminal");
    AppendMenu(mu, MF_SEPARATOR, 0, NULL);
    AppendMenu(mu, MF_STRING, ID_SETTINGS, "S&ettings...");
    AppendMenu(mu, MF_STRING, ID_EXIT, "E&xit tray");
    SetMenuDefaultItem(mu, ID_CHAT, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(main_wnd); /* or the menu stays up when clicked away from */
    TrackPopupMenu(mu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, main_wnd, NULL);
    PostMessage(main_wnd, WM_NULL, 0, 0);
    DestroyMenu(mu);
}

static void open_settings(void)
{
    char exe[MAX_PATH];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    GetModuleFileName(NULL, exe, sizeof exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcess(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == taskbar_created && taskbar_created) {
        icon_set(NIM_ADD); /* Explorer restarted */
        return 0;
    }
    switch (m) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP)
            menu();
        else if (lp == WM_LBUTTONDBLCLK)
            chat_show(0);
        return 0;
    case WM_LINKSTATE:
        linked = (int)wp;
        if (!linked) {
            sessions = 0;
            peer_names[0] = 0;
            if (service_state() == SVC_ABSENT) {
                DestroyWindow(h); /* the agent was removed: so goes the tray */
                return 0;
            }
        }
        icon_set(NIM_MODIFY);
        chat_title();
        return 0;
    case WM_LINKMSG:
        on_link_msg((tray_msg *)lp);
        free((void *)lp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CHAT:
            chat_show(0);
            break;
        case ID_DISCONNECT:
            if (MessageBox(h, "End every remote session to this computer now?", "C-Desk-Vint",
                           MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                link_write(LINK_DISCONNECT, NULL, 0);
            break;
        case ID_RESTART_TERM:
            shell_restart();
            break;
        case ID_SETTINGS:
            open_settings();
            break;
        case ID_EXIT:
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_DESTROY:
        shell_stop();
        icon_set(NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

int tray_run(HINSTANCE hinst)
{
    WNDCLASS wc;
    MSG msg;
    HANDLE single;
    HMODULE u = GetModuleHandle("user32.dll");
    inst = hinst;
    if (!plat_nt())
        return 0; /* no tray on 9x yet: the agent cannot serve the pipe there */
    single = CreateMutex(NULL, TRUE, TRAY_MUTEX);
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0; /* one is running already */
    pSendMessageW = (SendMessageW_t)GetProcAddress(u, "SendMessageW");
    pGetWindowTextW = (GetWindowTextW_t)GetProcAddress(u, "GetWindowTextW");
    pCancelIo = (CancelIo_t)GetProcAddress(GetModuleHandle("kernel32.dll"), "CancelIo");
    {
        HMODULE sh = LoadLibrary("shell32.dll");
        if (sh)
            pShell_NotifyIcon = (Shell_NotifyIconA_t)GetProcAddress(sh, "Shell_NotifyIconA");
    }
    /* the small one by the clock */
    icon = (HICON)LoadImage(hinst, MAKEINTRESOURCE(IDI_CDV), IMAGE_ICON,
                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!icon)
        icon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_CDV));
    taskbar_created = RegisterWindowMessage("TaskbarCreated");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = "CDeskVintTray";
    if (!RegisterClass(&wc))
        return 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = chat_proc;
    wc.hInstance = hinst;
    wc.hIcon = icon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CDeskVintChat";
    if (!RegisterClass(&wc))
        return 1;
    main_wnd = CreateWindow("CDeskVintTray", "C-Desk-Vint tray", 0, 0, 0, 0, 0, NULL, NULL, hinst,
                            NULL);
    if (!main_wnd)
        return 1;
    icon_set(NIM_ADD);

    InitializeCriticalSection(&pipe_lock);
    quit_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    shell_init(shell_out, shell_ended);
    pipe_thr = plat_thread(pipe_thread, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (chat_wnd && IsDialogMessage(chat_wnd, &msg))
            continue; /* Tab between the boxes, Enter is Send */
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    SetEvent(quit_ev);
    if (pipe_thr) {
        WaitForSingleObject(pipe_thr, 3000);
        CloseHandle(pipe_thr);
    }
    CloseHandle(single);
    return 0;
}
