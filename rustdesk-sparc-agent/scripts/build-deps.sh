#!/bin/sh
#
# build-deps.sh -- the C libraries the agent links, built for sparcv9 on the
# machine that will run it.
#
# Sources are not downloaded here. Solaris 10's curl carries a certificate
# store from 2011 and half the download hosts have moved on since; fetching on
# the build host and copying the tarballs across means the thing that verifies
# the download is a machine that can.
#
#   scp libsodium-1.0.18.tar.gz zstd-1.5.6.tar.gz mbedtls-3.6.2.tar.bz2 blade:/tmp/
#   ssh blade 'sh /tmp/build-deps.sh all'
#
# Everything is built static, 64-bit and -fPIC, and installed under $PREFIX.
# Static because the agent should not need anything installed on the machine it
# runs on; -fPIC because mrustc compiles its own objects that way and a
# non-PIC archive will not link with them.
#
# Written for Solaris 10's /bin/sh: backticks, no `local`, no [[ ]].

set -e

PREFIX="${PREFIX:-$HOME/sparc-deps}"
SRCDIR="${SRCDIR:-/tmp}"
BUILDDIR="${BUILDDIR:-/tmp/sparc-deps-build}"
CC="${CC:-/opt/csw/bin/gcc-5.5}"     # 4.9 has no __builtin_add_overflow
CFLAGS="${CFLAGS:--m64 -O2 -fPIC}"
PATH=/opt/csw/bin:/usr/ccs/bin:/usr/bin:/usr/sbin
export PATH CC CFLAGS

SODIUM_VER="${SODIUM_VER:-1.0.18}"
ZSTD_VER="${ZSTD_VER:-1.5.6}"
MBEDTLS_VER="${MBEDTLS_VER:-3.6.2}"
VPX_VER="${VPX_VER:-1.13.1}"

log() { echo "build-deps: $*"; }

unpack() {
    # $1 tarball, $2 expected directory
    if [ ! -f "$1" ]; then
        log "missing $1 -- copy it to $SRCDIR first"
        exit 1
    fi
    mkdir -p "$BUILDDIR"
    rm -rf "$BUILDDIR/$2"
    case "$1" in
    *.bz2) gtar xjf "$1" -C "$BUILDDIR" ;;
    *)     gtar xzf "$1" -C "$BUILDDIR" ;;
    esac
}

build_sodium() {
    if [ -f "$PREFIX/lib/libsodium.a" ]; then
        log "libsodium already built"
        return 0
    fi
    log "libsodium $SODIUM_VER"
    unpack "$SRCDIR/libsodium-$SODIUM_VER.tar.gz" "libsodium-$SODIUM_VER"
    cd "$BUILDDIR/libsodium-$SODIUM_VER"
    ./configure --prefix="$PREFIX" --disable-shared --enable-static
    gmake
    gmake install
}

build_zstd() {
    if [ -f "$PREFIX/lib/libzstd.a" ]; then
        log "libzstd already built"
        return 0
    fi
    log "zstd $ZSTD_VER"
    unpack "$SRCDIR/zstd-$ZSTD_VER.tar.gz" "zstd-$ZSTD_VER"
    cd "$BUILDDIR/zstd-$ZSTD_VER"
    # Only the library: the command-line tool needs a C++ compiler for its
    # tests and the agent does not use it. ZSTD_NO_ASM because the build
    # otherwise hands huf_decompress_amd64.S to a SPARC assembler, which says
    # "statement syntax" and stops.
    gmake -C lib libzstd.a ZSTD_NO_ASM=1
    mkdir -p "$PREFIX/lib" "$PREFIX/include"
    cp lib/libzstd.a "$PREFIX/lib/"
    cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$PREFIX/include/"
}

build_mbedtls() {
    if [ -f "$PREFIX/lib/libmbedtls.a" ]; then
        log "mbedTLS already built"
        return 0
    fi
    log "mbedTLS $MBEDTLS_VER"
    unpack "$SRCDIR/mbedtls-$MBEDTLS_VER.tar.bz2" "mbedtls-$MBEDTLS_VER"
    cd "$BUILDDIR/mbedtls-$MBEDTLS_VER"
    # `make lib` rather than the CMake build: there is no cmake here, and the
    # release tarball ships the generated sources so nothing needs Python.
    gmake -C library CC="$CC" CFLAGS="$CFLAGS -I../include" SHARED=
    mkdir -p "$PREFIX/lib" "$PREFIX/include"
    cp library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a "$PREFIX/lib/"
    cp -r include/mbedtls include/psa "$PREFIX/include/"
}

build_vpx() {
    if [ -f "$PREFIX/lib/libvpx.a" ]; then
        log "libvpx already built"
        return 0
    fi
    log "libvpx $VPX_VER (this one takes a while)"
    unpack "$SRCDIR/libvpx-$VPX_VER.tar.gz" "libvpx-$VPX_VER"
    cd "$BUILDDIR/libvpx-$VPX_VER"
    # generic-gnu is the portable C build: there is no SPARC assembly in libvpx
    # and asking for a target it does not know ends the configure immediately.
    # VP8 only, and none of the tooling -- the agent encodes, and the examples
    # and unit tests drag in a C++ compiler and webm.
    CC="$CC" CFLAGS="$CFLAGS" ./configure \
        --prefix="$PREFIX" --target=generic-gnu \
        --enable-vp8 --disable-vp9 --enable-vp8-encoder --enable-vp8-decoder \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --disable-webm-io --disable-libyuv --enable-static --disable-shared \
        --enable-pic
    gmake
    gmake install
}

case "${1:-all}" in
sodium)  build_sodium ;;
vpx)     build_vpx ;;
zstd)    build_zstd ;;
mbedtls) build_mbedtls ;;
all)     build_sodium; build_zstd; build_mbedtls; build_vpx ;;
*) echo "usage: $0 sodium|zstd|mbedtls|vpx|all" >&2; exit 2 ;;
esac

log "installed in $PREFIX:"
ls -la "$PREFIX/lib" 2>/dev/null | grep '\.a$' || true
