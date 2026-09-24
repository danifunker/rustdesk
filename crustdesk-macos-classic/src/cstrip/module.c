/* C-Desk-Vint's Control Strip module: an icon for the agent's state and a
 * menu to start, stop or show it -- the classic Mac OS place for what a
 * menu-bar extra does later. A 68k code resource ('sdev'); PowerPC Macs run
 * it in the emulator, as they run every module. It talks to the agent only
 * through the block in the system heap (src/mac/strip.h).
 *
 * The icon, 16x14: a monitor. Its screen is solid when a peer is connected,
 * grey when sharing and waiting, crossed out when sharing is off; the whole
 * thing is drawn grey when the agent is not running.
 */
#include "../mac/strip.h"

#include <Multiverse.h>
#include <Retro68Runtime.h>
#include <string.h>

enum { sdevInitModule, sdevCloseModule, sdevFeatures, sdevGetDisplayWidth, sdevPeriodicTickle,
       sdevDrawStatus, sdevMouseClick, sdevSaveSettings, sdevShowBalloonHelp };
/* Feature bits: the click, on mouse-down (sdevDontAutoTrack), so the pop-up
 * menu tracks the button still held -- auto-tracking reports the click only
 * after the release, when a pop-up has nothing left to track. */
#define FEATURES ((1 << 0) | (1 << 1)) /* sdevWantMouseClicks, sdevDontAutoTrack */
#define WIDTH 20
#define launchNoFileFlags 0x0800 /* Processes.h; Multiversal lacks it */

/* The Control Strip's own utilities (ControlStrip.h: _ControlStripDispatch). */
static pascal short SBTrackPopupMenu(const Rect *r, MenuHandle m)
    M68K_INLINE(0x303C, 0x0408, 0xAAF2);

static uint16_t seen = 0xFFFF; /* the block's change count when last drawn */

static cdv_strip *block(void)
{
    long v = 0;
    if (Gestalt(STRIP_SELECTOR, &v) != noErr || !v || ((cdv_strip *)v)->magic != STRIP_MAGIC)
        return NULL;
    return (cdv_strip *)v;
}

static void pat(Pattern *p, uint8_t a, uint8_t b)
{
    int i;
    for (i = 0; i < 8; i++)
        p->pat[i] = (i & 1) ? b : a;
}

static void draw(const Rect *where, GrafPtr port)
{
    GrafPtr old;
    cdv_strip *s = block();
    Rect r, screen;
    Pattern white, black, grey;
    int running = s && s->app_running;
    pat(&white, 0x00, 0x00);
    pat(&black, 0xFF, 0xFF);
    pat(&grey, 0xAA, 0x55);
    GetPort(&old);
    SetPort(port);
    /* Centre a 16x14 icon in the module's slot. */
    r.left = where->left + (where->right - where->left - 16) / 2;
    r.top = where->top + (where->bottom - where->top - 14) / 2;
    r.right = r.left + 16;
    r.bottom = r.top + 11;
    PenNormal();
    EraseRect(&r);
    FrameRect(&r);
    screen = r;
    InsetRect(&screen, 2, 2);
    if (!running || !s->sharing) {
        FillRect(&screen, &white);
        MoveTo(screen.left, screen.top);
        LineTo(screen.right - 1, screen.bottom - 1);
        MoveTo(screen.right - 1, screen.top);
        LineTo(screen.left, screen.bottom - 1);
    } else {
        FillRect(&screen, s->live ? &black : &grey);
    }
    /* the stand */
    MoveTo(r.left + 6, r.bottom);
    LineTo(r.left + 9, r.bottom);
    MoveTo(r.left + 4, r.bottom + 2);
    LineTo(r.left + 11, r.bottom + 2);
    if (!running) {
        /* dimmed: a grey mask over it all */
        Rect all = r;
        all.bottom += 3;
        PenPat(&grey);
        PenMode(patBic);
        PaintRect(&all);
        PenNormal();
    }
    SetPort(old);
    seen = s ? s->changes : 0;
}

static void append(MenuHandle m, const char *c, int enabled)
{
    Str255 p;
    size_t n = strlen(c);
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
    /* AppendMenu would read metacharacters ('(' ';' '/') in the ID or
     * status; add a blank item and set its text instead. */
    AppendMenu(m, "\px");
    SetMenuItemText(m, CountMItems(m), p);
    if (!enabled)
        DisableItem(m, CountMItems(m));
}

static void click(const Rect *where)
{
    cdv_strip *s = block();
    MenuHandle m = NewMenu(30000, "\p");
    char line[48];
    short item;
    int running = s && s->app_running;
    if (!m)
        return;
    if (!running) {
        append(m, "C-Desk-Vint is not running", 0);
        append(m, "Open C-Desk-Vint", s && s->app.name[0]);
    } else {
        strcpy(line, "ID: ");
        strcat(line, s->sharing && s->id[0] ? s->id : "-");
        append(m, line, 0);
        append(m, !s->sharing       ? "Sharing is off"
                  : s->live         ? "A peer is connected"
                  : s->registered   ? (s->listed ? "Ready, and in the console" : "Ready")
                                    : "Not yet registered",
               0);
        AppendMenu(m, "\p(-");
        append(m, s->sharing ? "Stop Sharing" : "Start Sharing", 1);
        append(m, "Show C-Desk-Vint", 1);
    }
    InsertMenu(m, -1); /* hierarchical: out of the menu bar */
    item = SBTrackPopupMenu(where, m);
    DeleteMenu(30000);
    DisposeMenu(m);
    if (item <= 0)
        return;
    if (!running) {
        if (item == 2 && s) {
            LaunchParamBlockRec lp;
            memset(&lp, 0, sizeof lp);
            lp.launchBlockID = extendedBlock;
            lp.launchEPBLength = extendedBlockLen;
            lp.launchControlFlags = launchContinue | launchNoFileFlags;
            lp.launchAppSpec = &s->app;
            LaunchApplication(&lp);
        }
    } else if (item == 4) {
        s->cmd = s->sharing ? STRIP_CMD_STOP : STRIP_CMD_START;
    } else if (item == 5) {
        s->cmd = STRIP_CMD_SHOW;
        SetFrontProcess(&s->psn);
    }
}

pascal long cstrip_main(long message, long params, Rect *where, GrafPtr port)
{
    cdv_strip *s;
    RETRO68_RELOCATE();
    (void)params;
    switch (message) {
    case sdevInitModule:
        return 0;
    case sdevFeatures:
        return FEATURES;
    case sdevGetDisplayWidth:
        return WIDTH;
    case sdevDrawStatus:
        draw(where, port);
        return 0;
    case sdevPeriodicTickle:
        s = block();
        if ((s ? s->changes : 0) != seen)
            draw(where, port);
        return 0;
    case sdevMouseClick:
        click(where);
        draw(where, port);
        return 0;
    default:
        return 0;
    }
}
