/*
 * clipboard_shim.c -- clipboard text in both directions, over X selections.
 *
 * Presents the four functions `clipboard.rs` already calls on the Mac. The Mac
 * has a pasteboard the agent can simply read and write; X has no clipboard at
 * all, only a protocol between the window that owns a selection and whoever
 * asks it for one. Three consequences shape this file:
 *
 *   - Reading means asking, then waiting for the owner to answer. The wait is
 *     bounded (CLIP_WAIT_MS): a hung or slow owner must not stall the agent's
 *     poll loop, and an empty clipboard is a much better answer than a stuck
 *     session.
 *   - Writing means *becoming* the owner and then answering requests for as
 *     long as the text is meant to stay on the clipboard. Those requests arrive
 *     as events, so every entry point here pumps the queue first -- the agent
 *     calls one of them on every pass, which is what keeps the paste working.
 *   - Knowing whether it changed would otherwise mean fetching the text every
 *     pass and comparing. XFIXES reports a change of owner instead, so an idle
 *     clipboard costs one event-queue check.
 *
 * CLIPBOARD, not PRIMARY. PRIMARY is the select-to-copy buffer and changes
 * every time someone drags across a word; CLIPBOARD is what Ctrl-C means, and
 * it is what a peer expects to receive.
 *
 * UTF8_STRING, falling back to STRING (Latin-1) for an owner too old to offer
 * it -- which on CDE is most of them.
 *
 * Large transfers use the INCR protocol, which this does not implement: a
 * clipboard past CLIP_MAX is reported as empty rather than as a truncated
 * fragment that would paste as half a document.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#if RD_HAVE_XFIXES
#include <X11/extensions/Xfixes.h>
#else
/* Solaris 9 has no XFIXES, so there is no selection-owner notification to
 * subscribe to. g_have_xfixes stays 0 and the poll path below carries the
 * clipboard on its own; these exist only to compile. */
#define XFixesQueryExtension(dpy, ev, er)               (0)
#define XFixesSelectSelectionInput(dpy, w, sel, mask)   ((void)0)
#define XFixesSetSelectionOwnerNotifyMask               0
#define XFixesSelectionNotify                           0
#endif

#define CLIP_WAIT_MS  200        /* how long an owner gets to answer */
#define CLIP_MAX      (256 * 1024)

static Display *g_dpy;
static Window   g_win;           /* our unmapped window, the selection's face */
static Atom     A_CLIPBOARD, A_UTF8, A_TARGETS, A_PROP, A_INCR;
static int      g_have_xfixes;
static int      g_xfixes_ev;
static int      g_changed;       /* set by a SetSelectionOwner event */

/* What we are currently offering, when we are the owner. */
static char    *g_owned;
static int      g_owned_len;

static int on_x_error(Display *d, XErrorEvent *e)
{
    char buf[96];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "clipboard: X error %d (%s), request %d.%d\n",
            e->error_code, buf, e->request_code, e->minor_code);
    return 0;
}

static int init(void)
{
    int er;
    if (g_dpy)
        return 1;
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "clipboard: XOpenDisplay failed; clipboard is unavailable\n");
        return 0;
    }
    XSetErrorHandler(on_x_error);
    A_CLIPBOARD = XInternAtom(g_dpy, "CLIPBOARD", False);
    A_UTF8      = XInternAtom(g_dpy, "UTF8_STRING", False);
    A_TARGETS   = XInternAtom(g_dpy, "TARGETS", False);
    A_PROP      = XInternAtom(g_dpy, "RD_CLIP", False);
    A_INCR      = XInternAtom(g_dpy, "INCR", False);
    /* Unmapped and 1x1: it exists to be a selection owner and a place for
     * properties to land, and is never drawn. */
    g_win = XCreateSimpleWindow(g_dpy, DefaultRootWindow(g_dpy), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(g_dpy, g_win, PropertyChangeMask);

    g_have_xfixes = XFixesQueryExtension(g_dpy, &g_xfixes_ev, &er) ? 1 : 0;
    if (g_have_xfixes) {
        XFixesSelectSelectionInput(g_dpy, DefaultRootWindow(g_dpy), A_CLIPBOARD,
                                   XFixesSetSelectionOwnerNotifyMask);
        /* Whatever is on the clipboard when the agent starts counts as new. */
        g_changed = 1;
    }
    XSync(g_dpy, False);
    return 1;
}

/* Answer one SelectionRequest against the text we own. */
static void serve_request(XSelectionRequestEvent *req)
{
    XSelectionEvent note;

    memset(&note, 0, sizeof note);
    note.type      = SelectionNotify;
    note.display   = req->display;
    note.requestor = req->requestor;
    note.selection = req->selection;
    note.target    = req->target;
    note.time      = req->time;
    note.property  = None;          /* refused, unless it is filled in below */

    if (g_owned && req->target == A_TARGETS) {
        Atom targets[3];
        targets[0] = A_TARGETS;
        targets[1] = A_UTF8;
        targets[2] = XA_STRING;
        XChangeProperty(req->display, req->requestor, req->property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)targets, 3);
        note.property = req->property;
    } else if (g_owned && (req->target == A_UTF8 || req->target == XA_STRING)) {
        XChangeProperty(req->display, req->requestor, req->property, req->target, 8,
                        PropModeReplace, (unsigned char *)g_owned, g_owned_len);
        note.property = req->property;
    }

    XSendEvent(req->display, req->requestor, False, 0, (XEvent *)&note);
    XFlush(req->display);
}

/* Drain the queue: serve any request for the text we own, and note any change
 * of owner. Every entry point calls this, so the agent's normal polling is what
 * keeps a paste from a remote peer working. */
static void pump(void)
{
    XEvent e;
    if (!g_dpy)
        return;
    while (XPending(g_dpy)) {
        XNextEvent(g_dpy, &e);
        if (e.type == SelectionRequest) {
            serve_request(&e.xselectionrequest);
        } else if (e.type == SelectionClear) {
            /* Someone else copied something: we are no longer the owner, and
             * what they put there is a change worth reporting. */
            free(g_owned);
            g_owned = NULL;
            g_owned_len = 0;
            g_changed = 1;
        } else if (g_have_xfixes && e.type == g_xfixes_ev + XFixesSelectionNotify) {
            g_changed = 1;
        }
    }
}

int rd_clip_ok(void)
{
    if (!init())
        return 0;
    pump();
    return 1;
}

/* Has the clipboard changed since the last time it was read?
 *
 * Without XFIXES there is no way to know short of fetching the text every
 * pass, so say "yes" and let the caller compare -- correct, just chattier. */
int rd_clip_changed(void)
{
    if (!init())
        return 0;
    pump();
    if (!g_have_xfixes)
        return 1;
    return g_changed;
}

/* Fetch the clipboard as UTF-8. Returns the byte count, 0 for empty, -1 on
 * failure. Never blocks for longer than CLIP_WAIT_MS. */
int rd_clip_get(unsigned char *out, int cap)
{
    Atom targets[2];
    int t, waited;

    if (!out || cap <= 0 || !init())
        return -1;
    pump();
    g_changed = 0;

    /* If we are the owner, the answer is already here: asking ourselves would
     * mean answering our own request from inside this call. */
    if (g_owned) {
        int n = g_owned_len < cap ? g_owned_len : cap;
        memcpy(out, g_owned, (size_t)n);
        return n;
    }

    targets[0] = A_UTF8;
    targets[1] = XA_STRING;      /* an owner too old for UTF8_STRING */
    for (t = 0; t < 2; t++) {
        XDeleteProperty(g_dpy, g_win, A_PROP);
        XConvertSelection(g_dpy, A_CLIPBOARD, targets[t], A_PROP, g_win, CurrentTime);
        XFlush(g_dpy);

        for (waited = 0; waited < CLIP_WAIT_MS; waited += 10) {
            XEvent e;
            if (XCheckTypedWindowEvent(g_dpy, g_win, SelectionNotify, &e)) {
                Atom type = None;
                int fmt = 0;
                unsigned long nitems = 0, after = 0;
                unsigned char *data = NULL;
                int n;

                if (e.xselection.property == None)
                    break;      /* no owner, or it refused this target */

                if (XGetWindowProperty(g_dpy, g_win, A_PROP, 0, CLIP_MAX / 4, True,
                                       AnyPropertyType, &type, &fmt, &nitems,
                                       &after, &data) != Success || !data)
                    break;

                if (type == A_INCR) {
                    /* Bigger than CLIP_MAX and delivered in pieces. Reporting
                     * empty is honest; half a document is not. */
                    XFree(data);
                    fprintf(stderr, "clipboard: INCR transfer (>%d bytes), skipped\n",
                            CLIP_MAX);
                    return 0;
                }
                n = (int)nitems;
                if (n > cap) n = cap;
                memcpy(out, data, (size_t)n);
                XFree(data);
                return n;
            }
            /* Serve anything else that arrived while we waited. */
            pump();
            usleep(10 * 1000);
        }
    }
    return 0;
}

/* Put text on the clipboard by becoming its owner. Returns 0, or -1. */
int rd_clip_set(const unsigned char *text, int len)
{
    char *copy;

    if (!text || len < 0 || !init())
        return -1;
    if (len > CLIP_MAX)
        return -1;

    copy = (char *)malloc((size_t)len ? (size_t)len : 1);
    if (!copy)
        return -1;
    memcpy(copy, text, (size_t)len);
    free(g_owned);
    g_owned = copy;
    g_owned_len = len;

    XSetSelectionOwner(g_dpy, A_CLIPBOARD, g_win, CurrentTime);
    XFlush(g_dpy);
    if (XGetSelectionOwner(g_dpy, A_CLIPBOARD) != g_win) {
        fprintf(stderr, "clipboard: could not take ownership of CLIPBOARD\n");
        return -1;
    }
    /* Our own ownership is not a change to report back to the peer. */
    pump();
    g_changed = 0;
    return 0;
}
