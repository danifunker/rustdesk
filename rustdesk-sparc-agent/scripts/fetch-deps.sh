#!/usr/bin/env bash
#
# fetch-deps.sh -- download the C library sources build-deps.sh expects, and
# verify them, on a machine whose certificate store is from this decade.
#
# This runs on the **build host**, not on the Blade. Solaris 10's curl carries
# a root store from 2011 and every one of these hosts has rotated since, so the
# machine that can verify a download is not the machine that needs the source.
# The split is deliberate; this is the half that was missing.
#
#   ./scripts/fetch-deps.sh                 # into ./ports/distfiles
#   ./scripts/fetch-deps.sh --push HOST     # ...and scp them to HOST:/tmp
#
# The checksums are of the exact tarballs that built ~/sparc-deps on the Blade,
# read back off the machine rather than copied from a release page. Verified
# equal to what these URLs serve on 2026-08-25.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${DEST:-$HERE/ports/distfiles}"

# file <TAB> sha256 <TAB> url
DEPS="
libsodium-1.0.18.tar.gz	6f504490b342a4f8a4c4a02fc9b866cbef8622d5df4e5452b46be121e46636c1	https://github.com/jedisct1/libsodium/releases/download/1.0.18-RELEASE/libsodium-1.0.18.tar.gz
zstd-1.5.6.tar.gz	8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1	https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
mbedtls-3.6.2.tar.bz2	8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca	https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
libvpx-1.13.1.tar.gz	00dae80465567272abd077f59355f95ac91d7809a2d3006f9ace2637dd429d14	https://github.com/webmproject/libvpx/archive/refs/tags/v1.13.1.tar.gz
"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

mkdir -p "$DEST"
fail=0
while IFS=$'\t' read -r file want url; do
    [ -n "${file:-}" ] || continue
    out="$DEST/$file"
    if [ -f "$out" ] && [ "$(sha256_of "$out")" = "$want" ]; then
        echo "have  $file"
        continue
    fi
    echo "fetch $file"
    curl -fsSL --retry 3 -o "$out.part" "$url"
    got="$(sha256_of "$out.part")"
    if [ "$got" != "$want" ]; then
        echo "  CHECKSUM MISMATCH: got $got, wanted $want" >&2
        echo "  left at $out.part; the release was re-rolled or the download was tampered with" >&2
        fail=1
        continue
    fi
    mv "$out.part" "$out"
    echo "  ok $got"
done <<< "$DEPS"

[ "$fail" -eq 0 ] || exit 1
echo
echo "in $DEST:"
ls -la "$DEST"

if [ "${1:-}" = "--push" ]; then
    host="${2:?--push needs a host, e.g. dani@192.168.99.176}"
    echo
    echo "copying to $host:/tmp"
    scp "$DEST"/*.tar.gz "$DEST"/*.tar.bz2 "$host:/tmp/"
    scp "$HERE/scripts/build-deps.sh" "$host:/tmp/"
    echo "now: ssh $host 'sh /tmp/build-deps.sh all'"
fi
