/*
 * inputtest.c -- does injection actually move this server's pointer and reach
 * a window's keyboard?
 *
 * Calls src/input_shim.c the way input.rs will, and then *asks the server what
 * happened* rather than trusting that a request without an error did what it
 * meant to. The PowerPC port learned that the hard way: a mouse move whose
 * coordinates were mangled by the calling convention reported no error at all
 * and left the cursor at y=0.0000000805.
 *
 *   gcc -O1 -I../src -o inputtest inputtest.c ../src/input_shim.c -lXtst -lX11
 *
 *   gcc-5.5 -m64 -O2 -I/usr/openwin/include -o inputtest inputtest.c \
 *       ../src/input_shim.c -L/usr/openwin/lib/sparcv9 \
 *       -R/usr/openwin/lib/sparcv9 -lXtst -lX11
 *
 * DISPLAY picks the server. Typing goes wherever the focus is, so run it
 * against a scratch server with something focused, not over someone's session.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>

/* input_shim.c's interface, as input.rs declares it. */
void rd_mouse(int type, double x, double y, int button);
void rd_mouse_here(int type, int button);
void rd_key(int keycode, int down);
void rd_key_char(unsigned int cp, int down, unsigned int flags);
void rd_key_unicode(unsigned int cp, int down);
void rd_scroll(int dy, int dx, int pixels);
void rd_release_modifiers(void);
void rd_cursor_pos(double *x, double *y);

/* input.rs's mask: low 3 bits the kind, the rest the button. */
#define MOVE 0
#define DOWN 1
#define UP   2

static Display *d;
static int screen;

static void where(int *x, int *y)
{
    Window r, c;
    int wx, wy;
    unsigned int mask;
    XQueryPointer(d, RootWindow(d, screen), &r, &c, x, y, &wx, &wy, &mask);
}

static int check_move(int want_x, int want_y)
{
    int x = -1, y = -1;
    rd_mouse(MOVE, want_x, want_y, 0);
    XSync(d, False);
    usleep(30000);
    where(&x, &y);
    printf("  move to %4d,%4d -> pointer at %4d,%4d  %s\n",
           want_x, want_y, x, y, (x == want_x && y == want_y) ? "OK" : "MISMATCH");
    return x == want_x && y == want_y;
}

int main(void)
{
    int bad = 0;
    double cx = -1, cy = -1;

    d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "inputtest: cannot open display\n"); return 1; }
    screen = DefaultScreen(d);
    printf("display %s, screen %dx%d\n", DisplayString(d),
           DisplayWidth(d, screen), DisplayHeight(d, screen));

    puts("pointer:");
    bad += !check_move(100, 100);
    bad += !check_move(640, 480);
    bad += !check_move(1, 1);

    /* The shim's own idea of where the pointer is, which is what the agent
     * reports back to the peer. */
    rd_cursor_pos(&cx, &cy);
    printf("  rd_cursor_pos says %.0f,%.0f\n", cx, cy);

    puts("buttons: left down/up at 400,300 (watch for a click on the server)");
    check_move(400, 300);
    rd_mouse_here(DOWN, 0);
    XSync(d, False);
    usleep(50000);
    rd_mouse_here(UP, 0);
    XSync(d, False);

    puts("scroll: three notches down, then three up");
    rd_scroll(-3, 0, 0);
    rd_scroll(3, 0, 0);
    XSync(d, False);

    puts("keyboard: typing 'rustdesk' plus Return into whatever has focus");
    {
        const char *s = "rustdesk";
        const char *p;
        for (p = s; *p; p++) {
            rd_key_unicode((unsigned int)*p, 1);
            rd_key_unicode((unsigned int)*p, 0);
        }
        rd_key(36, 1);   /* Mac keycode 36 is Return */
        rd_key(36, 0);
    }
    rd_release_modifiers();
    XSync(d, False);

    printf("%s\n", bad ? "SOME POINTER MOVES DID NOT LAND" : "pointer moves all landed");
    return bad ? 1 : 0;
}
