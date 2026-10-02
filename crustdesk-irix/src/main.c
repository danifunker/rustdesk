/* C-Desk-Vint for IRIX: a RustDesk agent for IRIX 5.3 and later, on MIPS I.
 *
 * The protocol, the encryption and the VP8 encoder are the classic Mac
 * agent's portable core (crustdesk-macos-classic/src/core), built with GCC for
 * the R3000; this file is the IRIX side of it: one poll() loop over the X
 * connection, the ID server, the relay and the peer.
 *
 * One desktop session at a time. A frame is encoded a few macroblock rows at a
 * time with the network serviced in between, so the peer's pointer and keys
 * keep reaching X while a large frame is still being made.
 *
 *   C-Desk-Vint [-c prefs] [-d display] [-l logfile] [-v] [-p port]
 */
#include "clip.h"
#include "input.h"
#include "log.h"
#include "screen.h"
#include "unzstd.h"

#include "files.h"
#include "fs_posix.h"

#include "rdv.h"
#include "rng.h"
#include "session.h"
#include "sha256.h"
#include "vp8enc.h"
#include "sodium/crypto_sign_ed25519.h"

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <utmp.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifndef DEFAULT_SERVER
#define DEFAULT_SERVER "rs-ny.rustdesk.com"
#define DEFAULT_KEY "OeVuKk5nlHiXp+APNn0Y3pC1Iwpwn44JGqrQCsWqmBw="
#endif
#ifndef DEFAULT_KEY
#define DEFAULT_KEY ""
#endif
#define DEFAULT_PORT 21118
#define DEFAULT_Q 24
#define VERSION_STRING "C-Desk-Vint for IRIX " CDV_BUILD

#ifndef CDV_BUILD
#define CDV_BUILD "dev"
#endif

/* ---- preferences --------------------------------------------------------- */

static struct {
    char password[33];
    int port;
    int q;
    double gamma;
    int scan_ms;
    char id[16];
    uint8_t uuid[16], seed[32];
    int have_uuid, have_seed;
    char server[64];
    char key[64];
    char relay[64];
    char name[64];
    char files[256];      /* what the file manager sees as "/"; "off" for none */
} prefs;
static char prefs_path[256];

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static const char *hex(const uint8_t *b, int n, char *out)
{
    int i;
    for (i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
    return out;
}

static int unhex(const char *s, uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return 0;
        b[i] = (uint8_t)v;
    }
    return 1;
}

static void copy(char *dst, const char *src, size_t cap)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void read_prefs(void)
{
    char line[256];
    FILE *f = fopen(prefs_path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = 0;
        if (!eq || line[0] == '#')
            continue;
        *eq++ = 0;
        if (!strcmp(line, "password"))
            copy(prefs.password, eq, sizeof prefs.password);
        else if (!strcmp(line, "port"))
            prefs.port = atoi(eq);
        else if (!strcmp(line, "quality"))
            prefs.q = atoi(eq);
        else if (!strcmp(line, "gamma"))
            prefs.gamma = atof(eq);
        else if (!strcmp(line, "scan"))
            prefs.scan_ms = atoi(eq);
        else if (!strcmp(line, "id"))
            copy(prefs.id, eq, sizeof prefs.id);
        else if (!strcmp(line, "uuid"))
            prefs.have_uuid = unhex(eq, prefs.uuid, 16);
        else if (!strcmp(line, "seed"))
            prefs.have_seed = unhex(eq, prefs.seed, 32);
        else if (!strcmp(line, "server"))
            copy(prefs.server, eq, sizeof prefs.server);
        else if (!strcmp(line, "key"))
            copy(prefs.key, eq, sizeof prefs.key);
        else if (!strcmp(line, "relay"))
            copy(prefs.relay, eq, sizeof prefs.relay);
        else if (!strcmp(line, "name"))
            copy(prefs.name, eq, sizeof prefs.name);
        else if (!strcmp(line, "files"))
            copy(prefs.files, eq, sizeof prefs.files);
    }
    fclose(f);
}

static void write_prefs(void)
{
    char u[33], k[65];
    FILE *f;
    int fd = open(prefs_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || !(f = fdopen(fd, "w"))) {
        cdv_log("cannot write %s", prefs_path);
        return;
    }
    fprintf(f, "# C-Desk-Vint preferences. The seed is this machine's private key.\n");
    fprintf(f, "id=%s\npassword=%s\nport=%d\nquality=%d\nscan=%d\n", prefs.id,
            prefs.password, prefs.port, prefs.q, prefs.scan_ms);
    if (prefs.gamma >= 0)
        fprintf(f, "gamma=%g\n", prefs.gamma);
    fprintf(f, "server=%s\nkey=%s\nrelay=%s\nname=%s\nfiles=%s\n", prefs.server, prefs.key,
            prefs.relay, prefs.name, prefs.files);
    fprintf(f, "uuid=%s\nseed=%s\n", hex(prefs.uuid, 16, u), hex(prefs.seed, 32, k));
    fclose(f);
}

/* IRIX 5.3 has no /dev/random: timer jitter, the process, the machine. */
static void gather_entropy(void)
{
    struct timeval tv;
    struct utsname un;
    int i;
    long v[4];
    for (i = 0; i < 64; i++) {
        volatile int spin = 0, j;
        gettimeofday(&tv, 0);
        for (j = 0; j < (int)(tv.tv_usec & 255); j++)
            spin += j;
        rng_add(&tv, sizeof tv);
    }
    v[0] = (long)getpid();
    v[1] = (long)getppid();
    v[2] = (long)time(NULL);
    v[3] = (long)clock();
    rng_add(v, sizeof v);
    if (uname(&un) >= 0)
        rng_add(&un, sizeof un);
}

static void load_prefs(void)
{
    prefs.port = DEFAULT_PORT;
    prefs.q = DEFAULT_Q;
    prefs.gamma = -1; /* the system's: /etc/config/system.glGammaVal */
    prefs.scan_ms = 150;
    copy(prefs.files, "/", sizeof prefs.files);
    copy(prefs.server, DEFAULT_SERVER, sizeof prefs.server);
    copy(prefs.key, DEFAULT_KEY, sizeof prefs.key);
    read_prefs();
    gather_entropy();
    if (!prefs.password[0] || !prefs.id[0] || !prefs.have_uuid || !prefs.have_seed) {
        /* First run: a password nobody chose, an ID and a key of our own. */
        static const char a[] = "abcdefghjkmnpqrstuvwxyz23456789";
        static const char digits[] = "123456789";
        int i;
        if (!prefs.password[0]) {
            for (i = 0; i < 8; i++)
                prefs.password[i] = a[rng_u32() % (sizeof a - 1)];
            prefs.password[8] = 0;
        }
        if (!prefs.id[0]) {
            for (i = 0; i < 9; i++)
                prefs.id[i] = digits[rng_u32() % (sizeof digits - 1)];
            prefs.id[9] = 0;
        }
        if (!prefs.have_uuid) {
            rng_bytes(prefs.uuid, 16);
            prefs.have_uuid = 1;
        }
        if (!prefs.have_seed) {
            rng_bytes(prefs.seed, 32);
            prefs.have_seed = 1;
        }
        write_prefs();
    }
    if (prefs.gamma < 0) {
        /* What `gamma` set; IRIX's default is 1.7. */
        FILE *g = fopen("/etc/config/system.glGammaVal", "r");
        prefs.gamma = 1.7;
        if (g) {
            double v;
            if (fscanf(g, "%lf", &v) == 1 && v > 0.5 && v < 4)
                prefs.gamma = v;
            fclose(g);
        }
    }
    if (prefs.q < 0 || prefs.q > 127)
        prefs.q = DEFAULT_Q;
    if (prefs.port <= 0)
        prefs.port = DEFAULT_PORT;
    if (prefs.scan_ms < 20)
        prefs.scan_ms = 20;
}

/* ---- sockets ------------------------------------------------------------- */

static void nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static int resolve(const char *host, uint32_t *ip)
{
    struct hostent *h;
    unsigned long a = inet_addr(host);
    if (a != (unsigned long)-1 && a != 0xffffffffUL) {
        *ip = (uint32_t)ntohl(a);
        return 1;
    }
    h = gethostbyname(host);
    if (!h || h->h_length != 4)
        return 0;
    *ip = (uint32_t)((uint8_t)h->h_addr_list[0][0] << 24 | (uint8_t)h->h_addr_list[0][1] << 16 |
                     (uint8_t)h->h_addr_list[0][2] << 8 | (uint8_t)h->h_addr_list[0][3]);
    return 1;
}

static void sa_set(struct sockaddr_in *sa, uint32_t ip, uint16_t port)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(ip);
    sa->sin_port = htons(port);
}

/* Connect with a deadline; blocking otherwise. -1 on failure. */
static int tcp_connect(uint32_t ip, uint16_t port, int ms)
{
    struct sockaddr_in sa;
    struct pollfd p;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1, err = 0, len = sizeof err;
    if (fd < 0)
        return -1;
    sa_set(&sa, ip, port);
    nonblock(fd);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    p.fd = fd;
    p.events = POLLOUT;
    if (poll(&p, 1, ms) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) < 0 ||
        err) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    return fd;
}

static int listen_on(uint16_t port, int udp)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0), one = 1;
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
    sa_set(&sa, 0, port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || (!udp && listen(fd, 2) < 0)) {
        close(fd);
        return -1;
    }
    if (udp)
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (char *)&one, sizeof one);
    nonblock(fd);
    return fd;
}

static int send_all(int fd, const uint8_t *b, size_t n)
{
    while (n) {
        int w = send(fd, (const char *)b, (int)n, 0);
        if (w <= 0)
            return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---- the sessions ------------------------------------------------------------- *
 *
 * Two slots. A connection takes a free one; at login it becomes the desktop
 * (one at a time: it owns the screen, the encoder and the video buffer) or a
 * file manager (a client opens one beside its desktop window). */

static cdv_screen scr;
static cdv_ident ident;
static uint8_t sign_pk[32], sign_sk[64];
static char hostname[64];

#define CTL_CAP (64 * 1024)
#define VID_CAP (1024 * 1024)
#define IN_CAP (192 * 1024)
#define WORK_CAP (192 * 1024)
#define NSLOTS 2
/* The largest screen the buffers are made for: IRIX 5.3's Indigo and Indy
 * graphics top out at 1280x1024. A larger one is refused. */
#define MAX_W 1280
#define MAX_H 1024

enum { K_NONE, K_DESKTOP, K_FILES };

typedef struct {
    int fd;               /* -1: free */
    cdv_session s;
    cdv_hooks hooks;      /* .user is the slot */
    uint8_t *ctl, *in;
    int kind;
    cdv_files *files;
    uint8_t *work;
    int files_started;
    uint32_t started;
    char how[16];
} slot;
static slot slots[NSLOTS];
static slot *desk;        /* the slot showing the desktop, or NULL */

/* The desktop's own state, whichever slot it is in. */
static struct {
    vp8e *enc;
    int encoding, key;
    uint32_t last_frame, last_cursor, last_report;
    uint32_t f_start, f_enc;  /* this frame: when it began, encoder time so far */
    int f_mbs, f_detail, f_moved_hint, f_flat;
    int cx, cy;
    /* for the log */
    unsigned frames, bytes, mbs;
    uint32_t t_read, t_conv, t_enc;
} S;

static uint8_t *vidbuf, *encmem, *refmem;

/* The X connection. The agent runs without one -- at boot before the X
 * server is up, and across the server resetting at a logout -- registering
 * and listening all the same, and opens it again every few seconds. */
static int x_ok;
static uint32_t x_retry;
static const char *x_display;
static jmp_buf x_jmp;
static int x_jmp_set;
static const cdv_fs_ops *files_fs;  /* NULL: file transfer is off */

/* The frame budget. A frame codes what fits in about FRAME_BUDGET ms of
 * this CPU; whatever is left stays dirty for the next. So a big change (a
 * window opening, the first picture) paints in over a few frames while the
 * stream keeps moving, instead of freezing it for seconds. Costs are in
 * hundredths of a millisecond: one-colour macroblocks are cheap (prediction
 * gets them exactly), moved ones nearly so, anything else -- text, icons --
 * is learnt from the frames as they go (2.7 ms on a 33 MHz R3000 to start). */
#define FRAME_BUDGET 22000
#define COST_FLAT 50
#define COST_MOVED 60
static int cost_detail = 270;
static uint8_t *enc_dirty;    /* the macroblocks of the frame being encoded */
static uint8_t *dirty_age;    /* frames a macroblock has waited */
static uint8_t *flat_y, *flat_u, *flat_v;
static int frame_ms = 66; /* at most ~15 a second; the Indigo will set the real pace */

static void on_mouse(void *u, int mask, int x, int y)
{
    if ((slot *)u == desk)
        input_mouse(mask, x, y);
}

static void on_key(void *u, const cdv_key *k)
{
    if ((slot *)u == desk)
        input_key(k);
}

/* The peer's clipboard: compressed whenever that was smaller. */
static void on_clipboard(void *u, const char *utf8, size_t n, int compressed)
{
    if ((slot *)u != desk)
        return;
    if (compressed) {
        static char text[256 * 1024];
        long len = cdv_unzstd(text, sizeof text, utf8, n);
        if (len < 0) {
            cdv_log("clipboard: could not decompress the peer's text");
            return;
        }
        clip_from_peer(text, (size_t)len);
    } else {
        clip_from_peer(utf8, n);
    }
}

/* Text copied here, for the peer. */
static void clipboard_to_peer(const char *utf8, size_t n)
{
    if (desk && desk->s.state == CDV_LIVE)
        cdv_send_clipboard(&desk->s, utf8, n);
}

static void on_log(void *u, const char *m)
{
    (void)u;
    cdv_log("session: %s", m);
}

static void on_option(void *u, int quality, int fps)
{
    if ((slot *)u != desk)
        return;
    if (quality >= 0 && S.enc) {
        vp8e_set_q(S.enc, quality);
        cdv_log("peer set quality q=%d", quality);
    }
    if (fps > 0) {
        frame_ms = 1000 / fps;
        cdv_log("peer asked for %d fps", fps);
    }
}

/* Who is logged in at the display: the utmp entry for ":0" that xdm's
 * session writes, else the console's; root if neither (the login screen). */
static const char *console_user(void)
{
    static char name[sizeof(((struct utmp *)0)->ut_user) + 1];
    struct utmp *u;
    int best = 0;
    strcpy(name, "root");
    setutent();
    while ((u = getutent()) != NULL) {
        int rank;
        if (u->ut_type != USER_PROCESS)
            continue;
        rank = !strncmp(u->ut_line, ":0", 2) ? 2 : !strncmp(u->ut_line, "console", 7) ? 1 : 0;
        if (rank > best) {
            best = rank;
            memcpy(name, u->ut_user, sizeof u->ut_user);
            name[sizeof u->ut_user] = 0;
        }
    }
    endutent();
    return name;
}

static int on_login(void *u, int file_transfer)
{
    slot *sl = (slot *)u;
    if (file_transfer) {
        if (!files_fs) {
            cdv_log("refusing a file-transfer session (files=off)");
            return 0;
        }
        sl->kind = K_FILES;
        sl->files_started = 0;
        cdv_files_init(sl->files, &sl->s, files_fs, sl->work, WORK_CAP);
        return 1;
    }
    if (desk && desk != sl) {
        cdv_log("refusing a second desktop session");
        return 0;
    }
    if (!x_ok) {
        cdv_log("refusing a desktop session: no X display yet");
        return 0;
    }
    if (scr.width > MAX_W || scr.height > MAX_H) {
        cdv_log("refusing a desktop session: the screen is larger than %dx%d", MAX_W, MAX_H);
        return 0;
    }
    ident.width = scr.width;
    ident.height = scr.height;
    sl->kind = K_DESKTOP;
    desk = sl;
    cdv_set_video(&sl->s, vidbuf, VID_CAP);
    S.enc = vp8e_init(encmem, scr.width, scr.height, prefs.q);
    if (refmem)
        vp8e_set_ref(S.enc, refmem);
    S.encoding = 0;
    S.last_frame = S.last_cursor = S.last_report = now_ms();
    S.frames = S.bytes = S.mbs = 0;
    S.t_read = S.t_conv = S.t_enc = 0;
    S.cx = S.cy = -1;
    return 1;
}

static void on_file(void *u, int field, const uint8_t *d, size_t n)
{
    slot *sl = (slot *)u;
    if (sl->kind == K_FILES)
        cdv_files_message(sl->files, field, d, n);
}

/* An arrow for the peer to draw where our pointer is: X's left_ptr in black
 * with a white edge. */
static void send_arrow(cdv_session *s)
{
    static const char *shape[] = {
        "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
        "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
        "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
        "     X..X   ", "      XX    ",
    };
    uint8_t rgba[17 * 12 * 4];
    int x, y;
    for (y = 0; y < 17; y++)
        for (x = 0; x < 12; x++) {
            uint8_t *p = rgba + (y * 12 + x) * 4;
            char c = shape[y][x];
            p[0] = p[1] = p[2] = c == '.' ? 0 : 255;
            p[3] = c == ' ' ? 0 : 255;
        }
    cdv_send_cursor(s, 1, 0, 0, 12, 17, rgba);
}

static int slots_free(void)
{
    int i, n = 0;
    for (i = 0; i < NSLOTS; i++)
        n += slots[i].fd < 0;
    return n;
}

static void session_begin(int fd, int secure, const char *how)
{
    int one = 1, i;
    slot *sl = NULL;
    for (i = 0; i < NSLOTS && !sl; i++)
        if (slots[i].fd < 0)
            sl = &slots[i];
    if (!sl) {
        cdv_log("%s connection refused: both sessions are in use", how);
        close(fd);
        return;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    nonblock(fd);
    sl->fd = fd;
    sl->kind = K_NONE;
    sl->started = now_ms();
    copy(sl->how, how, sizeof sl->how);
    sl->hooks.mouse = on_mouse;
    sl->hooks.key = on_key;
    sl->hooks.clipboard = on_clipboard;
    sl->hooks.log = on_log;
    sl->hooks.option = on_option;
    sl->hooks.login = on_login;
    sl->hooks.file = on_file;
    sl->hooks.user = sl;
    ident.username = console_user();
    cdv_init2(&sl->s, sl->ctl, CTL_CAP, NULL, 0, sl->in, IN_CAP, &sl->hooks, &ident, now_ms());
    cdv_start(&sl->s, now_ms(), secure);
    cdv_log("%s connection; %s", how, secure ? "encrypted" : "direct");
}

static void session_end(slot *sl, const char *why)
{
    if (sl->fd < 0)
        return;
    close(sl->fd);
    sl->fd = -1;
    if (sl == desk) {
        if (S.encoding)
            vp8e_abandon(S.enc);
        S.encoding = 0;
        screen_stop();
        input_release_all();
        desk = NULL;
    }
    if (sl->kind == K_FILES)
        cdv_files_close(sl->files);
    cdv_log("%s session over: %s", sl->kind == K_FILES ? "file" : "desktop", why);
    sl->kind = K_NONE;
}

/* Bytes in and out; 0 when the session has ended. */
static int session_io(slot *sl, int readable)
{
    size_t n;
    const uint8_t *o;
    if (readable) {
        static uint8_t buf[16384];
        int r = recv(sl->fd, (char *)buf, sizeof buf, 0);
        if (r == 0 || (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
            cdv_debug("recv: %d, errno %d", r, r < 0 ? errno : 0);
            session_end(sl, r == 0 ? "the peer closed the connection" : strerror(errno));
            return 0;
        }
        if (r > 0) {
            int was_live = sl->s.state == CDV_LIVE;
            rng_add(buf, r < 32 ? (size_t)r : 32);
            if (cdv_feed(&sl->s, buf, (size_t)r) < 0 && !cdv_out_peek(&sl->s, &n)) {
                session_end(sl, "protocol error");
                return 0;
            }
            if (!was_live && sl->s.state == CDV_LIVE) {
                cdv_log("logged in: %s (%s %s), %s", sl->s.peer_name, sl->s.peer_platform,
                        sl->s.peer_version, sl->kind == K_FILES ? "file manager" : "desktop");
                if (sl == desk) {
                    uint32_t t0 = now_ms();
                    input_release_all();
                    screen_set_scan((uint32_t)prefs.scan_ms);
                    screen_start(&scr);
                    cdv_log("first look at the screen: %u ms", (unsigned)(now_ms() - t0));
                    S.key = 1;
                    send_arrow(&sl->s);
                } else if (sl->kind == K_FILES && !sl->files_started) {
                    cdv_files_start(sl->files);
                    sl->files_started = 1;
                }
            }
        }
    }
    if (sl->kind == K_FILES && sl->s.state == CDV_LIVE)
        cdv_files_pump(sl->files);
    for (;;) {
        int w;
        o = cdv_out_peek(&sl->s, &n);
        if (!n)
            break;
        w = send(sl->fd, (const char *)o, (int)n, 0);
        if (w < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                break;
            session_end(sl, "send failed");
            return 0;
        }
        cdv_out_consume(&sl->s, (size_t)w);
        if ((size_t)w < n)
            break;
    }
    cdv_tick(&sl->s, now_ms());
    if (sl->s.state == CDV_CLOSED && !cdv_out_peek(&sl->s, &n)) {
        session_end(sl, "closed");
        return 0;
    }
    if (sl->s.state != CDV_LIVE && now_ms() - sl->started > 15000) {
        session_end(sl, "no login within 15 s");
        return 0;
    }
    return 1;
}

/* Which dirty macroblocks this frame codes (enc_dirty), within the budget:
 * those that have waited longest first, so a busy corner cannot starve the
 * rest of the screen, then the rows nearest the pointer, where the person is
 * looking. Cheap ones always go. */
static void select_mbs(void)
{
    int n = scr.mbw * scr.mbh, i, budget = FRAME_BUDGET, prow, d, age, maxage = 0;
    memset(enc_dirty, 0, (size_t)n);
    S.f_mbs = S.f_detail = S.f_moved_hint = S.f_flat = 0;
    for (i = 0; i < n; i++)
        if (scr.dirty[i] && dirty_age[i] > maxage)
            maxage = dirty_age[i];
    prow = S.cy >= 0 ? S.cy / 16 : scr.mbh / 2;
    for (age = maxage; age >= 0; age--)
        for (d = 0; d < 2 * scr.mbh; d++) {
            int row = d & 1 ? prow + (d + 1) / 2 : prow - d / 2, c;
            if (row < 0 || row >= scr.mbh)
                continue;
            for (c = 0; c < scr.mbw; c++) {
                int cost, kind;
                i = row * scr.mbw + c;
                if (!scr.dirty[i] || dirty_age[i] != age)
                    continue;
                kind = scr.flat[i] ? 0 : (scr.hints[i].dx || scr.hints[i].dy) ? 1 : 2;
                cost = kind == 0 ? COST_FLAT : kind == 1 ? COST_MOVED : cost_detail;
                if (budget < cost && S.f_mbs > 0)
                    return;
                enc_dirty[i] = 1;
                S.f_mbs++;
                budget -= cost;
                if (kind == 0)
                    S.f_flat++;
                else if (kind == 1)
                    S.f_moved_hint++;
                else
                    S.f_detail++;
            }
        }
}

/* How many macroblock rows to encode between looks at the network: about
 * a tenth of a second's work on the R3000 at full damage. */
#define SLICE_ROWS 4

static void frame_step(void)
{
    uint32_t t;
    if (!desk || desk->s.state != CDV_LIVE)
        return;
    if (!S.encoding) {
        size_t cap;
        uint8_t *dst;
        vp8e_src src;
        uint32_t now = now_ms();
        if (desk->s.vstate != VID_FREE || now - S.last_frame < (uint32_t)frame_ms)
            return;
        if (cdv_take_refresh(&desk->s))
            S.key = 1;
        if (!S.key && !scr.ndirty && !screen_pending(&scr, now))
            return;
        screen_collect(&scr, 1, now);
        S.t_read += scr.read_ms;
        S.t_conv += scr.convert_ms;
        if (S.key) {
            /* The first picture, or the peer asked for one: a keyframe of
             * the background colour alone (one macroblock coded, the rest
             * the encoder's cache), then the real picture over the next
             * frames, within the budget. */
            uint8_t by, bu, bv;
            screen_background(&by, &bu, &bv);
            memset(flat_y, by, (size_t)scr.ystride * scr.mbh * 16);
            memset(flat_u, bu, (size_t)scr.uvstride * scr.mbh * 8);
            memset(flat_v, bv, (size_t)scr.uvstride * scr.mbh * 8);
            screen_mark_all(&scr);
            memset(dirty_age, 1, (size_t)(scr.mbw * scr.mbh));
            src.y = flat_y;
            src.u = flat_u;
            src.v = flat_v;
            memset(enc_dirty, 1, (size_t)(scr.mbw * scr.mbh));
            S.f_mbs = S.f_detail = S.f_moved_hint = S.f_flat = 0;
        } else {
            if (!scr.ndirty)
                return;
            select_mbs();
            src.y = scr.y;
            src.u = scr.u;
            src.v = scr.v;
        }
        dst = cdv_video_begin(&desk->s, &cap);
        if (!dst)
            return;
        src.ystride = scr.ystride;
        src.uvstride = scr.uvstride;
        vp8e_set_hints(S.enc, S.key || !scr.nhints ? NULL : scr.hints);
        if (!vp8e_begin(S.enc, &src, S.key ? NULL : enc_dirty, S.key, dst, cap)) {
            cdv_video_abort(&desk->s);
            return;
        }
        S.encoding = 1;
        S.mbs += (unsigned)S.f_mbs;
        S.last_frame = now;
        S.f_start = now;
        S.f_enc = 0;
        cdv_debug("frame: %d of %d dirty (%d detail, %d flat, %d hinted; %d damaged, %d "
                  "scanned rows), %d reads %d px, read %u ms, convert %u ms",
                  S.f_mbs, scr.ndirty, S.f_detail, S.f_flat, S.f_moved_hint, scr.damaged,
                  scr.scanned, scr.reads, scr.read_px, (unsigned)scr.read_ms,
                  (unsigned)scr.convert_ms);
    }
    t = now_ms();
    if (vp8e_rows(S.enc, SLICE_ROWS)) {
        size_t len = vp8e_end(S.enc);
        vp8e_stats st;
        S.encoding = 0;
        S.t_enc += now_ms() - t;
        S.f_enc += now_ms() - t;
        if (!len) {
            cdv_video_abort(&desk->s);
            S.key = 1;
            cdv_log("frame did not fit; a keyframe next");
            return;
        }
        vp8e_last_stats(S.enc, &st);
        cdv_video_commit(&desk->s, len, st.key);
        if (!st.key) {
            int i, n = scr.mbw * scr.mbh;
            screen_sent_some(&scr, enc_dirty);
            for (i = 0; i < n; i++)
                dirty_age[i] = scr.dirty[i] ? (uint8_t)(dirty_age[i] < 250 ? dirty_age[i] + 1 : 250)
                                            : 0;
            /* learn what a detailed macroblock costs here, now */
            if (S.f_detail >= 16) {
                int rest = (int)S.f_enc * 100 - S.f_flat * COST_FLAT -
                           (st.moved > 0 ? st.moved : 0) * COST_MOVED;
                int per = rest / S.f_detail;
                if (per < 50)
                    per = 50;
                if (per > 2000)
                    per = 2000;
                cost_detail = (cost_detail * 3 + per) / 4;
            }
        }
        S.key = 0;
        S.frames++;
        S.bytes += (unsigned)len;
        cdv_debug("sent %s frame: %d mbs (%d intra, %d moved, %d skipped) in %u bytes; encode "
                  "%u ms of %u ms", st.key ? "key" : "inter", S.f_mbs, st.intra, st.moved, st.skipped,
                  (unsigned)len, (unsigned)S.f_enc, (unsigned)(now_ms() - S.f_start));
    } else {
        S.t_enc += now_ms() - t;
        S.f_enc += now_ms() - t;
    }
}

static void session_report(void)
{
    uint32_t now = now_ms(), span = now - S.last_report;
    unsigned long trapped, ignored;
    if (!desk || span < 10000 || desk->s.state != CDV_LIVE)
        return;
    screen_stats(&trapped, &ignored);
    cdv_log("%u frames in %u.%u s (%u.%u fps), %u KB, %u macroblocks; ms read %u convert %u "
            "encode %u; %lu requests trapped",
            S.frames, (unsigned)(span / 1000), (unsigned)(span % 1000 / 100),
            (unsigned)(S.frames * 10000u / span / 10), (unsigned)(S.frames * 10000u / span % 10),
            S.bytes / 1024, S.mbs, (unsigned)S.t_read, (unsigned)S.t_conv, (unsigned)S.t_enc,
            trapped);
    S.frames = S.bytes = S.mbs = 0;
    S.t_read = S.t_conv = S.t_enc = 0;
    S.last_report = now;
}

static void cursor_step(void)
{
    int x, y;
    uint32_t now = now_ms();
    if (!desk || desk->s.state != CDV_LIVE || now - S.last_cursor < 100)
        return;
    S.last_cursor = now;
    if (screen_pointer(&x, &y) && (x != S.cx || y != S.cy)) {
        S.cx = x;
        S.cy = y;
        cdv_send_cursor_pos(&desk->s, x, y);
    }
}

/* ---- the ID server ------------------------------------------------------- */

static cdv_rdv rdv;
static int udp_rdv = -1, udp_lan = -1, tcp_direct = -1;
static uint32_t hbbs_ip, resolve_at;
static uint16_t hbbs_port;
static char hbbs_host[64];

static void rdv_setup(void)
{
    memset(&rdv, 0, sizeof rdv);
    rdv_init(&rdv);
    copy(rdv.id, prefs.id, sizeof rdv.id);
    memcpy(rdv.uuid, prefs.uuid, 16);
    memcpy(rdv.pk, sign_pk, 32);
    copy(rdv.key, prefs.key, sizeof rdv.key);
    copy(rdv.relay, prefs.relay, sizeof rdv.relay);
    if (prefs.server[0])
        rdv_split_host(prefs.server, hbbs_host, sizeof hbbs_host, &hbbs_port, RDV_PORT);
}

static void rdv_step(void)
{
    uint8_t buf[2048];
    size_t n;
    struct sockaddr_in sa;
    uint32_t now = now_ms();
    if (!prefs.server[0])
        return;
    if (!hbbs_ip) {
        if ((int32_t)(now - resolve_at) < 0)
            return;
        resolve_at = now + 30000;
        if (!resolve(hbbs_host, &hbbs_ip)) {
            cdv_log("cannot resolve %s; trying again in 30 s", hbbs_host);
            return;
        }
        cdv_log("ID server %s is %u.%u.%u.%u", hbbs_host, hbbs_ip >> 24, hbbs_ip >> 16 & 255,
                hbbs_ip >> 8 & 255, hbbs_ip & 255);
    }
    if (udp_rdv < 0 && (udp_rdv = listen_on(0, 1)) < 0)
        return;
    n = rdv_tick(&rdv, now, buf, sizeof buf);
    if (n) {
        sa_set(&sa, hbbs_ip, hbbs_port);
        sendto(udp_rdv, (const char *)buf, (int)n, 0, (struct sockaddr *)&sa, sizeof sa);
    }
}

/* Where to reach the peer through hbbr, told to hbbs, then joined. */
static void rdv_relay(rdv_action *act)
{
    uint8_t buf[1024];
    char rhost[128];
    uint16_t rport;
    uint32_t rip;
    size_t n;
    int t, fd;
    cdv_log("relay request (%s) from %u.%u.%u.%u via %s", act->initiate ? "punch hole" : "relay",
            act->peer_ip >> 24, act->peer_ip >> 16 & 255, act->peer_ip >> 8 & 255,
            act->peer_ip & 255, act->relay);
    if (!slots_free()) {
        cdv_log("both sessions are in use; ignoring it");
        return;
    }
    t = tcp_connect(hbbs_ip, hbbs_port, 5000);
    if (t >= 0) {
        n = rdv_relay_response(&rdv, act, buf, sizeof buf);
        send_all(t, buf, n);
        close(t);
    }
    rdv_split_host(act->relay, rhost, sizeof rhost, &rport, RELAY_PORT);
    if (!resolve(rhost, &rip)) {
        cdv_log("cannot resolve the relay %s", rhost);
        return;
    }
    fd = tcp_connect(rip, rport, 8000);
    if (fd < 0) {
        cdv_log("cannot reach the relay %s", act->relay);
        return;
    }
    n = rdv_request_relay(&rdv, act, buf, sizeof buf);
    if (send_all(fd, buf, n) < 0) {
        close(fd);
        return;
    }
    session_begin(fd, 1, "relayed");
}

/* The peer is on our network: listen, tell hbbs where, wait for it. */
static void rdv_local(rdv_action *act)
{
    struct sockaddr_in la, me;
    int l, t, ll = sizeof la, ml = sizeof me;
    uint8_t buf[512];
    size_t n;
    struct pollfd p;
    if (!slots_free())
        return;
    l = listen_on(0, 0);
    if (l < 0)
        return;
    getsockname(l, (struct sockaddr *)&la, &ll);
    t = tcp_connect(hbbs_ip, hbbs_port, 5000);
    if (t < 0) {
        close(l);
        return;
    }
    getsockname(t, (struct sockaddr *)&me, &ml);
    n = rdv_local_addr(&rdv, act, (uint32_t)ntohl(me.sin_addr.s_addr), ntohs(la.sin_port), buf,
                       sizeof buf);
    send_all(t, buf, n);
    close(t);
    cdv_log("told the ID server we are at port %u; waiting for the peer", ntohs(la.sin_port));
    p.fd = l;
    p.events = POLLIN;
    if (poll(&p, 1, 10000) > 0) {
        int fd = accept(l, NULL, NULL);
        if (fd >= 0)
            session_begin(fd, 1, "local");
    } else {
        cdv_log("the peer never came");
    }
    close(l);
}

static void rdv_input_step(void)
{
    uint8_t buf[2048], rep[512];
    size_t replen;
    rdv_action act;
    struct sockaddr_in sa;
    int r = recv(udp_rdv, (char *)buf, sizeof buf, 0), kind;
    if (r <= 0)
        return;
    rng_add(buf, r < 16 ? (size_t)r : 16);
    kind = rdv_input(&rdv, buf, (size_t)r, now_ms(), &act, rep, sizeof rep, &replen);
    if (replen) {
        sa_set(&sa, hbbs_ip, hbbs_port);
        sendto(udp_rdv, (const char *)rep, (int)replen, 0, (struct sockaddr *)&sa, sizeof sa);
    }
    if (kind == RDV_REGISTERED)
        cdv_log("registered: reachable as ID %s", prefs.id);
    else if (kind == RDV_REFUSED)
        cdv_log("the ID server refused the registration (%d)", act.refuse_code);
    else if (kind == RDV_RELAY)
        rdv_relay(&act);
    else if (kind == RDV_LOCAL)
        rdv_local(&act);
}

static void lan_step(void)
{
    uint8_t buf[1024], out[1024];
    struct sockaddr_in from;
    int fl = sizeof from;
    int r = recvfrom(udp_lan, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    size_t n;
    if (r <= 0)
        return;
    n = lan_answer(buf, (size_t)r, prefs.id, prefs.id, hostname, out, sizeof out);
    if (n)
        sendto(udp_lan, (const char *)out, (int)n, 0, (struct sockaddr *)&from, sizeof from);
}

/* ---- main ------------------------------------------------------------------ */

static int on_x_io(Display *d)
{
    (void)d;
    if (x_jmp_set)
        longjmp(x_jmp, 1);
    cdv_log("X I/O error outside the main loop");
    exit(1);
}

static void x_try_open(void)
{
    if (x_ok || (int32_t)(now_ms() - x_retry) < 0)
        return;
    x_retry = now_ms() + 5000;
    if (!screen_open(&scr, x_display, prefs.gamma))
        return;
    XSetIOErrorHandler(on_x_io);
    input_init(screen_display());
    clip_init(screen_display(), clipboard_to_peer);
    x_ok = 1;
    cdv_log("display open: %dx%d", scr.width, scr.height);
}

static void x_lost(void)
{
    cdv_log("the X display went away (a logout resets the server); reconnecting");
    /* Forget the connection first: ending the session would otherwise make
     * X calls on it, and fail back here. */
    x_ok = 0;
    screen_lost();
    input_init(NULL);
    if (desk)
        session_end(desk, "the X display went away");
    x_retry = now_ms() + 3000;
}

static void daemonize(void)
{
    int fd;
    if (fork() > 0)
        exit(0);
    setsid();
    if (fork() > 0)
        exit(0);
    chdir("/");
    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
}

static volatile int quit;
static void on_term(int sig)
{
    (void)sig;
    quit = 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: C-Desk-Vint [-c prefs] [-d display] [-l logfile] [-p port] [-v] [-D] [-i]\n"
                    "  -D  detach and run as a daemon (log with -l)\n"
                    "  -i  print this machine's ID and password, and exit\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *display = NULL, *logpath = NULL;
    int verbose = 0, i, port = 0, detach = 0, info = 0;
    const char *home = getenv("HOME");

    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN); /* started from a telnet session that ends */
    signal(SIGTERM, on_term); /* leave by exit(), so a profiling build writes mon.out */
    signal(SIGINT, on_term);
    snprintf(prefs_path, sizeof prefs_path, "%s/.c-desk-vint", home && *home ? home : "");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            copy(prefs_path, argv[++i], sizeof prefs_path);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            display = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc)
            logpath = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-D"))
            detach = 1;
        else if (!strcmp(argv[i], "-i"))
            info = 1;
        else
            usage();
    }
    if (info) {
        /* Who this machine is to RustDesk (made on first use), then out. */
        load_prefs();
        printf("ID: %s\nPassword: %s\nID server: %s\nDirect access: port %d\n", prefs.id,
               prefs.password, prefs.server[0] ? prefs.server : "(none)", prefs.port);
        return 0;
    }
    if (detach)
        daemonize();
    cdv_log_open(logpath, verbose);
    cdv_log("%s starting", VERSION_STRING);
    load_prefs();
    if (port)
        prefs.port = port;
    crypto_sign_ed25519_seed_keypair(sign_pk, sign_sk, prefs.seed);
    if (prefs.name[0])
        copy(hostname, prefs.name, sizeof hostname);
    else if (gethostname(hostname, sizeof hostname) < 0 || !hostname[0])
        copy(hostname, "IRIS", sizeof hostname);

    if (!display && !getenv("DISPLAY"))
        display = ":0";
    x_display = display;
    /* deadlines are compared as int32 differences of a wrapping clock */
    x_retry = resolve_at = now_ms();
    x_try_open();
    if (!x_ok)
        cdv_log("no X display yet; trying every 5 s");

    vidbuf = (uint8_t *)malloc(VID_CAP);
    encmem = (uint8_t *)malloc(vp8e_mem_size(MAX_W, MAX_H) + 16);
    for (i = 0; i < NSLOTS; i++) {
        slots[i].fd = -1;
        slots[i].ctl = (uint8_t *)malloc(CTL_CAP);
        slots[i].in = (uint8_t *)malloc(IN_CAP);
        slots[i].work = (uint8_t *)malloc(WORK_CAP);
        slots[i].files = (cdv_files *)malloc(cdv_files_size());
        if (!slots[i].ctl || !slots[i].in || !slots[i].work || !slots[i].files)
            encmem = NULL;
    }
    if (!vidbuf || !encmem) {
        cdv_log("out of memory");
        return 1;
    }
    if (strcmp(prefs.files, "off"))
        files_fs = fs_posix(strcmp(prefs.files, "/") ? prefs.files : "");
    encmem = (uint8_t *)(((unsigned long)encmem + 15) & ~15ul);
    refmem = (uint8_t *)malloc(vp8e_ref_size(MAX_W, MAX_H)); /* motion vectors */
    enc_dirty = (uint8_t *)calloc((size_t)(MAX_W / 16 * MAX_H / 16), 1);
    dirty_age = (uint8_t *)calloc((size_t)(MAX_W / 16 * MAX_H / 16), 1);
    flat_y = (uint8_t *)malloc((size_t)MAX_W * MAX_H);
    flat_u = (uint8_t *)malloc((size_t)MAX_W * MAX_H / 4);
    flat_v = (uint8_t *)malloc((size_t)MAX_W * MAX_H / 4);

    ident.id = prefs.id;
    ident.sign_sk = sign_sk;
    ident.password = prefs.password;
    ident.salt = "cdeskvint";
    ident.hostname = hostname;
    ident.width = 1024; /* set from the screen at each desktop login */
    ident.height = 768;
    ident.cursor_embedded = 0;
    ident.platform = "Linux";
    ident.username = "root";

    tcp_direct = listen_on((uint16_t)prefs.port, 0);
    if (tcp_direct < 0)
        cdv_log("cannot listen on port %d; direct IP access is off", prefs.port);
    udp_lan = listen_on(LAN_PORT, 1);
    rdv_setup();
    cdv_log("ID %s, password %s; direct access on port %d; ID server %s", prefs.id,
            prefs.password, prefs.port, prefs.server[0] ? prefs.server : "(none)");

    if (setjmp(x_jmp))
        x_lost();
    x_jmp_set = 1;
    while (!quit) {
        struct pollfd p[4 + NSLOTS];
        int np = 0, ix = -1, id = -1, ir = -1, il = -1, is[NSLOTS], timeout = 200, k;
        size_t n;

        rdv_step();
        x_try_open();
        if (desk && desk->s.state == CDV_LIVE) {
            /* busy: something to read back or a frame being encoded */
            if (S.encoding || (desk->s.vstate == VID_FREE &&
                               (S.key || scr.ndirty || screen_pending(&scr, now_ms()))))
                timeout = S.encoding ? 0 : 10;
            else
                timeout = 30;
        }
        if (x_ok) {
            ix = np;
            p[np].fd = screen_fd();
            p[np++].events = POLLIN;
        }
        if (tcp_direct >= 0) {
            id = np;
            p[np].fd = tcp_direct;
            p[np++].events = POLLIN;
        }
        if (udp_rdv >= 0) {
            ir = np;
            p[np].fd = udp_rdv;
            p[np++].events = POLLIN;
        }
        if (udp_lan >= 0) {
            il = np;
            p[np].fd = udp_lan;
            p[np++].events = POLLIN;
        }
        for (k = 0; k < NSLOTS; k++) {
            is[k] = -1;
            if (slots[k].fd < 0)
                continue;
            is[k] = np;
            p[np].fd = slots[k].fd;
            p[np].events = POLLIN;
            if (cdv_out_peek(&slots[k].s, &n), n)
                p[np].events |= POLLOUT;
            /* a download keeps the file manager busy */
            if (slots[k].kind == K_FILES && timeout > 10)
                timeout = 10;
            np++;
        }
        for (i = 0; i < np; i++)
            p[i].revents = 0;
        if (poll(p, (unsigned long)np, timeout) < 0 && errno != EINTR) {
            cdv_log("poll failed: %s", strerror(errno));
            sleep(1);
            continue;
        }
        if (ix >= 0 && (p[ix].revents & POLLIN))
            screen_events(&scr);
        if (id >= 0 && (p[id].revents & POLLIN)) {
            int fd = accept(tcp_direct, NULL, NULL);
            if (fd >= 0)
                session_begin(fd, 0, "direct");
        }
        if (ir >= 0 && (p[ir].revents & POLLIN))
            rdv_input_step();
        if (il >= 0 && (p[il].revents & POLLIN))
            lan_step();
        for (k = 0; k < NSLOTS; k++) {
            slot *sl = &slots[k];
            int rd;
            if (sl->fd < 0 || is[k] < 0)
                continue;
            rd = (p[is[k]].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
            if (!session_io(sl, rd))
                continue;
            if (sl == desk) {
                screen_events(&scr);
                clip_tick();
                frame_step();
                cursor_step();
                session_report();
            }
            if (sl->fd >= 0)
                session_io(sl, 0);
        }
    }
    for (i = 0; i < NSLOTS; i++)
        session_end(&slots[i], "the agent is stopping");
    cdv_log("stopped");
    return 0;
}
