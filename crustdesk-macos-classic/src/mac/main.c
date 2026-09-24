/* C-Desk-Vint: a RustDesk agent for classic Mac OS.
 *
 * One small status window and a main loop that polls everything: MacTCP, the
 * session, the screen and the encoder. It runs in the background, so the Mac
 * stays usable at the console while a peer watches or drives it.
 *
 * Settings live in "C-Desk-Vint Prefs" in the Preferences folder, a text file
 * of key=value lines (password, port, quality, the ID server...). The first
 * run writes one with a random ID, key and password, which the window shows.
 */
#include "cursor.h"
#include "engine.h"
#include "input.h"
#include "mem.h"
#include "traps.h"
#include "net.h"
#include "dnr.h"
#include "screen.h"
#include "../core/dns.h"
#include "../core/macroman.h"
#include "../core/rdv.h"
#include "../core/rng.h"
#include "../core/session.h"
#include "../core/vp8enc.h"

#include <Multiverse.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "C-Desk-Vint"
#define PREFS_NAME "\pC-Desk-Vint Prefs"
#define DEFAULT_PORT 21118
#define DEFAULT_Q 16
#define LOCAL_PORT 21120  /* for a peer hbbs sent our local address to */
/* The ID server, until the prefs say otherwise: the public one, unless the
 * build names another (cmake -DCDV_SERVER=... -DCDV_KEY=...). */
#ifndef DEFAULT_SERVER
#define DEFAULT_SERVER "rs-ny.rustdesk.com"
#define DEFAULT_KEY "OeVuKk5nlHiXp+APNn0Y3pC1Iwpwn44JGqrQCsWqmBw="
#endif
#ifndef DEFAULT_KEY
#define DEFAULT_KEY ""
#endif
#define LOG_LINES 9

/* What Multiversal does not declare. */
#define kOnSystemDisk ((short)0x8000)

static struct {
    char password[33];
    unsigned short port;
    int q;
    int gamma; /* apply the video driver's gamma table (default on) */
    int cursor_separate; /* send the pointer's shape even if it is in VRAM */
    char id[16];         /* RustDesk ID */
    uint8_t uuid[16];
    uint8_t seed[32];    /* Ed25519 seed: the key that signs our ID */
    int have_seed, have_uuid;
    char server[64];     /* hbbs "host[:port]"; "" for none */
    char key[64];        /* the server's public key */
    char relay[64];      /* hbbr override; "" to take the one hbbs names */
    char dns[20];        /* a DNS server, if the Mac's resolver is not enough */
} prefs;

static WindowPtr win;
static char loglines[LOG_LINES][80];
static int nlog;
static char status[80], addr_line[80], id_line[80];
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
    draw_line(14, id_line);
    snprintf(line, sizeof line, "Password: %s", prefs.password);
    draw_line(27, line);
    TextFace(0);
    draw_line(40, addr_line);
    draw_line(53, status);
    MoveTo(0, 59);
    LineTo(r.right, 59);
    for (i = 0; i < LOG_LINES; i++)
        draw_line(71 + 11 * i, i < nlog ? loglines[i] : "");
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

static void set_id_line(const char *s)
{
    if (strcmp(s, id_line)) {
        strncpy(id_line, s, sizeof id_line - 1);
        invalidate();
    }
}

static void set_status(const char *s)
{
    if (strcmp(s, status)) {
        strncpy(status, s, sizeof status - 1);
        invalidate();
    }
}

/* ---- prefs ------------------------------------------------------------------ */

static int unhex(const char *s, uint8_t *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

static char *hex(const uint8_t *b, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
    return out;
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
            else if (!strcmp(line, "gamma"))
                prefs.gamma = atoi(eq);
            else if (!strcmp(line, "cursor"))
                prefs.cursor_separate = !strcmp(eq, "separate");
            else if (!strcmp(line, "id"))
                strncpy(prefs.id, eq, sizeof prefs.id - 1);
            else if (!strcmp(line, "uuid"))
                prefs.have_uuid = unhex(eq, prefs.uuid, sizeof prefs.uuid);
            else if (!strcmp(line, "seed"))
                prefs.have_seed = unhex(eq, prefs.seed, sizeof prefs.seed);
            else if (!strcmp(line, "server"))
                strncpy(prefs.server, eq, sizeof prefs.server - 1);
            else if (!strcmp(line, "key"))
                strncpy(prefs.key, eq, sizeof prefs.key - 1);
            else if (!strcmp(line, "relay"))
                strncpy(prefs.relay, eq, sizeof prefs.relay - 1);
            else if (!strcmp(line, "dns"))
                strncpy(prefs.dns, eq, sizeof prefs.dns - 1);
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
    char text[700], u[33], k[65];
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
    len = snprintf(text, sizeof text,
                   "password=%s\rport=%u\rquality=%d\rgamma=%d\rcursor=%s\r"
                   "id=%s\ruuid=%s\rseed=%s\rserver=%s\rkey=%s\rrelay=%s\rdns=%s\r",
                   prefs.password, prefs.port, prefs.q, prefs.gamma,
                   prefs.cursor_separate ? "separate" : "auto", prefs.id,
                   hex(prefs.uuid, 16, u), hex(prefs.seed, 32, k), prefs.server, prefs.key,
                   prefs.relay, prefs.dns);
    if (len > (long)sizeof text - 1)
        len = sizeof text - 1;
    FSWrite(pb.ioParam.ioRefNum, &len, text);
    SetEOF(pb.ioParam.ioRefNum, len);
    FSClose(pb.ioParam.ioRefNum);
}

/* Entropy for the first run's keys. A classic Mac has no /dev/random, but
 * the microsecond timer drifts against the tick and the system tasks, the
 * pointer is wherever the user left it, and the screen is the screen. */
static void gather_entropy(void)
{
    uint32_t us;
    unsigned long t;
    int i;
    for (i = 0; i < 64; i++) {
        us = cdv_microseconds();
        rng_add(&us, sizeof us);
        t = TickCount();
        rng_add(&t, sizeof t);
        if (i % 8 == 0) {
            long free = FreeMem();
            rng_add(&free, sizeof free);
            SystemTask(); /* let the timer drift against something */
        }
    }
    rng_add((void *)0x830, 4); /* Mouse */
    rng_add(&qd.randSeed, sizeof qd.randSeed);
    {
        GDHandle gd = GetMainDevice();
        PixMapHandle pm = (**gd).gdPMap;
        rng_add((**pm).baseAddr, 4096);
    }
}

static void init_prefs(void)
{
    static char buf[1024];
    long len = 0;
    prefs.port = DEFAULT_PORT;
    prefs.q = DEFAULT_Q;
    prefs.gamma = 1;
    prefs.password[0] = 0;
    strcpy(prefs.server, DEFAULT_SERVER);
    strcpy(prefs.key, DEFAULT_KEY);
    if (read_prefs_file(buf, sizeof buf - 1, &len)) {
        buf[len] = 0;
        parse_prefs(buf);
    }
    gather_entropy();
    if (!prefs.password[0] || !prefs.id[0] || !prefs.have_uuid || !prefs.have_seed) {
        /* First run: a password nobody chose, an ID and a key of our own. */
        static const char a[] = "abcdefghjkmnpqrstuvwxyz23456789";
        static const char digits[] = "123456789";
        int i;
        if (!prefs.password[0]) {
            for (i = 0; i < 8; i++)
                prefs.password[i] = a[rng_u32() % (sizeof a - 1)];
            prefs.password[8] = 0;
        }
        if (!prefs.id[0]) {
            /* Nine digits, like RustDesk's own: what a person can read out. */
            for (i = 0; i < 9; i++)
                prefs.id[i] = digits[rng_u32() % (sizeof digits - 1)];
            prefs.id[9] = 0;
        }
        if (!prefs.have_uuid) {
            rng_bytes(prefs.uuid, sizeof prefs.uuid);
            prefs.have_uuid = 1;
        }
        if (!prefs.have_seed) {
            rng_bytes(prefs.seed, sizeof prefs.seed);
            prefs.have_seed = 1;
        }
        write_prefs_file();
    }
    if (prefs.q < 0 || prefs.q > 127)
        prefs.q = DEFAULT_Q;
    if (!prefs.port)
        prefs.port = DEFAULT_PORT;
}

/* ---- the agent: set up here, run by the engine ---------------------------- */

static cdv_tcp t_direct, t_local, t_relay, t_helper;
static cdv_udp u_rdv, u_lan;
static int have_direct, have_local, have_relay, have_helper, have_rdv, have_lan;
static ip_addr my_ip;
static long my_mask;
static uint8_t sign_pk[32], sign_sk[64];
static char server_addr[80]; /* the server as the engine gets it: dotted, with the port */
static int have_dnr;
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
    encmem = big_alloc((long)vp8e_mem_size(scr.width, scr.height));
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
        big_free(encmem);
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
        eng.drop_req = 1;
        t0 = TickCount();
        while (eng.drop_req && TickCount() - t0 < 5 * 60)
            ;
        big_free(outq);
        outcap = queue_size(&scr);
        outq = (uint8_t *)big_alloc((long)outcap);
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
static void renew(cdv_tcp *t, int have);

static void chores(void)
{
    static unsigned long last_status, last_kchr, frames0, bytes0;
    unsigned long now = TickCount();
    const char *m;
    char line[80];

    while ((m = engine_next_log()) != NULL)
        say(m);
    clipboard_chores();

    renew(&t_helper, have_helper);
    renew(&t_relay, have_relay);
    if (name_q.req && !name_q.ans) {
        /* The engine wants a relay's address: the Mac's resolver first. */
        uint32_t ip;
        if (have_dnr && dnr_lookup(name_q.name, &ip, 5 * 60)) {
            name_q.ip = ip;
            name_q.ans = 1;
        } else {
            name_q.ans = -1;
        }
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
        } else {
            static const char *const rs[] = {
                "no ID server", "looking up the ID server", "cannot find the ID server",
                "registering with the ID server", "ready", "the ID server refused us"
            };
            snprintf(line, sizeof line, "Waiting: %s", rs[eng.rdv_state]);
            set_status(line);
            snprintf(line, sizeof line, "ID: %s%s", prefs.id,
                     eng.rdv_state == RS_REGISTERED ? "" : "  (not registered)");
            set_id_line(line);
        }
        frames0 = eng.frames;
        bytes0 = eng.bytes;
        last_status = now;
    }
}

/* ---- the network ---------------------------------------------------------------- */

static void close_network(void)
{
    /* MacTCP owns each stream's buffer until it is released; quitting
     * without this leaves it writing into a heap that no longer exists. */
    if (have_direct) tcp_release(&t_direct);
    if (have_local) tcp_release(&t_local);
    if (have_relay) tcp_release(&t_relay);
    if (have_helper) tcp_release(&t_helper);
    if (have_rdv) udp_release(&u_rdv);
    if (have_lan) udp_release(&u_lan);
    have_direct = have_local = have_relay = have_helper = have_rdv = have_lan = 0;
}

/* The streams a session can arrive on, and the ones that find it. Only the
 * direct one is essential; without the others it is a direct-IP agent. */
static OSErr open_network(void)
{
    char line[100], host[64], a[20];
    unsigned short port;
    uint32_t ip;
    OSErr err = net_open(&my_ip, &my_mask);
    if (err != noErr) {
        snprintf(line, sizeof line, "MacTCP did not open (%d)", err);
        say(line);
        return err;
    }
    err = tcp_create(&t_direct, 32 * 1024L, 8 * 1024);
    if (err != noErr) {
        snprintf(line, sizeof line, "no TCP stream (%d)", err);
        say(line);
        return err;
    }
    have_direct = 1;
    have_local = tcp_create(&t_local, 32 * 1024L, 8 * 1024) == noErr;
    have_relay = tcp_create(&t_relay, 32 * 1024L, 8 * 1024) == noErr;
    have_helper = tcp_create(&t_helper, 4 * 1024L, 1024) == noErr;
    have_rdv = udp_create(&u_rdv, 0, 4 * 1024L) == noErr;
    have_lan = udp_create(&u_lan, LAN_PORT, 4 * 1024L) == noErr;
    if (!have_lan)
        say("another program has the discovery port; no LAN discovery");
    if (!have_local || !have_relay || !have_helper || !have_rdv)
        say("not enough memory for every stream: direct connections only");

    /* The ID server's address, with the Mac's own resolver if it has one;
     * the engine asks DNS itself if not. */
    server_addr[0] = 0;
    if (prefs.server[0] && have_rdv && have_relay && have_helper) {
        rdv_split_host(prefs.server, host, sizeof host, &port, RDV_PORT);
        if (dns_parse_ip(host, &ip)) {
            strcpy(server_addr, prefs.server);
        } else {
            have_dnr = dnr_open() == noErr;
            if (!have_dnr)
                say("no resolver in this System; asking DNS directly");
            if (have_dnr && dnr_lookup(host, &ip, 10 * 60)) {
                net_addr_string(ip, a);
                snprintf(server_addr, sizeof server_addr, "%s:%u", a, port);
                snprintf(line, sizeof line, "%s is %s", host, a);
                say(line);
            } else {
                strncpy(server_addr, prefs.server, sizeof server_addr - 1);
            }
        }
    }
    return noErr;
}

/* DNS servers for when the engine resolves a name itself: the prefs', the
 * router's (a guess: the first host of our network), and a public one. */
static void pick_dns(ip_addr *dns, int *n)
{
    uint32_t ip;
    *n = 0;
    if (prefs.dns[0] && dns_parse_ip(prefs.dns, &ip))
        dns[(*n)++] = ip;
    if (my_mask && my_mask != -1L)
        dns[(*n)++] = (my_ip & (ip_addr)my_mask) | 1;
    dns[(*n)++] = 0x08080808UL;
}

/* The engine wants a fresh stream for an outgoing connection. Only once it
 * has gone idle: the engine is not using it then. */
static void renew(cdv_tcp *t, int have)
{
    if (have && t->renew_req && t->state == T_IDLE) {
        OSErr err = tcp_renew(t);
        if (err != noErr) {
            char line[60];
            snprintf(line, sizeof line, "could not renew a TCP stream (%d)", err);
            say(line);
        }
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
    SetRect(&r, r.right - 330, r.bottom - 183, r.right - 10, r.bottom - 10);
    win = NewWindow(NULL, &r, "\pC-Desk-Vint", 1, noGrowDocProc, (WindowPtr)-1, 0, 0);

    init_prefs();
    log_open();
#if defined(__powerpc__) || defined(__ppc__)
    say(APP_NAME " 0.1d, native PowerPC");
#else
    say(APP_NAME " 0.1d, 68k");
#endif
    strcpy(status, "Starting");
    snprintf(id_line, sizeof id_line, "ID: %s", prefs.id);
    crypto_sign_ed25519_seed_keypair(sign_pk, sign_sk, prefs.seed);
    ident.id = prefs.id;
    ident.sign_sk = sign_sk;
    strcpy(hostname, "Macintosh");
    ident.password = prefs.password;
    ident.salt = "cdeskvint";
    ident.hostname = hostname;

    inq = (uint8_t *)NewPtr(INQ_SIZE);
    err = inq ? open_video() : memFullErr;
    if (err == noErr) {
        outcap = queue_size(&scr);
        outq = (uint8_t *)big_alloc((long)outcap);
        if (!outq)
            err = memFullErr;
    }
    if (err != noErr) {
        snprintf(line, sizeof line, "not enough memory for this screen (%d)", err);
        say(line);
        say("give C-Desk-Vint more in Get Info, or quit other programs");
    } else {
        snprintf(line, sizeof line, "screen %dx%d, %d bits, %s cursor", scr.width, scr.height,
                 scr.depth, ident.cursor_embedded ? "software" : "hardware");
        say(line);
        if (big_temp_bytes()) {
            snprintf(line, sizeof line, "%ld KB of it in temporary memory", big_temp_bytes() / 1024);
            say(line);
        }
        if (scr.gamma_info[0])
            snprintf(line, sizeof line, "gamma from the driver (%d-channel), mid-grey %d -> %d",
                     scr.gamma_info[0], 128, scr.gamma[1][128]);
        else
            snprintf(line, sizeof line, "no gamma table from the driver (%d); colours as drawn",
                     scr.gamma_err);
        say(line);
        err = open_network();
    }
    if (err == noErr) {
        engine_ctx ctx;
        char a[20];
        input_set_kchr((Ptr)GetScriptManagerVariable(smKCHRCache));
        memset(&ctx, 0, sizeof ctx);
        ctx.direct = have_direct ? &t_direct : NULL;
        ctx.local = have_local ? &t_local : NULL;
        ctx.relay = have_relay ? &t_relay : NULL;
        ctx.helper = have_helper ? &t_helper : NULL;
        ctx.rdv = have_rdv ? &u_rdv : NULL;
        ctx.lan = have_lan ? &u_lan : NULL;
        ctx.my_ip = my_ip;
        ctx.direct_port = prefs.port;
        ctx.local_port = LOCAL_PORT;
        ctx.server = server_addr;
        ctx.relay_host = prefs.relay;
        ctx.key = prefs.key;
        ctx.uuid = prefs.uuid;
        ctx.pk = sign_pk;
        pick_dns(ctx.dns, &ctx.ndns);
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
        net_addr_string(my_ip, a);
        snprintf(addr_line, sizeof addr_line, "Or by address: %s  (port %u)", a, prefs.port);
        snprintf(line, sizeof line, "ID: %s", prefs.id);
        set_id_line(line);
        set_status("Starting");
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
        close_network();
    }
    dnr_close();
    if (logref)
        FSClose(logref);
    return 0;
}
