/* The clipboard, both ways, through X selections.
 *
 * The peer's text: we own CLIPBOARD (a Motif application's Paste) and
 * PRIMARY (a middle click in a winterm) and hand it out as STRING -- Latin-1,
 * which is what IRIX 5.3's applications read; characters beyond it become
 * '?'.
 *
 * Text copied on the IRIX side: X11R6-era IRIX has no XFixes to say when a
 * selection changes hands, but XTrap sees every client's SetSelectionOwner
 * (screen.c passes them here). A moment after one, we ask the new owner for
 * STRING and send what comes back to the peer as UTF-8.
 */
#include "clip.h"
#include "log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#define MAX_TEXT (256 * 1024)

static Display *dpy;
static Window win;
static Atom a_clipboard, a_targets, a_text, a_ctext, a_prop, a_timestamp;
static char *ours;          /* what we own, Latin-1 */
static size_t ours_len;
static char *last_sent;     /* the last text sent to the peer, Latin-1 */
static size_t last_sent_len;
static Atom want;           /* a selection to fetch, None for none */
static uint32_t want_at;
static void (*to_peer)(const char *utf8, size_t n);

static uint32_t now_ms(void)
{
    return cdv_ms();
}

void clip_init(Display *d, void (*send)(const char *utf8, size_t n))
{
    XSetWindowAttributes a;
    dpy = d;
    to_peer = send;
    a.override_redirect = True;
    win = XCreateWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, InputOnly,
                        CopyFromParent, CWOverrideRedirect, &a);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_text = XInternAtom(dpy, "TEXT", False);
    a_ctext = XInternAtom(dpy, "COMPOUND_TEXT", False);
    a_timestamp = XInternAtom(dpy, "TIMESTAMP", False);
    a_prop = XInternAtom(dpy, "CDV_CLIP", False);
}

Window clip_window(void) { return win; }

/* UTF-8 to Latin-1, '?' for what does not fit. */
static size_t to_latin1(const char *u, size_t n, char *out)
{
    const unsigned char *p = (const unsigned char *)u, *end = p + n;
    size_t o = 0;
    while (p < end) {
        unsigned cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xe0) == 0xc0 && p + 1 < end) {
            cp = (unsigned)(p[0] & 0x1f) << 6 | (p[1] & 0x3f);
            p += 2;
        } else {
            /* three and four byte sequences: nothing Latin-1 can show */
            int len = (*p & 0xf0) == 0xe0 ? 3 : (*p & 0xf8) == 0xf0 ? 4 : 1;
            p += len;
            cp = '?';
        }
        if (cp == '\r' && p < end && *p == '\n')
            continue; /* CRLF from Windows: X wants LF */
        out[o++] = (char)(cp > 0xff ? '?' : cp);
    }
    return o;
}

static size_t to_utf8(const char *l, size_t n, char *out)
{
    size_t i, o = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)l[i];
        if (c < 0x80) {
            out[o++] = (char)c;
        } else {
            out[o++] = (char)(0xc0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    return o;
}

void clip_from_peer(const char *utf8, size_t n)
{
    char *l;
    if (n > MAX_TEXT)
        n = MAX_TEXT;
    l = (char *)malloc(n + 1);
    if (!l)
        return;
    ours_len = to_latin1(utf8, n, l);
    free(ours);
    ours = l;
    /* remember it as sent, so taking ownership does not echo it back */
    free(last_sent);
    last_sent = (char *)malloc(ours_len + 1);
    if (last_sent)
        memcpy(last_sent, ours, ours_len);
    last_sent_len = last_sent ? ours_len : 0;
    XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
    XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
    XFlush(dpy);
    cdv_debug("clipboard: %u bytes from the peer", (unsigned)ours_len);
}

/* A client took a selection (from XTrap). */
void clip_owner_changed(Atom selection, Window owner)
{
    if (owner == None || owner == win)
        return;
    if (selection != a_clipboard && selection != XA_PRIMARY)
        return;
    /* CLIPBOARD wins if both change at once: it is the deliberate copy */
    if (want == a_clipboard && selection == XA_PRIMARY)
        return;
    want = selection;
    want_at = now_ms() + 300; /* let the application finish copying */
}

void clip_tick(void)
{
    if (want == None || (int32_t)(now_ms() - want_at) < 0)
        return;
    XConvertSelection(dpy, want, XA_STRING, a_prop, win, CurrentTime);
    XFlush(dpy);
    want = None;
}

static void answer(XSelectionRequestEvent *rq)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xselection.type = SelectionNotify;
    ev.xselection.display = dpy;
    ev.xselection.requestor = rq->requestor;
    ev.xselection.selection = rq->selection;
    ev.xselection.target = rq->target;
    ev.xselection.time = rq->time;
    ev.xselection.property = rq->property != None ? rq->property : rq->target;
    if (!ours) {
        ev.xselection.property = None;
    } else if (rq->target == a_targets) {
        Atom t[5];
        t[0] = a_targets;
        t[1] = XA_STRING;
        t[2] = a_text;
        t[3] = a_ctext;
        t[4] = a_timestamp;
        XChangeProperty(dpy, rq->requestor, ev.xselection.property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)t, 5);
    } else if (rq->target == XA_STRING || rq->target == a_text || rq->target == a_ctext) {
        /* Latin-1 is also valid COMPOUND_TEXT */
        XChangeProperty(dpy, rq->requestor, ev.xselection.property,
                        rq->target == a_ctext ? a_ctext : XA_STRING, 8, PropModeReplace,
                        (unsigned char *)ours, (int)ours_len);
    } else {
        ev.xselection.property = None;
    }
    XSendEvent(dpy, rq->requestor, False, NoEventMask, &ev);
    XFlush(dpy);
}

static void received(XSelectionEvent *se)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    char *u;
    size_t ul;
    if (se->property == None)
        return;
    if (XGetWindowProperty(dpy, win, se->property, 0, MAX_TEXT / 4, True, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) != Success || !data)
        return;
    if (fmt != 8 || !n || (last_sent && n == last_sent_len && !memcmp(data, last_sent, n))) {
        XFree(data);
        return;
    }
    free(last_sent);
    last_sent = (char *)malloc(n);
    if (last_sent) {
        memcpy(last_sent, data, n);
        last_sent_len = n;
    }
    u = (char *)malloc(2 * n + 1);
    if (u) {
        ul = to_utf8((const char *)data, n, u);
        cdv_debug("clipboard: %u bytes to the peer", (unsigned)ul);
        to_peer(u, ul);
        free(u);
    }
    XFree(data);
}

int clip_event(XEvent *e)
{
    switch (e->type) {
    case SelectionRequest:
        if (e->xselectionrequest.owner != win)
            return 0;
        answer(&e->xselectionrequest);
        return 1;
    case SelectionClear:
        if (e->xselectionclear.window != win)
            return 0;
        /* someone else has it now; XTrap will have told us who */
        return 1;
    case SelectionNotify:
        if (e->xselection.requestor != win)
            return 0;
        received(&e->xselection);
        return 1;
    }
    return 0;
}
