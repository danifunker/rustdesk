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
#include "engine.h"
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

/* ---- the agent: set up here, run by the engine ---------------------------- */

static cdv_net net;
static cdv_screen scr;
static cdv_session sess;
static cdv_ident ident;
static char hostname[64];
static vp8e *enc;
static void *encmem;
static uint8_t *outq, *inq;
static size_t outcap;

#define INQ_SIZE (64 * 1024L)

static void hook_mouse(void *u, int mask, int x, int y)
{
    (void)u;
    input_mouse(mask, x, y);
}

static void hook_key(void *u, const cdv_key *k)
{
    (void)u;
    input_key(k);
}

static void hook_log(void *u, const char *m)
{
    (void)u;
    engine_log(m); /* the engine's context: no drawing from here */
}

static const cdv_hooks hooks = { hook_mouse, hook_key, NULL, hook_log, NULL };

/* Room for a keyframe of a busy screen: about a byte a pixel at q 16. */
static size_t queue_size(const cdv_screen *s)
{
    return (size_t)s->width * s->height + 64 * 1024;
}

static OSErr open_video(void)
{
    OSErr err = screen_open(&scr);
    if (err != noErr)
        return err;
    encmem = NewPtr((long)vp8e_mem_size(scr.width, scr.height));
    if (!encmem)
        return memFullErr;
    enc = vp8e_init(encmem, scr.width, scr.height, prefs.q);
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
    encmem = NULL;
    enc = NULL;
}

/* The display changed size or depth. The engine stops touching the screen
 * and encoder, they are rebuilt, and it carries on and tells the peer. */
static void screen_changed(void)
{
    unsigned long t0 = TickCount();
    char line[80];
    eng.suspend_req = 1;
    while (!eng.suspended && TickCount() - t0 < 60)
        ;
    close_video();
    if (open_video() != noErr) {
        say("the screen changed and there is not enough memory for it");
        return; /* stays suspended */
    }
    if (queue_size(&scr) > outcap) {
        /* A bigger picture than the queue was sized for: drop the peer,
         * which will reconnect to a fresh one. */
        eng.need_reset = 1;
        while (!net_send_idle(&net))
            ;
        DisposePtr((Ptr)outq);
        outcap = queue_size(&scr);
        outq = (uint8_t *)NewPtr((long)outcap);
    }
    engine_replace_video(enc, outq, outcap);
    snprintf(line, sizeof line, "screen now %dx%d, %d bits", scr.width, scr.height, scr.depth);
    say(line);
    eng.announce = 1;
    eng.suspend_req = 0;
}

/* Main-loop chores: everything the engine may not do itself. */
static void chores(void)
{
    static unsigned long last_status, last_kchr, frames0, bytes0;
    unsigned long now = TickCount();
    const char *m;
    char line[80];

    while ((m = engine_next_log()) != NULL)
        say(m);

    if (eng.need_reset) {
        net_reset(&net);
        eng.need_reset = 0;
        set_status("Waiting for a connection");
    }
    if (now - last_kchr > 60) {
        input_set_kchr((Ptr)GetScriptManagerVariable(smKCHRCache));
        last_kchr = now;
    }
    if (!eng.suspend_req) {
        if (screen_geometry_changed(&scr))
            screen_changed();
        else
            screen_check_palette(&scr);
    }
    if (now - last_status >= 120) {
        if (eng.live) {
            snprintf(line, sizeof line, "Live: %lu frames, %lu KB in 2s", eng.frames - frames0,
                     (eng.bytes - bytes0) / 1024);
            set_status(line);
        } else if (net.state == NET_CONNECTED) {
            set_status("Logging in...");
        } else {
            set_status("Waiting for a connection");
        }
        frames0 = eng.frames;
        bytes0 = eng.bytes;
        last_status = now;
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
        if (part == inMenuBar)
            menu_choice(MenuSelect(ev->where));
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

    inq = (uint8_t *)NewPtr(INQ_SIZE);
    err = inq ? open_video() : memFullErr;
    if (err == noErr) {
        outcap = queue_size(&scr);
        outq = (uint8_t *)NewPtr((long)outcap);
        if (!outq)
            err = memFullErr;
    }
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
        engine_ctx ctx;
        char a[20];
        input_set_kchr((Ptr)GetScriptManagerVariable(smKCHRCache));
        memset(&ctx, 0, sizeof ctx);
        ctx.net = &net;
        ctx.scr = &scr;
        ctx.sess = &sess;
        ctx.hooks = &hooks;
        ctx.ident = &ident;
        ctx.enc = enc;
        ctx.outq = outq;
        ctx.outcap = outcap;
        ctx.inq = inq;
        ctx.incap = INQ_SIZE;
        ctx.q = prefs.q;
        engine_setup(&ctx);
        engine_start();
        net_addr_string(net.local, a);
        snprintf(addr_line, sizeof addr_line, "Connect to %s  (port %u)", a, prefs.port);
        set_status("Waiting for a connection");
    } else {
        strcpy(addr_line, "Not running");
    }
    redraw();

    while (!quit) {
        EventRecord ev;
        if (WaitNextEvent(everyEvent, &ev, 6, NULL))
            handle_event(&ev);
        if (err == noErr)
            chores();
    }
    if (err == noErr) {
        engine_stop();
        input_release_all();
        /* MacTCP owns the stream's buffer until it is released; quitting
         * without this leaves it writing into a heap that no longer exists. */
        net_shutdown(&net);
    }
    return 0;
}
