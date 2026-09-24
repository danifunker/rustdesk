# libsodium, the part RustDesk's crypto uses

From libsodium 1.0.18 (`~/sol9-deps/build/libsodium-1.0.18` on the build
machine; https://download.libsodium.org), ISC licence -- see `LICENSE`. Only
the portable reference implementations RustDesk's sodiumoxide calls reach:

| | |
|---|---|
| Ed25519 (`crypto_sign_ed25519`, combined mode) | `signed_id` |
| X25519 + XSalsa20-Poly1305 (`crypto_box_easy`) | the session key a peer sends |
| XSalsa20-Poly1305 (`crypto_secretbox_easy`) | every message after the handshake |
| SHA-512, Salsa20, HSalsa20, Poly1305 (donna32) | underneath those |

The files are unchanged, at their original paths. `sodium_glue.c` is ours: it
supplies what the rest of libsodium would (`randombytes_buf` from
`src/core/rng.c`, `sodium_memzero`, `sodium_memcmp`, `sodium_misuse`). Built
with `CONFIGURED=1` and neither `HAVE_TI_MODE` nor `NATIVE_LITTLE_ENDIAN`, so
the field arithmetic is the 32-bit (fe_25_5) kind and byte order is explicit:
right on 68k and PowerPC alike.

`host/test_crypto.py` checks it byte for byte against PyNaCl.
