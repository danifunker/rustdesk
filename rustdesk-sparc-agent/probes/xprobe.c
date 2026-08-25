/*
 * xprobe.c -- what the Sun Blade 2500's X server can actually do, measured
 * rather than assumed. Run this before any capture code is written; every
 * number it prints is a decision the agent would otherwise have to guess at.
 *
 * The IRIX agent learned this the expensive way: the pixel byte order was
 * A,B,G,R in memory, not the Mac's A,R,G,B and not libyuv's "ARGB" (which is
 * B,G,R,A). Getting it wrong swaps red and blue, and that is miserable to spot
 * over a remote display. So this probe dumps raw bytes, not an interpretation.
 *
 * Note where the libraries are: Xlib/Xext/Xtst are under /usr/openwin/lib, but
 * Xfixes and Xdamage are under /usr/openwin/sfw/lib -- a different prefix, with
 * the same headers. Looking only in /usr/openwin/lib says the machine has no
 * XFIXES at all, which is wrong.
 *
 * Build 32-bit (the default Solaris 10 X libraries):
 *   gcc -O2 -I/usr/openwin/include -o xprobe xprobe.c \
 *       -L/usr/openwin/lib -R/usr/openwin/lib \
 *       -L/usr/openwin/sfw/lib -R/usr/openwin/sfw/lib \
 *       -lXext -lXfixes -lXdamage -lX11
 *
 * Build 64-bit (what a sparcv9 agent will have to link against):
 *   gcc -m64 -O2 -I/usr/openwin/include -o xprobe64 xprobe.c \
 *       -L/usr/openwin/lib/sparcv9 -R/usr/openwin/lib/sparcv9 \
 *       -L/usr/openwin/sfw/lib/sparcv9 -R/usr/openwin/sfw/lib/sparcv9 \
 *       -lXext -lXfixes -lXdamage -lX11
 *
 * Run it from the console session:
 *   ./xprobe                 # uses $DISPLAY
 *   DISPLAY=:0 ./xprobe      # from ssh, after `xhost +local:` on the console
 *
 * Optional: -DUSE_XTEST and -lXtst adds a live pointer-injection test, which
 * nudges the real cursor by one pixel and puts it back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xdamage.h>

#ifdef USE_XTEST
# include <X11/extensions/XTest.h>
#endif

#define ROUNDS 10

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static const char *visual_class_name(int c)
{
    switch (c) {
    case StaticGray:  return "StaticGray";
    case GrayScale:   return "GrayScale";
    case StaticColor: return "StaticColor";
    case PseudoColor: return "PseudoColor";
    case TrueColor:   return "TrueColor";
    case DirectColor: return "DirectColor";
    default:          return "?";
    }
}

static void dump_bytes(const char *label, const unsigned char *p, int n)
{
    int i;
    printf("  %-22s", label);
    for (i = 0; i < n; i++)
        printf(" %02x", p[i]);
    printf("\n");
}

/* Report an extension by the name the server advertises, so no header for it
 * has to exist on this machine for us to learn whether it is there. */
static int report_ext(Display *dpy, const char *name)
{
    int op = 0, ev = 0, er = 0;
    int present = XQueryExtension(dpy, name, &op, &ev, &er);
    printf("  %-16s %s", name, present ? "yes" : "NO");
    if (present)
        printf("  (opcode %d, first event %d, first error %d)", op, ev, er);
    printf("\n");
    return present;
}

int main(int argc, char **argv)
{
    Display *dpy;
    Window root;
    Screen *scr;
    Visual *vis;
    XImage *img;
    int screen, w, h, depth, i;
    double t0, best, total;
    char **exts;
    int n_exts;

    (void)argc; (void)argv;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "XOpenDisplay failed (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }

    screen = DefaultScreen(dpy);
    scr    = ScreenOfDisplay(dpy, screen);
    root   = RootWindow(dpy, screen);
    vis    = DefaultVisual(dpy, screen);
    w      = WidthOfScreen(scr);
    h      = HeightOfScreen(scr);
    depth  = DefaultDepth(dpy, screen);

    printf("== server ==\n");
    printf("  display          %s\n", DisplayString(dpy));
    printf("  vendor           %s\n", ServerVendor(dpy));
    printf("  vendor release   %d\n", (int)VendorRelease(dpy));
    printf("  protocol         %d.%d\n", ProtocolVersion(dpy), ProtocolRevision(dpy));
    printf("  screens          %d (using %d)\n", ScreenCount(dpy), screen);
    printf("  image byte order %s\n",
           ImageByteOrder(dpy) == MSBFirst ? "MSBFirst" : "LSBFirst");
    printf("  bitmap unit/pad  %d/%d\n", BitmapUnit(dpy), BitmapPad(dpy));

    printf("== screen ==\n");
    printf("  geometry         %dx%d\n", w, h);
    printf("  default depth    %d\n", depth);
    printf("  visual class     %s\n", visual_class_name(vis->class));
    printf("  visual masks     R=%08lx G=%08lx B=%08lx bits_per_rgb=%d\n",
           vis->red_mask, vis->green_mask, vis->blue_mask, vis->bits_per_rgb);
    printf("  backing store    %d (NotUseful=0 WhenMapped=1 Always=2)\n",
           DoesBackingStore(scr));

    printf("== extensions the server advertises ==\n");
    exts = XListExtensions(dpy, &n_exts);
    for (i = 0; i < n_exts; i++)
        printf("  %s\n", exts[i]);
    XFreeExtensionList(exts);

    printf("== extensions that matter to the agent ==\n");
    report_ext(dpy, "MIT-SHM");     /* fast full-screen grabs */
    report_ext(dpy, "XTEST");       /* pointer + keyboard injection */
    report_ext(dpy, "XFIXES");      /* cursor shape, so we need not fake one */
    report_ext(dpy, "DAMAGE");      /* change tracking; without it we poll */
    report_ext(dpy, "SUN_OVL");     /* Sun overlay visuals -- may hide windows */
    report_ext(dpy, "XInputExtension");

    /* --- core XGetImage: correctness first, then cost ---------------------- */
    printf("== XGetImage (full screen, %dx%d) ==\n", w, h);
    img = XGetImage(dpy, root, 0, 0, w, h, AllPlanes, ZPixmap);
    if (!img) {
        printf("  FAILED -- the agent cannot capture this screen with core X11\n");
    } else {
        printf("  format           %d (XYPixmap=1 ZPixmap=2)\n", img->format);
        printf("  depth/bpp        %d / %d\n", img->depth, img->bits_per_pixel);
        printf("  byte_order       %s\n",
               img->byte_order == MSBFirst ? "MSBFirst" : "LSBFirst");
        printf("  bytes_per_line   %d (width*4 would be %d)\n",
               img->bytes_per_line, w * 4);
        printf("  masks            R=%08lx G=%08lx B=%08lx\n",
               img->red_mask, img->green_mask, img->blue_mask);
        dump_bytes("first 16 bytes", (unsigned char *)img->data, 16);
        printf("  pixel(0,0)       %08lx\n", XGetPixel(img, 0, 0));
        printf("  pixel(w/2,h/2)   %08lx\n", XGetPixel(img, w / 2, h / 2));
        XDestroyImage(img);

        best = 1e9; total = 0;
        for (i = 0; i < ROUNDS; i++) {
            t0 = now_ms();
            img = XGetImage(dpy, root, 0, 0, w, h, AllPlanes, ZPixmap);
            {
                double dt = now_ms() - t0;
                if (dt < best) best = dt;
                total += dt;
            }
            if (img) XDestroyImage(img);
        }
        printf("  %d grabs         best %.1f ms, mean %.1f ms  (=> %.1f fps ceiling)\n",
               ROUNDS, best, total / ROUNDS, 1000.0 / (total / ROUNDS));
    }

    /* --- MIT-SHM: the same grab without the round trip --------------------- */
    printf("== XShmGetImage (full screen) ==\n");
    {
        XShmSegmentInfo shminfo;
        int shm_ok = 0;

        memset(&shminfo, 0, sizeof(shminfo));
        img = XShmCreateImage(dpy, vis, depth, ZPixmap, NULL, &shminfo, w, h);
        if (!img) {
            printf("  XShmCreateImage failed\n");
        } else {
            shminfo.shmid = shmget(IPC_PRIVATE,
                                   (size_t)img->bytes_per_line * img->height,
                                   IPC_CREAT | 0600);
            if (shminfo.shmid < 0) {
                printf("  shmget failed (%s)\n", strerror(errno));
            } else {
                shminfo.shmaddr = img->data = (char *)shmat(shminfo.shmid, NULL, 0);
                shminfo.readOnly = False;
                if (img->data == (char *)-1) {
                    printf("  shmat failed\n");
                } else if (!XShmAttach(dpy, &shminfo)) {
                    printf("  XShmAttach refused -- not a local display?\n");
                } else {
                    XSync(dpy, False);
                    shm_ok = XShmGetImage(dpy, root, img, 0, 0, AllPlanes);
                    if (!shm_ok) {
                        printf("  XShmGetImage failed\n");
                    } else {
                        printf("  bytes_per_line   %d\n", img->bytes_per_line);
                        dump_bytes("first 16 bytes", (unsigned char *)img->data, 16);
                        best = 1e9; total = 0;
                        for (i = 0; i < ROUNDS; i++) {
                            t0 = now_ms();
                            XShmGetImage(dpy, root, img, 0, 0, AllPlanes);
                            XSync(dpy, False);
                            {
                                double dt = now_ms() - t0;
                                if (dt < best) best = dt;
                                total += dt;
                            }
                        }
                        printf("  %d grabs         best %.1f ms, mean %.1f ms  (=> %.1f fps ceiling)\n",
                               ROUNDS, best, total / ROUNDS, 1000.0 / (total / ROUNDS));
                    }
                    XShmDetach(dpy, &shminfo);
                }
                if (img->data && img->data != (char *)-1) shmdt(img->data);
                shmctl(shminfo.shmid, IPC_RMID, NULL);
            }
            img->data = NULL;
            XDestroyImage(img);
        }
    }

    /* --- pointer ----------------------------------------------------------- */
    printf("== pointer ==\n");
    {
        Window r, c;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (XQueryPointer(dpy, root, &r, &c, &rx, &ry, &wx, &wy, &mask))
            printf("  at %d,%d  buttons/mods %08x\n", rx, ry, mask);
        else
            printf("  XQueryPointer says the pointer is on another screen\n");
    }

    /* --- XFIXES: the real cursor, rather than a drawn-on stand-in ---------- */
    printf("== XFIXES cursor ==\n");
    {
        int ev, er, major = 0, minor = 0;
        if (!XFixesQueryExtension(dpy, &ev, &er)) {
            printf("  absent -- the agent would have to draw its own cursor\n");
        } else {
            XFixesQueryVersion(dpy, &major, &minor);
            printf("  version          %d.%d\n", major, minor);
            {
                XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
                if (!ci) {
                    printf("  XFixesGetCursorImage returned nothing\n");
                } else {
                    printf("  size             %dx%d hot %d,%d at %d,%d\n",
                           ci->width, ci->height, ci->xhot, ci->yhot, ci->x, ci->y);
                    /* Each pixel occupies a whole `unsigned long` -- 8 bytes on
                     * sparcv9 -- holding one 32-bit ARGB value. Reading it as a
                     * packed u32 array gets every second pixel wrong. */
                    printf("  pixel stride     %d bytes per pixel in memory\n",
                           (int)sizeof(unsigned long));
                    printf("  first 4 pixels   %08lx %08lx %08lx %08lx\n",
                           ci->pixels[0] & 0xffffffffUL, ci->pixels[1] & 0xffffffffUL,
                           ci->pixels[2] & 0xffffffffUL, ci->pixels[3] & 0xffffffffUL);
                    XFree(ci);
                }
            }
        }
    }

    /* --- DAMAGE: does the server report changes, or must the agent poll? --- */
    printf("== DAMAGE (2 second sample -- move a window to make this interesting) ==\n");
    {
        int ev_base, er_base, major = 0, minor = 0;
        if (!XDamageQueryExtension(dpy, &ev_base, &er_base)) {
            printf("  absent -- the agent has to detect changes itself\n");
        } else {
            Damage dmg;
            int n = 0, first = 1;
            double until;

            XDamageQueryVersion(dpy, &major, &minor);
            printf("  version          %d.%d\n", major, minor);
            dmg = XDamageCreate(dpy, root, XDamageReportBoundingBox);
            XSync(dpy, False);
            until = now_ms() + 2000.0;
            while (now_ms() < until) {
                while (XPending(dpy)) {
                    XEvent e;
                    XNextEvent(dpy, &e);
                    if (e.type == ev_base + XDamageNotify) {
                        XDamageNotifyEvent *de = (XDamageNotifyEvent *)&e;
                        n++;
                        if (first) {
                            printf("  first rectangle  %dx%d at %d,%d\n",
                                   de->area.width, de->area.height,
                                   de->area.x, de->area.y);
                            first = 0;
                        }
                        XDamageSubtract(dpy, dmg, None, None);
                    }
                }
            }
            printf("  %d damage events in 2s%s\n", n,
                   n ? "" : "  (nothing changed on screen, or the root reports none)");
            XDamageDestroy(dpy, dmg);
        }
    }

#ifdef USE_XTEST
    printf("== XTEST injection (moves the real cursor) ==\n");
    {
        Window r, c;
        int rx, ry, wx, wy;
        unsigned int mask;
        XQueryPointer(dpy, root, &r, &c, &rx, &ry, &wx, &wy, &mask);
        XTestFakeMotionEvent(dpy, screen, rx + 1, ry, CurrentTime);
        XSync(dpy, False);
        {
            int nx, ny;
            XQueryPointer(dpy, root, &r, &c, &nx, &ny, &wx, &wy, &mask);
            printf("  asked for %d,%d -> cursor at %d,%d %s\n",
                   rx + 1, ry, nx, ny, (nx == rx + 1 && ny == ry) ? "OK" : "MISMATCH");
        }
        XTestFakeMotionEvent(dpy, screen, rx, ry, CurrentTime);
        XSync(dpy, False);
    }
#else
    printf("== XTEST injection ==\n  not compiled in (rebuild with -DUSE_XTEST -lXtst)\n");
#endif

    XCloseDisplay(dpy);
    return 0;
}
