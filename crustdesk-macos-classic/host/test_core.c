/* Unit checks for the portable core pieces that have exact answers. */
#include "../src/core/pb.h"
#include "../src/core/sha256.h"
#include "../src/core/yuv.h"
#include "../src/core/macroman.h"
#include "../src/core/dns.h"
#include "../src/core/rdv.h"
#include "../src/core/console.h"
#include "../src/core/https.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void hex(const uint8_t *b, int n, char *out)
{
    int i;
    for (i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
}

int main(void)
{
    uint8_t d[32], buf[512];
    char h[65];
    sha256_ctx c;
    pbw w;
    pbr r, sub;
    int i;

    /* FIPS 180-2 examples */
    sha256_init(&c);
    sha256_update(&c, "abc", 3);
    sha256_final(&c, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha256_init(&c);
    sha256_update(&c, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    sha256_final(&c, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* Nested message whose body outgrows a one-byte length. */
    pbw_init(&w, buf, sizeof buf);
    pbw_begin(&w, 8);
    pbw_begin(&w, 2);
    for (i = 0; i < 20; i++)
        pbw_string(&w, 2, "0123456789");
    pbw_end(&w);
    pbw_end(&w);
    CHECK(!w.overflow);
    pbr_init(&r, buf, pbw_len(&w, buf));
    CHECK(pbr_next(&r) && r.field == 8 && r.wire == PB_LEN);
    pbr_sub(&r, &sub);
    CHECK(pbr_next(&sub) && sub.field == 2 && sub.len == 20 * 12);
    CHECK(!pbr_next(&r) && !r.error);

    /* sint32 zigzag both ways */
    pbw_init(&w, buf, sizeof buf);
    pbw_sint(&w, 2, -5);
    pbw_sint(&w, 3, 70000);
    pbr_init(&r, buf, pbw_len(&w, buf));
    CHECK(pbr_next(&r) && pb_unzigzag32(r.v) == -5);
    CHECK(pbr_next(&r) && pb_unzigzag32(r.v) == 70000);

    /* Colour: white and black at every depth land on studio range. */
    {
        static const uint8_t rgb[2][3] = { { 255, 255, 255 }, { 0, 0, 0 } };
        yuv_clut t;
        yuv_fb fb;
        uint8_t px[4 * 16 * 2], Y[16 * 16], U[64], V[64];
        int depth;
        yuv_clut_build(&t, rgb, 2);
        for (depth = 1; depth <= 32; depth *= 2) {
            memset(px, 0, sizeof px);
            /* pixel 0 white, pixel 1 black */
            if (depth < 8)
                px[0] = (uint8_t)(1 << (8 - 2 * depth)); /* index 0 then index 1 */
            else if (depth == 8)
                px[1] = 1;
            else if (depth == 16)
                px[0] = 0x7f, px[1] = 0xff;
            else
                px[1] = px[2] = px[3] = 255;
            fb.base = px;
            fb.rowbytes = 64;
            fb.depth = depth;
            fb.width = 2;
            fb.height = 1;
            fb.clut = &t;
            fb.gamma = NULL;
            memset(Y, 0, sizeof Y);
            yuv_convert(&fb, Y, U, V, 16, 8, NULL);
            CHECK(Y[0] == 235 && Y[1] == 16);
            if (!(Y[0] == 235 && Y[1] == 16))
                printf("  depth %d: %d %d\n", depth, Y[0], Y[1]);
        }
    }

    /* Mac Roman round trip, with the line endings each side expects. */
    {
        static const uint8_t mac[] = { 'c', 'a', 'f', 0x8E, '\r', 0xD2, 'x', 0xD3, 0xA5 };
        uint8_t u[64], back[64];
        size_t nu = macroman_to_utf8(mac, sizeof mac, u, sizeof u), nb;
        CHECK(nu == 16 && !memcmp(u, "caf\xc3\xa9\n\xe2\x80\x9cx\xe2\x80\x9d\xe2\x80\xa2", 16));
        nb = utf8_to_macroman(u, nu, back, sizeof back);
        CHECK(nb == sizeof mac && !memcmp(back, mac, sizeof mac));
        nb = utf8_to_macroman((const uint8_t *)"a\r\nb\xe2\x82\xac", 7, back, sizeof back);
        CHECK(nb == 4 && back[1] == '\r' && back[3] == 0xDB); /* the euro, since Mac OS 8.5 */
    }

    /* DNS: a query, and an answer with a CNAME before the A record. */
    {
        uint8_t q[128], a[128];
        uint32_t ip = 0;
        size_t n = dns_query(q, sizeof q, 0x1234, "relay.home.dani.tech");
        static const uint8_t ans_tail[] = {
            0xC0, 0x0C, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xC0, 0x0C, /* CNAME -> itself */
            0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 50, 4, 7, 36 /* A */
        };
        CHECK(n == 12 + 22 + 4 && q[12] == 5 && !memcmp(q + 13, "relay", 5));
        memcpy(a, q, n);
        a[2] = 0x81;
        a[3] = 0x80;
        a[7] = 2; /* two answers */
        memcpy(a + n, ans_tail, sizeof ans_tail);
        CHECK(dns_answer(a, n + sizeof ans_tail, 0x1234, &ip) == 1 && ip == 0x32040724);
        CHECK(dns_answer(a, n + sizeof ans_tail, 0x9999, &ip) == 0);
        CHECK(dns_parse_ip("192.168.99.67", &ip) && ip == 0xC0A86343);
        CHECK(!dns_parse_ip("relay.home.dani.tech", &ip));
    }

    /* hbbs's address mangling round-trips (rendezvous.rs mangle/unmangle). */
    {
        uint8_t m[16];
        uint32_t ip;
        uint16_t port;
        size_t n = rdv_mangle(0xC0A86343, 21118, 0x9e3779b9u, m);
        rdv_unmangle(m, n, &ip, &port);
        CHECK(ip == 0xC0A86343 && port == 21118);
    }

    /* The console: URLs, bodies, and what its replies mean. */
    {
        console_url u;
        static const uint8_t uuid[16] = { 0xde, 0xad, 0xbe, 0xef };
        char out[512], b64[32];
        CHECK(console_parse_url("https://remote.home.dani.tech", &u) && u.secure &&
              u.port == 443 && !strcmp(u.host, "remote.home.dani.tech") && !u.prefix[0]);
        CHECK(console_parse_url("http://192.168.1.10:8080/", &u) && !u.secure && u.port == 8080 &&
              !strcmp(u.host, "192.168.1.10") && !u.prefix[0]);
        CHECK(console_parse_url("console.example.org/rd/", &u) && u.secure &&
              !strcmp(u.prefix, "/rd"));
        CHECK(!console_parse_url("ftp://nope", &u));
        CHECK(!console_parse_url("http://host:99999", &u));
        console_base64((const uint8_t *)"\xde\xad\xbe\xef", 4, b64);
        CHECK(!strcmp(b64, "3q2+7w=="));
        CHECK(console_heartbeat_json(out, sizeof out, "123456789", uuid) &&
              !strcmp(out, "{\"id\":\"123456789\",\"uuid\":\"3q2+7wAAAAAAAAAAAAAAAA==\"}"));
        /* Mac Roman to UTF-8, and JSON escapes: "Dani\xd5s \"Mac\"" */
        CHECK(console_sysinfo_json(out, sizeof out, "1", uuid, "68040", "64 MB", "System 7.5.5",
                                   "Dani\xd5s \"Mac\"", "") &&
              strstr(out, "\"hostname\":\"Dani\xe2\x80\x99s \\\"Mac\\\"\"") &&
              strstr(out, "\"version\":\"1.4.5\""));
        CHECK(console_sysinfo_json(out, sizeof out, "1", uuid, "", "", "", "", "") &&
              strstr(out, "\"hostname\":\"Macintosh\""));
        CHECK(!console_sysinfo_json(out, 40, "1", uuid, "", "", "", "x", ""));
        CHECK(console_wants_sysinfo("{\"sysinfo\":1}"));
        CHECK(console_wants_sysinfo("{ \"a\": [1,{\"sysinfo\":0}], \"sysinfo\" : true }"));
        CHECK(!console_wants_sysinfo("{}"));
        CHECK(!console_wants_sysinfo("{\"modified_at\":9,\"strategy\":{\"sysinfo\":1}}"));
        CHECK(!console_wants_sysinfo("{\"sysinfo\":false}"));
        CHECK(!console_wants_sysinfo("{\"x\":\"\\\"sysinfo\\\":1\"}"));
        CHECK(https_days(1970, 1, 1) == 719528 && https_days(0, 1, 1) == 0 &&
              https_days(2026, 9, 24) == 719528 + 20720);
    }

    /* HTTP replies, fed a few bytes at a time as a network would. */
    {
        static cdv_https h;
        static const char *const replies[] = {
            "HTTP/1.1 200 OK\r\nContent-Length: 15\r\n\r\nSYSINFO_UPDATED",
            "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
            "\r\n4\r\n{\"sy\r\n9;x=1\r\nsinfo\":1}\r\n0\r\n\r\n",
            "HTTP/1.0 301 Moved\r\nLocation: https://x/api/heartbeat\r\nContent-Length: 0\r\n\r\n",
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody until close",
        };
        static const char *const bodies[] = { "SYSINFO_UPDATED", "{\"sysinfo\":1}", "",
                                              "body until close" };
        int i;
        for (i = 0; i < 4; i++) {
            const char *r = replies[i];
            size_t len = strlen(r), off = 0;
            const uint8_t *o;
            memset(&h, 0, sizeof h);
            https_connected(&h, 0, "x", NULL, 0, 0, 0, NULL);
            CHECK(https_post(&h, "x", "/api/sysinfo", "{}"));
            CHECK(https_out(&h, &o) > 60 && !memcmp(o, "POST /api/sysinfo HTTP/1.1\r\n", 28));
            https_out_done(&h, https_out(&h, &o));
            while (off < len) {
                uint8_t *room;
                size_t k = https_in(&h, &room);
                if (k > 3)
                    k = 3;
                if (k > len - off)
                    k = len - off;
                memcpy(room, r + off, k);
                https_in_done(&h, k);
                off += k;
            }
            if (i == 3) {
                CHECK(https_poll(&h) == HS_BUSY);
                https_eof(&h);
            }
            CHECK(https_poll(&h) == HS_DONE);
            CHECK(!strcmp(h.body, bodies[i]));
            CHECK(https_needs_reconnect(&h) == (i >= 2));
        }
        CHECK(h.status == 200);
    }

    printf("%s\n", fails ? "FAIL" : "PASS: core");
    return fails != 0;
}
