/* C-Desk-Vint for Windows NT: a RustDesk agent for NT 4.0 on x86, MIPS,
 * Alpha and PowerPC.
 *
 * The protocol, the encryption and the VP8 encoder are the classic Mac
 * agent's portable core (crustdesk-macos-classic/src/core), built with the
 * Visual C++ that came with NT; this file is the NT side of it, the IRIX
 * port's main loop (crustdesk-irix/src/main.c) with WinSock's select() in
 * place of poll(): the ID server, the relay and the peer, and the screen
 * read back between them.
 *
 * One desktop session at a time, beside a file manager. A frame is encoded
 * a few macroblock rows at a time with the network serviced in between, so
 * the peer's pointer and keys keep arriving while a large frame is made.
 */
#include "agent.h"
#include "clip.h"
#include "fs_win32.h"
#include "hooks.h"
#include "input.h"
#include "link.h"
#include "log.h"
#include "plat.h"
#include "prefs.h"
#include "report.h"
#include "screen.h"
#include "unzstd.h"

#include "files.h"
#include "png.h"
#include "rdv.h"
#include "rng.h"
#include "session.h"
#include "vp8enc.h"
#include "sodium/crypto_sign_ed25519.h"

#include "win.h"
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now_ms(void)
{
    return cdv_ms();
}

#ifdef VP8E_PROFILE
/* The encoder's profiler (vp8enc.c, built with VP8E_PROFILE): its clock, in
 * microseconds, from the performance counter (every NT has one). */
uint32_t vp8e_clock(void)
{
    static double per_us;
    LARGE_INTEGER c;
    if (per_us == 0.0) {
        LARGE_INTEGER f;
        per_us = QueryPerformanceFrequency(&f) && f.QuadPart ? (double)f.QuadPart / 1e6 : -1.0;
    }
    if (per_us < 0.0 || !QueryPerformanceCounter(&c))
        return 0;
    return (uint32_t)((double)c.QuadPart / per_us);
}
#endif

/* This process's share of the processor since the last call, in percent; -1
 * where that cannot be told (GetProcessTimes is NT's). */
static int cpu_percent(void)
{
    typedef BOOL(WINAPI * times_fn)(HANDLE, FILETIME *, FILETIME *, FILETIME *, FILETIME *);
    static times_fn f;
    static int looked;
    static double last_cpu;
    static uint32_t last_at;
    FILETIME a, b, k, u;
    double cpu;
    uint32_t now = cdv_ms(), span;
    int pct;
    if (!looked) {
        f = (times_fn)GetProcAddress(GetModuleHandle("kernel32.dll"), "GetProcessTimes");
        looked = 1;
    }
    if (!f || !f(GetCurrentProcess(), &a, &b, &k, &u))
        return -1;
    /* 100 ns units, to milliseconds */
    cpu = ((double)k.dwHighDateTime * 4294967296.0 + (double)k.dwLowDateTime +
           (double)u.dwHighDateTime * 4294967296.0 + (double)u.dwLowDateTime) / 10000.0;
    span = now - last_at;
    pct = last_at && span ? (int)((cpu - last_cpu) * 100.0 / (double)span) : -1;
    last_cpu = cpu;
    last_at = now;
    return pct;
}

static void copy(char *dst, const char *src, size_t cap)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

/* ---- sockets ------------------------------------------------------------- */

static void nonblock(SOCKET fd)
{
    unsigned long one = 1;
    ioctlsocket(fd, FIONBIO, &one);
}

static int resolve(const char *host, uint32_t *ip)
{
    struct hostent *h;
    unsigned long a = inet_addr(host);
    if (a != INADDR_NONE) {
        *ip = (uint32_t)ntohl(a);
        return 1;
    }
    h = gethostbyname(host);
    if (!h || h->h_length != 4)
        return 0;
    *ip = (uint32_t)((uint8_t)h->h_addr_list[0][0] << 24 | (uint8_t)h->h_addr_list[0][1] << 16 |
                     (uint8_t)h->h_addr_list[0][2] << 8 | (uint8_t)h->h_addr_list[0][3]);
    return 1;
}

static void sa_set(struct sockaddr_in *sa, uint32_t ip, uint16_t port)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(ip);
    sa->sin_port = htons(port);
}

/* Connect with a deadline; blocking otherwise. INVALID_SOCKET on failure. */
static SOCKET tcp_connect(uint32_t ip, uint16_t port, int ms)
{
    struct sockaddr_in sa;
    fd_set wr, ex;
    struct timeval tv;
    unsigned long zero = 0;
    int one = 1;
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return fd;
    sa_set(&sa, ip, port);
    nonblock(fd);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0 &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(fd, &wr);
    FD_SET(fd, &ex);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = ms % 1000 * 1000;
    if (select(0, NULL, &wr, &ex, &tv) <= 0 || FD_ISSET(fd, &ex)) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    ioctlsocket(fd, FIONBIO, &zero);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    return fd;
}

static SOCKET listen_on(uint16_t port, int udp)
{
    struct sockaddr_in sa;
    SOCKET fd = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    int one = 1;
    if (fd == INVALID_SOCKET)
        return fd;
    if (udp)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
    sa_set(&sa, 0, port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || (!udp && listen(fd, 2) != 0)) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    if (udp)
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (char *)&one, sizeof one);
    nonblock(fd);
    return fd;
}

static int send_all(SOCKET fd, const uint8_t *b, size_t n)
{
    while (n) {
        int w = send(fd, (const char *)b, (int)n, 0);
        if (w <= 0)
            return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---- the sessions ------------------------------------------------------------- *
 *
 * A connection takes a free slot; at login it becomes the desktop (one at a
 * time: it owns the screen, the encoder and the video buffer), a file manager
 * (a client opens one beside its desktop window), the terminal (one at a time,
 * its shell in the tray) or a tunnel. */

static cdv_screen scr;
static int scr_ok;
static cdv_ident ident;
static uint8_t sign_pk[32], sign_sk[64];
static char hostname[64];
static const char *desk_name = "Default";

#define CTL_CAP (64 * 1024)
#define VID_CAP (2 * 1024 * 1024)
#define IN_CAP (192 * 1024)
#define WORK_CAP (192 * 1024)
#define NSLOTS 4

enum { K_NONE, K_DESKTOP, K_FILES, K_TERM, K_TUNNEL };

typedef struct {
    SOCKET fd;            /* INVALID_SOCKET: free */
    cdv_session s;
    cdv_hooks hooks;      /* .user is the slot */
    uint8_t *ctl, *in;
    int kind;
    cdv_files *files;
    uint8_t *work;
    int files_started;
    uint32_t started;
    char how[16];
    int chat_told;        /* the peer has been told nobody reads chat here */
    SOCKET fwd;           /* a tunnel: where it goes */
    int fwd_dead;         /* that broke while the peer's bytes were going in */
} slot;
static slot slots[NSLOTS];
static slot *desk;        /* the slot showing the desktop, or NULL */
static slot *term;        /* the slot with the terminal, or NULL */
static int term_id;       /* the peer's number for it */

/* The desktop's own state, whichever slot it is in. */
static struct {
    vp8e *enc;
    int encoding, key;
    uint32_t last_frame, last_cursor, last_report, last_change, last_clip;
    uint32_t f_start, f_enc;  /* this frame: when it began, encoder time so far */
    int f_mbs, f_detail, f_flat;
    int cx, cy;
    /* where the peer last put the pointer, and when (cursor_step) */
    int in_x, in_y, in_seen;
    uint32_t in_at;
    uint32_t btn_at;          /* the peer's last button press or release */
    uint32_t gather_at;       /* when reads began to be left over; 0 none */
    uint32_t pass_max;        /* the longest pass of the main loop, for the log */
    uint32_t busy_ms;         /* what the last look and frame cost (the duty cap) */
    int want_shot;
    /* for the log */
    unsigned frames, bytes, mbs;
    uint32_t t_read, t_conv, t_enc;
    unsigned looks, rects;  /* looks at the screen; rectangles read (hooks) */
    unsigned long px;       /* pixels read (hooks) */
} S;

static uint8_t *vidbuf, *encmem, *encraw;
static const cdv_fs_ops *files_fs;  /* NULL: file transfer is off */
static int status_dirty = 1;

/* The frame budget: a frame codes what fits in about FRAME_BUDGET hundredths
 * of a millisecond of this CPU; the rest stays dirty for the next. Large
 * enough that a dialog or a window arrives whole, not in blocks: the network
 * is looked at every SLICE_MS while a frame is coded, so a long frame does
 * not hold up the peer's input; only a change bigger than this paints in. Flat
 * macroblocks are cheap; what a detailed one costs is learnt as it goes. */
#define FRAME_BUDGET 60000
#define COST_FLAT 30
static int cost_detail = 150;
static uint8_t *enc_dirty;    /* the macroblocks of the frame being encoded */
static uint8_t *dirty_age;    /* frames a macroblock has waited */
static uint8_t *flat_y, *flat_u, *flat_v;
static int enc_w, enc_h;      /* the screen size they were made for */
static int frame_ms = 66;     /* at most ~15 a second unless the peer asks */

/* With nothing changing, the screen is looked at less often: a look reads
 * from the display card, and where that is slow (the Alpha: about 120 ms a
 * look, 60% of the machine at five looks a second with nothing on screen
 * changing) it is the machine's time, not just the session's. Anything the
 * peer does -- a key, the mouse -- brings it straight back to full speed;
 * a change nobody made shows within QUIET_LOOK_MS. */
#define IDLE_AFTER_MS 2000
#define IDLE_LOOK_MS 200
#define QUIET_AFTER_MS 5000
#define QUIET_LOOK_MS 1000
/* How long one look at the screen may read for before handing back: the
 * rows around the pointer, then bands of the rest -- a quarter of a
 * 1280x1024 screen in QEMU's PowerPC, where a whole one takes 90 ms to
 * BitBlt. A change away from the pointer shows within a few frames. */
#define LOOK_BUDGET_MS 25

static int enc_buffers(int w, int h)
{
    size_t px;
    int n;
    if (w <= enc_w && h <= enc_h && encraw)
        return 1;
    w = (w + 15) & ~15;
    h = (h + 15) & ~15;
    px = (size_t)w * h;
    n = w / 16 * (h / 16);
    free(encraw);
    free(enc_dirty);
    free(dirty_age);
    free(flat_y);
    free(flat_u);
    free(flat_v);
    encraw = (uint8_t *)malloc(vp8e_mem_size(w, h) + 16);
    enc_dirty = (uint8_t *)calloc((size_t)n, 1);
    dirty_age = (uint8_t *)calloc((size_t)n, 1);
    flat_y = (uint8_t *)malloc(px);
    flat_u = (uint8_t *)malloc(px / 4);
    flat_v = (uint8_t *)malloc(px / 4);
    enc_w = enc_h = 0;
    if (!encraw || !enc_dirty || !dirty_age || !flat_y || !flat_u || !flat_v)
        return 0;
    encmem = (uint8_t *)(((unsigned long)encraw + 15) & ~15ul);
    enc_w = w;
    enc_h = h;
    return 1;
}

static void on_mouse(void *u, int mask, int x, int y)
{
    if ((slot *)u != desk)
        return;
    input_mouse(mask, x, y);
    if ((mask & 7) == 1 || (mask & 7) == 2) {
        /* every button, for finding out what a double-click looks like here */
        POINT at;
        GetCursorPos(&at);
        cdv_log("click: %s button %d, peer %d,%d, pointer %d,%d, %u ms after the last",
                (mask & 7) == 1 ? "down" : "up", mask >> 3, x, y, (int)at.x, (int)at.y,
                (unsigned)(now_ms() - S.btn_at));
        S.btn_at = now_ms(); /* a click: a second may follow (CLICK_HOLD_MS) */
        screen_note_input();
    }
    if ((mask & 7) < 3) { /* a move, a press or a release: x, y is where */
        S.in_x = x;
        S.in_y = y;
        S.in_seen = 1;
        S.in_at = now_ms();
    }
    S.last_change = now_ms(); /* the screen will answer: look often */
}

static void lock_screen(void);

static void on_key(void *u, const cdv_key *k)
{
    if ((slot *)u != desk)
        return;
    if (k->kind == KEY_CONTROL && k->value == 101) { /* LockScreen */
        if (k->down || k->press)
            lock_screen();
        return;
    }
    input_key(k);
    screen_note_input();
    S.last_change = now_ms();
}

/* The peer's clipboard: compressed whenever that was smaller. */
static void on_clipboard(void *u, const char *utf8, size_t n, int compressed)
{
    if ((slot *)u != desk)
        return;
    if (compressed) {
        static char text[256 * 1024];
        long len = cdv_unzstd(text, sizeof text, utf8, n);
        if (len < 0) {
            cdv_log("clipboard: could not decompress the peer's text");
            return;
        }
        clip_from_peer(text, (size_t)len);
    } else {
        clip_from_peer(utf8, n);
    }
}

static void on_log(void *u, const char *m)
{
    (void)u;
    cdv_log("session: %s", m);
}

static void on_option(void *u, int quality, int fps)
{
    if ((slot *)u != desk)
        return;
    if (quality >= 0 && S.enc) {
        vp8e_set_q(S.enc, quality);
        cdv_log("peer set quality q=%d", quality);
    }
    if (fps > 0) {
        frame_ms = 1000 / fps;
        if (frame_ms < 20)
            frame_ms = 20;
        cdv_log("peer asked for %d fps", fps);
    }
}

/* Restart, as the peer asked: SYSTEM may, once it says so. */
static void on_restart(void *u)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;
    (void)u;
    cdv_log("the peer asked for a restart");
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0))
        cdv_log("restart refused (error %lu)", GetLastError());
}

/* ---- chat, lock and block input: the tray program (tray.c) shows chat ---- */

static int input_blocked;

/* The double-click time, widened for a desktop session. Two clicks made at
 * the peer reach Windows late and unevenly -- the network, this agent -- and
 * measured on the PowerPC the time left is about 0.4 s even at the machine.
 * Not saved: it lasts until restored here, or the next logon. */
#define SESSION_DBLCLK_MS 900
static UINT saved_dblclk;

static void widen_dblclk(void)
{
    UINT now = GetDoubleClickTime();
    cdv_log("double-click time here is %u ms", now);
    if (now >= SESSION_DBLCLK_MS)
        return;
    if (SetDoubleClickTime(SESSION_DBLCLK_MS))
        saved_dblclk = now;
    else
        cdv_log("cannot change the double-click time (error %lu)", GetLastError());
}

static void restore_dblclk(void)
{
    if (saved_dblclk)
        SetDoubleClickTime(saved_dblclk);
    saved_dblclk = 0;
}

/* A chat line from the peer: to the tray, which shows it to whoever is at
 * the console. Without a tray the peer is told so, once a session. */
static void on_chat(void *u, const char *utf8, size_t n)
{
    slot *sl = (slot *)u;
    static char m[LINK_MAX];
    size_t k;
    if (link_connected()) {
        k = (size_t)snprintf(m, sizeof m, "%s\n", sl->s.peer_name[0] ? sl->s.peer_name : "Peer");
        if (n > sizeof m - k)
            n = sizeof m - k;
        memcpy(m + k, utf8, n);
        link_send(LINK_PEER_CHAT, m, k + n);
        return;
    }
    cdv_log("chat from the peer, and no tray to show it");
    if (!sl->chat_told) {
        static const char none[] =
            "(C-Desk-Vint) Nobody can read this: the C-Desk-Vint tray is not running here "
            "(it starts when someone logs on to Windows NT).";
        sl->chat_told = 1;
        cdv_send_chat(&sl->s, none, sizeof none - 1);
    }
}

/* The peer's Block user input. BlockInput is Windows 98's and 2000's, not
 * NT 4.0's; it has to be called by a thread on the input desktop, which the
 * main loop is (screen_follow). Ctrl-Alt-Del at the machine undoes it. */
static void set_block(int on)
{
    typedef BOOL(WINAPI * block_fn)(BOOL);
    static block_fn f;
    static int looked;
    if (!looked) {
        f = (block_fn)GetProcAddress(GetModuleHandle("user32.dll"), "BlockInput");
        looked = 1;
    }
    if (on == input_blocked)
        return;
    if (!f) {
        if (on)
            cdv_log("the peer asked to block this machine's keyboard and mouse; this Windows "
                    "cannot (Windows 98 and 2000 can)");
        return;
    }
    if (f(on ? TRUE : FALSE)) {
        input_blocked = on;
        cdv_log("this machine's keyboard and mouse are %s", on ? "blocked" : "free again");
    } else {
        cdv_log("cannot %sblock input (error %lu)", on ? "" : "un", GetLastError());
    }
}

static void on_block_input(void *u, int on)
{
    if ((slot *)u == desk)
        set_block(on);
}

/* The peer's Insert lock: LockWorkStation, Windows 2000's. The tray does it
 * if it is running -- it is the user's program on their desktop, which the
 * call wants; else the agent tries itself. NT 4.0 has no way to. */
static void lock_screen(void)
{
    typedef BOOL(WINAPI * lock_fn)(void);
    lock_fn f = (lock_fn)GetProcAddress(GetModuleHandle("user32.dll"), "LockWorkStation");
    if (!f) {
        cdv_log("the peer asked to lock the screen; this Windows cannot (2000 and later can)");
        return;
    }
    if (link_connected()) {
        cdv_log("the peer asked to lock the screen; the tray locks it");
        link_send(LINK_LOCK, NULL, 0);
    } else if (!f()) {
        cdv_log("cannot lock the screen (error %lu)", GetLastError());
    }
}

static void on_screenshot(void *u)
{
    if ((slot *)u == desk)
        S.want_shot = 1;
}

/* The account the shell (Explorer: Program Manager's window is its desktop)
 * runs as -- the person at the console -- as TOKEN_USER in buf; 0 if there
 * is no shell (nobody logged on, or not on NT). The tray link lets in only
 * a program of theirs. */
static int shell_owner(char *buf, DWORD cap)
{
    HWND shell;
    DWORD pid = 0, got;
    int ok = 0;
    if (!plat_nt() || (shell = FindWindow("Progman", NULL)) == NULL)
        return 0;
    GetWindowThreadProcessId(shell, &pid);
    if (pid) {
        HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid), tok;
        if (p) {
            if (OpenProcessToken(p, TOKEN_QUERY, &tok)) {
                ok = GetTokenInformation(tok, TokenUser, buf, cap, &got) != 0;
                CloseHandle(tok);
            }
            CloseHandle(p);
        }
    }
    return ok;
}

static void console_sid_step(void)
{
    static uint32_t next;
    char buf[256];
    uint32_t now = now_ms();
    if ((int32_t)(now - next) < 0)
        return;
    next = now + 2000;
    link_set_console_sid(shell_owner(buf, sizeof buf) ? ((TOKEN_USER *)buf)->User.Sid : NULL);
}

/* Who is logged in at the console: the shell's owner, else whoever last
 * logged on. */
static const char *console_user(void)
{
    static char name[64];
    char dom[64], buf[256];
    DWORD nl = sizeof name, dl = sizeof dom;
    SID_NAME_USE use;
    int found = 0;
    if (!plat_nt()) {
        /* 9x: whoever logged on (nobody, at the logon dialog) */
        DWORD n = sizeof name;
        if (!GetUserName(name, &n) || !name[0])
            strcpy(name, "nobody");
        return name;
    }
    strcpy(name, "SYSTEM");
    if (shell_owner(buf, sizeof buf) &&
        LookupAccountSid(NULL, ((TOKEN_USER *)buf)->User.Sid, name, &nl, dom, &dl, &use))
        found = 1;
    if (!found) {
        HKEY k;
        DWORD type, len = sizeof name;
        strcpy(name, "SYSTEM");
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 0, KEY_READ,
                         &k) == ERROR_SUCCESS) {
            if (RegQueryValueEx(k, "DefaultUserName", NULL, &type, (LPBYTE)name, &len) !=
                    ERROR_SUCCESS || type != REG_SZ || !name[0])
                strcpy(name, "SYSTEM");
            RegCloseKey(k);
        }
    }
    name[sizeof name - 1] = 0;
    return name;
}

static int open_screen(void)
{
    if (!screen_open(&scr))
        return 0;
    input_init(scr.width, scr.height);
    scr_ok = 1;
    return 1;
}

static int on_login(void *u, int file_transfer)
{
    slot *sl = (slot *)u;
    if (file_transfer) {
        if (!files_fs) {
            cdv_log("refusing a file-transfer session (files=off)");
            return 0;
        }
        sl->kind = K_FILES;
        sl->files_started = 0;
        cdv_files_init(sl->files, &sl->s, files_fs, sl->work, WORK_CAP);
        return 1;
    }
    if (desk && desk != sl) {
        cdv_log("refusing a second desktop session");
        return 0;
    }
    if ((!scr_ok || screen_resized(&scr)) && !open_screen()) {
        cdv_log("refusing a desktop session: cannot read the screen");
        return 0;
    }
    if (!enc_buffers(scr.width, scr.height)) {
        cdv_log("refusing a desktop session: no memory for a %dx%d screen", scr.width, scr.height);
        return 0;
    }
    ident.width = scr.width;
    ident.height = scr.height;
    sl->kind = K_DESKTOP;
    desk = sl;
    /* only while someone is looking: nothing is hooked between sessions */
    hooks_start(scr.width, scr.height);
    widen_dblclk();
    cdv_set_video(&sl->s, vidbuf, VID_CAP);
    S.enc = vp8e_init(encmem, scr.width, scr.height, prefs.q);
    S.encoding = 0;
    S.last_frame = S.last_cursor = S.last_report = S.last_change = S.last_clip = now_ms();
    S.in_seen = 0;
    S.want_shot = 0;
    S.frames = S.bytes = S.mbs = 0;
    S.t_read = S.t_conv = S.t_enc = 0;
    S.looks = S.rects = 0;
    S.px = 0;
    S.cx = S.cy = -1;
    return 1;
}

/* A terminal or a tunnel, at login. */
static int on_login_kind(void *u, int kind)
{
    slot *sl = (slot *)u;
    if (kind == CDV_CONN_TERMINAL) {
        if (!prefs.terminal || !link_connected() || term) {
            cdv_log("refusing a terminal: %s", !prefs.terminal ? "off in the settings"
                                               : term ? "one is open"
                                                      : "no tray (nobody logged on?)");
            return 0;
        }
        sl->kind = K_TERM;
        term = sl;
        return 1;
    }
    if (kind == CDV_CONN_TUNNEL) {
        const char *host = sl->s.tunnel_host;
        int port = sl->s.tunnel_port;
        uint32_t ip;
        if (!prefs.tunnel) {
            cdv_log("refusing a tunnel: off in the settings");
            return 0;
        }
        if (!strcmp(host, "RDP") && !port) {
            host = "";
            port = 3389;
        }
        if (!host[0])
            host = "localhost";
        if (!strcmp(host, "localhost"))
            ip = 0x7f000001;
        else if (!resolve(host, &ip)) {
            cdv_log("tunnel: cannot resolve %s", host);
            return 0;
        }
        sl->fwd = tcp_connect(ip, (uint16_t)port, 3000);
        if (sl->fwd == INVALID_SOCKET) {
            cdv_log("tunnel: nothing at %s:%d", host, port);
            return 0;
        }
        cdv_log("tunnel to %s:%d", host, port);
        sl->kind = K_TUNNEL;
        return 1;
    }
    return 0;
}

/* The peer's terminal: its shell is the tray's. One shell, whatever the
 * peer's number for it; closing a tab leaves it running for next time. */
static void on_term(void *u, int what, int id, const uint8_t *d, size_t n, int rows, int cols)
{
    slot *sl = (slot *)u;
    (void)rows;
    (void)cols;
    if (sl != term)
        return;
    if (what == CDV_TERM_OPEN) {
        term_id = id;
        if (link_connected())
            link_send(LINK_TERM_OPEN, NULL, 0);
        else
            cdv_term_error(&sl->s, id, "The tray is not running here");
    } else if (what == CDV_TERM_DATA) {
        link_send(LINK_TERM_IN, (const char *)d, n);
    } else if (what == CDV_TERM_CLOSE) {
        cdv_term_closed(&sl->s, id, 0);
    }
}

/* What the peer sends down its tunnel. */
static void on_tunnel_data(void *u, const uint8_t *d, size_t n)
{
    slot *sl = (slot *)u;
    if (sl->fwd != INVALID_SOCKET && send_all(sl->fwd, d, n) < 0)
        sl->fwd_dead = 1;
}

static const char *kind_name(int kind)
{
    return kind == K_FILES ? "files" : kind == K_TERM ? "terminal" : kind == K_TUNNEL ? "tunnel"
                                                                                      : "desktop";
}

static void on_file(void *u, int field, const uint8_t *d, size_t n)
{
    slot *sl = (slot *)u;
    if (sl->kind == K_FILES)
        cdv_files_message(sl->files, field, d, n);
}

static int slots_free(void)
{
    int i, n = 0;
    for (i = 0; i < NSLOTS; i++)
        n += slots[i].fd == INVALID_SOCKET;
    return n;
}

static void session_begin(SOCKET fd, int secure, const char *how)
{
    int one = 1, i;
    slot *sl = NULL;
    for (i = 0; i < NSLOTS && !sl; i++)
        if (slots[i].fd == INVALID_SOCKET)
            sl = &slots[i];
    if (!sl) {
        cdv_log("%s connection refused: both sessions are in use", how);
        closesocket(fd);
        return;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    nonblock(fd);
    sl->fd = fd;
    sl->kind = K_NONE;
    sl->started = now_ms();
    copy(sl->how, how, sizeof sl->how);
    memset(&sl->hooks, 0, sizeof sl->hooks);
    sl->hooks.mouse = on_mouse;
    sl->hooks.key = on_key;
    sl->hooks.clipboard = on_clipboard;
    sl->hooks.log = on_log;
    sl->hooks.option = on_option;
    sl->hooks.restart = on_restart;
    sl->hooks.screenshot = on_screenshot;
    sl->hooks.login = on_login;
    sl->hooks.file = on_file;
    sl->hooks.chat = on_chat;
    sl->hooks.block_input = on_block_input;
    sl->hooks.login_kind = on_login_kind;
    sl->hooks.term = on_term;
    sl->hooks.tunnel_data = on_tunnel_data;
    sl->hooks.user = sl;
    sl->chat_told = 0;
    sl->fwd = INVALID_SOCKET;
    sl->fwd_dead = 0;
    ident.username = console_user();
    cdv_init2(&sl->s, sl->ctl, CTL_CAP, NULL, 0, sl->in, IN_CAP, &sl->hooks, &ident, now_ms());
    cdv_start(&sl->s, now_ms(), secure);
    cdv_log("%s connection; %s", how, secure ? "encrypted" : "direct");
}

static void session_end(slot *sl, const char *why)
{
    if (sl->fd == INVALID_SOCKET)
        return;
    closesocket(sl->fd);
    sl->fd = INVALID_SOCKET;
    if (sl == desk) {
        if (S.encoding)
            vp8e_abandon(S.enc);
        S.encoding = 0;
        input_release_all();
        set_block(0);
        hooks_stop();
        restore_dblclk();
        desk = NULL;
    }
    if (sl->kind == K_FILES)
        cdv_files_close(sl->files);
    if (sl == term)
        term = NULL;
    if (sl->fwd != INVALID_SOCKET) {
        closesocket(sl->fwd);
        sl->fwd = INVALID_SOCKET;
    }
    cdv_log("%s session over: %s", kind_name(sl->kind), why);
    sl->kind = K_NONE;
    status_dirty = 1;
}

/* Bits a pixel on the display card: what a look at the screen copies. */
static int screen_depth(void)
{
    HDC dc = GetDC(NULL);
    int bpp = 0;
    if (dc) {
        bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
        ReleaseDC(NULL, dc);
    }
    return bpp;
}

static void desktop_started(slot *sl)
{
    uint32_t t0 = now_ms();
    const char *name;
    screen_follow(&name);
    desk_name = name;
    input_release_all();
    screen_start(&scr);
    clip_reset();
    cdv_log("first look at the screen (%s desktop, %dx%d, %d-bit): %u ms", desk_name, scr.width,
            scr.height, screen_depth(), (unsigned)(now_ms() - t0));
    S.key = 1;
}

/* Bytes in and out; 0 when the session has ended. */
static int session_io(slot *sl, int readable)
{
    size_t n;
    const uint8_t *o;
    if (readable) {
        static uint8_t buf[16384];
        int r = recv(sl->fd, (char *)buf, sizeof buf, 0);
        if (r == 0 || (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
            cdv_debug("recv: %d, error %d", r, r < 0 ? WSAGetLastError() : 0);
            session_end(sl, r == 0 ? "the peer closed the connection" : "the connection broke");
            return 0;
        }
        if (r > 0) {
            int was_live = sl->s.state == CDV_LIVE;
            rng_add(buf, r < 32 ? (size_t)r : 32);
            if (cdv_feed(&sl->s, buf, (size_t)r) < 0 && !cdv_out_peek(&sl->s, &n)) {
                session_end(sl, "protocol error");
                return 0;
            }
            if (!was_live && sl->s.state == CDV_LIVE) {
                cdv_log("logged in: %s (%s %s), %s", sl->s.peer_name, sl->s.peer_platform,
                        sl->s.peer_version, kind_name(sl->kind));
                status_dirty = 1;
                if (sl == desk) {
                    desktop_started(sl);
                } else if (sl->kind == K_FILES && !sl->files_started) {
                    cdv_files_start(sl->files);
                    sl->files_started = 1;
                }
            }
        }
    }
    if (sl->kind == K_FILES && sl->s.state == CDV_LIVE)
        cdv_files_pump(sl->files);
    for (;;) {
        int w;
        o = cdv_out_peek(&sl->s, &n);
        if (!n)
            break;
        w = send(sl->fd, (const char *)o, (int)n, 0);
        if (w < 0) {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                break;
            session_end(sl, "send failed");
            return 0;
        }
        cdv_out_consume(&sl->s, (size_t)w);
        if ((size_t)w < n)
            break;
    }
    cdv_tick(&sl->s, now_ms());
    if (sl->s.state == CDV_CLOSED && !cdv_out_peek(&sl->s, &n)) {
        session_end(sl, "closed");
        return 0;
    }
    if (sl->s.state != CDV_LIVE && now_ms() - sl->started > 15000) {
        session_end(sl, "no login within 15 s");
        return 0;
    }
    return 1;
}

/* Which dirty macroblocks this frame codes (enc_dirty), within the budget:
 * those that have waited longest first, so a busy corner cannot starve the
 * rest of the screen, then the rows nearest the pointer, where the person is
 * looking. Cheap ones always go. */
static void select_mbs(void)
{
    int n = scr.mbw * scr.mbh, i, budget = FRAME_BUDGET, prow, d, age, maxage = 0;
    memset(enc_dirty, 0, (size_t)n);
    S.f_mbs = S.f_detail = S.f_flat = 0;
    for (i = 0; i < n; i++)
        if (scr.dirty[i] && dirty_age[i] > maxage)
            maxage = dirty_age[i];
    prow = S.cy >= 0 ? S.cy / 16 : scr.mbh / 2;
    for (age = maxage; age >= 0; age--)
        for (d = 0; d < 2 * scr.mbh; d++) {
            int row = d & 1 ? prow + (d + 1) / 2 : prow - d / 2, c;
            if (row < 0 || row >= scr.mbh)
                continue;
            for (c = 0; c < scr.mbw; c++) {
                int cost;
                i = row * scr.mbw + c;
                if (!scr.dirty[i] || dirty_age[i] != age)
                    continue;
                cost = scr.flat[i] ? COST_FLAT : cost_detail;
                if (budget < cost && S.f_mbs > 0)
                    return;
                enc_dirty[i] = 1;
                S.f_mbs++;
                budget -= cost;
                if (scr.flat[i])
                    S.f_flat++;
                else
                    S.f_detail++;
            }
        }
}

/* How long the encoder may hold the main loop before the network is looked
 * at again; it codes a macroblock row at a time (a full-width row is 50 on an
 * 800-pixel screen: 130 ms on the Alpha), so that is a floor, not a limit. */
#define SLICE_MS 15

/* After the peer presses or releases a button, no new look or frame starts
 * for this long: a click usually changes the screen, and the work of sending
 * that must not sit between it and the second click of a double-click
 * (Windows takes two clicks more than 500 ms apart for two single ones). */
#define CLICK_HOLD_MS 300

/* A look reads for a limited time and keeps the rest (screen.c). A frame
 * waits up to this long for those reads to finish, so a change is coded
 * whole and not as the blocks that happened to be read first. */
#define GATHER_MS 500

static void frame_step(void)
{
    uint32_t t;
    if (!desk || desk->s.state != CDV_LIVE)
        return;
    if (!S.encoding) {
        size_t cap;
        uint8_t *dst;
        vp8e_src src;
        uint32_t now = now_ms(), gap;
        if (desk->s.vstate != VID_FREE)
            return;
        if (now - S.btn_at < CLICK_HOLD_MS)
            return;
        gap = (uint32_t)frame_ms;
        if (now - S.last_change > QUIET_AFTER_MS && gap < QUIET_LOOK_MS)
            gap = QUIET_LOOK_MS;
        else if (now - S.last_change > IDLE_AFTER_MS && gap < IDLE_LOOK_MS)
            gap = IDLE_LOOK_MS;
        /* at most half the processor: the time between the starts of two
         * looks is at least twice what the last look and frame took, and
         * the rest is for the programs on this machine and its mouse */
        if (gap < 2 * S.busy_ms)
            gap = 2 * S.busy_ms;
        if (cdv_take_refresh(&desk->s))
            S.key = 1;
        if (!S.key && now - S.last_frame < gap)
            return;
        S.last_frame = now;
        screen_collect(&scr, LOOK_BUDGET_MS);
        S.t_read += scr.read_ms;
        S.t_conv += scr.convert_ms;
        S.busy_ms = scr.read_ms + scr.convert_ms;
        S.looks++;
        S.rects += (unsigned)scr.rects_read;
        S.px += (unsigned long)scr.px_read;
        /* a change, or marks the look had no time for: look again soon */
        if (scr.ndirty || screen_pending(&scr) || screen_settling())
            S.last_change = now;
        if (!screen_pending(&scr)) {
            S.gather_at = 0;
        } else if (!S.key) {
            if (!S.gather_at)
                S.gather_at = now;
            if (now - S.gather_at < GATHER_MS) {
                S.last_frame = now - gap; /* read on, at once */
                return;
            }
            S.gather_at = 0;
        }
        if (S.key) {
            /* The first picture, or the peer asked for one: a keyframe of
             * the background colour alone (one macroblock coded, the rest
             * the encoder's cache), then the real picture over the next
             * frames, within the budget. */
            uint8_t by, bu, bv;
            screen_background(&scr, &by, &bu, &bv);
            memset(flat_y, by, (size_t)scr.ystride * scr.mbh * 16);
            memset(flat_u, bu, (size_t)scr.uvstride * scr.mbh * 8);
            memset(flat_v, bv, (size_t)scr.uvstride * scr.mbh * 8);
            screen_mark_all(&scr);
            memset(dirty_age, 1, (size_t)(scr.mbw * scr.mbh));
            src.y = flat_y;
            src.u = flat_u;
            src.v = flat_v;
            memset(enc_dirty, 1, (size_t)(scr.mbw * scr.mbh));
            S.f_mbs = S.f_detail = S.f_flat = 0;
        } else {
            if (!scr.ndirty)
                return;
            select_mbs();
            src.y = scr.y;
            src.u = scr.u;
            src.v = scr.v;
        }
        dst = cdv_video_begin(&desk->s, &cap);
        if (!dst)
            return;
        src.ystride = scr.ystride;
        src.uvstride = scr.uvstride;
        vp8e_set_hints(S.enc, NULL);
        if (!vp8e_begin(S.enc, &src, S.key ? NULL : enc_dirty, S.key, dst, cap)) {
            cdv_video_abort(&desk->s);
            return;
        }
        S.encoding = 1;
        S.mbs += (unsigned)S.f_mbs;
        S.f_start = now;
        S.f_enc = 0;
        cdv_debug("frame: %d of %d dirty (%d detail, %d flat), %d rows read, read %u ms, "
                  "convert %u ms", S.f_mbs, scr.ndirty, S.f_detail, S.f_flat, scr.rows_read,
                  (unsigned)scr.read_ms, (unsigned)scr.convert_ms);
    }
    t = now_ms();
    {
        int done;
        while (!(done = vp8e_rows(S.enc, 1)) && now_ms() - t < SLICE_MS)
            ;
        if (!done) {
            S.t_enc += now_ms() - t;
            S.f_enc += now_ms() - t;
            return;
        }
    }
    {
        size_t len = vp8e_end(S.enc);
        vp8e_stats st;
        S.encoding = 0;
        S.t_enc += now_ms() - t;
        S.f_enc += now_ms() - t;
        if (!len) {
            cdv_video_abort(&desk->s);
            S.key = 1;
            cdv_log("frame did not fit; a keyframe next");
            return;
        }
        S.busy_ms += S.f_enc;
        vp8e_last_stats(S.enc, &st);
        cdv_video_commit(&desk->s, len, st.key);
        if (!st.key) {
            int i, n = scr.mbw * scr.mbh;
            screen_sent_some(&scr, enc_dirty);
            for (i = 0; i < n; i++)
                dirty_age[i] = scr.dirty[i] ? (uint8_t)(dirty_age[i] < 250 ? dirty_age[i] + 1 : 250)
                                            : 0;
            /* learn what a detailed macroblock costs here, now */
            if (S.f_detail >= 16) {
                int per = ((int)S.f_enc * 100 - S.f_flat * COST_FLAT) / S.f_detail;
                if (per < 10)
                    per = 10;
                if (per > 2000)
                    per = 2000;
                cost_detail = (cost_detail * 3 + per) / 4;
            }
        }
        S.key = 0;
        S.frames++;
        S.bytes += (unsigned)len;
        cdv_debug("sent %s frame: %d mbs (%d intra, %d skipped) in %u bytes; encode %u ms of %u ms",
                  st.key ? "key" : "inter", S.f_mbs, st.intra, st.skipped, (unsigned)len,
                  (unsigned)S.f_enc, (unsigned)(now_ms() - S.f_start));
    }
}

/* The screenshot the peer asked for, as a PNG in the video buffer (between
 * frames, so it is free). */
static void screenshot_step(void)
{
    static png_writer *pw;
    size_t cap, len;
    uint8_t *dst, *row;
    int y;
    if (!S.want_shot || S.encoding || desk->s.vstate != VID_FREE)
        return;
    S.want_shot = 0;
    if (!pw)
        pw = (png_writer *)malloc(png_writer_size());
    row = (uint8_t *)malloc((size_t)scr.width * 3);
    dst = cdv_big_begin(&desk->s, &cap);
    if (!pw || !row || !dst) {
        free(row);
        if (dst)
            cdv_screenshot_commit(&desk->s, 0, "out of memory");
        return;
    }
    screen_grab_all(&scr);
    png_begin(pw, dst, cap, scr.width, scr.height, NULL, 0);
    for (y = 0; y < scr.height; y++) {
        screen_rgb_row(&scr, y, row);
        png_row(pw, row);
    }
    len = png_end(pw);
    free(row);
    cdv_screenshot_commit(&desk->s, len, len ? NULL : "the screenshot is too big");
    cdv_log("screenshot: %u bytes", (unsigned)len);
}

static void session_report(void)
{
    uint32_t now = now_ms(), span = now - S.last_report;
    if (!desk || span < 10000 || desk->s.state != CDV_LIVE)
        return;
    if (S.frames)
        cdv_log("%u frames in %u.%u s (%u.%u fps), %u KB, %u macroblocks; ms read %u convert %u "
                "encode %u; %u looks, %u rects, %lu kpx; longest pass %u ms; cpu %d%%",
                S.frames, (unsigned)(span / 1000), (unsigned)(span % 1000 / 100),
                (unsigned)(S.frames * 10000u / span / 10), (unsigned)(S.frames * 10000u / span % 10),
                S.bytes / 1024, S.mbs, (unsigned)S.t_read, (unsigned)S.t_conv, (unsigned)S.t_enc,
                S.looks, S.rects, S.px / 1000, (unsigned)S.pass_max, cpu_percent());
#ifdef VP8E_PROFILE
    if (S.frames && S.enc) {
        uint32_t p[5];
        vp8e_profile(S.enc, p);
        cdv_log("  encode, ms: mode search %u, transform %u, reconstruct %u, exactness %u, "
                "tokens %u", (unsigned)(p[0] / 1000), (unsigned)(p[1] / 1000),
                (unsigned)(p[2] / 1000), (unsigned)(p[3] / 1000), (unsigned)(p[4] / 1000));
    }
#endif
    S.frames = S.bytes = S.mbs = 0;
    S.t_read = S.t_conv = S.t_enc = 0;
    S.looks = S.rects = 0;
    S.px = 0;
    S.pass_max = 0;
    S.last_report = now;
}

static void cursor_step(void)
{
    static uint8_t rgba[64 * 64 * 4];
    int x, y, w, h, hx, hy;
    uint32_t id, now = now_ms();
    if (!desk || desk->s.state != CDV_LIVE || now - S.last_cursor < 100)
        return;
    S.last_cursor = now;
    if (screen_cursor_shape(&id, &hx, &hy, &w, &h, rgba))
        cdv_send_cursor(&desk->s, id, hx, hy, w, h, rgba);
    if (!screen_pointer(&x, &y) || (x == S.cx && y == S.cy))
        return;
    S.cx = x;
    S.cy = y;
    /* Only a move the peer did not make: a client takes every position as
     * the remote side moving the pointer, hides its own and holds back its
     * mouse for a moment (see the IRIX port). */
    if (S.in_seen && ((int32_t)(now - S.in_at) < 300 || (x == S.in_x && y == S.in_y)))
        return;
    cdv_send_cursor_pos(&desk->s, x, y);
}

static void clip_step(void)
{
    static char text[256 * 1024];
    long n;
    uint32_t now = now_ms();
    if (!desk || desk->s.state != CDV_LIVE || now - S.last_clip < 250)
        return;
    S.last_clip = now;
    n = clip_take(text, sizeof text);
    if (n >= 0)
        cdv_send_clipboard(&desk->s, text, (size_t)n);
}

/* The input desktop changed (a logon, a lock, the logon screen) or the
 * screen changed size: follow, and show it all again. */
static uint32_t desk_check_at;

static void desk_step(void)
{
    const char *name;
    uint32_t now = now_ms();
    if ((int32_t)(now - desk_check_at) < 0)
        return;
    desk_check_at = now + 250;
    if (screen_follow(&name)) {
        cdv_log("the input desktop is now %s", name);
        desk_name = name;
        status_dirty = 1;
        if (desk && desk->s.state == CDV_LIVE) {
            if (S.encoding) {
                vp8e_abandon(S.enc);
                cdv_video_abort(&desk->s);
                S.encoding = 0;
            }
            screen_start(&scr);
            S.key = 1;
        }
    }
    if (desk && desk->s.state == CDV_LIVE && !S.encoding && screen_resized(&scr)) {
        if (!open_screen() || !enc_buffers(scr.width, scr.height)) {
            session_end(desk, "the screen changed size, and there is no memory for the new one");
            scr_ok = 0;
            return;
        }
        cdv_log("the screen is now %dx%d", scr.width, scr.height);
        hooks_set_screen(scr.width, scr.height);
        ident.width = scr.width;
        ident.height = scr.height;
        S.enc = vp8e_init(encmem, scr.width, scr.height, prefs.q);
        cdv_send_display(&desk->s, scr.width, scr.height);
        screen_start(&scr);
        S.key = 1;
    }
}

/* ---- the ID server ------------------------------------------------------- */

static cdv_rdv rdv;
static SOCKET udp_rdv = INVALID_SOCKET, udp_lan = INVALID_SOCKET, tcp_direct = INVALID_SOCKET;
static uint32_t hbbs_ip, resolve_at;
static uint16_t hbbs_port;
static char hbbs_host[64];
static int was_registered;

static void rdv_setup(void)
{
    memset(&rdv, 0, sizeof rdv);
    rdv_init(&rdv);
    copy(rdv.id, prefs.id, sizeof rdv.id);
    memcpy(rdv.uuid, prefs.uuid, 16);
    memcpy(rdv.pk, sign_pk, 32);
    copy(rdv.key, prefs.key, sizeof rdv.key);
    copy(rdv.relay, prefs.relay, sizeof rdv.relay);
    if (prefs.server[0])
        rdv_split_host(prefs.server, hbbs_host, sizeof hbbs_host, &hbbs_port, RDV_PORT);
}

/* Looking the ID server up without waiting on it: gethostbyname can take
 * tens of seconds when DNS is unreachable, and the main loop must not. A
 * thread asks and leaves the answer here. */
static HANDLE look_thread;
static volatile LONG look_done;
static volatile uint32_t look_ip;
static char look_host[64];
static uint32_t look_began;

static DWORD WINAPI lookup_thread(LPVOID unused)
{
    uint32_t ip = 0;
    (void)unused;
    resolve(look_host, &ip);
    look_ip = ip;
    InterlockedExchange((LONG *)&look_done, 1);
    return 0;
}

static void lookup_start(const char *host)
{
    copy(look_host, host, sizeof look_host);
    look_done = 0;
    look_ip = 0;
    look_thread = plat_thread(lookup_thread, NULL);
    look_began = now_ms();
}

/* -1 still asking, 0 no answer, 1 *ip is the address. */
static int lookup_finished(uint32_t *ip)
{
    if (!look_done) {
        if ((int32_t)(now_ms() - look_began) < 60000)
            return -1;
        /* abandon it: it will finish in its own time and touch only statics */
        CloseHandle(look_thread);
        look_thread = NULL;
        return 0;
    }
    CloseHandle(look_thread);
    look_thread = NULL;
    if (!look_ip)
        return 0;
    *ip = look_ip;
    return 1;
}

static void rdv_step(void)
{
    uint8_t buf[2048];
    size_t n;
    struct sockaddr_in sa;
    uint32_t now = now_ms(), ip;
    if (!prefs.server[0])
        return;
    if (look_thread) {
        int r = lookup_finished(&ip);
        if (r == 1 && ip != hbbs_ip) {
            cdv_log("ID server %s is %u.%u.%u.%u", hbbs_host, ip >> 24, ip >> 16 & 255,
                    ip >> 8 & 255, ip & 255);
            hbbs_ip = ip;
        } else if (r == 0 && !hbbs_ip) {
            cdv_log("cannot resolve %s; trying again in 30 s", hbbs_host);
        }
    }
    /* Not yet known, or no answer from it for a while (a server on a home
     * line changes address): look it up (again). */
    if (!look_thread && (int32_t)(now - resolve_at) >= 0 &&
        (!hbbs_ip || (int32_t)(now - rdv.answered_at) > 90000)) {
        resolve_at = now + (hbbs_ip ? 60000 : 30000);
        if (inet_addr(hbbs_host) != INADDR_NONE)
            resolve(hbbs_host, &hbbs_ip);
        else
            lookup_start(hbbs_host);
    }
    if (!hbbs_ip)
        return;
    if (udp_rdv == INVALID_SOCKET && (udp_rdv = listen_on(0, 1)) == INVALID_SOCKET)
        return;
    n = rdv_tick(&rdv, now, buf, sizeof buf);
    if (n) {
        sa_set(&sa, hbbs_ip, hbbs_port);
        sendto(udp_rdv, (const char *)buf, (int)n, 0, (struct sockaddr *)&sa, sizeof sa);
    }
    /* registered goes stale once hbbs stops answering */
    if (was_registered && (int32_t)(now - rdv.answered_at) > 60000) {
        was_registered = 0;
        status_dirty = 1;
        cdv_log("the ID server has stopped answering");
    }
}

/* Where to reach the peer through hbbr, told to hbbs, then joined. */
static void rdv_relay(rdv_action *act)
{
    uint8_t buf[1024];
    char rhost[128];
    uint16_t rport;
    uint32_t rip;
    size_t n;
    SOCKET t, fd;
    cdv_log("relay request (%s) from %u.%u.%u.%u via %s", act->initiate ? "punch hole" : "relay",
            act->peer_ip >> 24, act->peer_ip >> 16 & 255, act->peer_ip >> 8 & 255,
            act->peer_ip & 255, act->relay);
    if (!slots_free()) {
        cdv_log("both sessions are in use; ignoring it");
        return;
    }
    t = tcp_connect(hbbs_ip, hbbs_port, 5000);
    if (t != INVALID_SOCKET) {
        n = rdv_relay_response(&rdv, act, buf, sizeof buf);
        send_all(t, buf, n);
        closesocket(t);
    }
    rdv_split_host(act->relay, rhost, sizeof rhost, &rport, RELAY_PORT);
    if (!resolve(rhost, &rip)) {
        cdv_log("cannot resolve the relay %s", rhost);
        return;
    }
    fd = tcp_connect(rip, rport, 8000);
    if (fd == INVALID_SOCKET) {
        cdv_log("cannot reach the relay %s", act->relay);
        return;
    }
    n = rdv_request_relay(&rdv, act, buf, sizeof buf);
    if (send_all(fd, buf, n) < 0) {
        closesocket(fd);
        return;
    }
    session_begin(fd, 1, "relayed");
}

/* The peer is on our network: listen, tell hbbs where, wait for it. */
static void rdv_local(rdv_action *act)
{
    struct sockaddr_in la, me;
    int ll = sizeof la, ml = sizeof me;
    SOCKET l, t;
    uint8_t buf[512];
    size_t n;
    fd_set rd;
    struct timeval tv;
    if (!slots_free())
        return;
    l = listen_on(0, 0);
    if (l == INVALID_SOCKET)
        return;
    getsockname(l, (struct sockaddr *)&la, &ll);
    t = tcp_connect(hbbs_ip, hbbs_port, 5000);
    if (t == INVALID_SOCKET) {
        closesocket(l);
        return;
    }
    getsockname(t, (struct sockaddr *)&me, &ml);
    n = rdv_local_addr(&rdv, act, (uint32_t)ntohl(me.sin_addr.s_addr), ntohs(la.sin_port), buf,
                       sizeof buf);
    send_all(t, buf, n);
    closesocket(t);
    cdv_log("told the ID server we are at port %u; waiting for the peer", ntohs(la.sin_port));
    FD_ZERO(&rd);
    FD_SET(l, &rd);
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    if (select(0, &rd, NULL, NULL, &tv) > 0) {
        SOCKET fd = accept(l, NULL, NULL);
        if (fd != INVALID_SOCKET)
            session_begin(fd, 1, "local");
    } else {
        cdv_log("the peer never came");
    }
    closesocket(l);
}

static void rdv_input_step(void)
{
    uint8_t buf[2048], rep[512];
    size_t replen;
    rdv_action act;
    struct sockaddr_in sa;
    int r = recv(udp_rdv, (char *)buf, sizeof buf, 0), kind;
    if (r <= 0)
        return;
    rng_add(buf, r < 16 ? (size_t)r : 16);
    kind = rdv_input(&rdv, buf, (size_t)r, now_ms(), &act, rep, sizeof rep, &replen);
    if (replen) {
        sa_set(&sa, hbbs_ip, hbbs_port);
        sendto(udp_rdv, (const char *)rep, (int)replen, 0, (struct sockaddr *)&sa, sizeof sa);
    }
    if (kind == RDV_REGISTERED) {
        if (!was_registered)
            cdv_log("registered: reachable as ID %s", prefs.id);
        was_registered = 1;
        status_dirty = 1;
    } else if (kind == RDV_REFUSED) {
        cdv_log("the ID server refused the registration (%d)", act.refuse_code);
        status_dirty = 1;
    } else if (kind == RDV_RELAY) {
        rdv_relay(&act);
    } else if (kind == RDV_LOCAL) {
        rdv_local(&act);
    }
}

static void lan_step(void)
{
    uint8_t buf[1024], out[1024];
    struct sockaddr_in from;
    int fl = sizeof from;
    int r = recvfrom(udp_lan, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    size_t n;
    if (r <= 0)
        return;
    n = lan_answer(buf, (size_t)r, prefs.id, prefs.id, hostname, out, sizeof out);
    if (n)
        sendto(udp_lan, (const char *)out, (int)n, 0, (struct sockaddr *)&from, sizeof from);
}

/* ---- what the settings window shows --------------------------------------------- */

static void status_step(void)
{
    char text[1024], line[256];
    int i;
    if (!status_dirty)
        return;
    status_dirty = 0;
    snprintf(text, sizeof text,
             "version=%s\r\nid=%s\r\nserver=%s\r\nregistered=%d\r\nrefused=%d\r\ndesktop=%s\r\n"
             "direct=%d\r\n",
             CDV_BUILD, prefs.id, prefs.server, was_registered, rdv.refused, desk_name,
             tcp_direct != INVALID_SOCKET ? prefs.port : 0);
    for (i = 0; i < NSLOTS; i++) {
        slot *sl = &slots[i];
        if (sl->fd == INVALID_SOCKET || sl->s.state != CDV_LIVE)
            continue;
        snprintf(line, sizeof line, "session=%s %s (%s)\r\n", kind_name(sl->kind),
                 sl->s.peer_name, sl->how);
        strncat(text, line, sizeof text - strlen(text) - 1);
    }
    prefs_status_write(text);
    link_send(LINK_STATUS, text, strlen(text));
}

/* What the tray sent: chat for the peer, Disconnect. */
static void link_step(void)
{
    static char m[LINK_MAX];
    int type, i;
    long n;
    /* a terminal's output waits in the link until its queue has room */
    while ((!term || cdv_ctl_room(&term->s) > LINK_MAX + 256) &&
           (n = link_take(&type, m, sizeof m)) >= 0) {
        if (type == LINK_TERM_READY) {
            unsigned long pid = strtoul(m, NULL, 10);
            const char *sp = strchr(m, ' ');
            if (term)
                cdv_term_opened(&term->s, term_id, pid != 0,
                                pid ? "Terminal opened" : "cmd would not start", pid, "cdv-term",
                                sp && sp[1] == '1');
        } else if (type == LINK_TERM_OUT) {
            if (term)
                cdv_term_data(&term->s, term_id, m, (size_t)n);
        } else if (type == LINK_JOINED) {
            status_dirty = 1; /* it wants to know who is connected */
        } else if (type == LINK_CHAT) {
            if (desk && desk->s.state == CDV_LIVE) {
                cdv_send_chat(&desk->s, m, (size_t)n);
            } else {
                static const char none[] = "Not sent: nobody is connected.";
                link_send(LINK_NOTE, none, sizeof none - 1);
            }
        } else if (type == LINK_DISCONNECT) {
            cdv_log("the user at the console ended the remote sessions");
            for (i = 0; i < NSLOTS; i++)
                session_end(&slots[i], "ended from the console");
        }
    }
}

/* ---- main ------------------------------------------------------------------ */

int agent_run(HANDLE stop, int console, int verbose)
{
    WSADATA wd;
    char logpath[MAX_PATH];
    report_config rc;
    int i, reporting = 0, loaded;
    uint32_t pass_at = 0, idle_report_at = 0;
    DWORD hl = sizeof hostname;

    /* the settings first: they say where the log goes */
    loaded = prefs_load(1);
    prefs_log_path(logpath, sizeof logpath);
    cdv_log_open(logpath, console, verbose);
    cdv_log("%s starting", VERSION_STRING);
    if (prefs.logdir[0] && _strnicmp(logpath, prefs.logdir, strlen(prefs.logdir)))
        cdv_log("the log folder %s is not there; logging beside the program", prefs.logdir);
    if (WSAStartup(MAKEWORD(1, 1), &wd) != 0) {
        cdv_log("no WinSock");
        return 1;
    }
    if (!loaded)
        cdv_log("cannot save the settings in %s", prefs_dir());
    prefs_gather_entropy();
    crypto_sign_ed25519_seed_keypair(sign_pk, sign_sk, prefs.seed);
    if (prefs.name[0])
        copy(hostname, prefs.name, sizeof hostname);
    else if (!GetComputerName(hostname, &hl) || !hostname[0])
        copy(hostname, "Windows NT", sizeof hostname);

    /* The interactive window station, whatever the service was given. */
    if (plat_nt()) {
        HWINSTA ws = OpenWindowStation("WinSta0", FALSE, MAXIMUM_ALLOWED);
        if (ws && !SetProcessWindowStation(ws))
            cdv_log("cannot use the interactive window station (error %lu)", GetLastError());
    }
    screen_follow(&desk_name);
    if (!open_screen())
        cdv_log("cannot read the screen yet; trying again at the first session");
    clip_start();

    vidbuf = (uint8_t *)malloc(VID_CAP);
    for (i = 0; i < NSLOTS; i++) {
        slots[i].fd = INVALID_SOCKET;
        slots[i].fwd = INVALID_SOCKET;
        slots[i].ctl = (uint8_t *)malloc(CTL_CAP);
        slots[i].in = (uint8_t *)malloc(IN_CAP);
        slots[i].work = (uint8_t *)malloc(WORK_CAP);
        slots[i].files = (cdv_files *)malloc(cdv_files_size());
        if (!slots[i].ctl || !slots[i].in || !slots[i].work || !slots[i].files)
            vidbuf = NULL;
    }
    if (!vidbuf) {
        cdv_log("out of memory");
        return 1;
    }
    if (strcmp(prefs.files, "off"))
        files_fs = fs_win32(prefs.files);
    if (prefs.hook)
        hooks_unpack(); /* now, while no program holds the DLL */

    ident.id = prefs.id;
    ident.sign_sk = sign_sk;
    ident.password = prefs.password;
    ident.salt = "cdeskvint";
    ident.hostname = hostname;
    ident.width = scr_ok ? scr.width : 640; /* set from the screen at each desktop login */
    ident.height = scr_ok ? scr.height : 480;
    ident.cursor_embedded = 0;
    ident.platform = "Windows";
    ident.username = "SYSTEM";
    /* Ctrl-Alt-Del reaches Winlogon only from SYSTEM, which the NT service
     * is (input_sas); 9x has none to send, and -run is an Administrator */
    ident.sas_enabled = plat_nt() && !console;
    ident.has_terminal = plat_nt() && prefs.terminal;

    if (prefs.port) {
        tcp_direct = listen_on((uint16_t)prefs.port, 0);
        if (tcp_direct == INVALID_SOCKET)
            cdv_log("cannot listen on port %d (is another copy running?)", prefs.port);
    }
    if (prefs.lan)
        udp_lan = listen_on(LAN_PORT, 1);
    rdv_setup();
    if (tcp_direct != INVALID_SOCKET)
        cdv_log("ID %s; direct access on port %d; ID server %s", prefs.id, prefs.port,
                prefs.server[0] ? prefs.server : "(none)");
    else
        cdv_log("ID %s; direct access off; ID server %s", prefs.id,
                prefs.server[0] ? prefs.server : "(none)");
    if (prefs.api[0]) {
        rc.url = prefs.api;
        rc.id = prefs.id;
        rc.uuid = prefs.uuid;
        rc.secret = prefs.seed;
        rc.hostname = hostname;
        rc.username = console_user;
        cdv_log("console: reporting to %s", prefs.api);
        reporting = report_start(&rc);
    }
    resolve_at = desk_check_at = now_ms();
    if (plat_nt() && !link_start())
        cdv_log("no tray link: chat goes nowhere");

    while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
        fd_set rd, wr;
        struct timeval tv;
        int timeout = 100, k, nfds = 0;
        size_t n;

        /* how long the last pass held off the peer's input, not counting
         * its wait in select() */
        if (pass_at && now_ms() - pass_at > S.pass_max)
            S.pass_max = now_ms() - pass_at;
        if (!desk && now_ms() - idle_report_at >= 60000) {
            int pct = cpu_percent();
            idle_report_at = now_ms();
            if (pct >= 0)
                cdv_log("no session: cpu %d%%", pct);
        }
        rdv_step();
        desk_step();
        console_sid_step();
        link_step();
        status_step();
        if (desk && desk->s.state == CDV_LIVE)
            timeout = S.encoding ? 0 : 10;

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        if (tcp_direct != INVALID_SOCKET) {
            FD_SET(tcp_direct, &rd);
            nfds++;
        }
        if (udp_rdv != INVALID_SOCKET) {
            FD_SET(udp_rdv, &rd);
            nfds++;
        }
        if (udp_lan != INVALID_SOCKET) {
            FD_SET(udp_lan, &rd);
            nfds++;
        }
        for (k = 0; k < NSLOTS; k++) {
            if (slots[k].fd == INVALID_SOCKET)
                continue;
            FD_SET(slots[k].fd, &rd);
            if (cdv_out_peek(&slots[k].s, &n), n)
                FD_SET(slots[k].fd, &wr);
            /* a download keeps the file manager busy; a terminal and a tunnel
             * are answered at once */
            if (slots[k].kind >= K_FILES && timeout > 10)
                timeout = 10;
            /* a tunnel's far end, while there is room for what it sends */
            if (slots[k].fwd != INVALID_SOCKET && cdv_ctl_room(&slots[k].s) > 16384)
                FD_SET(slots[k].fwd, &rd);
            nfds++;
        }
        tv.tv_sec = 0;
        tv.tv_usec = timeout * 1000;
        if (!nfds) {
            /* WinSock's select() refuses an empty set */
            WaitForSingleObject(stop, (DWORD)timeout);
            continue;
        }
        pass_at = 0;
        if (select(0, &rd, &wr, NULL, &tv) < 0) {
            cdv_log("select failed (%d)", WSAGetLastError());
            Sleep(1000);
            continue;
        }
        pass_at = now_ms();
        if (tcp_direct != INVALID_SOCKET && FD_ISSET(tcp_direct, &rd)) {
            SOCKET fd = accept(tcp_direct, NULL, NULL);
            if (fd != INVALID_SOCKET)
                session_begin(fd, 0, "direct");
        }
        if (udp_rdv != INVALID_SOCKET && FD_ISSET(udp_rdv, &rd))
            rdv_input_step();
        if (udp_lan != INVALID_SOCKET && FD_ISSET(udp_lan, &rd))
            lan_step();
        for (k = 0; k < NSLOTS; k++) {
            slot *sl = &slots[k];
            int readable;
            if (sl->fd == INVALID_SOCKET)
                continue;
            readable = FD_ISSET(sl->fd, &rd) != 0;
            if (sl->fwd != INVALID_SOCKET && FD_ISSET(sl->fwd, &rd)) {
                static char fbuf[16384];
                int r = recv(sl->fwd, fbuf, sizeof fbuf, 0);
                if (r > 0)
                    cdv_tunnel_send(&sl->s, fbuf, (size_t)r);
                else
                    sl->fwd_dead = 1;
            }
            if (!session_io(sl, readable))
                continue;
            if (sl->fwd_dead) {
                session_io(sl, 0); /* what it said last */
                session_end(sl, "the tunnel's far end closed");
                continue;
            }
            if (sl == desk && sl->s.state == CDV_LIVE) {
                /* the screen is background work: the programs here and
                 * their mouse come first. Back up for the peer's input. */
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                clip_step();
                screenshot_step();
                frame_step();
                cursor_step();
                session_report();
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
            }
            if (sl->fd != INVALID_SOCKET)
                session_io(sl, 0);
        }
    }
    for (i = 0; i < NSLOTS; i++)
        session_end(&slots[i], "the agent is stopping");
    if (reporting)
        report_stop();
    link_stop();
    hooks_stop();
    prefs_status_write("stopped=1\r\n");
    cdv_log("stopped");
    WSACleanup();
    return 0;
}
