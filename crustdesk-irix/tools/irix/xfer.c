/* xfer: move a file between an IRIX machine and the build host over TCP.
 * IRIX 5.3 has telnet and little else; scripts/indigo.py uploads this once,
 * compiles it with the system cc, and uses it for everything after.
 *
 *   xfer get HOST PORT FILE    connect, read until EOF into FILE
 *   xfer put HOST PORT FILE    connect, send FILE, close
 *
 * Plain C89 for IDO's cc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(int argc, char **argv)
{
    struct sockaddr_in sa;
    char buf[8192];
    int s, f, n, put;
    long total = 0;
    if (argc != 5)
        return 2;
    put = !strcmp(argv[1], "put");
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr(argv[2]);
    sa.sin_port = htons(atoi(argv[3]));
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("xfer: connect");
        return 1;
    }
    f = put ? open(argv[4], O_RDONLY) : open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (f < 0) {
        perror(argv[4]);
        return 1;
    }
    if (put) {
        while ((n = read(f, buf, sizeof buf)) > 0) {
            char *p = buf;
            while (n > 0) {
                int w = write(s, p, n);
                if (w <= 0) { perror("xfer: write"); return 1; }
                p += w; n -= w; total += w;
            }
        }
    } else {
        while ((n = read(s, buf, sizeof buf)) > 0) {
            if (write(f, buf, n) != n) { perror(argv[4]); return 1; }
            total += n;
        }
        if (n < 0) { perror("xfer: read"); return 1; }
    }
    close(f);
    close(s);
    printf("xfer: %ld bytes\n", total);
    return 0;
}
