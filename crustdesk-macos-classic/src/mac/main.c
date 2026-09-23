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
#include "cursor.h"
#include "engine.h"
#include "input.h"
#include "traps.h"
#include "net.h"
#include "screen.h"
#include "../core/macroman.h"
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

static struct {
    char password[33];
    unsigned short port;
    int q;
    int gamma; /* apply the video driver's gamma table (default on) */
    int cursor_separate; /* send the pointer's shape even if it is in VRAM */
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

/* Draw a line of text over whatever was there, without erasing first: the
 * screen is being captured, and an erase-then-draw is two changes for the
 * price of one -- worse, a change the capture can catch half-way. */
static void draw_line(int y, const char *s)
{
    Rect r;
    MoveTo(8, y);
    draw_c(s);
    SetRect(&r, qd.thePort->pnLoc.h, y - 9, win->portRect.right, y + 3);
    EraseRect(&r);
    SetRect(&r, 0, y - 9, 8, y + 3);
    EraseRect(&r);
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
    TextFont(1); /* application font: Geneva */
    TextSize(9);
    TextMode(srcCopy);
    TextFace(1); /* bold */
    draw_line(14, addr_line);
    TextFace(0);
    snprintf(line, sizeof line, "Password: %s", prefs.password);
    draw_line(27, line);
    draw_line(40, status);
    MoveTo(0, 46);
    LineTo(r.right, 46);
    for (i = 0; i < LOG_LINES; i++)
        draw_line(58 + 11 * i, i < nlog ? loglines[i] : "");
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

/* The log also goes to "C-Desk-Vint Log" in the Preferences folder, so a
 * machine nobody is watching still has a story to tell. Main loop only. */
static short logref;

static void log_open(void)
{
    short vref;
    long dir;
    FSSpec spec;
    HParamBlockRec pb;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, 1, &vref, &dir) != noErr)
        return;
    FSMakeFSSpec(vref, dir, "\pC-Desk-Vint Log", &spec);
    cdv_fsp_create(&spec, 'ttxt', 'TEXT', 0);
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr) "\pC-Desk-Vint Log";
    pb.ioParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    pb.ioParam.ioPermssn = fsWrPerm;
    if (PBHOpenDFSync(&pb) != noErr)
        return;
    logref = pb.ioParam.ioRefNum;
    SetEOF(logref, 0);
}

static void log_write(const char *msg)
{
    static unsigned long last_flush;
    char line[100];
    long len;
    unsigned long t = TickCount() / 60;
    if (!logref)
        return;
    len = snprintf(line, sizeof line, "%5lu:%02lu  %s\r", t / 60, t % 60, msg);
    if (len > (long)sizeof line - 1)
        len = sizeof line - 1;
    FSWrite(logref, &len, line);
    if (TickCount() - last_flush > 300) {
        short vref;
        if (GetVRefNum(logref, &vref) == noErr)
            FlushVol(NULL, vref);
        last_flush = TickCount();
    }
}

static void say(const char *msg)
{
    log_write(msg);
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
            else if (!strcmp(line, "gamma"))
                prefs.gamma = atoi(eq);
            else if (!strcmp(line, "cursor"))
                prefs.cursor_separate = !strcmp(eq, "separate");
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
    cdv_fsp_create(&spec, 'ttxt', 'TEXT', 0);
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
    prefs.gamma = 1;
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
    engine_peer_moved_pointer();
}

static void hook_key(void *u, const cdv_key *k)
{
    (void)u;
    input_key(k);
}

/* Deferred-task time: park the text for the main loop, which owns the scrap. */
static void hook_clipboard(void *u, const char *utf8, size_t n)
{
    (void)u;
    if (clip_in.ready)
        return; /* the last one is not in yet; the newer can wait for another copy */
    if (n > CLIP_MAX)
        n = CLIP_MAX;
    memcpy(clip_in.text, utf8, n);
    clip_in.len = n;
    clip_in.ready = 1;
}

static void hook_log(void *u, const char *m)
{
    (void)u;
    engine_log(m); /* the engine's context: no drawing from here */
}

static const cdv_hooks hooks = { hook_mouse, hook_key, hook_clipboard, hook_log, NULL };

/* Room for a keyframe of a busy screen: about a byte a pixel at q 16. */
static size_t queue_size(const cdv_screen *s)
{
    return (size_t)s->width * s->height + 64 * 1024;
}

static OSErr open_video(void)
{
    OSErr err = screen_open(&scr, prefs.gamma);
    if (err != noErr)
        return err;
    encmem = NewPtr((long)vp8e_mem_size(scr.width, scr.height));
    if (!encmem)
        return memFullErr;
    enc = vp8e_init(encmem, scr.width, scr.height, prefs.q);
    ident.width = scr.width;
    ident.height = scr.height;
    /* A hardware cursor is not in the framebuffer: send it separately. */
    ident.cursor_embedded = !prefs.cursor_separate && !cursor_is_hardware(scr.refnum);
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

/* Most System 7 programs keep a private scrap (TextEdit's) and trade it with
 * the desk scrap only when they are switched out and back in: out, they
 * export theirs; in, they import the desk's. So the agent comes to the front
 * for a moment and hands it back, and does its own part in between -- once
 * the other program has exported, before it re-imports:
 *   - text from the peer goes onto the desk scrap then, not before, or the
 *     program's export on the way out would overwrite it;
 *   - after the peer pressed Command-C, the program's export is exactly what
 *     the poll then picks up. */
static struct {
    int state; /* 0 idle, 1 coming forward, 2 give the front back */
    ProcessSerialNumber front;
    unsigned long at;
} bounce;

static uint8_t mr[CLIP_MAX];
static short last_count = -32768;

static void put_peer_text(void)
{
    size_t n = utf8_to_macroman((const uint8_t *)clip_in.text, clip_in.len, mr, sizeof mr);
    ZeroScrap();
    PutScrap((long)n, 'TEXT', (Ptr)mr);
    last_count = InfoScrap()->scrapCount;
    clip_in.ready = 0;
    say("clipboard from the peer");
}

/* Returns nonzero if a switch began; zero if we are in front already. */
static int bounce_start(void)
{
    ProcessSerialNumber me;
    Boolean same = 0;
    if (bounce.state)
        return 1;
    if (cdv_get_front_process(&bounce.front) != noErr || GetCurrentProcess(&me) != noErr)
        return 0;
    SameProcess(&bounce.front, &me, &same);
    if (same)
        return 0;
    SetFrontProcess(&me);
    bounce.state = 1;
    bounce.at = TickCount();
    return 1;
}

static void bounce_step(void)
{
    if (bounce.state == 1 && TickCount() - bounce.at > 15) {
        /* In front, and the other program has exported. */
        if (clip_in.ready)
            put_peer_text();
        bounce.state = 2;
        bounce.at = TickCount();
    } else if (bounce.state == 2 && TickCount() - bounce.at > 5) {
        SetFrontProcess(&bounce.front);
        bounce.state = 0;
    }
}

/* The Scrap Manager, both ways. A copy on the Mac goes to the peer, and so
 * does whatever is on the clipboard when a peer logs in; text from the peer
 * goes onto the scrap, and its scrapCount is then taken as ours so it does
 * not bounce straight back. */
static void clipboard_chores(void)
{
    static int was_live;
    static unsigned long last_poll, copy_at;
    int just_live = eng.live && !was_live;
    was_live = eng.live;

    if (clip_in.ready && !bounce.state && !bounce_start())
        put_peer_text(); /* already in front: nothing to trade with */
    bounce_step();
    if (input_take_copy())
        copy_at = TickCount() + 30; /* give the program time to copy */
    if (copy_at && TickCount() >= copy_at) {
        copy_at = 0;
        bounce_start();
    }

    if (!eng.live || clip_out.ready || bounce.state ||
        (!just_live && TickCount() - last_poll < 30))
        return;
    last_poll = TickCount();
    if (InfoScrap()->scrapCount == last_count && !just_live)
        return;
    last_count = InfoScrap()->scrapCount;
    {
        Handle h = NewHandle(0);
        long off = 0, len;
        if (!h)
            return;
        len = GetScrap(h, 'TEXT', &off);
        if (len > 0) {
            if (len > (long)sizeof mr)
                len = sizeof mr;
            HLock(h);
            clip_out.len = macroman_to_utf8((const uint8_t *)*h, (size_t)len,
                                            (uint8_t *)clip_out.text, CLIP_MAX);
            HUnlock(h);
            clip_out.ready = 1;
        }
        DisposeHandle(h);
    }
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
    clipboard_chores();

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
    log_open();
#if defined(__powerpc__) || defined(__ppc__)
    say(APP_NAME " 0.1d, native PowerPC");
#else
    say(APP_NAME " 0.1d, 68k");
#endif
    strcpy(status, "Starting");
    strcpy(hostname, "Macintosh");
    ident.password = prefs.password;
    ident.salt = "cdeskvint";
    ident.hostname = hostname;

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
        snprintf(line, sizeof line, "screen %dx%d, %d bits, %s cursor", scr.width, scr.height,
                 scr.depth, ident.cursor_embedded ? "software" : "hardware");
        say(line);
        if (scr.gamma_info[0])
            snprintf(line, sizeof line, "gamma from the driver (%d-channel), mid-grey %d -> %d",
                     scr.gamma_info[0], 128, scr.gamma[1][128]);
        else
            snprintf(line, sizeof line, "no gamma table from the driver (%d); colours as drawn",
                     scr.gamma_err);
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
    if (logref)
        FSClose(logref);
    return 0;
}
