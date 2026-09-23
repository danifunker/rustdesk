/* Unit checks for the portable core pieces that have exact answers. */
#include "../src/core/pb.h"
#include "../src/core/sha256.h"
#include "../src/core/yuv.h"

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
            memset(Y, 0, sizeof Y);
            yuv_convert(&fb, Y, U, V, 16, 8, NULL);
            CHECK(Y[0] == 235 && Y[1] == 16);
            if (!(Y[0] == 235 && Y[1] == 16))
                printf("  depth %d: %d %d\n", depth, Y[0], Y[1]);
        }
    }

    printf("%s\n", fails ? "FAIL" : "PASS: core");
    return fails != 0;
}
