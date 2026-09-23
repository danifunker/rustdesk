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
#include "../src/core/vp8enc.h"
#include "../src/core/yuv.h"

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

static int serve(int fd, const char *password, int q)
{
    static uint8_t outbuf[1 << 20], inbuf[64 * 1024];
    static uint8_t encmem_raw[4 << 20];
    static const cdv_hooks hooks = { on_mouse, on_key, NULL, on_log, NULL };
    cdv_ident id = { password, "classicsalt", "Host Quadra", W, H, 1 };
    cdv_session s;
    vp8e *e = vp8e_init(encmem_raw, W, H, q);
    uint8_t dirty[40 * 30];
    uint32_t frames = 0, bytes = 0, last_report = now_ms();

    if (vp8e_mem_size(W, H) > sizeof encmem_raw)
        return -1;
    cdv_init(&s, outbuf, sizeof outbuf, inbuf, sizeof inbuf, &hooks, &id, now_ms());
    cdv_start(&s, now_ms());
    memset(shadow, 0, sizeof shadow);

    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        size_t n;
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
            cdv_video_commit(&s, len, key);
            {
                vp8e_stats st;
                vp8e_last_stats(e, &st);
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
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0)
            continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        printf("connection\n");
        serve(fd, password, q);
        close(fd);
        printf("closed\n");
    }
}
