/* C-Desk-Vint: a RustDesk agent for classic Mac OS.
 *
 * One small status window and a main loop that polls everything: MacTCP, the
 * session, the screen and the encoder. It runs in the background, so the Mac
 * stays usable at the console while a peer watches or drives it.
 *
 * Settings live in "C-Desk-Vint Prefs" in the Preferences folder, a text file
 * of key=value lines (password, port, quality). The first run writes one with
 * a random password, which the window shows.
 */
#include "input.h"
#include "net.h"
#include "screen.h"
#include "../core/session.h"
#include "../core/vp8enc.h"

#include <Multiverse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "C-Desk-Vint"
#define PREFS_NAME "\pC-Desk-Vint Prefs"
#define DEFAULT_PORT 21118
#define DEFAULT_Q 16
#define SCAN_MS 60           /* at most this often when frames are flowing */
#define IDLE_REFRESH_MS 400  /* a refresh band this often on a still screen */
#define LOG_LINES 9

/* What Multiversal does not declare. */
#define kOnSystemDisk ((short)0x8000)
pascal OSErr CDV_FSpCreate(const FSSpec *spec, OSType creator, OSType type, short script)
    M68K_INLINE(0x7004, 0xAA52);

static struct {
    char password[33];
    unsigned short port;
    int q;
} prefs;

static WindowPtr win;
static char loglines[LOG_LINES][80];
static int nlog;
static char status[80], addr_line[80];
static int quit;

/* ---- the window ------------------------------------------------------------ */

static void draw_c(const char *s)
{
    Str255 p;
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
    DrawString(p);
}

static void redraw(void)
{
    GrafPtr old;
    Rect r;
    int i;
    char line[120];
    if (!win)
        return;
    GetPort(&old);
    SetPort(win);
    r = win->portRect;
    EraseRect(&r);
    TextFont(1); /* application font: Geneva */
    TextSize(9);
    MoveTo(8, 14);
    TextFace(1); /* bold */
    draw_c(addr_line);
    TextFace(0);
    MoveTo(8, 27);
    snprintf(line, sizeof line, "Password: %s", prefs.password);
    draw_c(line);
    MoveTo(8, 40);
    draw_c(status);
    MoveTo(0, 46);
    LineTo(r.right, 46);
    for (i = 0; i < nlog; i++) {
        MoveTo(8, 58 + 11 * i);
        draw_c(loglines[i]);
    }
    SetPort(old);
}

static void invalidate(void)
{
    GrafPtr old;
    if (!win)
        return;
    GetPort(&old);
    SetPort(win);
    InvalRect(&win->portRect);
    SetPort(old);
}

static void say(const char *msg)
{
    if (nlog == LOG_LINES) {
        memmove(loglines[0], loglines[1], sizeof loglines[0] * (LOG_LINES - 1));
        nlog--;
    }
    strncpy(loglines[nlog], msg, sizeof loglines[0] - 1);
    loglines[nlog][sizeof loglines[0] - 1] = 0;
    nlog++;
    invalidate();
}

static void set_status(const char *s)
{
    if (strcmp(s, status)) {
        strncpy(status, s, sizeof status - 1);
        invalidate();
    }
}

/* ---- prefs ------------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    return (uint32_t)(TickCount() * 50UL / 3UL);
}

static void parse_prefs(char *text)
{
    char *line = text;
    while (line && *line) {
        char *next = strpbrk(line, "\r\n"), *eq;
        if (next)
            *next++ = 0;
        eq = strchr(line, '=');
        if (eq) {
            *eq++ = 0;
            if (!strcmp(line, "password"))
                strncpy(prefs.password, eq, sizeof prefs.password - 1);
            else if (!strcmp(line, "port"))
                prefs.port = (unsigned short)atoi(eq);
            else if (!strcmp(line, "quality"))
                prefs.q = atoi(eq);
        }
        line = next;
    }
}

/* The prefs file is opened with PBHOpenDF, which hands back the refnum. */
static int read_prefs_file(char *buf, long cap, long *len)
{
    short vref;
    long dir;
    HParamBlockRec pb;
    OSErr err;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, 1, &vref, &dir) != noErr)
        return 0;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)PREFS_NAME;
    pb.ioParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    pb.ioParam.ioPermssn = fsRdPerm;
    if (PBHOpenDFSync(&pb) != noErr)
        return 0;
    *len = cap;
    err = FSRead(pb.ioParam.ioRefNum, len, buf);
    FSClose(pb.ioParam.ioRefNum);
    return err == noErr || err == eofErr;
}

static void write_prefs_file(void)
{
    short vref;
    long dir, len;
    FSSpec spec;
    HParamBlockRec pb;
    char text[200];
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, 1, &vref, &dir) != noErr)
        return;
    FSMakeFSSpec(vref, dir, PREFS_NAME, &spec);
    CDV_FSpCreate(&spec, 'ttxt', 'TEXT', 0);
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)PREFS_NAME;
    pb.ioParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    pb.ioParam.ioPermssn = fsWrPerm;
    if (PBHOpenDFSync(&pb) != noErr)
        return;
    len = snprintf(text, sizeof text, "password=%s\rport=%u\rquality=%d\r", prefs.password,
                   prefs.port, prefs.q);
    FSWrite(pb.ioParam.ioRefNum, &len, text);
    SetEOF(pb.ioParam.ioRefNum, len);
    FSClose(pb.ioParam.ioRefNum);
}

static void init_prefs(void)
{
    static char buf[1024];
    long len = 0;
    prefs.port = DEFAULT_PORT;
    prefs.q = DEFAULT_Q;
    prefs.password[0] = 0;
    if (read_prefs_file(buf, sizeof buf - 1, &len)) {
        buf[len] = 0;
        parse_prefs(buf);
    }
    if (!prefs.password[0]) {
        /* First run: a password nobody chose, shown in the window. */
        static const char a[] = "abcdefghjkmnpqrstuvwxyz23456789";
        unsigned long s = TickCount() * 2654435761UL ^ (unsigned long)qd.randSeed;
        int i;
        for (i = 0; i < 8; i++) {
            s = s * 1103515245UL + 12345UL;
            prefs.password[i] = a[(s >> 16) % (sizeof a - 1)];
        }
        prefs.password[8] = 0;
        write_prefs_file();
    }
    if (prefs.q < 0 || prefs.q > 127)
        prefs.q = DEFAULT_Q;
    if (!prefs.port)
        prefs.port = DEFAULT_PORT;
}

/* ---- the agent ---------------------------------------------------------------- */

static cdv_net net;
static cdv_screen scr;
static cdv_session sess;
static cdv_ident ident;
static char hostname[64];
static vp8e *enc;
static void *encmem;
static uint8_t *outq, *inq;
static size_t outcap;
static int q_now;

extern int input_last_post_err;

static void hook_mouse(void *u, int mask, int x, int y)
{
    (void)u;
    input_mouse(mask, x, y);
    if ((mask & 7) == 1 || (mask & 7) == 2) {
        char line[80];
        snprintf(line, sizeof line, "mouse %s at %d,%d: posted %d, MBState %02x",
                 (mask & 7) == 1 ? "down" : "up", x, y, input_last_post_err,
                 *(volatile unsigned char *)0x0172);
        say(line);
    }
}

static void hook_key(void *u, const cdv_key *k)
{
    (void)u;
    input_key(k);
}

static void hook_log(void *u, const char *m)
{
    (void)u;
    say(m);
}

static const cdv_hooks hooks = { hook_mouse, hook_key, NULL, hook_log, NULL };

static OSErr open_video(void)
{
    OSErr err = screen_open(&scr);
    if (err != noErr)
        return err;
    encmem = NewPtr((long)vp8e_mem_size(scr.width, scr.height));
    /* Room for a keyframe of a busy screen: about a byte a pixel at q 16. */
    outcap = (size_t)scr.width * scr.height + 64 * 1024;
    outq = (uint8_t *)NewPtr((long)outcap);
    if (!encmem || !outq)
        return memFullErr;
    q_now = prefs.q;
    enc = vp8e_init(encmem, scr.width, scr.height, q_now);
    ident.width = scr.width;
    ident.height = scr.height;
    input_init(scr.width, scr.height);
    return enc ? noErr : paramErr;
}

static void close_video(void)
{
    screen_close(&scr);
    if (encmem)
        DisposePtr((Ptr)encmem);
    if (outq)
        DisposePtr((Ptr)outq);
    encmem = NULL;
    outq = NULL;
    enc = NULL;
}

static void new_session(void)
{
    cdv_init(&sess, outq, outcap, inq, 64 * 1024, &hooks, &ident, TickCount());
}

static void send_frame(void)
{
    static uint32_t last_scan, last_refresh, frames, bytes, last_report;
    uint32_t now = now_ms();
    int key, all, idle_refresh, n;
    size_t cap, len;
    uint8_t *dst;
    char line[80];

    if (now - last_scan < SCAN_MS)
        return;
    last_scan = now;

    if (screen_geometry_changed(&scr)) {
        close_video();
        if (open_video() != noErr) {
            cdv_close(&sess, "The screen changed and there is not enough memory for it");
            return;
        }
        cdv_send_display(&sess, scr.width, scr.height);
        sess.refresh = 1;
        say("screen changed size or depth");
    }
    /* Only scan when the frame can go straight out: a scan moves the shadow
     * copy forward, so changes it found and could not send would be lost. */
    dst = cdv_video_begin(&sess, &cap);
    if (!dst)
        return;
    key = cdv_take_refresh(&sess);
    if (screen_palette_changed(&scr))
        key = 1;
    all = key;
    idle_refresh = now - last_refresh >= IDLE_REFRESH_MS;
    n = screen_scan(&scr, idle_refresh, all);
    if (idle_refresh)
        last_refresh = now;
    if (!n)
        return;
    {
        vp8e_src src;
        vp8e_stats st;
        src.y = scr.Y;
        src.u = scr.U;
        src.v = scr.V;
        src.ystride = scr.ystride;
        src.uvstride = scr.uvstride;
        len = vp8e_encode(enc, &src, key ? NULL : scr.dirty, key, dst, cap);
        vp8e_last_stats(enc, &st);
        key = st.key;
    }
    if (!len) {
        /* Did not fit. Coarser, and a keyframe, next time. */
        q_now = q_now + 16 > 127 ? 127 : q_now + 16;
        vp8e_set_q(enc, q_now);
        sess.refresh = 1;
        say("frame too large; lowering quality");
        return;
    }
    cdv_video_commit(&sess, len, key);
    if (key && q_now != prefs.q) {
        q_now = prefs.q;
        vp8e_set_q(enc, q_now);
    }
    frames++;
    bytes += (uint32_t)len;
    if (now - last_report >= 2000) {
        snprintf(line, sizeof line, "Live: %lu frames, %lu KB in %lus", (unsigned long)frames,
                 (unsigned long)(bytes / 1024), (unsigned long)((now - last_report) / 1000));
        set_status(line);
        frames = bytes = 0;
        last_report = now;
    }
}

static void service(void)
{
    const uint8_t *data;
    size_t len;
    char line[80];

    if (net_accepted(&net)) {
        char a[20];
        net_addr_string(net.remote, a);
        snprintf(line, sizeof line, "connection from %s", a);
        say(line);
        set_status("Logging in...");
        new_session();
        cdv_start(&sess, now_ms());
    }
    if (net.state != NET_CONNECTED)
        return;

    while (net_recv(&net, &data, &len))
        cdv_feed(&sess, data, len);
    {
        size_t sent = net_sent(&net);
        if (sent)
            cdv_out_consume(&sess, sent);
    }
    cdv_tick(&sess, now_ms());
    if (sess.state == CDV_LIVE)
        send_frame();
    if (net_send_idle(&net)) {
        const uint8_t *o = cdv_out_peek(&sess, &len);
        if (len)
            net_send(&net, o, len);
    }

    if (net_failed(&net) || (sess.state == CDV_CLOSED && !cdv_out_peek(&sess, &len) &&
                             net_send_idle(&net))) {
        snprintf(line, sizeof line, "session ended (%d)", net.err);
        say(line);
        input_release_all();
        net_reset(&net);
        set_status("Waiting for a connection");
    }
}

/* ---- setup and events -------------------------------------------------------- */

static void make_menus(void)
{
    MenuHandle apple = NewMenu(128, "\p\024"), file = NewMenu(129, "\pFile");
    AppendMenu(apple, "\pAbout C-Desk-Vint...");
    AppendMenu(apple, "\p(-");
    AppendResMenu(apple, 'DRVR');
    AppendMenu(file, "\pQuit/Q");
    InsertMenu(apple, 0);
    InsertMenu(file, 0);
    DrawMenuBar();
}

static void menu_choice(long choice)
{
    short menu = (short)(choice >> 16), item = (short)(choice & 0xFFFF);
    if (menu == 128 && item > 2) {
        Str255 name;
        GetMenuItemText(GetMenuHandle(128), item, name);
        OpenDeskAcc(name);
    } else if (menu == 128 && item == 1) {
        say(APP_NAME " -- RustDesk for classic Mac OS");
    } else if (menu == 129 && item == 1) {
        quit = 1;
    }
    HiliteMenu(0);
}

static void handle_event(EventRecord *ev)
{
    WindowPtr w;
    short part;
    switch (ev->what) {
    case mouseDown:
        part = FindWindow(ev->where, &w);
        {
            char line[80];
            snprintf(line, sizeof line, "event: mouseDown at %d,%d part %d mods %04x", ev->where.h,
                     ev->where.v, part, ev->modifiers);
            say(line);
        }
        if (part == inMenuBar) {
            long c = MenuSelect(ev->where);
            char line[80];
            snprintf(line, sizeof line, "MenuSelect -> %08lx, Button %d", c, Button());
            say(line);
            menu_choice(c);
        }
        else if (part == inSysWindow)
            SystemClick(ev, w);
        else if (part == inDrag && w == win) {
            Rect bounds = qd.screenBits.bounds;
            DragWindow(w, ev->where, &bounds);
        } else if (part == inContent && w != FrontWindow())
            SelectWindow(w);
        break;
    case keyDown:
        if (ev->modifiers & cmdKey)
            menu_choice(MenuKey((char)(ev->message & charCodeMask)));
        break;
    case updateEvt:
        if ((WindowPtr)ev->message == win) {
            BeginUpdate(win);
            redraw();
            EndUpdate(win);
        }
        break;
    default:
        break;
    }
}

int main(void)
{
    Rect r;
    OSErr err;
    char line[80];

    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    make_menus();

    r = qd.screenBits.bounds;
    SetRect(&r, r.right - 330, r.bottom - 170, r.right - 10, r.bottom - 10);
    win = NewWindow(NULL, &r, "\pC-Desk-Vint", 1, noGrowDocProc, (WindowPtr)-1, 0, 0);

    init_prefs();
    strcpy(status, "Starting");
    strcpy(hostname, "Macintosh");
    ident.password = prefs.password;
    ident.salt = "cdeskvint";
    ident.hostname = hostname;
    ident.cursor_embedded = 1;

    inq = (uint8_t *)NewPtr(64 * 1024);
    err = inq ? open_video() : memFullErr;
    if (err != noErr) {
        snprintf(line, sizeof line, "not enough memory for this screen (%d)", err);
        say(line);
    } else {
        snprintf(line, sizeof line, "screen %dx%d, %d bits", scr.width, scr.height, scr.depth);
        say(line);
        err = net_init(&net, prefs.port);
        if (err != noErr) {
            snprintf(line, sizeof line, "MacTCP did not open (%d)", err);
            say(line);
        }
    }
    if (err == noErr) {
        char a[20];
        net_addr_string(net.local, a);
        snprintf(addr_line, sizeof addr_line, "Connect to %s  (port %u)", a, prefs.port);
        set_status("Waiting for a connection");
    } else {
        strcpy(addr_line, "Not running");
    }
    redraw();

    while (!quit) {
        EventRecord ev;
        int busy = net.state == NET_CONNECTED;
        if (WaitNextEvent(everyEvent, &ev, busy ? 0 : 3, NULL))
            handle_event(&ev);
        if (err == noErr)
            service();
    }
    if (err == noErr) {
        input_release_all();
        /* MacTCP owns the stream's buffer until it is released; quitting
         * without this leaves it writing into a heap that no longer exists. */
        net_shutdown(&net);
    }
    return 0;
}
