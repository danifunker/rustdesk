/*
 * xcapture.c -- what the IRIX X server will actually give us, and how fast.
 *
 * This exists for the same reason rustdesk-ppc-agent/probes/ does: on the Mac
 * the whole capture design turned on two facts that no header states -- that
 * the framebuffer is uncached VRAM where per-pixel reads cost 220x a bulk copy,
 * and that its byte order is A,R,G,B in memory rather than libyuv's "ARGB".
 * Both were found by measuring. The equivalents here are unknown, so measure
 * them before writing a capture path around a guess.
 *
 * Questions, in the order the answers matter:
 *
 *   1. Is SGI's ReadDisplay extension actually advertised at run time? It is
 *      compiled into every 6.5.22 X server -- verified against the install
 *      media for both the Indy's REX3 build and the GENERIC one that O2,
 *      Octane, Fuel and Tezro all run -- but compiled in is not advertised.
 *   2. What does it hand back? Depth, bits per pixel, and above all the byte
 *      order of a pixel in memory. Getting this wrong swaps red and blue,
 *      which is miserable to find over a remote display.
 *   3. Does XRD_READ_POINTER composite the hardware cursor for us? If so the
 *      agent sends cursor_embedded and never has to synthesise a pointer.
 *   4. How does the cost scale -- full screen, half the rows, a small rect?
 *      That decides whether dirty-rect capture is worth its complexity.
 *   5. How does plain XGetImage compare, since that is the fallback on any
 *      server without ReadDisplay?
 *
 * Prints one line per finding. No dependencies beyond Xlib and Xext.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/readdisplay.h>

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* Report the byte order of a pixel as it sits in memory, which is the thing
 * that actually matters to a converter, rather than as a masked word. */
static void describe_pixel_order(XImage *im)
{
    unsigned long rm = im->red_mask, gm = im->green_mask, bm = im->blue_mask;
    int bpp = im->bits_per_pixel;
    printf("  masks           R=%08lx G=%08lx B=%08lx\n", rm, gm, bm);
    printf("  bits_per_pixel  %d   depth %d   bytes_per_line %d\n",
           bpp, im->depth, im->bytes_per_line);
    printf("  byte_order      %s   bitmap_unit %d  bitmap_bit_order %s\n",
           im->byte_order == MSBFirst ? "MSBFirst" : "LSBFirst",
           im->bitmap_unit,
           im->bitmap_bit_order == MSBFirst ? "MSBFirst" : "LSBFirst");

    if (bpp == 32 && im->data) {
        unsigned char *p = (unsigned char *)im->data;
        int i;
        printf("  first 4 pixels  ");
        for (i = 0; i < 16; i++) {
            printf("%02x", p[i]);
            if ((i % 4) == 3) printf(" ");
        }
        printf("\n");
        /* Name the in-memory order by finding which byte each mask selects,
         * on a big-endian host where the high byte comes first. */
        {
            const char *name[4] = { "?", "?", "?", "?" };
            int k;
            for (k = 0; k < 4; k++) {
                unsigned long byte_mask = 0xffUL << ((3 - k) * 8);
                if (rm & byte_mask) name[k] = "R";
                else if (gm & byte_mask) name[k] = "G";
                else if (bm & byte_mask) name[k] = "B";
                else name[k] = "x";   /* pad or alpha */
            }
            printf("  memory order    %s,%s,%s,%s\n",
                   name[0], name[1], name[2], name[3]);
        }
    }
}

int main(int argc, char **argv)
{
    Display *dpy;
    Window root;
    Screen *scr;
    int w, h, ev = 0, er = 0, major = 0, minor = 0;
    int have_rd;
    const char *dname = (argc > 1) ? argv[1] : NULL;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("opening display %s ...\n",
           dname ? dname : (getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)"));
    dpy = XOpenDisplay(dname);
    if (!dpy) {
        printf("XOpenDisplay(%s) FAILED -- is DISPLAY set and access allowed?\n",
               dname ? dname : (getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)"));
        return 1;
    }

    scr  = DefaultScreenOfDisplay(dpy);
    root = RootWindowOfScreen(scr);
    w    = WidthOfScreen(scr);
    h    = HeightOfScreen(scr);

    printf("display         %s\n", DisplayString(dpy));
    printf("vendor          %s r%d\n", ServerVendor(dpy), VendorRelease(dpy));
    printf("screen          %dx%d depth %d\n", w, h, DefaultDepthOfScreen(scr));
    printf("image byte order %s\n",
           ImageByteOrder(dpy) == MSBFirst ? "MSBFirst" : "LSBFirst");

    /* --- 1. Is ReadDisplay there? --- */
    have_rd = XReadDisplayQueryExtension(dpy, &ev, &er);
    printf("\nReadDisplay     %s\n", have_rd ? "PRESENT" : "absent");
    if (have_rd && XReadDisplayQueryVersion(dpy, &major, &minor))
        printf("  version       %d.%d\n", major, minor);

    /* --- 2 & 3. What it returns, with and without the pointer --- */
    if (have_rd) {
        unsigned long hints_ret = 0;
        XImage *im;
        double t0;

        printf("\nXReadDisplay 64x64 (format probe -- small on purpose) ...\n");
        t0 = now_ms();
        im = XReadDisplay(dpy, root, 0, 0, 64, 64, 0, &hints_ret);
        printf("  %.1f ms  hints_ret=0x%lx\n", now_ms() - t0, hints_ret);
        if (!im) {
            printf("  returned NULL\n");
        } else {
            describe_pixel_order(im);
            XDestroyImage(im);
        }

        printf("\nXRD_READ_POINTER (64x64) ...\n");
        t0 = now_ms();
        hints_ret = 0;
        im = XReadDisplay(dpy, root, 0, 0, 64, 64, XRD_READ_POINTER, &hints_ret);
        printf("  %.1f ms  hints_ret=0x%lx  %s\n",
               now_ms() - t0, hints_ret,
               (hints_ret & XRD_READ_POINTER)
                   ? "<-- honoured: cursor is composited for us"
                   : "<-- NOT honoured: we must send a cursor ourselves");
        if (im) XDestroyImage(im);

        /* --- 4. How the cost scales --- */
        printf("\nscaling (XReadDisplay):\n");
        {
            struct { const char *what; int x, y, w, h; } cases[] = {
                { "full screen",  0, 0, 0, 0 },
                { "top half",     0, 0, 0, 0 },
                { "top quarter",  0, 0, 0, 0 },
                { "256x256 rect", 0, 0, 256, 256 },
                { "64x64 rect",   0, 0, 64, 64 },
            };
            int i;
            cases[0].w = 64;     cases[0].h = 64;
            cases[1].w = 256;    cases[1].h = 256;
            cases[2].w = w;      cases[2].h = h / 8;
            cases[3].w = w;      cases[3].h = h / 2;
            cases[4].w = w;      cases[4].h = h;
            cases[0].what = "64x64";        cases[1].what = "256x256";
            cases[2].what = "eighth screen"; cases[3].what = "half screen";
            cases[4].what = "full screen";
            for (i = 0; i < 5; i++) {
                double t;
                printf("  %-14s %5dx%-5d ... ", cases[i].what, cases[i].w, cases[i].h);
                t = now_ms();
                XImage *p = XReadDisplay(dpy, root, cases[i].x, cases[i].y,
                                         cases[i].w, cases[i].h, 0, &hints_ret);
                double ms = now_ms() - t;
                long px = (long)cases[i].w * cases[i].h;
                printf("%8.1f ms  %6.2f MB  %7.2f MB/s\n", ms,
                       px * 4.0 / 1048576.0,
                       ms > 0 ? (px * 4.0 / 1048576.0) / (ms / 1000.0) : 0.0);
                if (p) XDestroyImage(p);
            }
        }
    }

    /* --- 5. The fallback path, for servers without ReadDisplay --- */
    {
        double t0;
        XImage *im;
        printf("\nXGetImage full screen (fallback path) ...\n");
        t0 = now_ms();
        im = XGetImage(dpy, root, 0, 0, w, h, AllPlanes, ZPixmap);
        printf("  %.1f ms\n", now_ms() - t0);
        if (im) {
            describe_pixel_order(im);
            XDestroyImage(im);
        } else {
            printf("  returned NULL\n");
        }
    }

    XCloseDisplay(dpy);
    return 0;
}
