#!/usr/bin/env python3
"""Check the vendored libsodium subset against PyNaCl, byte for byte."""
import subprocess
import sys
from nacl.bindings import (crypto_box, crypto_scalarmult_base, crypto_secretbox,
                           crypto_sign, crypto_sign_seed_keypair)

exe = sys.argv[1]
seed = bytes((i * 7 + 1) & 255 for i in range(32))
bsk = bytes((i * 13 + 5) & 255 for i in range(32))
key = bytes((i * 3 + 9) & 255 for i in range(32))
nonce = bytes(range(24))
msg = b"C-Desk-Vint signs this"

pk, sk = crypto_sign_seed_keypair(seed)
bpk = crypto_scalarmult_base(bsk)
# A peer (sodiumoxide on the client) boxes a secretbox key to our box key.
peer_sk = bytes((i * 29 + 3) & 255 for i in range(32))
peer_pk = crypto_scalarmult_base(peer_sk)
sym = bytes(range(100, 132))
boxed = crypto_box(sym, bytes(24), bpk, peer_sk)

out = subprocess.run([exe, peer_pk.hex(), boxed.hex()], capture_output=True, text=True).stdout
got = dict(line.split(' ', 1) for line in out.strip().splitlines())
want = {
    'sign_pk': pk.hex(),
    'signed': crypto_sign(msg, sk).hex(),
    'box_pk': bpk.hex(),
    'secretbox': crypto_secretbox(msg, nonce, key).hex(),
    'opened': sym.hex(),
}
fails = [k for k in want if got.get(k) != want[k]]
for k in fails:
    print('FAIL', k, '\n  got ', got.get(k), '\n  want', want[k])
print('FAIL' if fails else 'PASS: crypto matches libsodium (sign, box, secretbox)')
sys.exit(1 if fails else 0)
