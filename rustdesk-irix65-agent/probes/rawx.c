/*
 * rawx.c -- speak the X11 connection setup by hand, with no Xlib at all.
 *
 * Written to split the blocker in half. A cross-built Xlib client connects,
 * rings SGI's shared-memory doorbell, and is then dropped by the server 75
 * seconds later without a reply. That is consistent with two very different
 * faults, and Xlib sits in the middle of both:
 *
 *   - the server never saw a well-formed setup request (a client-side or
 *     toolchain fault), or
 *   - the server saw it and refused us (an access-control fault).
 *
 * This probe removes Xlib from the picture. It opens a socket itself, writes
 * the 12-byte xConnClientPrefix, and reads the reply, printing whatever comes
 * back. An X server that refuses a client still *answers*: success=0 plus a
 * human-readable reason. Silence followed by EOF means the request never
 * arrived in a form the server could parse.
 *
 * Uses nothing but libc, so it compiles with nekoware gcc as happily as with
 * our clang toolchain, and the two can be compared directly.
 *
 *   rawx                 -> try the local UNIX socket, then TCP 127.0.0.1
 *   rawx tcp             -> TCP only
 *   rawx unix            -> UNIX socket only
 *   rawx <path>          -> that UNIX socket path
 *
 * Second argument is the display number (default 0).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* The setup request, big-endian because MIPS is. Byte order is stated in the
 * first byte, so we write the fields out by hand rather than trusting a struct
 * layout the compiler is free to pad. */
static int build_setup(unsigned char *buf)
{
    memset(buf, 0, 12);
    buf[0] = 'B';          /* MSB first */
    buf[1] = 0;            /* pad */
    buf[2] = 0; buf[3] = 11;   /* protocol major 11 */
    buf[4] = 0; buf[5] = 0;    /* protocol minor 0 */
    buf[6] = 0; buf[7] = 0;    /* auth proto name length 0 */
    buf[8] = 0; buf[9] = 0;    /* auth proto data length 0 */
    buf[10] = 0; buf[11] = 0;  /* pad */
    return 12;
}

static void dump(const unsigned char *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        printf("%02x", p[i]);
        if ((i & 3) == 3) printf(" ");
    }
    printf("\n");
}

/* Read up to want bytes, giving up after timeout_s seconds of no data.
 * Returns bytes read, or -1 on error, and reports EOF distinctly. */
static int read_reply(int fd, unsigned char *buf, int want, int timeout_s, int *saw_eof)
{
    int got = 0;
    double t0 = now_ms();
    *saw_eof = 0;
    while (got < want) {
        fd_set rf;
        struct timeval tv;
        int r;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        tv.tv_sec = timeout_s;
        tv.tv_usec = 0;
        r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) {
            printf("  select errno=%d (%s) after %.0f ms\n", errno, strerror(errno), now_ms() - t0);
            return -1;
        }
        if (r == 0) {
            printf("  TIMEOUT after %.0f ms with %d/%d bytes\n", now_ms() - t0, got, want);
            return got;
        }
        r = read(fd, buf + got, want - got);
        if (r < 0) {
            printf("  read errno=%d (%s) after %.0f ms\n", errno, strerror(errno), now_ms() - t0);
            return -1;
        }
        if (r == 0) {
            printf("  EOF from server after %.0f ms with %d/%d bytes\n", now_ms() - t0, got, want);
            *saw_eof = 1;
            return got;
        }
        got += r;
        printf("  read %d bytes at %.0f ms (total %d)\n", r, now_ms() - t0, got);
    }
    return got;
}

static void interpret(const unsigned char *r, int n)
{
    unsigned int len;
    if (n < 8) { printf("  reply too short to interpret\n"); return; }
    printf("  success byte    %u (%s)\n", r[0],
           r[0] == 0 ? "FAILED - server refused, reason follows" :
           r[0] == 1 ? "SUCCESS - server accepted us" :
           r[0] == 2 ? "AUTHENTICATE - further auth required" : "unknown");
    printf("  proto version   %u.%u\n", (r[2] << 8) | r[3], (r[4] << 8) | r[5]);
    len = (r[6] << 8) | r[7];
    printf("  extra length    %u words (%u bytes)\n", len, len * 4);
    if (r[0] == 0) printf("  reason length   %u bytes\n", r[1]);
}

static int try_unix(const char *path)
{
    struct sockaddr_un sa;
    int fd;
    printf("UNIX socket %s\n", path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { printf("  socket: %s\n", strerror(errno)); return -1; }
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        printf("  connect: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    printf("  connected\n");
    return fd;
}

static int try_tcp(int dpy)
{
    struct sockaddr_in sa;
    int fd, one = 1;
    printf("TCP 127.0.0.1:%d\n", 6000 + dpy);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("  socket: %s\n", strerror(errno)); return -1; }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)(6000 + dpy));
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        printf("  connect: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    printf("  connected\n");
    return fd;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "auto";
    int dpy = argc > 2 ? atoi(argv[2]) : 0;
    unsigned char req[12], reply[64];
    int fd = -1, n, w, eof = 0;
    char upath[128];

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("rawx: hand-rolled X11 setup, display %d, mode %s\n", dpy, mode);

    sprintf(upath, "/tmp/.X11-unix/X%d", dpy);

    if (strcmp(mode, "tcp") == 0) {
        fd = try_tcp(dpy);
    } else if (strcmp(mode, "unix") == 0) {
        fd = try_unix(upath);
    } else if (mode[0] == '/') {
        fd = try_unix(mode);
    } else {
        fd = try_unix(upath);
        if (fd < 0) fd = try_tcp(dpy);
    }
    if (fd < 0) { printf("no transport available\n"); return 2; }

    n = build_setup(req);
    printf("writing %d-byte setup request: ", n);
    dump(req, n);
    w = write(fd, req, n);
    if (w != n) {
        printf("  short write %d (%s)\n", w, strerror(errno));
        return 3;
    }
    printf("  wrote %d bytes\n", w);

    printf("waiting for the 8-byte reply header (90 s budget)\n");
    n = read_reply(fd, reply, 8, 90, &eof);
    if (n > 0) { printf("  raw: "); dump(reply, n); }
    if (n >= 8) {
        interpret(reply, n);
        if (reply[0] == 0 && reply[1] > 0) {
            int rn = reply[1] > 60 ? 60 : reply[1];
            int e2 = 0;
            int m = read_reply(fd, reply, rn, 10, &e2);
            if (m > 0) { reply[m] = 0; printf("  REASON: \"%s\"\n", (char *)reply); }
        }
    }
    printf("verdict: %s\n",
           n >= 8 ? (reply[0] == 1 ? "SERVER ANSWERED - handshake works without Xlib"
                                   : "SERVER ANSWERED with a refusal")
                  : (eof ? "SERVER DROPPED US WITHOUT ANSWERING"
                         : "NO ANSWER AND NO EOF"));
    close(fd);
    return 0;
}
