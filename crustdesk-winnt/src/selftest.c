/* -selftest: did this machine's compiler build the shared core right?
 *
 * Visual C++ 4 is a 1996 compiler, with four code generators (x86, MIPS,
 * Alpha, PowerPC) that saw far less use than GCC's. Everything the agent
 * computes -- hashes, signatures, the session's encryption, the VP8
 * bitstream, PNG, zstd -- is run here on fixed inputs, and the results are
 * compared with what the same code gives built by GCC on Linux (the
 * EXPECT_* values; `make selftest-host` checks them there). The signature
 * and the two boxes were checked against libsodium itself (PyNaCl); PNG and
 * VP8 are what GCC's build makes, whose PNG the host tests read back with
 * PIL and whose VP8 they decode with libvpx.
 *
 * Plain C over the core only, so the same file builds on the host. */
#include "selftest.h"

#include "png.h"
#include "sha256.h"
#include "unzstd.h"
#include "vp8enc.h"
#include "sodium/crypto_box.h"
#include "sodium/crypto_secretbox.h"
#include "sodium/crypto_sign_ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT_SHA256 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define EXPECT_SIGN "d92ecdcb016c577b8eddd179d3fb0523332ff352e0fce75d9e80619a71747344"
#define EXPECT_BOX "4a432de4355724b36f39e24a7f236e2030e00d3cc2549896e014dbbcbef71a41"
#define EXPECT_SECRETBOX "34e480ef8ce7f0d8b442a51a67311f4507386e83762e3e1e700069f13b1e2db5"
#define EXPECT_PNG "740c6e34c7110475ede0596c1ccb36e4aa0423359a53a2c271daa50ec3f1c235"
#define EXPECT_VP8 "712b1e25fb784a2b7496bdfbbc760e77346c8c8085e273d9182a139095f6d08a"

static void (*say)(const char *);
static int failed;

static void hexify(const uint8_t *d, char out[65])
{
    int i;
    for (i = 0; i < 32; i++)
        sprintf(out + 2 * i, "%02x", d[i]);
}

static void hash_hex(const void *data, size_t n, char out[65])
{
    uint8_t h[32];
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, h);
    hexify(h, out);
}

static void check(const char *name, const char *got, const char *want)
{
    char line[200];
    if (!strcmp(want, "?"))
        sprintf(line, "INFO %-10s %s\r\n", name, got);
    else if (!strcmp(got, want))
        sprintf(line, "PASS %-10s\r\n", name);
    else {
        sprintf(line, "FAIL %-10s got %s\r\n", name, got);
        failed++;
    }
    say(line);
}

static void t_sha256(void)
{
    char h[65];
    hash_hex("abc", 3, h);
    check("sha256", h, EXPECT_SHA256);
}

static void t_sign(void)
{
    uint8_t seed[32], pk[32], sk[64], sm[64 + 32], both[96];
    unsigned long long smlen;
    char h[65];
    int i;
    for (i = 0; i < 32; i++)
        seed[i] = (uint8_t)i;
    crypto_sign_ed25519_seed_keypair(pk, sk, seed);
    crypto_sign_ed25519(sm, &smlen, (const uint8_t *)"C-Desk-Vint selftest message..", 30, sk);
    memcpy(both, pk, 32);
    memcpy(both + 32, sm, 64);
    hash_hex(both, 96, h);
    check("ed25519", h, EXPECT_SIGN);
}

static void t_box(void)
{
    uint8_t sa[32], sb[32], pka[32], ska[32], pkb[32], skb[32], nonce[24];
    uint8_t m[64], c[64 + 16], back[64];
    char h[65];
    int i;
    for (i = 0; i < 32; i++) {
        sa[i] = (uint8_t)(3 * i + 1);
        sb[i] = (uint8_t)(5 * i + 2);
    }
    for (i = 0; i < 24; i++)
        nonce[i] = (uint8_t)(i * 11);
    for (i = 0; i < 64; i++)
        m[i] = (uint8_t)(i * 7 + 3);
    crypto_box_seed_keypair(pka, ska, sa);
    crypto_box_seed_keypair(pkb, skb, sb);
    crypto_box_easy(c, m, 64, nonce, pkb, ska);
    hash_hex(c, sizeof c, h);
    if (crypto_box_open_easy(back, c, sizeof c, nonce, pka, skb) != 0 || memcmp(back, m, 64))
        strcpy(h, "does-not-open");
    check("box", h, EXPECT_BOX);
}

static void t_secretbox(void)
{
    uint8_t key[32], nonce[24], m[100], c[100 + 16], back[100];
    char h[65];
    int i;
    for (i = 0; i < 32; i++)
        key[i] = (uint8_t)(7 * i);
    for (i = 0; i < 24; i++)
        nonce[i] = (uint8_t)(255 - i);
    for (i = 0; i < 100; i++)
        m[i] = (uint8_t)(i ^ 0x5a);
    crypto_secretbox_easy(c, m, 100, nonce, key);
    hash_hex(c, sizeof c, h);
    c[20] ^= 1;
    if (crypto_secretbox_open_easy(back, c, sizeof c, nonce, key) == 0)
        strcpy(h, "opens-when-forged");
    check("secretbox", h, EXPECT_SECRETBOX);
}

/* `zstd -19` of the lines below. */
static const uint8_t zframe[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x64, 0x7a, 0x02, 0xad, 0x03, 0x00, 0x04, 0x05,
    0x43, 0x2d, 0x44, 0x65, 0x73, 0x6b, 0x2d, 0x56, 0x69, 0x6e, 0x74, 0x20,
    0x73, 0x65, 0x6c, 0x66, 0x74, 0x65, 0x73, 0x74, 0x20, 0x6c, 0x69, 0x6e,
    0x65, 0x20, 0x30, 0x3a, 0x20, 0x74, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69,
    0x63, 0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x20, 0x66, 0x6f, 0x78,
    0x20, 0x6a, 0x75, 0x6d, 0x70, 0x73, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x6c,
    0x61, 0x7a, 0x79, 0x20, 0x64, 0x6f, 0x67, 0x2e, 0x0a, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x31, 0x31, 0x0d, 0x20, 0x80, 0x33,
    0x64, 0x1d, 0x38, 0x8c, 0x04, 0xd1, 0x41, 0xcf, 0x50, 0x66, 0x51, 0x31,
    0xf4, 0x0c, 0x65, 0x16, 0x15, 0x43, 0xcf, 0xa0, 0xcc, 0x50, 0xb1, 0x43,
    0x6f, 0x43, 0x8b, 0x56, 0x08, 0x11, 0x0a, 0xed, 0x90, 0xf3, 0xa6
};

static void t_zstd(void)
{
    char want[1024], *got = (char *)malloc(4096);
    size_t n = 0;
    long len;
    int i;
    for (i = 0; i < 12; i++)
        n += (size_t)sprintf(want + n,
                             "C-Desk-Vint selftest line %d: the quick brown fox jumps over the lazy dog.\n", i);
    len = got ? cdv_unzstd(got, 4096, zframe, sizeof zframe) : -1;
    check("zstd", len == (long)n && !memcmp(got, want, n) ? "ok" : "wrong", "ok");
    free(got);
}

static void t_png(void)
{
    png_writer *p = (png_writer *)malloc(png_writer_size());
    uint8_t *out = (uint8_t *)malloc(65536), row[96 * 3];
    size_t len = 0;
    int x, y;
    char h[65];
    if (p && out) {
        png_begin(p, out, 65536, 96, 64, NULL, 0);
        for (y = 0; y < 64; y++) {
            for (x = 0; x < 96; x++) {
                row[3 * x] = (uint8_t)(x * 2 + y);
                row[3 * x + 1] = (uint8_t)((x / 8 + y / 8) & 1 ? 200 : 30);
                row[3 * x + 2] = (uint8_t)(y * 4);
            }
            png_row(p, row);
        }
        len = png_end(p);
    }
    hash_hex(out, len, h);
    check("png", len ? h : "no-png", EXPECT_PNG);
    free(p);
    free(out);
}

/* A desktop-like picture: flat fills, an edge, a gradient, "text" -- then
 * a second frame where some of it changed, coded as an inter frame. */
static void picture(uint8_t *y, uint8_t *u, uint8_t *v, int w, int h, int t)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            int val = j < 20 ? 40 : i < 60 ? 180 : (i + j * 2) & 255;
            if (j > 60 && j < 100 && i > 70 && i < 150 && ((i * 7 + j * 13 + t * 5) % 11) < 4)
                val = 16;
            y[j * w + i] = (uint8_t)val;
        }
    for (j = 0; j < h / 2; j++)
        for (i = 0; i < w / 2; i++) {
            u[j * (w / 2) + i] = (uint8_t)(j < 10 ? 200 : 128 + ((i + t * 3) & 31));
            v[j * (w / 2) + i] = (uint8_t)(i < 30 ? 90 : 128);
        }
}

static void t_vp8(void)
{
    enum { W = 176, H = 144 };
    uint8_t *mem = (uint8_t *)malloc(vp8e_mem_size(W, H) + 16), *al;
    uint8_t *y = (uint8_t *)malloc(W * H), *u = (uint8_t *)malloc(W * H / 4),
            *v = (uint8_t *)malloc(W * H / 4), *out = (uint8_t *)malloc(200000);
    uint8_t dirty[(W / 16) * (H / 16)];
    const uint8_t *ry, *ru, *rv;
    int ys, uvs, i, ok = 1;
    size_t n1 = 0, n2 = 0;
    vp8e *e;
    vp8e_src src;
    sha256_ctx c;
    uint8_t d[32];
    char h[65];
    if (!mem || !y || !u || !v || !out) {
        check("vp8", "no-memory", EXPECT_VP8);
        return;
    }
    al = (uint8_t *)(((unsigned long)mem + 15) & ~15ul);
    e = vp8e_init(al, W, H, 24);
    src.y = y;
    src.u = u;
    src.v = v;
    src.ystride = W;
    src.uvstride = W / 2;
    picture(y, u, v, W, H, 0);
    n1 = vp8e_encode(e, &src, NULL, 1, out, 100000);
    picture(y, u, v, W, H, 1);
    for (i = 0; i < (int)sizeof dirty; i++)
        dirty[i] = (uint8_t)((i / (W / 16)) >= 3 && (i / (W / 16)) <= 6);
    /* the second frame a few rows at a time, as the agent does it */
    if (!vp8e_begin(e, &src, dirty, 0, out + n1, 100000))
        ok = 0;
    while (ok && !vp8e_rows(e, 2))
        ;
    n2 = ok ? vp8e_end(e) : 0;
    vp8e_recon(e, &ry, &ru, &rv, &ys, &uvs);
    sha256_init(&c);
    sha256_update(&c, out, n1 + n2);
    for (i = 0; i < H; i++)
        sha256_update(&c, ry + i * ys, W);
    for (i = 0; i < H / 2; i++) {
        sha256_update(&c, ru + i * uvs, W / 2);
        sha256_update(&c, rv + i * uvs, W / 2);
    }
    sha256_final(&c, d);
    hexify(d, h);
    check("vp8", n1 && n2 ? h : "no-frames", EXPECT_VP8);
    free(mem);
    free(y);
    free(u);
    free(v);
    free(out);
}

int selftest(void (*out)(const char *))
{
    char line[64];
    say = out;
    failed = 0;
    t_sha256();
    t_sign();
    t_box();
    t_secretbox();
    t_zstd();
    t_png();
    t_vp8();
    sprintf(line, failed ? "%d FAILED\r\n" : "all passed\r\n", failed);
    say(line);
    return failed ? 1 : 0;
}

#ifdef SELFTEST_MAIN
static void put(const char *s)
{
    fputs(s, stdout);
}

int main(void)
{
    return selftest(put);
}
#endif
