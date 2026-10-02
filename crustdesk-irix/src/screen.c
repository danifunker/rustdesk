/* The IRIX screen: damage from XTrap, readback through MIT-SHM, conversion
 * to I420. See screen.h for why it is built this way. */
#define NEED_EVENTS
#define NEED_REPLIES
#include "screen.h"
#include "clip.h"
#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/xtraplib.h>
#include <X11/extensions/xtraplibp.h>

/* In SGI's libXext, but not in its XShm.h. */
Bool XShmQueryExtension(Display *);

/* XTrap hands each request over with this much of it: the header (16 bytes)
 * and the first 48 bytes of the request, which holds the drawable, the GC and
 * the first few rectangles, points or characters. A request longer than that is
 * marked by its window instead of its exact shape. Each 24 bytes is another X
 * event on a 33 MHz machine, so this is a trade. */
#define TRAP_PACKET 64
#define TRAP_HDR 16

/* The background scan: macroblock rows read every pass, whatever happened. */
#define SCAN_ROWS 3

static Display *dpy;
static Window root;
static Visual *vis;
static int W, H, MBW, MBH;
static XShmSegmentInfo shm;
static int shm_ok;
static uint8_t *fb;            /* the screen as last read, 8-bit indices */
static uint8_t *dmg;           /* MBW*MBH: something drew here */
/* Motion since the last frame: hint_next fills from events; screen_collect
 * hands it to the frame (hint_frame) and starts afresh, so a scroll
 * reported while a frame is being encoded goes to the next one. */
static vp8e_hint *hint_next, *hint_frame, *hint_tmp;
static int nhint_next;
static int ndmg;
static int cmap_dirty;
static Colormap cmap_cur;
static uint8_t ty[256], tu[256], tv[256];
static uint8_t gtab[256];
static XETC *tc;
static int watching;
static int scan_row;
static uint32_t scan_due, scan_every = 150;

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* ---- damage ----------------------------------------------------------------- */

static void read_overlays(cdv_screen *s);
static int noverlays;             /* mapped overlay windows */
static void ov_damage(int x, int y, int w, int h);
static int in_overlay;            /* on_request is handling a request on an overlay */

static void mark(int x, int y, int w, int h)
{
    int x0, y0, x1, y1, mx, my;
    if (w <= 0 || h <= 0)
        return;
    if (in_overlay) {
        ov_damage(x, y, w, h);
        return;
    }
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > W ? W : x + w;
    y1 = y + h > H ? H : y + h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (my = y0 >> 4; my <= (y1 - 1) >> 4; my++)
        for (mx = x0 >> 4; mx <= (x1 - 1) >> 4; mx++)
            if (!dmg[my * MBW + mx]) {
                dmg[my * MBW + mx] = 1;
                ndmg++;
            }
}

/* The rectangle (x, y, w, h) now holds what was at (x - mx, y - my): a
 * scroll or a moved window. Macroblocks wholly inside it get a hint; one
 * already moved this frame composes (two 13-pixel scrolls are one of 26),
 * by way of the macroblock its contents came from. */
static void add_motion(int x, int y, int w, int h, int mx, int my)
{
    int bx0 = (x + 15) >> 4, by0 = (y + 15) >> 4, bx1 = (x + w) >> 4, by1 = (y + h) >> 4;
    int bx, by;
    if (!mx && !my)
        return;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > MBW) bx1 = MBW;
    if (by1 > MBH) by1 = MBH;
    if (bx0 >= bx1 || by0 >= by1)
        return;
    memcpy(hint_tmp, hint_next, sizeof(vp8e_hint) * (size_t)(MBW * MBH));
    for (by = by0; by < by1; by++)
        for (bx = bx0; bx < bx1; bx++) {
            int sx = (bx * 16 + 8 - mx) >> 4, sy = (by * 16 + 8 - my) >> 4, dx = mx, dy = my;
            vp8e_hint *h = &hint_next[by * MBW + bx];
            if (sx >= 0 && sy >= 0 && sx < MBW && sy < MBH) {
                dx += hint_tmp[sy * MBW + sx].dx;
                dy += hint_tmp[sy * MBW + sx].dy;
            }
            if (!h->dx && !h->dy && (dx || dy))
                nhint_next++;
            h->dx = (short)dx;
            h->dy = (short)dy;
        }
}

/* Top-level windows (the root's children: 4Dwm's frames, override-redirect
 * menus), from SubstructureNotify. */
typedef struct {
    Window w;
    int x, y, wd, ht, mapped;
    /* Overlay windows (menus: 2-bit, in the popup planes on Entry graphics)
     * are not in what XShmGetImage reads from the root. Their own pixels are
     * read and laid over the screen when converting. */
    int known;            /* depth and colormap looked up */
    int overlay, bw, ow, oh, odirty, depth;
    int dx0, dy0, dx1, dy1; /* what was drawn on it, in screen coordinates */
    Colormap ocmap;
    Visual *ovis;
    uint8_t *opix;        /* ow*oh pixel values */
    uint8_t oy[4], ou[4], ov[4];
} toplevel;
#define MAXTOP 512
static toplevel tops[MAXTOP];
static int ntops;

static toplevel *top_find(Window w)
{
    int i;
    for (i = 0; i < ntops; i++)
        if (tops[i].w == w)
            return &tops[i];
    return NULL;
}

static toplevel *top_add(Window w)
{
    toplevel *t = top_find(w);
    if (t)
        return t;
    if (ntops == MAXTOP)
        return NULL;
    t = &tops[ntops++];
    memset(t, 0, sizeof *t);
    t->w = w;
    return t;
}

static void top_del(Window w)
{
    toplevel *t = top_find(w);
    if (t) {
        free(t->opix);
        *t = tops[--ntops];
    }
}

static void top_mark(const toplevel *t)
{
    if (t->mapped)
        mark(t->x, t->y, t->wd, t->ht);
}

/* ---- overlays ----------------------------------------------------------------- */

static uint8_t *ovmap;            /* MBW*MBH: an overlay window covers part of it */
static unsigned long ov_transparent; /* SERVER_OVERLAY_VISUALS: see-through pixel */

/* The windows inside overlay top-levels (a Motif menu draws in its shell's
 * child, not the shell): a request on one of them changes that overlay and
 * nothing in the normal planes. */
#define MAXOVWIN 256
static struct {
    Window w, top;
} ovwin[MAXOVWIN];
static int novwin;

static void ovwin_add_tree(Window top, Window w, int depth)
{
    Window r, p, *k = NULL;
    unsigned int n, i;
    if (novwin < MAXOVWIN) {
        ovwin[novwin].w = w;
        ovwin[novwin].top = top;
        novwin++;
    }
    if (depth > 4 || !XQueryTree(dpy, w, &r, &p, &k, &n))
        return;
    for (i = 0; i < n; i++)
        ovwin_add_tree(top, k[i], depth + 1);
    if (k)
        XFree(k);
}

static void ovwin_drop(Window top)
{
    int i, j = 0;
    for (i = 0; i < novwin; i++)
        if (ovwin[i].top != top)
            ovwin[j++] = ovwin[i];
    novwin = j;
}

/* A request drew on `id`: if it is part of an overlay, that overlay is to be
 * read again. Returns nonzero if so (the normal planes are untouched). */
static toplevel *cur_ov;

static int ov_drawn(XID id)
{
    int i, k;
    for (i = 0; i < novwin; i++)
        if (ovwin[i].w == (Window)id) {
            for (k = 0; k < ntops; k++)
                if (tops[k].w == ovwin[i].top && tops[k].overlay) {
                    cur_ov = &tops[k];
                    return 1;
                }
            return 0;
        }
    return 0;
}

/* Grow the current overlay's damage box (screen coordinates). */
static void ov_damage(int x, int y, int w, int h)
{
    toplevel *t = cur_ov;
    if (!t)
        return;
    if (!t->odirty) {
        t->dx0 = x;
        t->dy0 = y;
        t->dx1 = x + w;
        t->dy1 = y + h;
        t->odirty = 1;
        return;
    }
    if (x < t->dx0) t->dx0 = x;
    if (y < t->dy0) t->dy0 = y;
    if (x + w > t->dx1) t->dx1 = x + w;
    if (y + h > t->dy1) t->dy1 = y + h;
}

/* These macroblocks must be converted and sent whatever the screen's pixels
 * say: an overlay came, went or changed over them. */
static uint8_t *conv;
static void force(cdv_screen *s, int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w,
        y1 = y + h > H ? H : y + h, mx, my;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (my = y0 >> 4; my <= (y1 - 1) >> 4; my++)
        for (mx = x0 >> 4; mx <= (x1 - 1) >> 4; mx++) {
            conv[my * MBW + mx] = 1;
            if (!s->dirty[my * MBW + mx]) {
                s->dirty[my * MBW + mx] = 1;
                s->ndirty++;
            }
        }
}

/* Which macroblocks an overlay actually shows in: where one of its pixels
 * is not the transparent one. (The desktop's FmBackground covers the whole
 * screen and is transparent nearly everywhere; marking its rectangle would
 * send every macroblock through the slow converter.) Recomputed for the
 * macroblocks in [x0, x1) x [y0, y1). */
static void ov_update_map(int x0, int y0, int x1, int y1)
{
    int i, mx, my;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (my = y0 >> 4; my <= (y1 - 1) >> 4; my++)
        for (mx = x0 >> 4; mx <= (x1 - 1) >> 4; mx++) {
            int shows = 0;
            for (i = 0; i < ntops && !shows; i++) {
                toplevel *t = &tops[i];
                int ox = t->x + t->bw, oy = t->y + t->bw, a0, b0, a1, b1, a, b;
                if (!t->mapped || !t->overlay || !t->opix)
                    continue;
                a0 = mx * 16 - ox;
                b0 = my * 16 - oy;
                a1 = a0 + 16;
                b1 = b0 + 16;
                if (a0 < 0) a0 = 0;
                if (b0 < 0) b0 = 0;
                if (a1 > t->ow) a1 = t->ow;
                if (b1 > t->oh) b1 = t->oh;
                for (b = b0; b < b1 && !shows; b++)
                    for (a = a0; a < a1; a++)
                        if (t->opix[b * t->ow + a] != ov_transparent) {
                            shows = 1;
                            break;
                        }
            }
            ovmap[my * MBW + mx] = (uint8_t)shows;
        }
}

static void ov_rebuild_map(void)
{
    int i;
    noverlays = 0;
    for (i = 0; i < ntops; i++)
        if (tops[i].mapped && tops[i].overlay && tops[i].opix)
            noverlays++;
    if (!noverlays) {
        memset(ovmap, 0, (size_t)(MBW * MBH));
        return;
    }
    ov_update_map(0, 0, W, H);
}

/* Colours of an overlay window's colormap, as Y, U, V. */
static void ov_colors(toplevel *t)
{
    XColor c[4];
    int i;
    for (i = 0; i < 4; i++)
        c[i].pixel = (unsigned long)i;
    XQueryColors(dpy, t->ocmap, c, 4);
    for (i = 0; i < 4; i++) {
        int r = gtab[c[i].red >> 8], g = gtab[c[i].green >> 8], b = gtab[c[i].blue >> 8];
        int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        t->oy[i] = (uint8_t)(y < 0 ? 0 : y > 255 ? 255 : y);
        t->ou[i] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
        t->ov[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

/* What kind of window is this top-level? One round trip, once per mapping. */
static void top_learn(toplevel *t)
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, t->w, &a))
        return;
    t->known = 1;
    t->bw = a.border_width;
    /* InputOnly windows (4Dwm has several) have no pixels; a menu is a
     * small InputOutput window of the popup planes' depth. */
    t->overlay = a.class == InputOutput && a.depth > 0 &&
                 a.depth < DefaultDepth(dpy, DefaultScreen(dpy)) && a.depth <= 4 &&
                 a.map_state == IsViewable && a.colormap != None && a.width <= W &&
                 a.height <= H;
    if (!t->overlay)
        return;
    t->ocmap = a.colormap;
    if (!t->opix || t->ow != a.width || t->oh != a.height) {
        free(t->opix);
        t->ow = a.width;
        t->oh = a.height;
        t->opix = (uint8_t *)calloc((size_t)t->ow * t->oh, 1);
    }
    t->depth = a.depth;
    t->ovis = a.visual;
    ov_colors(t);
    /* A menu is read at once. A big overlay (the desktop's FmBackground is
     * one, the whole screen, and transparent) is read only where something
     * is drawn on it: a whole one is seconds of reading. */
    t->odirty = (long)a.width * a.height <= 128L * 1024;
    t->dx0 = t->x;
    t->dy0 = t->y;
    t->dx1 = t->x + t->wd;
    t->dy1 = t->y + t->ht;
    ovwin_drop(t->w);
    ovwin_add_tree(t->w, t->w, 0);
    cdv_debug("overlay window 0x%lx: %dx%d+%d+%d depth %d", (unsigned long)t->w, t->ow, t->oh,
              t->x, t->y, a.depth);
}

static void top_forget_overlay(toplevel *t)
{
    ovwin_drop(t->w);
    free(t->opix);
    t->opix = NULL;
    t->overlay = 0;
    t->known = 0;
}

/* Something changed inside a window at (x, y) whose size we do not know:
 * repaint every mapped top-level that holds the point. */
static void mark_toplevels_at(int x, int y)
{
    if (in_overlay && cur_ov) {
        mark(cur_ov->x, cur_ov->y, cur_ov->wd, cur_ov->ht);
        return;
    }
    /* The smallest that holds it: the desktop's own full-screen windows
     * (FmBackground) hold every point, and would turn every change into a
     * read of the whole screen. */
    toplevel *best = NULL;
    int i;
    for (i = 0; i < ntops; i++) {
        toplevel *t = &tops[i];
        if (t->mapped && x >= t->x && y >= t->y && x < t->x + t->wd && y < t->y + t->ht &&
            (!best || t->wd * t->ht < best->wd * best->ht))
            best = t;
    }
    if (best && best->wd * best->ht < W * H)
        top_mark(best);
    else
        mark(x - 32, y - 32, 96, 96);
}

static void top_load(void)
{
    Window r, p, *kids = NULL;
    unsigned int n, i;
    ntops = 0;
    if (!XQueryTree(dpy, root, &r, &p, &kids, &n))
        return;
    for (i = 0; i < n; i++) {
        XWindowAttributes a;
        toplevel *t;
        if (!XGetWindowAttributes(dpy, kids[i], &a))
            continue;
        t = top_add(kids[i]);
        if (!t)
            break;
        t->x = a.x;
        t->y = a.y;
        t->wd = a.width + 2 * a.border_width;
        t->ht = a.height + 2 * a.border_width;
        t->mapped = a.map_state != IsUnmapped;
    }
    if (kids)
        XFree(kids);
}

/* Drawables: is this ID a window, and where is it; or a pixmap, to ignore.
 * Filled from what XTrap says as requests go by, and by asking. */
enum { D_UNKNOWN, D_WINDOW, D_PIXMAP };
typedef struct {
    XID id;
    uint8_t kind;
    short x, y;
} drawable;
#define DCACHE 2048
static drawable dcache[DCACHE];

static drawable *dc_slot(XID id)
{
    unsigned h = (unsigned)(id * 2654435761u) >> 21; /* 11 bits */
    int i;
    for (i = 0; i < 8; i++) {
        drawable *d = &dcache[(h + i) & (DCACHE - 1)];
        if (d->id == id || d->kind == D_UNKNOWN)
            return d;
    }
    return &dcache[h & (DCACHE - 1)]; /* evict */
}

static void dc_set(XID id, int kind, int x, int y)
{
    drawable *d = dc_slot(id);
    d->id = id;
    d->kind = (uint8_t)kind;
    d->x = (short)x;
    d->y = (short)y;
}

static drawable *dc_get(XID id)
{
    drawable *d = dc_slot(id);
    return d->id == id && d->kind != D_UNKNOWN ? d : NULL;
}

/* Windows moved: every remembered window position may be stale. */
static void dc_forget_windows(void)
{
    int i;
    for (i = 0; i < DCACHE; i++)
        if (dcache[i].kind == D_WINDOW)
            dcache[i].kind = D_UNKNOWN;
}

static int x_error;
static int (*prev_handler)(Display *, XErrorEvent *);
static int quiet_errors(Display *d, XErrorEvent *e)
{
    (void)d;
    x_error = e->error_code;
    return 0;
}

/* Ask the server. A pixmap fails XTranslateCoordinates with BadWindow. */
static drawable *dc_classify(XID id)
{
    Window child;
    int x = 0, y = 0;
    XSync(dpy, False);
    x_error = 0;
    prev_handler = XSetErrorHandler(quiet_errors);
    if (!XTranslateCoordinates(dpy, (Window)id, root, 0, 0, &x, &y, &child))
        x_error = BadWindow;
    XSync(dpy, False);
    XSetErrorHandler(prev_handler);
    dc_set(id, x_error ? D_PIXMAP : D_WINDOW, x, y);
    return dc_get(id);
}

static drawable *dc_lookup(XID id)
{
    drawable *d = dc_get(id);
    return d ? d : dc_classify(id);
}

/* ---- what a request drew ------------------------------------------------------ */

static int s16(const uint8_t *p) { return (short)(p[0] << 8 | p[1]); }
static int u16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static XID u32(const uint8_t *p)
{
    return (XID)p[0] << 24 | (XID)p[1] << 16 | (XID)p[2] << 8 | p[3];
}

typedef struct {
    int x0, y0, x1, y1, any;
} bbox;

static void bb_add(bbox *b, int x0, int y0, int x1, int y1)
{
    if (!b->any) {
        b->x0 = x0;
        b->y0 = y0;
        b->x1 = x1;
        b->y1 = y1;
        b->any = 1;
        return;
    }
    if (x0 < b->x0) b->x0 = x0;
    if (y0 < b->y0) b->y0 = y0;
    if (x1 > b->x1) b->x1 = x1;
    if (y1 > b->y1) b->y1 = y1;
}

static void bb_mark(const bbox *b, int ox, int oy, int pad)
{
    if (b->any)
        mark(ox + b->x0 - pad, oy + b->y0 - pad, b->x1 - b->x0 + 2 * pad,
             b->y1 - b->y0 + 2 * pad);
}

/* Text: the font is not known here, so be generous. */
#define TEXT_ASCENT 24
#define TEXT_HEIGHT 32
#define TEXT_CHAR 16

static unsigned long n_trapped, n_ignored;

static void on_request2(XETC *t, XETrapDatum *d, BYTE *user);
static void on_request(XETC *t, XETrapDatum *d, BYTE *user)
{
    on_request2(t, d, user);
    in_overlay = 0;
    cur_ov = NULL;
}

static void on_request2(XETC *t, XETrapDatum *d, BYTE *user)
{
    const uint8_t *r = (const uint8_t *)&d->u.req;
    int op = r[0], reqlen = u16(r + 2) * 4;
    int have = (int)d->hdr.count - TRAP_HDR, wx = d->hdr.win_x, wy = d->hdr.win_y;
    int complete, i;
    XID id = u32(r + 4);
    drawable *dr;
    bbox b;
    (void)t;
    (void)user;
    if (have > reqlen)
        have = reqlen;
    complete = have >= reqlen;
    n_trapped++;
    memset(&b, 0, sizeof b);

    switch (op) {
    case X_SetSelectionOwner:
        clip_owner_changed((Atom)u32(r + 8), (Window)id);
        return;
    case X_CreatePixmap:
        dc_set(u32(r + 4), D_PIXMAP, 0, 0);
        return;
    case X_FreePixmap:
        dc_set(id, D_UNKNOWN, 0, 0);
        return;
    case X_StoreColors: case X_StoreNamedColor: case X_InstallColormap:
    case X_UninstallColormap: case X_AllocColor: case X_AllocNamedColor:
        cmap_dirty = 1;
        return;
    case X_ConfigureWindow: case X_ReparentWindow: case X_CirculateWindow:
        dc_forget_windows();
        /* fall through */
    case X_MapWindow: case X_UnmapWindow: case X_DestroyWindow:
    case X_MapSubwindows: case X_UnmapSubwindows: case X_DestroySubwindows:
        if (id != root)
            mark_toplevels_at(wx, wy);
        return;
    case X_CopyArea:
    case X_CopyPlane: {
        XID dst = u32(r + 8);
        if (id != root && (wx || wy))
            dc_set(id, D_WINDOW, wx, wy);
        if (novwin && ov_drawn(dst)) {
            /* a copy onto an overlay: its whole box, as its origin is
             * not in this report */
            toplevel *o = cur_ov;
            in_overlay = 1;
            mark(o->x, o->y, o->wd, o->ht);
            return;
        }
        dr = dst == root ? NULL : dc_lookup(dst);
        if (dr && dr->kind == D_PIXMAP) {
            n_ignored++;
            return;
        }
        {
            int ox = dr ? dr->x : 0, oy = dr ? dr->y : 0;
            int x = ox + s16(r + 20), y = oy + s16(r + 22), w = u16(r + 24), h = u16(r + 26);
            mark(x, y, w, h);
            /* A copy from a window on the screen (not a pixmap): the
             * contents moved, which the encoder can send as a vector. */
            if (op == X_CopyArea && (id == root || dc_get(id)) &&
                (id == root || dc_get(id)->kind == D_WINDOW)) {
                drawable *sd = id == root ? NULL : dc_get(id);
                int sx = (sd ? sd->x : 0) + s16(r + 16), sy = (sd ? sd->y : 0) + s16(r + 18);
                add_motion(x, y, w, h, x - sx, y - sy);
            }
        }
        return;
    }
    }

    /* The rest draw on the drawable at r+4: on an overlay, what they draw
     * is that overlay's damage (mark() sends it there), at the position
     * XTrap gives, like any window. */
    if (novwin && ov_drawn(id)) {
        in_overlay = 1;
        dr = NULL;
        goto shape;
    }
    if (id == root) {
        wx = wy = 0;
    } else {
        dr = dc_get(id);
        if (dr && dr->kind == D_PIXMAP) {
            n_ignored++;
            return;
        }
        if (!dr && !wx && !wy)
            dr = dc_classify(id);
        else
            dc_set(id, D_WINDOW, wx, wy);
        if (dr && dr->kind == D_PIXMAP) {
            n_ignored++;
            return;
        }
        if (dr) {
            wx = dr->x;
            wy = dr->y;
        }
    }

shape:
    switch (op) {
    case X_ClearArea: {
        int w = u16(r + 12), h = u16(r + 14);
        if (!w || !h)
            mark_toplevels_at(wx + s16(r + 8), wy + s16(r + 10));
        else
            mark(wx + s16(r + 8), wy + s16(r + 10), w, h);
        return;
    }
    case X_PutImage:
        mark(wx + s16(r + 16), wy + s16(r + 18), u16(r + 12), u16(r + 14));
        return;
    case X_PolyPoint:
    case X_PolyLine: {
        int prev = r[1] == CoordModePrevious, x = 0, y = 0;
        for (i = 12; i + 4 <= have; i += 4) {
            if (prev && i > 12) {
                x += s16(r + i);
                y += s16(r + i + 2);
            } else {
                x = s16(r + i);
                y = s16(r + i + 2);
            }
            bb_add(&b, x, y, x + 1, y + 1);
        }
        if (complete) {
            bb_mark(&b, wx, wy, 8);
            return;
        }
        break;
    }
    case X_FillPoly: {
        int prev = r[13] == CoordModePrevious, x = 0, y = 0;
        for (i = 16; i + 4 <= have; i += 4) {
            if (prev && i > 16) {
                x += s16(r + i);
                y += s16(r + i + 2);
            } else {
                x = s16(r + i);
                y = s16(r + i + 2);
            }
            bb_add(&b, x, y, x + 1, y + 1);
        }
        if (complete) {
            bb_mark(&b, wx, wy, 1);
            return;
        }
        break;
    }
    case X_PolySegment:
        for (i = 12; i + 8 <= have; i += 8) {
            int x1 = s16(r + i), y1 = s16(r + i + 2), x2 = s16(r + i + 4), y2 = s16(r + i + 6);
            bb_add(&b, x1 < x2 ? x1 : x2, y1 < y2 ? y1 : y2, (x1 > x2 ? x1 : x2) + 1,
                   (y1 > y2 ? y1 : y2) + 1);
        }
        if (complete) {
            bb_mark(&b, wx, wy, 8);
            return;
        }
        break;
    case X_PolyRectangle:
    case X_PolyFillRectangle:
        for (i = 12; i + 8 <= have; i += 8)
            bb_add(&b, s16(r + i), s16(r + i + 2), s16(r + i) + u16(r + i + 4) + 1,
                   s16(r + i + 2) + u16(r + i + 6) + 1);
        if (complete) {
            bb_mark(&b, wx, wy, op == X_PolyRectangle ? 8 : 0);
            return;
        }
        break;
    case X_PolyArc:
    case X_PolyFillArc:
        for (i = 12; i + 12 <= have; i += 12)
            bb_add(&b, s16(r + i), s16(r + i + 2), s16(r + i) + u16(r + i + 4) + 1,
                   s16(r + i + 2) + u16(r + i + 6) + 1);
        if (complete) {
            bb_mark(&b, wx, wy, op == X_PolyArc ? 8 : 1);
            return;
        }
        break;
    case X_ImageText8:
    case X_ImageText16:
    case X_PolyText8:
    case X_PolyText16: {
        int x = s16(r + 12), y = s16(r + 14), n;
        if (op == X_ImageText8)
            n = r[1];
        else if (op == X_ImageText16)
            n = r[1];
        else
            n = (reqlen - 16) / (op == X_PolyText8 ? 1 : 2); /* items carry 2 bytes each */
        mark(wx + x - 2, wy + y - TEXT_ASCENT, n * TEXT_CHAR + 4, TEXT_HEIGHT);
        return;
    }
    default:
        return;
    }
    /* Too long to see whole: the part we saw, and its window. */
    bb_mark(&b, wx, wy, 8);
    mark_toplevels_at(wx, wy);
}

static void on_event(cdv_screen *s, XEvent *e)
{
    toplevel *t;
    switch (e->type) {
    case CreateNotify:
        if (e->xcreatewindow.parent != root)
            return;
        t = top_add(e->xcreatewindow.window);
        if (t) {
            t->x = e->xcreatewindow.x;
            t->y = e->xcreatewindow.y;
            t->wd = e->xcreatewindow.width + 2 * e->xcreatewindow.border_width;
            t->ht = e->xcreatewindow.height + 2 * e->xcreatewindow.border_width;
            t->mapped = 0;
        }
        return;
    case ConfigureNotify:
        if (e->xconfigure.event != root)
            return;
        t = top_find(e->xconfigure.window);
        if (!t)
            return;
        top_mark(t);
        if (t->mapped && t->overlay)
            force(s, t->x, t->y, t->wd, t->ht);
        if (t->x != e->xconfigure.x || t->y != e->xconfigure.y) {
            dc_forget_windows();
            /* moved, not resized: its picture is where it was, shifted */
            if (t->mapped && !t->overlay &&
                t->wd == e->xconfigure.width + 2 * e->xconfigure.border_width &&
                t->ht == e->xconfigure.height + 2 * e->xconfigure.border_width)
                add_motion(e->xconfigure.x, e->xconfigure.y, t->wd, t->ht,
                           e->xconfigure.x - t->x, e->xconfigure.y - t->y);
        }
        t->x = e->xconfigure.x;
        t->y = e->xconfigure.y;
        t->wd = e->xconfigure.width + 2 * e->xconfigure.border_width;
        t->ht = e->xconfigure.height + 2 * e->xconfigure.border_width;
        top_mark(t);
        if (t->mapped && t->overlay) {
            top_learn(t);
            ov_rebuild_map();
            force(s, t->x, t->y, t->wd, t->ht);
        }
        return;
    case MapNotify:
        if (e->xmap.event != root)
            return;
        t = top_find(e->xmap.window);
        if (t) {
            t->mapped = 1;
            top_learn(t);
            top_mark(t);
            if (t->overlay) {
                ov_rebuild_map();
                force(s, t->x, t->y, t->wd, t->ht);
            }
        }
        return;
    case UnmapNotify:
        if (e->xunmap.event != root)
            return;
        t = top_find(e->xunmap.window);
        if (t) {
            top_mark(t);
            t->mapped = 0;
            if (t->overlay) {
                force(s, t->x, t->y, t->wd, t->ht);
                top_forget_overlay(t);
                ov_rebuild_map();
            }
        }
        return;
    case DestroyNotify:
        if (e->xdestroywindow.event != root)
            return;
        t = top_find(e->xdestroywindow.window);
        if (t) {
            int was = t->mapped && t->overlay;
            top_mark(t);
            if (was)
                force(s, t->x, t->y, t->wd, t->ht);
            top_del(e->xdestroywindow.window);
            if (was)
                ov_rebuild_map();
        }
        dc_set(e->xdestroywindow.window, D_UNKNOWN, 0, 0);
        return;
    case ReparentNotify:
        if (e->xreparent.event != root)
            return;
        if (e->xreparent.parent == root) {
            XWindowAttributes a;
            if (XGetWindowAttributes(dpy, e->xreparent.window, &a)) {
                t = top_add(e->xreparent.window);
                if (t) {
                    t->x = a.x;
                    t->y = a.y;
                    t->wd = a.width + 2 * a.border_width;
                    t->ht = a.height + 2 * a.border_width;
                    t->mapped = a.map_state != IsUnmapped;
                    top_mark(t);
                }
            }
        } else {
            t = top_find(e->xreparent.window);
            if (t) {
                top_mark(t);
                top_del(e->xreparent.window);
            }
        }
        dc_forget_windows();
        return;
    case CirculateNotify:
    case GravityNotify:
        t = top_find(e->xany.window);
        if (t)
            top_mark(t);
        return;
    }
}

/* ---- colour ---------------------------------------------------------------- */

/* IRIX loads the video hardware with every colour raised to 1/gamma (1.7 by
 * default) and the monitor applies about 2.5; a PC shows sRGB at about 2.2.
 * So the colour a person at the Indigo sees is the X value to the power
 * 2.5 / (gamma * 2.2) on a PC -- midtones markedly lighter than the raw values. */
static void gamma_build(double gamma)
{
    int i;
    double e = gamma > 0 ? 2.5 / (gamma * 2.2) : 1.0;
    for (i = 0; i < 256; i++)
        gtab[i] = (uint8_t)(255.0 * pow(i / 255.0, e) + 0.5);
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* Read the installed colormap. Returns how many entries changed, and marks
 * changed[i]. */
static int cmap_load(uint8_t changed[256])
{
    XColor c[256];
    Colormap *list, cm = DefaultColormap(dpy, DefaultScreen(dpy));
    int n = 0, i, k, nchanged = 0;
    /* The installed list holds the overlay planes' maps too (2 and 4 bits on
     * an Indigo), which refuse a query for 256 entries: take the first that
     * answers. */
    list = XListInstalledColormaps(dpy, root, &n);
    XSync(dpy, False);
    prev_handler = XSetErrorHandler(quiet_errors);
    /* The default colormap if it is installed -- most windows use it, and
     * the list's order changes as focus moves -- else the first that
     * answers. */
    for (k = 0; k < n; k++)
        if (list[k] == DefaultColormap(dpy, DefaultScreen(dpy)))
            break;
    if (k < n) {
        Colormap t = list[0];
        list[0] = list[k];
        list[k] = t;
    }
    for (k = 0; k <= n; k++) {
        Colormap try = k < n ? list[k] : DefaultColormap(dpy, DefaultScreen(dpy));
        for (i = 0; i < 256; i++)
            c[i].pixel = (unsigned long)i;
        x_error = 0;
        XQueryColors(dpy, try, c, 256);
        XSync(dpy, False);
        if (!x_error) {
            cm = try;
            break;
        }
    }
    XSetErrorHandler(prev_handler);
    if (list)
        XFree(list);
    if (cm != cmap_cur)
        cdv_debug("colormap 0x%lx (%d installed)", (unsigned long)cm, n);
    cmap_cur = cm;
    for (i = 0; i < 256; i++) {
        int r = gtab[c[i].red >> 8], g = gtab[c[i].green >> 8], b = gtab[c[i].blue >> 8];
        uint8_t y = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        uint8_t u = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
        uint8_t v = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        changed[i] = y != ty[i] || u != tu[i] || v != tv[i];
        nchanged += changed[i];
        ty[i] = y;
        tu[i] = u;
        tv[i] = v;
    }
    return nchanged;
}

/* ---- conversion ------------------------------------------------------------ */

/* A macroblock with an overlay window over some of it: per pixel, the
 * overlay's colour where it is not transparent, the screen's elsewhere; the
 * last overlay in the list (the most recently stacked) wins. */
static void convert_mb_overlaid(cdv_screen *s, int mx, int my)
{
    uint8_t Yb[256], Ub[256], Vb[256];
    const toplevel *ov[8];
    int nov = 0, i, j, k, x0 = mx * 16, y0 = my * 16;
    for (i = 0; i < ntops && nov < 8; i++) {
        const toplevel *t = &tops[i];
        if (t->mapped && t->overlay && t->opix && x0 < t->x + t->wd && y0 < t->y + t->ht &&
            x0 + 16 > t->x && y0 + 16 > t->y)
            ov[nov++] = t;
    }
    for (j = 0; j < 16; j++)
        for (i = 0; i < 16; i++) {
            int x = x0 + i, y = y0 + j, p = fb[y * W + x], o = j * 16 + i;
            Yb[o] = ty[p];
            Ub[o] = tu[p];
            Vb[o] = tv[p];
            for (k = 0; k < nov; k++) {
                const toplevel *t = ov[k];
                int ox = x - t->x - t->bw, oy = y - t->y - t->bw, v;
                if (ox < 0 || oy < 0 || ox >= t->ow || oy >= t->oh)
                    continue;
                v = t->opix[oy * t->ow + ox];
                if ((unsigned long)v == ov_transparent)
                    continue;
                Yb[o] = t->oy[v & 3];
                Ub[o] = t->ou[v & 3];
                Vb[o] = t->ov[v & 3];
            }
        }
    for (j = 0; j < 16; j++)
        memcpy(s->y + (y0 + j) * s->ystride + x0, Yb + j * 16, 16);
    for (j = 0; j < 8; j++)
        for (i = 0; i < 8; i++) {
            int a = 2 * j * 16 + 2 * i;
            s->u[(y0 / 2 + j) * s->uvstride + x0 / 2 + i] =
                (uint8_t)((Ub[a] + Ub[a + 1] + Ub[a + 16] + Ub[a + 17] + 2) >> 2);
            s->v[(y0 / 2 + j) * s->uvstride + x0 / 2 + i] =
                (uint8_t)((Vb[a] + Vb[a + 1] + Vb[a + 16] + Vb[a + 17] + 2) >> 2);
        }
}

static void convert_mb(cdv_screen *s, int mx, int my)
{
    if (ovmap[my * MBW + mx]) {
        s->flat[my * MBW + mx] = 0;
        convert_mb_overlaid(s, mx, my);
        return;
    }
    {
        /* one colour? (words of four equal pixels, all the same word) */
        const uint32_t *w = (const uint32_t *)(fb + (my * 16) * W + mx * 16);
        uint32_t first = w[0];
        int j, flat = (first & 0xff) * 0x01010101u == first;
        for (j = 0; j < 16 && flat; j++, w += W / 4)
            flat = w[0] == first && w[1] == first && w[2] == first && w[3] == first;
        s->flat[my * MBW + mx] = (uint8_t)flat;
    }
    {
    const uint8_t *src = fb + (my * 16) * W + mx * 16;
    uint8_t *yp = s->y + (my * 16) * s->ystride + mx * 16;
    uint8_t *up = s->u + (my * 8) * s->uvstride + mx * 8;
    uint8_t *vp = s->v + (my * 8) * s->uvstride + mx * 8;
    int j, i;
    for (j = 0; j < 8; j++) {
        const uint8_t *a = src + 2 * j * W, *b = a + W;
        uint8_t *y0 = yp + 2 * j * s->ystride, *y1 = y0 + s->ystride;
        uint8_t *uo = up + j * s->uvstride, *vo = vp + j * s->uvstride;
        for (i = 0; i < 16; i += 2) {
            int p = a[i], q = a[i + 1], r = b[i], t = b[i + 1];
            y0[i] = ty[p];
            y0[i + 1] = ty[q];
            y1[i] = ty[r];
            y1[i + 1] = ty[t];
            if (p == q && p == r && p == t) {
                uo[i >> 1] = tu[p];
                vo[i >> 1] = tv[p];
            } else {
                uo[i >> 1] = (uint8_t)((tu[p] + tu[q] + tu[r] + tu[t] + 2) >> 2);
                vo[i >> 1] = (uint8_t)((tv[p] + tv[q] + tv[r] + tv[t] + 2) >> 2);
            }
        }
    }
    }
}

/* ---- readback ---------------------------------------------------------------- */

/* Read a macroblock-aligned rectangle (in macroblocks) and fold it into fb:
 * a macroblock that differs is copied and marked dirty. */
static void read_rect(cdv_screen *s, int mx0, int my0, int mx1, int my1)
{
    int x = mx0 * 16, y = my0 * 16, w = (mx1 - mx0) * 16, h = (my1 - my0) * 16;
    int mx, my, j;
    XImage *im;
    if (x + w > W)
        w = W - x;
    if (y + h > H)
        h = H - y;
    im = XShmCreateImage(dpy, vis, 8, ZPixmap, (char *)shm.shmaddr, &shm, (unsigned)w,
                         (unsigned)h);
    if (!im)
        return;
    if (!XShmGetImage(dpy, root, im, x, y, AllPlanes)) {
        XFree(im);
        return;
    }
    s->reads++;
    s->read_px += w * h;
    for (my = my0; my < my1; my++)
        for (mx = mx0; mx < mx1; mx++) {
            const uint8_t *src = (const uint8_t *)im->data + ((my - my0) * 16) * im->bytes_per_line +
                                 (mx - mx0) * 16;
            uint8_t *dst = fb + (my * 16) * W + mx * 16;
            int diff = 0;
            for (j = 0; j < 16 && !diff; j++) {
                const uint32_t *a = (const uint32_t *)(src + j * im->bytes_per_line);
                const uint32_t *b = (const uint32_t *)(dst + j * W);
                diff = (a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]);
            }
            if (!diff)
                continue;
            for (j = 0; j < 16; j++)
                memcpy(dst + j * W, src + j * im->bytes_per_line, 16);
            conv[my * MBW + mx] = 1;
            if (!s->dirty[my * MBW + mx]) {
                s->dirty[my * MBW + mx] = 1;
                s->ndirty++;
            }
        }
    XFree(im);
}

/* The damaged macroblocks as few rectangles as is worth it: a request costs
 * about as much as 7000 pixels, so a row's damage is one span unless the gap is
 * wide, and a row joins the rectangle above when that costs fewer pixels than a
 * request would. */
#define REQUEST_PX 7000
static void read_damage(cdv_screen *s)
{
    int my, rx0 = -1, rx1 = -1, ry0 = -1;
    for (my = 0; my <= MBH; my++) {
        int a = -1, b = -1, mx;
        if (my < MBH)
            for (mx = 0; mx < MBW; mx++)
                if (dmg[my * MBW + mx]) {
                    if (a < 0)
                        a = mx;
                    else if ((mx - b - 1) * 256 > REQUEST_PX) {
                        /* a wide gap: read what came before on its own */
                        read_rect(s, a, my, b + 1, my + 1);
                        a = mx;
                    }
                    b = mx;
                }
        if (rx0 >= 0) {
            int ua = a < 0 ? rx0 : (a < rx0 ? a : rx0), ub = b < 0 ? rx1 : (b > rx1 ? b : rx1);
            int extra = (ub - ua + 1) * (my - ry0 + 1) - (rx1 - rx0 + 1) * (my - ry0) -
                        (a < 0 ? 0 : b - a + 1);
            if (a >= 0 && extra * 256 <= REQUEST_PX) {
                rx0 = ua;
                rx1 = ub;
                continue;
            }
            read_rect(s, rx0, ry0, rx1 + 1, my);
            rx0 = -1;
        }
        if (a >= 0) {
            rx0 = a;
            rx1 = b;
            ry0 = my;
        }
    }
    memset(dmg, 0, (size_t)(MBW * MBH));
    ndmg = 0;
}

/* ---- the interface -------------------------------------------------------------- */

static int open_shm(void)
{
    shm.shmid = shmget(IPC_PRIVATE, (size_t)W * H, IPC_CREAT | 0600);
    if (shm.shmid < 0)
        return 0;
    shm.shmaddr = (char *)shmat(shm.shmid, 0, 0);
    shm.readOnly = False;
    if (shm.shmaddr == (char *)-1)
        return 0;
    x_error = 0;
    prev_handler = XSetErrorHandler(quiet_errors);
    XShmAttach(dpy, &shm);
    XSync(dpy, False);
    XSetErrorHandler(prev_handler);
    /* Marked for removal now, so it goes away when we do, whatever happens. */
    shmctl(shm.shmid, IPC_RMID, 0);
    return !x_error;
}

/* Any other error: a window that went away between a report and a query,
 * most often. Log it and carry on -- Xlib's default handler exits. */
static int log_errors(Display *d, XErrorEvent *e)
{
    char msg[80];
    XGetErrorText(d, e->error_code, msg, sizeof msg);
    cdv_debug("X error: %s (request %d.%d, id 0x%lx)", msg, e->request_code, e->minor_code,
              (unsigned long)e->resourceid);
    return 0;
}

/* Buffers sized to the screen, kept across a lost and reopened display. */
static uint8_t *arena;
static int arena_w, arena_h;

int screen_open(cdv_screen *s, const char *display, double gamma)
{
    int scr, a, b, maj;
    uint8_t changed[256];
    dpy = XOpenDisplay(display);
    if (!dpy) {
        cdv_debug("cannot open display %s", display ? display : "(DISPLAY)");
        return 0;
    }
    XSetErrorHandler(log_errors);
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    vis = DefaultVisual(dpy, scr);
    W = DisplayWidth(dpy, scr);
    H = DisplayHeight(dpy, scr);
    if (DefaultDepth(dpy, scr) != 8) {
        cdv_log("the root window is %d bits deep; only 8 is supported yet",
                DefaultDepth(dpy, scr));
        XCloseDisplay(dpy);
        dpy = NULL;
        return 0;
    }
    if (!XShmQueryExtension(dpy) || !(shm_ok = open_shm())) {
        cdv_log("MIT-SHM is not usable; cannot read the screen");
        XCloseDisplay(dpy);
        dpy = NULL;
        return 0;
    }
    if (!XQueryExtension(dpy, "DEC-XTRAP", &a, &b, &maj))
        cdv_log("DEC-XTRAP is missing: changes will be found by scanning only (slow)");
    MBW = (W + 15) / 16;
    MBH = (H + 15) / 16;
    if (!arena || arena_w != W || arena_h != H) {
        /* One arena, the buffers at different offsets modulo the R3000's
         * 32 KB direct-mapped data cache: the screen and the luma plane are
         * the same size, a multiple of 32 KB, and read in step, so placed
         * naively every access to one would evict the other. */
        size_t fbsz = (size_t)MBW * 16 * MBH * 16, ysz = fbsz, uvsz = fbsz / 4;
        int n = MBW * MBH;
        free(arena);
        arena = (uint8_t *)malloc(fbsz + ysz + 2 * uvsz + 6 * 32768);
        free(dmg);
        free(conv);
        free(ovmap);
        free(hint_next);
        free(hint_frame);
        free(hint_tmp);
        free(s->flat);
        free(s->dirty);
        dmg = (uint8_t *)calloc((size_t)n, 1);
        conv = (uint8_t *)calloc((size_t)n, 1);
        ovmap = (uint8_t *)calloc((size_t)n, 1);
        hint_next = (vp8e_hint *)calloc((size_t)n, sizeof(vp8e_hint));
        hint_frame = (vp8e_hint *)calloc((size_t)n, sizeof(vp8e_hint));
        hint_tmp = (vp8e_hint *)calloc((size_t)n, sizeof(vp8e_hint));
        s->flat = (uint8_t *)calloc((size_t)n, 1);
        s->dirty = (uint8_t *)calloc((size_t)n, 1);
        if (!arena || !dmg || !conv || !ovmap || !hint_next || !hint_frame || !hint_tmp ||
            !s->flat || !s->dirty) {
            cdv_log("out of memory");
            exit(1);
        }
        arena_w = W;
        arena_h = H;
    }
    {
        /* ...and fb half a cache away from the shared-memory segment the
         * server writes into, which shmat put on a page boundary: read_rect
         * compares the two row by row. */
        size_t fbsz = (size_t)MBW * 16 * MBH * 16, ysz = fbsz, uvsz = fbsz / 4;
        uint8_t *p = (uint8_t *)(((unsigned long)arena + 32767) & ~32767ul);
        p += (((unsigned long)shm.shmaddr & 32767) + 16384) & 32767;
        fb = p;
        s->y = fb + fbsz + 8192;
        s->u = s->y + ysz + 8192 + 4096;
        s->v = s->u + uvsz + 4096;
    }
    s->width = W;
    s->height = H;
    s->mbw = MBW;
    s->mbh = MBH;
    s->ystride = MBW * 16;
    s->uvstride = MBW * 8;
    s->hints = hint_frame;
    s->ndirty = 0;
    memset(s->dirty, 0, (size_t)(MBW * MBH));
    memset(dmg, 0, (size_t)(MBW * MBH));
    ndmg = 0;
    {
        /* SERVER_OVERLAY_VISUALS: (visual, transparent type, value, layer) */
        Atom a = XInternAtom(dpy, "SERVER_OVERLAY_VISUALS", True), type;
        int fmt;
        unsigned long n, after;
        unsigned char *data = NULL;
        if (a != None && XGetWindowProperty(dpy, root, a, 0, 64, False, AnyPropertyType, &type,
                                            &fmt, &n, &after, &data) == Success && data) {
            if (fmt == 32 && n >= 4 && ((long *)data)[1] == 1)
                ov_transparent = (unsigned long)((long *)data)[2];
            XFree(data);
        }
    }
    s->dirty = (uint8_t *)calloc((size_t)(MBW * MBH), 1);
    memset(fb, 0, (size_t)W * H);
    gamma_build(gamma);
    cmap_load(changed);
    cdv_log("screen %dx%d, 8-bit, colormap 0x%lx", W, H, (unsigned long)cmap_cur);
    return 1;
}

Display *screen_display(void) { return dpy; }

/* The connection died (the server reset at a logout, or went away): give up
 * on it without touching Xlib -- after an I/O error the Display is beyond use,
 * and XCloseDisplay on it has been seen (on IRIX 6.5) to poison the next
 * connection. The socket is closed; the struct is left. */
void screen_lost(void)
{
    int i;
    if (dpy)
        close(ConnectionNumber(dpy));
    dpy = NULL;
    tc = NULL;
    watching = 0;
    if (shm_ok) {
        shmdt(shm.shmaddr);
        shm_ok = 0;
    }
    for (i = 0; i < ntops; i++)
        free(tops[i].opix);
    ntops = 0;
    noverlays = 0;
    novwin = 0;
    cmap_cur = 0;
    memset(ty, 0, sizeof ty);
    memset(tu, 0, sizeof tu);
    memset(tv, 0, sizeof tv);
}
int screen_fd(void) { return dpy ? ConnectionNumber(dpy) : -1; }

static void trap_on(void)
{
    static const int ops[] = {
        X_SetSelectionOwner, X_CreatePixmap, X_FreePixmap, X_StoreColors, X_StoreNamedColor, X_InstallColormap,
        X_UninstallColormap, X_AllocColor, X_AllocNamedColor, X_ConfigureWindow,
        X_ReparentWindow, X_CirculateWindow, X_MapWindow, X_UnmapWindow, X_DestroyWindow,
        X_MapSubwindows, X_UnmapSubwindows, X_DestroySubwindows, X_CopyArea, X_CopyPlane,
        X_ClearArea, X_PutImage, X_PolyPoint, X_PolyLine, X_FillPoly, X_PolySegment,
        X_PolyRectangle, X_PolyFillRectangle, X_PolyArc, X_PolyFillArc, X_ImageText8,
        X_ImageText16, X_PolyText8, X_PolyText16, 0
    };
    ReqFlags req;
    int i;
    if (!tc) {
        tc = XECreateTC(dpy, 0L, NULL);
        if (!tc) {
            cdv_log("XTrap: cannot create a trap context; scanning only");
            return;
        }
        memset(req, 0, sizeof req);
        for (i = 0; ops[i]; i++)
            BitTrue(req, ops[i]);
        XETrapSetRequests(tc, True, req);
        XETrapSetMaxPacket(tc, True, TRAP_PACKET);
        XETrapSetWinXY(tc, True);
        XEAddRequestCBs(tc, req, (void_function)on_request, NULL);
    }
    XEStartTrapRequest(tc);
}

void screen_start(cdv_screen *s)
{
    uint8_t changed[256];
    int i;
    cmap_load(changed);
    top_load();
    for (i = 0; i < ntops; i++)
        if (tops[i].mapped)
            top_learn(&tops[i]);
    ov_rebuild_map();
    XSelectInput(dpy, root, SubstructureNotifyMask);
    memset(dcache, 0, sizeof dcache);
    trap_on();
    XSync(dpy, False);
    /* Everything, once: read the whole screen and convert it all. */
    memset(dmg, 1, (size_t)(MBW * MBH));
    memset(s->dirty, 0, (size_t)(MBW * MBH));
    s->ndirty = 0;
    memset(fb, 0, (size_t)W * H);
    read_rect(s, 0, 0, MBW, MBH);
    read_overlays(s);
    memset(dmg, 0, (size_t)(MBW * MBH));
    ndmg = 0;
    {
        int mx, my;
        for (my = 0; my < MBH; my++)
            for (mx = 0; mx < MBW; mx++)
                convert_mb(s, mx, my);
    }
    memset(conv, 0, (size_t)(MBW * MBH));
    screen_mark_all(s);
    watching = 1;
    scan_due = now_ms();
}

void screen_stop(void)
{
    if (!dpy || !watching)
        return;
    if (tc)
        XEStopTrapRequest(tc);
    XSelectInput(dpy, root, NoEventMask);
    XFlush(dpy);
    watching = 0;
}

void screen_events(cdv_screen *s)
{
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        /* Not XETrapDispatchEvent: it hands every other event to
         * XtDispatchEvent, and Xt exits ("Couldn't find per display
         * information") on a display it did not open. */
        if (tc && e.type == tc->eventBase + XETrapData) {
            XETrapDispatchXLib((XETrapDataEvent *)&e, tc);
            continue;
        }
        if (clip_event(&e))
            continue;
        if (watching)
            on_event(s, &e);
    }
}

int screen_pending(const cdv_screen *s, uint32_t now)
{
    (void)s;
    return ndmg || cmap_dirty || (int32_t)(now - scan_due) >= 0;
}

/* Overlay windows something drew on: read their own pixels (a plain
 * XGetImage of the window, 1 byte a pixel) and send what changed. */
static void read_overlays(cdv_screen *s)
{
    int i;
    for (i = 0; i < ntops; i++) {
        toplevel *t = &tops[i];
        int ox = t->x + t->bw, oy = t->y + t->bw, x0, y0, x1, y1, x, y;
        XImage *im;
        if (!t->mapped || !t->overlay || !t->opix || !t->odirty)
            continue;
        t->odirty = 0;
        /* the damaged box, in the window's coordinates, inside it and the screen */
        x0 = t->dx0 - ox;
        y0 = t->dy0 - oy;
        x1 = t->dx1 - ox;
        y1 = t->dy1 - oy;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > t->ow) x1 = t->ow;
        if (y1 > t->oh) y1 = t->oh;
        if (ox + x0 < 0) x0 = -ox;
        if (oy + y0 < 0) y0 = -oy;
        if (ox + x1 > W) x1 = W - ox;
        if (oy + y1 > H) y1 = H - oy;
        if (x0 >= x1 || y0 >= y1)
            continue;
        im = XShmCreateImage(dpy, t->ovis, (unsigned)t->depth, ZPixmap, (char *)shm.shmaddr,
                             &shm, (unsigned)(x1 - x0), (unsigned)(y1 - y0));
        if (im && !XShmGetImage(dpy, t->w, im, x0, y0, AllPlanes)) {
            XFree(im);
            im = NULL;
        }
        if (im)
            im->f.destroy_image = NULL; /* shared memory: not ours to free */
        else
            im = XGetImage(dpy, t->w, x0, y0, (unsigned)(x1 - x0), (unsigned)(y1 - y0),
                           AllPlanes, ZPixmap);
        if (!im)
            continue;
        s->reads++;
        s->read_px += (x1 - x0) * (y1 - y0);
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                uint8_t v = (uint8_t)(im->bits_per_pixel == 8
                                          ? ((uint8_t *)im->data)[(y - y0) * im->bytes_per_line + x - x0]
                                          : XGetPixel(im, x - x0, y - y0));
                uint8_t *d = &t->opix[y * t->ow + x];
                if (*d != v) {
                    *d = v;
                    force(s, ox + x, oy + y, 1, 1);
                }
            }
        if (im->f.destroy_image)
            XDestroyImage(im);
        else
            XFree(im);
        ov_update_map(ox + x0, oy + y0, ox + x1, oy + y1);
    }
}

void screen_collect(cdv_screen *s, int scan, uint32_t now)
{
    uint32_t t0 = now_ms(), t1;
    int mx, my, k;
    memcpy(hint_frame, hint_next, sizeof(vp8e_hint) * (size_t)(MBW * MBH));
    s->nhints = nhint_next;
    memset(hint_next, 0, sizeof(vp8e_hint) * (size_t)(MBW * MBH));
    nhint_next = 0;
    s->reads = s->read_px = 0;
    s->damaged = ndmg;
    s->scanned = 0;
    if (cmap_dirty) {
        uint8_t changed[256];
        int i;
        cmap_dirty = 0;
        for (i = 0; i < ntops; i++)
            if (tops[i].mapped && tops[i].overlay) {
                ov_colors(&tops[i]);
                force(s, tops[i].x, tops[i].y, tops[i].wd, tops[i].ht);
            }
        if ((k = cmap_load(changed)) != 0) {
            cdv_debug("colormap: %d entries changed", k);
            /* recolour every macroblock holding a changed index */
            for (my = 0; my < MBH; my++)
                for (mx = 0; mx < MBW; mx++) {
                    int j, i, hit = 0;
                    for (j = 0; j < 16 && !hit; j++) {
                        const uint8_t *p = fb + (my * 16 + j) * W + mx * 16;
                        for (i = 0; i < 16; i++)
                            hit |= changed[p[i]];
                    }
                    if (hit)
                        conv[my * MBW + mx] = 1;
                    if (hit && !s->dirty[my * MBW + mx]) {
                        s->dirty[my * MBW + mx] = 1;
                        s->ndirty++;
                    }
                }
        }
    }
    if (scan && (int32_t)(now - scan_due) >= 0) {
        for (k = 0; k < SCAN_ROWS && scan_row + k < MBH; k++)
            for (mx = 0; mx < MBW; mx++)
                if (!dmg[(scan_row + k) * MBW + mx]) {
                    dmg[(scan_row + k) * MBW + mx] = 1;
                    ndmg++;
                }
        s->scanned = k;
        scan_due = now + scan_every;
        scan_row += k;
        if (scan_row >= MBH)
            scan_row = 0;
    }
    if (ndmg)
        read_damage(s);
    if (noverlays)
        read_overlays(s);
    t1 = now_ms();
    s->read_ms = t1 - t0;
    for (my = 0; my < MBH; my++)
        for (mx = 0; mx < MBW; mx++)
            if (conv[my * MBW + mx]) {
                convert_mb(s, mx, my);
                conv[my * MBW + mx] = 0;
            }
    s->convert_ms = now_ms() - t1;
}

void screen_set_scan(uint32_t ms) { scan_every = ms; }

/* The planes are kept current for every macroblock, so a keyframe needs no
 * reading or converting: only everything marked. */
void screen_mark_all(cdv_screen *s)
{
    memset(s->dirty, 1, (size_t)(MBW * MBH));
    s->ndirty = MBW * MBH;
}

void screen_sent_some(cdv_screen *s, const uint8_t *sent)
{
    int i;
    for (i = 0; i < MBW * MBH; i++)
        if (sent[i] && s->dirty[i]) {
            s->dirty[i] = 0;
            s->ndirty--;
        }
}

void screen_background(uint8_t *y, uint8_t *u, uint8_t *v)
{
    static unsigned n[256];
    int i, best = 0;
    memset(n, 0, sizeof n);
    for (i = 0; i < W * H; i += 61)
        n[fb[i]]++;
    for (i = 1; i < 256; i++)
        if (n[i] > n[best])
            best = i;
    *y = ty[best];
    *u = tu[best];
    *v = tv[best];
}

void screen_sent(cdv_screen *s)
{
    memset(s->dirty, 0, (size_t)(MBW * MBH));
    s->ndirty = 0;
}

int screen_pointer(int *x, int *y)
{
    Window r, c;
    int wx, wy;
    unsigned int m;
    return XQueryPointer(dpy, root, &r, &c, x, y, &wx, &wy, &m);
}

void screen_stats(unsigned long *trapped, unsigned long *ignored)
{
    *trapped = n_trapped;
    *ignored = n_ignored;
    n_trapped = n_ignored = 0;
}
