/* MacBinary and BinHex against files made by real tools: testdata/installer.bin
 * (Retro68's MacBinary II) and installer.hqx (the same file as rb-cli writes
 * BinHex). The decoded BinHex must give exactly the MacBinary's forks. */
#include "../src/core/macbin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    rewind(f);
    d = malloc(*n);
    if (fread(d, 1, *n, f) != *n)
        *n = 0;
    fclose(f);
    return d;
}

static uint8_t forks[2][1 << 20];
static size_t flen[2];
static macbin_info hdr;
static int got_header;

static void on_header(void *u, const macbin_info *i)
{
    (void)u;
    hdr = *i;
    got_header++;
}

static void on_out(void *u, int fork, const uint8_t *d, size_t n)
{
    (void)u;
    memcpy(forks[fork] + flen[fork], d, n);
    flen[fork] += n;
}

int main(void)
{
    size_t bn, hn, i;
    uint8_t *bin = slurp("testdata/installer.bin", &bn), *hqx = slurp("testdata/installer.hqx", &hn);
    macbin_info mb, rt;
    uint8_t h[128];
    binhex_dec dec;
    if (!bin || !hqx) {
        printf("FAIL: testdata missing\n");
        return 1;
    }
    CHECK(macbin_decode_header(bin, &mb));
    CHECK(!strcmp(mb.name, "Install C-Desk-Vint") && mb.type == 0x4150504C /* APPL */ &&
          mb.creator == 0x43445669 /* CDVi */ && (mb.flags & 0x2000));
    CHECK(bn >= macbin_size(mb.dlen, mb.rlen));

    /* The BinHex, a byte at a time: streaming must not care where it splits. */
    binhex_init(&dec, on_header, on_out, NULL);
    for (i = 0; i < hn; i++)
        binhex_feed(&dec, hqx + i, 1);
    CHECK(binhex_done(&dec) == 1);
    CHECK(got_header == 1 && !strcmp(hdr.name, mb.name) && hdr.type == mb.type &&
          hdr.creator == mb.creator && hdr.dlen == mb.dlen && hdr.rlen == mb.rlen);
    CHECK(flen[0] == mb.dlen && !memcmp(forks[0], bin + 128, mb.dlen));
    CHECK(flen[1] == mb.rlen && !memcmp(forks[1], bin + 128 + macbin_pad(mb.dlen), mb.rlen));

    /* A damaged BinHex is caught by its CRCs. */
    {
        uint8_t *bad = malloc(hn);
        memcpy(bad, hqx, hn);
        bad[hn / 2] = bad[hn / 2] == 'A' ? 'B' : 'A';
        memset(flen, 0, sizeof flen);
        binhex_init(&dec, on_header, on_out, NULL);
        binhex_feed(&dec, bad, hn);
        CHECK(binhex_done(&dec) == -1);
        free(bad);
    }

    /* Our header, read back; and recognised as MacBinary by its CRC. */
    macbin_encode_header(h, &mb);
    CHECK(macbin_decode_header(h, &rt) && !strcmp(rt.name, mb.name) && rt.type == mb.type &&
          rt.flags == mb.flags && rt.dlen == mb.dlen && rt.rlen == mb.rlen);
    h[50] ^= 1; /* in the name: the CRC no longer matches */
    CHECK(!macbin_decode_header(h, &rt));
    static const uint8_t text[128] = "not a macbinary header at all, just text";
    CHECK(!macbin_decode_header(text, &rt));

    printf("%s\n", fails ? "FAIL" : "PASS: macbinary and binhex");
    return fails != 0;
}
