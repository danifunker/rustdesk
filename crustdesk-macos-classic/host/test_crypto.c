/* The vendored libsodium subset, driven with fixed inputs; host/test_crypto.py
 * checks every output against PyNaCl (the system libsodium). */
#include "sodium/crypto_box.h"
#include "sodium/crypto_secretbox.h"
#include "sodium/crypto_sign_ed25519.h"

#include <stdio.h>
#include <string.h>

static void hex(const char *label, const unsigned char *b, unsigned long long n)
{
    unsigned long long i;
    printf("%s ", label);
    for (i = 0; i < n; i++)
        printf("%02x", b[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    unsigned char seed[32], pk[32], sk[64], sm[64 + 64], bpk[32], bsk[32], key[32], nonce[24];
    unsigned char c[256], m[256];
    unsigned long long n;
    const char *msg = "C-Desk-Vint signs this";
    int i;
    for (i = 0; i < 32; i++) {
        seed[i] = (unsigned char)(i * 7 + 1);
        bsk[i] = (unsigned char)(i * 13 + 5);
        key[i] = (unsigned char)(i * 3 + 9);
    }
    for (i = 0; i < 24; i++)
        nonce[i] = (unsigned char)i;

    crypto_sign_ed25519_seed_keypair(pk, sk, seed);
    hex("sign_pk", pk, 32);
    crypto_sign_ed25519(sm, &n, (const unsigned char *)msg, strlen(msg), sk);
    hex("signed", sm, n);

    crypto_scalarmult_curve25519_base(bpk, bsk);
    hex("box_pk", bpk, 32);

    crypto_secretbox_easy(c, (const unsigned char *)msg, strlen(msg), nonce, key);
    hex("secretbox", c, strlen(msg) + 16);

    /* argv[1]: a box made by PyNaCl for us: open it. */
    if (argc > 2) {
        unsigned char peer[32], boxed[128];
        size_t bl = strlen(argv[2]) / 2;
        for (i = 0; i < 32; i++)
            sscanf(argv[1] + 2 * i, "%2hhx", &peer[i]);
        for (i = 0; i < (int)bl; i++)
            sscanf(argv[2] + 2 * i, "%2hhx", &boxed[i]);
        memset(nonce, 0, sizeof nonce);
        if (crypto_box_open_easy(m, boxed, bl, nonce, peer, bsk) == 0)
            hex("opened", m, bl - 16);
        else
            printf("opened FAILED\n");
    }
    return 0;
}
