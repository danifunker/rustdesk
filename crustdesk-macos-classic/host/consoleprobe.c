/* The console client against a real console, from Linux, over the same
 * portable code the Mac runs:
 *
 *   build/consoleprobe URL ID [sysinfo]
 *
 * Sends two heartbeats for ID on one connection (keep-alive is the point: a
 * 68040 pays seconds for each handshake), or the inventory then a heartbeat
 * with "sysinfo". A heartbeat for an ID the console does not know creates no
 * device row; the inventory does. The uuid is a fixed test value.
 */
#include "../src/core/console.h"
#include "../src/core/https.h"
#include "../src/core/rng.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint8_t iobuf[HTTPS_IOBUF];
static cdv_https h;

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int dial(const console_url *u)
{
    struct addrinfo hints, *ai;
    char port[8];
    int fd;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%u", u->port);
    if (getaddrinfo(u->host, port, &hints, &ai))
        return -1;
    fd = socket(ai->ai_family, ai->ai_socktype, 0);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    return fd;
}

/* Move bytes until the request is answered or fails. */
static int exchange(int fd)
{
    for (;;) {
        const uint8_t *o;
        uint8_t *in;
        size_t n;
        int st = https_poll(&h);
        if (st != HS_BUSY)
            return st;
        if ((n = https_out(&h, &o)) != 0) {
            ssize_t w = write(fd, o, n);
            if (w <= 0)
                return -1;
            https_out_done(&h, (size_t)w);
            continue;
        }
        if ((n = https_in(&h, &in)) != 0) {
            ssize_t r = read(fd, in, n);
            if (r <= 0) {
                https_eof(&h);
                continue;
            }
            https_in_done(&h, (size_t)r);
            continue;
        }
        return -2; /* neither side can move: a bug */
    }
}

static void post(int fd, const console_url *u, const char *path, const char *body)
{
    char full[128];
    double t0 = now();
    int st;
    snprintf(full, sizeof full, "%s%s", u->prefix, path);
    if (!https_post(&h, u->host, full, body)) {
        printf("%s: could not queue\n", path);
        return;
    }
    st = exchange(fd);
    if (st == HS_DONE)
        printf("%s: %d %s  (%.0f ms%s)\n", path, h.status, h.body, (now() - t0) * 1000,
               https_needs_reconnect(&h) ? ", server closes" : ", kept open");
    else
        printf("%s: failed, state %d err %d tls %d\n", path, st, h.err, h.tls_err);
}

int main(int argc, char **argv)
{
    console_url u;
    static const uint8_t uuid[16] = "cdv-console-test";
    uint8_t seed[32];
    char body[512];
    time_t t = time(NULL);
    struct tm *g = gmtime(&t);
    int fd;
    double t0;
    if (argc < 3 || !console_parse_url(argv[1], &u)) {
        fprintf(stderr, "usage: %s URL ID [sysinfo]\n", argv[0]);
        return 2;
    }
    rng_add(&t, sizeof t);
    rng_bytes(seed, sizeof seed);
    fd = dial(&u);
    if (fd < 0) {
        perror("connect");
        return 1;
    }
    t0 = now();
    https_connected(&h, u.secure, u.host, iobuf, sizeof iobuf,
                    https_days(g->tm_year + 1900, g->tm_mon + 1, g->tm_mday),
                    (uint32_t)(g->tm_hour * 3600 + g->tm_min * 60 + g->tm_sec), seed);
    if (argc > 3 && !strcmp(argv[3], "sysinfo")) {
        console_sysinfo_json(body, sizeof body, argv[2], uuid, "host test", "", "Linux",
                             "consoleprobe", "");
        post(fd, &u, "/api/sysinfo", body);
    }
    console_heartbeat_json(body, sizeof body, argv[2], uuid);
    post(fd, &u, "/api/heartbeat", body);
    printf("first request, handshake included: %.0f ms\n", (now() - t0) * 1000);
    if (!https_needs_reconnect(&h))
        post(fd, &u, "/api/heartbeat", body);
    printf("wants inventory: %s\n", console_wants_sysinfo(h.body) ? "yes" : "no");
    close(fd);
    return 0;
}
