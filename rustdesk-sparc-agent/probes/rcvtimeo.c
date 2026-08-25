/*
 * rcvtimeo.c -- does Solaris 10 have SO_RCVTIMEO?
 *
 * The port assumed it did. `session.rs` sets a read timeout on the peer socket
 * with `?` on the non-IRIX path, and every session ended the instant the video
 * pump started with "Option not supported by protocol (os error 99)" -- which
 * is ENOPROTOOPT, and is exactly the failure the IRIX arm of that code exists
 * for. This asks the kernel directly rather than reading errno through two
 * layers of Rust.
 *
 *   gcc -m64 -o /tmp/rcvtimeo /tmp/rcvtimeo.c -lsocket -lnsl && /tmp/rcvtimeo
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>

static void try_opt(int fd, int opt, const char *name)
{
    struct timeval tv;
    socklen_t len;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    if (setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv) == 0) {
        memset(&tv, 0, sizeof tv);
        len = sizeof tv;
        if (getsockopt(fd, SOL_SOCKET, opt, &tv, &len) == 0)
            printf("  %-12s set OK, reads back %ld.%06ld s\n",
                   name, (long)tv.tv_sec, (long)tv.tv_usec);
        else
            printf("  %-12s set OK, but getsockopt failed: %s\n", name, strerror(errno));
    } else {
        printf("  %-12s setsockopt FAILED: errno %d (%s)\n", name, errno, strerror(errno));
    }
}

int main(void)
{
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    int udp = socket(AF_INET, SOCK_DGRAM, 0);

    printf("SO_RCVTIMEO = %d, SO_SNDTIMEO = %d\n", SO_RCVTIMEO, SO_SNDTIMEO);
    printf("on a TCP socket:\n");
    try_opt(tcp, SO_RCVTIMEO, "SO_RCVTIMEO");
    try_opt(tcp, SO_SNDTIMEO, "SO_SNDTIMEO");
    printf("on a UDP socket:\n");
    try_opt(udp, SO_RCVTIMEO, "SO_RCVTIMEO");

    /* A control: an option this machine certainly has, so a failure above is
     * about that option and not about the way this program calls setsockopt. */
    {
        int one = 1;
        if (setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0)
            printf("control: SO_REUSEADDR set OK -- so setsockopt itself works here\n");
        else
            printf("control: SO_REUSEADDR FAILED too (%s) -- this probe is wrong\n",
                   strerror(errno));
    }
    close(tcp);
    close(udp);
    return 0;
}
