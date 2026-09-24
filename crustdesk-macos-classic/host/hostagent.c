/* The portable core on Linux: the same session and encoder the Mac runs,
 * serving a synthetic 8-bit desktop over a POSIX socket.
 *
 * It exists so the protocol can be proven against real RustDesk clients
 * without an emulator in the loop. The desktop is painted into an indexed
 * framebuffer with a colour table -- the shape of a Mac's 256-colour screen --
 * and converted with the same converter the Mac uses. Mouse events move a
 * drawn pointer, so input is visible in the picture.
 *
 *   hostagent [port] [password] [q]
 */
#include "../src/core/session.h"
#include "../src/core/files.h"
#include "fs_posix.h"
#include "../src/core/vp8enc.h"
#include "../src/core/yuv.h"
#include "../src/core/rng.h"
#include "../src/core/rdv.h"
#include <netdb.h>
#include <fcntl.h>
#include "sodium/crypto_sign_ed25519.h"
#include "../src/core/sha256.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define W 640
#define H 480

static uint8_t fb[W * H];          /* the "VRAM": 8-bit indexed */
static uint8_t shadow[W * H];      /* what was last encoded */
static yuv_clut clut;
static uint8_t Y[W * H], U[W * H / 4], V[W * H / 4];
static int mouse_x = W / 2, mouse_y = H / 2, buttons;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void fill(int x0, int y0, int w, int h, uint8_t c)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if (x >= 0 && y >= 0 && x < W && y < H)
                fb[y * W + x] = c;
}

/* The Mac's standard 8-bit table: a 6x6x6 cube and ramps. Enough to look right. */
static void make_clut(void)
{
    uint8_t rgb[256][3];
    int i;
    for (i = 0; i < 216; i++) {
        rgb[i][0] = (uint8_t)(255 - 51 * (i / 36));
        rgb[i][1] = (uint8_t)(255 - 51 * ((i / 6) % 6));
        rgb[i][2] = (uint8_t)(255 - 51 * (i % 6));
    }
    for (; i < 256; i++)
        rgb[i][0] = rgb[i][1] = rgb[i][2] = (uint8_t)(255 - (i - 216) * 255 / 39);
    yuv_clut_build(&clut, rgb, 256);
}

enum { WHITE = 0, BLACK = 255, BLUE = 213, GREY = 43, LILAC = 86, RED = 35 };

static void paint_desktop(void)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            fb[y * W + x] = ((x ^ y) & 3) ? GREY : BLUE;
    fill(0, 0, W, 20, WHITE);
    fill(0, 20, W, 1, BLACK);
    fill(60, 60, 360, 240, BLACK);
    fill(61, 61, 358, 18, LILAC);
    fill(61, 80, 358, 219, WHITE);
    for (y = 90; y < 290; y += 12)
        for (x = 70; x < 400; x += 7)
            fill(x, y, 5, 8, (x / 7 + y / 12) % 3 ? BLACK : WHITE);
}

/* An arrow, drawn into the framebuffer the way the Mac's software cursor is. */
static uint8_t under[16 * 16];
static int drawn_x = -100, drawn_y = -100;

static void cursor(int draw)
{
    int x, y;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int px = drawn_x + x, py = drawn_y + y;
            if (px < 0 || py < 0 || px >= W || py >= H)
                continue;
            if (!draw)
                fb[py * W + px] = under[y * 16 + x];
        }
    if (!draw)
        return;
    drawn_x = mouse_x;
    drawn_y = mouse_y;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int px = drawn_x + x, py = drawn_y + y;
            if (px < 0 || py < 0 || px >= W || py >= H)
                continue;
            under[y * 16 + x] = fb[py * W + px];
            if (x <= y && x < 10)
                fb[py * W + px] = (x == 0 || x == y || y == 14) ? WHITE : (buttons ? RED : BLACK);
        }
}

static void clock_tick(void)
{
    static int last = -1;
    int sec = (int)(time(NULL) % 60), i;
    if (sec == last)
        return;
    last = sec;
    fill(560, 4, 60, 12, WHITE);
    for (i = 0; i <= sec / 2; i++)
        fill(560 + i * 2, 6, 1, 8, BLACK);
}

static void on_mouse(void *u, int mask, int x, int y)
{
    int kind = mask & 7;
    (void)u;
    if (kind == 0 || ((kind == 1 || kind == 2) && (x || y))) {
        mouse_x = x;
        mouse_y = y;
    }
    if (kind == 1)
        buttons |= mask >> 3;
    if (kind == 2)
        buttons &= ~(mask >> 3);
    if (kind == 1 && (mask >> 3) == 1) /* a click leaves a mark */
        fill(mouse_x - 2, mouse_y - 2, 5, 5, RED);
}

static void on_key(void *u, const cdv_key *k)
{
    (void)u;
    printf("key: kind %d value %u down %d press %d mods %x mode %d\n", k->kind, k->value,
           k->down, k->press, k->mods, k->mode);
}

static void on_log(void *u, const char *m)
{
    (void)u;
    printf("session: %s\n", m);
}

/* The host agent's identity: fixed for a run, from CDV_ID (default cdvhost1)
 * and a keypair derived from it, so hbbs sees the same key every time. */
static char host_id[16] = "cdvhost1";
static uint8_t host_pk[32], host_sk[64], host_uuid[16];

static int tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints, *ai;
    char ps[8];
    int fd;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%u", port);
    if (getaddrinfo(host, ps, &hints, &ai))
        return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    return fd;
}

/* File transfer: the protocol in src/core/files.c over the directory in
 * CDV_FILES (default ./files-root), as the client's "/". */
static cdv_files *files;
static uint8_t files_work[192 * 1024];

static int on_login(void *u, int file_transfer)
{
    (void)u;
    printf("login: %s\n", file_transfer ? "file transfer" : "desktop");
    return 1;
}

static void on_file(void *u, int field, const uint8_t *d, size_t n)
{
    (void)u;
    cdv_files_message(files, field, d, n);
}

static int serve(int fd, const char *password, int q, int secure)
{
    static uint8_t outbuf[1 << 20], inbuf[192 * 1024];
    static uint8_t encmem_raw[4 << 20];
    static const cdv_hooks hooks = { on_mouse, on_key, NULL,    on_log, NULL, NULL,
                                     NULL,     NULL,   on_login, on_file, NULL };
    int files_started = 0;
    static uint8_t pk[32], sk[64];
    static int have_keys;
    cdv_ident id = { host_id, host_sk, password, "classicsalt", "Host Quadra", W, H, 1 };
    (void)pk;
    (void)sk;
    (void)have_keys;
    cdv_session s;
    vp8e *e = vp8e_init(encmem_raw, W, H, q);
    uint8_t dirty[40 * 30];
    uint32_t frames = 0, bytes = 0, last_report = now_ms();

    if (vp8e_mem_size(W, H) > sizeof encmem_raw)
        return -1;
    cdv_init(&s, outbuf, sizeof outbuf, inbuf, sizeof inbuf, &hooks, &id, now_ms());
    if (!files)
        files = malloc(cdv_files_size());
    cdv_files_init(files, &s, fs_posix(getenv("CDV_FILES") ? getenv("CDV_FILES") : "files-root"),
                   files_work, sizeof files_work);
    cdv_start(&s, now_ms(), secure || getenv("CDV_SECURE") != NULL);
    uint32_t started = now_ms();
    memset(shadow, 0, sizeof shadow);

    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        size_t n;
        /* A connection that never logs in (a client that probed one route and
         * took another) must not hold the agent: hbbs is still talking. */
        if (s.state != CDV_LIVE && now_ms() - started > 5000) {
            printf("no login within 5 s; dropping it\n");
            return 0;
        }
        const uint8_t *o = cdv_out_peek(&s, &n);
        if (n)
            p.events |= POLLOUT;
        if (poll(&p, 1, n ? 30 : 15) < 0 && errno != EINTR)
            return -1;
        if (p.revents & (POLLERR | POLLHUP))
            return 0;
        if (p.revents & POLLIN) {
            uint8_t buf[16384];
            ssize_t r = recv(fd, buf, sizeof buf, 0);
            if (r <= 0)
                return 0;
            if (cdv_feed(&s, buf, (size_t)r) < 0 && !cdv_out_peek(&s, &n))
                return 0;
        }
        o = cdv_out_peek(&s, &n);
        if (n && (p.revents & POLLOUT)) {
            ssize_t w = send(fd, o, n, MSG_NOSIGNAL);
            if (w < 0 && errno != EAGAIN)
                return 0;
            if (w > 0)
                cdv_out_consume(&s, (size_t)w);
        }
        cdv_tick(&s, now_ms());
        if (s.state == CDV_CLOSED && !cdv_out_peek(&s, &n))
            return 0;
        if (s.state == CDV_LIVE && s.file_mode) {
            if (!files_started) {
                cdv_files_start(files);
                files_started = 1;
            }
            cdv_files_pump(files);
            continue;
        }
        cdv_out_peek(&s, &n);
        if (s.state == CDV_LIVE && !n) {
            int key = cdv_take_refresh(&s), mx, my, any = key;
            size_t cap, len;
            uint8_t *dst;
            cursor(0);
            clock_tick();
            cursor(1);
            /* Dirty macroblocks, by comparing with what was last sent. */
            for (my = 0; my < H / 16; my++)
                for (mx = 0; mx < W / 16; mx++) {
                    int y, d = 0;
                    for (y = 0; y < 16 && !d; y++)
                        d = memcmp(fb + (my * 16 + y) * W + mx * 16,
                                   shadow + (my * 16 + y) * W + mx * 16, 16) != 0;
                    dirty[my * (W / 16) + mx] = (uint8_t)d;
                    any |= d;
                }
            if (!any)
                continue;
            yuv_from_indexed(&clut, fb, W, W, H, Y, U, V, W, W / 2, key ? NULL : dirty);
            dst = cdv_video_begin(&s, &cap);
            len = vp8e_encode(e, &(vp8e_src){ Y, U, V, W, W / 2 }, key ? NULL : dirty, key, dst,
                              cap);
            if (!len) {
                cdv_take_refresh(&s);
                s.refresh = 1; /* the encoder needs a keyframe next */
                continue;
            }
            {
                vp8e_stats st;
                vp8e_last_stats(e, &st);
                key = st.key;
                cdv_video_commit(&s, len, key);
                if (getenv("CDV_VERBOSE"))
                    printf("frame %s %zu bytes: %d of %d macroblocks skipped, %d intra\n",
                           key ? "key" : "inter", len, st.skipped, st.mbs, st.intra);
            }
            memcpy(shadow, fb, sizeof fb);
            frames++;
            bytes += (uint32_t)len;
        }
        if (now_ms() - last_report > 5000) {
            printf("%u frames, %u bytes in the last 5 s\n", frames, bytes);
            frames = bytes = 0;
            last_report = now_ms();
        }
    }
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 21118;
    const char *password = argc > 2 ? argv[2] : "classic";
    int q = argc > 3 ? atoi(argv[3]) : 12, one = 1, ls;
    struct sockaddr_in a;

    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    make_clut();
    paint_desktop();
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) {
        perror("listen");
        return 1;
    }
    printf("C-Desk-Vint host agent on port %d, password '%s', q=%d\n", port, password, q);
    {
        const char *server = getenv("CDV_SERVER");
        cdv_rdv rdv;
        int udp = -1;
        struct sockaddr_in hbbs;
        char shost[128];
        uint16_t sport;
        uint8_t seed[32];
        sha256_ctx hc;
        if (getenv("CDV_ID"))
            snprintf(host_id, sizeof host_id, "%s", getenv("CDV_ID"));
        sha256_init(&hc);
        sha256_update(&hc, host_id, strlen(host_id));
        sha256_final(&hc, seed);
        crypto_sign_ed25519_seed_keypair(host_pk, host_sk, seed);
        memcpy(host_uuid, seed, 16);
        memset(&rdv, 0, sizeof rdv);
        rdv_init(&rdv);
        snprintf(rdv.id, sizeof rdv.id, "%s", host_id);
        memcpy(rdv.uuid, host_uuid, 16);
        memcpy(rdv.pk, host_pk, 32);
        snprintf(rdv.key, sizeof rdv.key, "%s", getenv("CDV_KEY") ? getenv("CDV_KEY") : "");
        if (server) {
            struct addrinfo hints, *ai;
            char ps[8];
            rdv_split_host(server, shost, sizeof shost, &sport, RDV_PORT);
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            snprintf(ps, sizeof ps, "%u", sport);
            if (getaddrinfo(shost, ps, &hints, &ai) == 0) {
                memcpy(&hbbs, ai->ai_addr, sizeof hbbs);
                freeaddrinfo(ai);
                udp = socket(AF_INET, SOCK_DGRAM, 0);
                printf("registering with %s as id %s\n", server, host_id);
            }
        }
        for (;;) {
            struct pollfd p[2] = { { ls, POLLIN, 0 }, { udp, POLLIN, 0 } };
            uint8_t buf[2048], rep[512];
            size_t n;
            if (udp >= 0 && (n = rdv_tick(&rdv, now_ms(), buf, sizeof buf)))
                sendto(udp, buf, n, 0, (struct sockaddr *)&hbbs, sizeof hbbs);
            poll(p, udp >= 0 ? 2 : 1, 200);
            if (p[0].revents & POLLIN) {
                int fd = accept(ls, NULL, NULL);
                if (fd >= 0) {
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    printf("direct connection\n");
                    serve(fd, password, q, 0);
                    close(fd);
                    printf("closed\n");
                }
            }
            if (udp >= 0 && (p[1].revents & POLLIN)) {
                ssize_t r = recv(udp, buf, sizeof buf, 0);
                rdv_action act;
                size_t replen;
                int kind;
                if (r <= 0)
                    continue;
                kind = rdv_input(&rdv, buf, (size_t)r, now_ms(), &act, rep, sizeof rep, &replen);
                if (replen)
                    sendto(udp, rep, replen, 0, (struct sockaddr *)&hbbs, sizeof hbbs);
                if (kind == RDV_REGISTERED)
                    printf("registered: reachable as id %s\n", host_id);
                else if (kind == RDV_REFUSED)
                    printf("hbbs refused the registration (%d)\n", act.refuse_code);
                else if (kind == RDV_RELAY) {
                    char rhost[128];
                    uint16_t rport;
                    int t = tcp_connect(shost, sport), fd;
                    printf("relay request (%s) from %u.%u.%u.%u via %s, uuid %s\n",
                           act.initiate ? "punch hole" : "request relay", act.peer_ip >> 24,
                           act.peer_ip >> 16 & 255, act.peer_ip >> 8 & 255, act.peer_ip & 255,
                           act.relay, act.uuid);
                    if (t >= 0) {
                        n = rdv_relay_response(&rdv, &act, buf, sizeof buf);
                        send(t, buf, n, 0);
                        close(t);
                    }
                    rdv_split_host(act.relay, rhost, sizeof rhost, &rport, RELAY_PORT);
                    fd = tcp_connect(rhost, rport);
                    if (fd >= 0) {
                        n = rdv_request_relay(&rdv, &act, buf, sizeof buf);
                        send(fd, buf, n, 0);
                        printf("joined the relay\n");
                        serve(fd, password, q, 1);
                        close(fd);
                        printf("relay session over\n");
                    }
                } else if (kind == RDV_LOCAL) {
                    int l = socket(AF_INET, SOCK_STREAM, 0), t;
                    struct sockaddr_in la;
                    socklen_t ll = sizeof la;
                    memset(&la, 0, sizeof la);
                    la.sin_family = AF_INET;
                    bind(l, (struct sockaddr *)&la, sizeof la);
                    listen(l, 1);
                    getsockname(l, (struct sockaddr *)&la, &ll);
                    t = tcp_connect(shost, sport);
                    if (t >= 0) {
                        struct sockaddr_in me;
                        socklen_t ml = sizeof me;
                        getsockname(t, (struct sockaddr *)&me, &ml);
                        n = rdv_local_addr(&rdv, &act, ntohl(me.sin_addr.s_addr),
                                           ntohs(la.sin_port), buf, sizeof buf);
                        send(t, buf, n, 0);
                        close(t);
                        printf("told hbbs we are at port %u; waiting for the peer\n",
                               ntohs(la.sin_port));
                        {
                            struct pollfd pl = { l, POLLIN, 0 };
                            if (poll(&pl, 1, 10000) > 0) {
                                int fd = accept(l, NULL, NULL);
                                printf("local connection\n");
                                serve(fd, password, q, 1);
                                close(fd);
                                printf("local session over\n");
                            } else {
                                printf("the peer never came\n");
                            }
                        }
                    }
                    close(l);
                }
            }
        }
    }
}
