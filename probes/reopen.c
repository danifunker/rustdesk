/*
 * reopen.c -- why does the second ReadDisplay client in a process fail?
 *
 * capture_test found that a fresh process reads the screen happily, and that a
 * second rd_capture_open in the same process gets BadRequest with request code
 * 0 -- i.e. the ReadDisplay extension's opcode came back as nothing. That
 * matters well beyond the probe: this X server wedges and gets restarted, so
 * the agent has to reopen the display, and if reopening cannot work the agent
 * is finished the first time X hiccups.
 *
 * Four variants of the same loop, differing only in what teardown they do, to
 * find which step poisons the next connection:
 *
 *   0  full teardown        destroy buf, detach, shmdt, XCloseDisplay
 *   1  keep the buf         everything except XShmDestroyReadDisplayBuf
 *   2  keep the display     everything except XCloseDisplay
 *   3  keep both            neither of the two above
 *
 * If 2 and 3 pass while 0 and 1 fail, XCloseDisplay is the culprit and the
 * agent's answer is to leak the connection rather than close it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/readdisplay.h>

static int err_count;
static char err_text[128];

static int on_err(Display *d, XErrorEvent *e)
{
    char b[64];
    XGetErrorText(d, e->error_code, b, sizeof b);
    snprintf(err_text, sizeof err_text, "%d (%s) req %d.%d",
             e->error_code, b, e->request_code, e->minor_code);
    err_count++;
    return 0;
}

/* keep_dpy: 0 = XCloseDisplay, 1 = leave the connection entirely alone,
 *           2 = close the socket by hand but never call XCloseDisplay. */
static int one_round(int round, int keep_buf, int keep_dpy)
{
    Display *dpy;
    Window root;
    XShmSegmentInfo si;
    ShmReadDisplayBuf *rb;
    XRectangle r;
    unsigned long hints = 0;
    unsigned char *buf;
    int W, H, ev, er, maj, min, ok;
    Bool pm;
    size_t sz;

    memset(&si, 0, sizeof si);
    dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("    round %d: XOpenDisplay failed\n", round); return -1; }
    XSetErrorHandler(on_err);
    root = DefaultRootWindow(dpy);
    W = DisplayWidth(dpy, DefaultScreen(dpy));
    H = DisplayHeight(dpy, DefaultScreen(dpy));

    if (!XShmQueryExtension(dpy)) { printf("    no MIT-SHM\n"); return -1; }
    XShmQueryVersion(dpy, &maj, &min, &pm);
    if (!XReadDisplayQueryExtension(dpy, &ev, &er)) {
        printf("    round %d: ReadDisplay absent\n", round);
        return -1;
    }

    sz = (size_t)W * H * 4;
    si.shmid = shmget(IPC_PRIVATE, sz, IPC_CREAT | 0600);
    if (si.shmid < 0) { printf("    shmget: %s\n", strerror(errno)); return -1; }
    si.shmaddr = (char *)shmat(si.shmid, NULL, 0);
    si.readOnly = False;
    buf = (unsigned char *)si.shmaddr;
    XShmAttach(dpy, &si);
    XSync(dpy, False);
    shmctl(si.shmid, IPC_RMID, NULL);

    rb = XShmCreateReadDisplayBuf(dpy, (char *)buf, &si, W, H);
    if (!rb) { printf("    round %d: CreateReadDisplayBuf NULL\n", round); return -1; }

    err_count = 0; err_text[0] = 0;
    r.x = 0; r.y = 0; r.width = 64; r.height = 64;
    XShmReadDisplayRects(dpy, root, &r, 1, rb, 0, 0, 0, &hints);
    XSync(dpy, False);
    ok = (err_count == 0);
    printf("    round %d: %s%s%s\n", round, ok ? "OK" : "FAILED",
           ok ? "" : " -- ", ok ? "" : err_text);

    if (!keep_buf) XShmDestroyReadDisplayBuf(rb);
    XShmDetach(dpy, &si);
    XSync(dpy, False);
    shmdt(si.shmaddr);
    if (keep_dpy == 0) XCloseDisplay(dpy);
    else if (keep_dpy == 2) close(ConnectionNumber(dpy));
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    /* One variant per process. Run together they contaminate each other: a
     * single XCloseDisplay poisons every later connection in the process, so a
     * variant that follows a closing one looks broken when it is not. */
    static const char *names[5] = {
        "0: full teardown (destroy buf + XCloseDisplay)",
        "1: keep the ReadDisplayBuf, still XCloseDisplay",
        "2: destroy the buf, never XCloseDisplay",
        "3: keep both",
        "4: destroy the buf, close(fd) by hand, never XCloseDisplay",
    };
    int v = argc > 1 ? atoi(argv[1]) : 0;
    int i, fails = 0, keep_buf, keep_dpy;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (v < 0 || v > 4) { printf("variant must be 0..4\n"); return 2; }

    keep_buf = (v == 1 || v == 3);
    keep_dpy = (v == 0 || v == 1) ? 0 : (v == 4 ? 2 : 1);

    printf("reopen variant %s\n", names[v]);
    for (i = 1; i <= 3; i++)
        if (one_round(i, keep_buf, keep_dpy) != 0) fails++;
    printf("  -> %d of 3 failed\n", fails);
    return fails ? 1 : 0;
}
