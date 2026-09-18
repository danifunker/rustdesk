#!/bin/sh
# Provision the NON-LICENSED half of the IRIX cross toolchain, from source, in
# one directory. This is what lets a hosted CI runner build the agent.
#
# The cross build needs four things. Only the first is licensed, and it comes
# from the boot image (scripts/make-sysroot.sh). This script makes the rest:
#
#   sysroot    SGI's headers and libraries           make-sysroot.sh, per job
#   cross/     clang-18 (the distribution's), and    symlinks; LLD is the
#              mogrix's patched LLD 18                binary mogrix commits
#   sgug/      the staging tree: mogrix's irix-cc    copied from mogrix, and
#              and irix-ld, its crt and runtime      compiled here against the
#              objects, and five static C libraries  sysroot
#   rust/      nightly with IRIX-patched std, a      rustup, then mogrix's two
#              patched crate registry, and the       patchers
#              compat archive
#
# It is what /opt/cross, /opt/sgug-staging and ports/rust/{rustup,cargo} are on
# the machine this port was developed on, reproduced from pinned sources. Every
# input is pinned below, so `--key` names exactly one result and actions/cache
# can keep it. The local tree was never reproducible like this -- RESUME.md
# called the staging libraries "not reproducible from any repo" -- and checking
# this script against it is how the recipes below were confirmed; RESUME.md
# records how close the two came.
#
# WHAT IS SAFE TO CACHE. Nothing this writes is SGI's. The static libraries and
# runtime objects are compiled AGAINST the sysroot's headers, the same way the
# agent itself is -- and the agent is the thing this project publishes -- but
# no header, library or crt object from the sysroot is copied in. (The fixed
# crt1.o/crtn.o ARE copies, which is why they live in the sysroot, made per
# job, and not here.) mogrix's wrappers and compat headers are public in its
# repository. So this directory may go in actions/cache on a public fork; the
# sysroot and the image may not.
#
# Usage:
#   scripts/toolchain.sh [--dir DIR] [--sysroot DIR] [--jobs N]   build what is missing
#   scripts/toolchain.sh --key [--image-key K]    the cache key for this set of pins
#   scripts/toolchain.sh --env [--dir DIR]        shell assignments for a build
#
# DIR defaults to build/toolchain. The sysroot defaults to $IRIX_SYSROOT, then
# build/irix-sysroot. Each phase leaves a stamp and is skipped when its stamp
# is present, so an interrupted run resumes and a cache hit costs nothing.
#
# Needs on the host: clang-18 and llvm-18 (apt), rustup, git, curl, make,
# python3 with PyYAML, patch. Never touches ~/.rustup or ~/.cargo: every Rust
# command runs with RUSTUP_HOME and CARGO_HOME inside DIR, and that is checked
# before either patcher runs, because both rewrite what they are pointed at.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

# ---- pins ----------------------------------------------------------------------
# mogrix: danifunker-ports, the branch this port's mogrix changes are pushed to.
MOGRIX_URL="https://github.com/danifunker/mogrix.git"
MOGRIX_REV="1164e6c6abd7f09425b8afab1d8e540173c82601"
# The nightly every build of this agent has used: rustc 1.99.0-nightly
# (1ed2df61a 2026-08-04). rustup names nightlies by PUBLICATION date, a day
# after the commit date rustc prints.
RUST_TOOLCHAIN="nightly-2026-08-05"
RUST_COMMIT="1ed2df61a"
LLVM_BIN="${LLVM_BIN:-/usr/lib/llvm-18/bin}"

SODIUM_URL="https://github.com/jedisct1/libsodium/releases/download/1.0.18-RELEASE/libsodium-1.0.18.tar.gz"
SODIUM_SHA="6f504490b342a4f8a4c4a02fc9b866cbef8622d5df4e5452b46be121e46636c1"
VPX_URL="https://github.com/webmproject/libvpx/archive/refs/tags/v1.13.1.tar.gz"
VPX_SHA="00dae80465567272abd077f59355f95ac91d7809a2d3006f9ace2637dd429d14"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2"
MBEDTLS_SHA="8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca"
ZSTD_URL="https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz"
ZSTD_SHA="8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1"
ZLIB_URL="https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz"
ZLIB_SHA="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"

TC=""
SYSROOT=""
JOBS=""
MODE="build"
IMAGE_KEY=""

die() { echo "toolchain: $*" >&2; exit 1; }
say() { echo ">>> $*" >&2; }

while [ $# -gt 0 ]; do
	case "$1" in
		--dir)       TC="$2"; shift 2 ;;
		--sysroot)   SYSROOT="$2"; shift 2 ;;
		--jobs)      JOBS="$2"; shift 2 ;;
		--key)       MODE="key"; shift ;;
		--image-key) IMAGE_KEY="$2"; shift 2 ;;
		--env)       MODE="env"; shift ;;
		-h|--help)   sed -n '2,/^set -eu$/{/^set -eu$/!p;}' "$0"; exit 0 ;;
		*)           die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$TC" ] || TC="${IRIX_TOOLCHAIN:-$REPO/build/toolchain}"
[ -n "$JOBS" ] || JOBS=$(nproc 2>/dev/null || echo 2)

# ---- --key -----------------------------------------------------------------------
# Everything that decides what ends up in DIR: this file (which holds every pin
# and every recipe), the files it builds from in this repository, and the image
# the staging libraries were compiled against. Not the agent's own sources --
# those are built after, from the cache, every time.
if [ "$MODE" = key ]; then
	{
		cat "$0"
		cat "$REPO/ports/toolchain/"* "$REPO/patches/"*.patch
		cat "$REPO/ports/rust/agent-portable/Cargo.lock" "$REPO/ports/rust/build-compat.sh"
		printf '%s\n' "$IMAGE_KEY"
	} | { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-16
	exit 0
fi

mkdir -p "$TC"
TC=$(cd "$TC" && pwd)
STAGING="$TC/sgug"
CROSS="$TC/cross/bin"
MOGRIX="$TC/mogrix"

# ---- --env -------------------------------------------------------------------------
# One variable, and ports/rust/env.sh and ci-lib.sh's toolchain_env derive the
# rest from it -- so a workflow step, a developer shell and build.sh all agree.
if [ "$MODE" = env ]; then
	echo "IRIX_TOOLCHAIN=$TC; export IRIX_TOOLCHAIN"
	exit 0
fi

# ---- build -------------------------------------------------------------------------
[ -n "$SYSROOT" ] || SYSROOT="${IRIX_SYSROOT:-$REPO/build/irix-sysroot}"
[ -f "$SYSROOT/usr/include/stdio.h" ] || die "no sysroot at $SYSROOT -- run scripts/make-sysroot.sh"
SYSROOT=$(cd "$SYSROOT" && pwd)

# What irix-cc and irix-ld read. Everything below compiles through them.
export IRIX_SYSROOT="$SYSROOT"
export SGUG_STAGING="$STAGING"
export IRIX_CLANG="$CROSS/clang"
export IRIX_LLD="$CROSS/ld.lld-irix"
CC="$STAGING/bin/irix-cc"
AR="$CROSS/llvm-ar"
RANLIB="$CROSS/llvm-ranlib"

SRC="$TC/src"
# Logs live beside the toolchain, not in it, so they survive the cleanup at the
# end and stay out of the cache. CI uploads them when a phase fails.
LOGS="${TC}-logs"
mkdir -p "$SRC" "$LOGS"
done_() { [ -f "$TC/.done-$1" ]; }
mark()  { : > "$TC/.done-$1"; }

fetch() {	# URL SHA256 -> path of the verified download
	_f="$SRC/dl/$(basename "$1")"
	mkdir -p "$SRC/dl"
	if [ ! -f "$_f" ]; then
		curl -fsSL --retry 3 "$1" -o "$_f.part" || die "could not download $1"
		mv "$_f.part" "$_f"
	fi
	_got=$(sha256sum "$_f" | cut -d' ' -f1)
	[ "$_got" = "$2" ] || die "checksum mismatch for $1: got $_got"
	printf '%s\n' "$_f"
}

unpack() {	# ARCHIVE -> the one directory it made, under $SRC
	_d=$(mktemp -d "$SRC/x.XXXXXX")
	tar -xf "$1" -C "$_d"
	_top=$(ls "$_d")
	rm -rf "$SRC/$_top"
	mv "$_d/$_top" "$SRC/$_top"
	rmdir "$_d"
	printf '%s\n' "$SRC/$_top"
}

# ---- 1. mogrix, at the pinned commit ----------------------------------------------
if ! done_ mogrix || [ "$(cat "$MOGRIX/.rev" 2>/dev/null)" != "$MOGRIX_REV" ]; then
	say "mogrix $MOGRIX_REV"
	rm -rf "$MOGRIX"
	git init -q "$MOGRIX"
	git -C "$MOGRIX" fetch -q --depth 1 "$MOGRIX_URL" "$MOGRIX_REV"
	git -C "$MOGRIX" -c advice.detachedHead=false checkout -q FETCH_HEAD
	rm -rf "$MOGRIX/.git"	# 150 MB of history nobody here reads
	printf '%s\n' "$MOGRIX_REV" > "$MOGRIX/.rev"
	mark mogrix
fi

# ---- 2. cross/bin: clang from the distribution, LLD from mogrix -------------------
if ! done_ cross; then
	say "cross/bin: $LLVM_BIN + mogrix's ld.lld-irix-18"
	[ -x "$LLVM_BIN/clang" ] || die "no clang in $LLVM_BIN -- apt-get install clang-18 llvm-18"
	mkdir -p "$CROSS"
	for _t in clang clang++ llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-ranlib llvm-strip; do
		[ -x "$LLVM_BIN/$_t" ] || die "no $_t in $LLVM_BIN -- apt-get install llvm-18"
		ln -sfn "$LLVM_BIN/$_t" "$CROSS/$_t"
	done
	# Relative, so the directory can be restored from a cache anywhere.
	ln -sfn ../../mogrix/tools/bin/ld.lld-irix-18 "$CROSS/ld.lld-irix"
	"$CROSS/ld.lld-irix" --version > /dev/null ||
		die "mogrix's ld.lld-irix-18 does not run here (it needs libxml2 and ICU)"
	mark cross
fi

# ---- 3. sgug: what `mogrix setup-cross` deploys, and the runtime objects ----------
# setup-cross only copies files out of mogrix's cross/ tree; doing that here
# avoids needing mogrix's Python environment for a handful of cp commands. The
# objects are compiled with EXACTLY the commands in mogrix's
# scripts/build-runtime-objects.sh, and each one came out byte-identical to the
# copy in the development machine's /opt/sgug-staging when checked. Only the
# ones an executable links are built; shared-library support is not needed.
if ! done_ staging; then
	say "staging: wrappers, headers and runtime objects"
	mkdir -p "$STAGING/bin" "$STAGING/include" "$STAGING/lib32"
	for _b in irix-cc irix-ld strip-verneed fix-anon-relocs strip-eh-relocs; do
		cp "$MOGRIX/cross/bin/$_b" "$STAGING/bin/$_b"
		chmod 755 "$STAGING/bin/$_b"
	done
	rm -rf "$STAGING/include/dicl-clang-compat" "$STAGING/include/mogrix-compat"
	cp -R "$MOGRIX/cross/include/dicl-clang-compat" "$STAGING/include/"
	cp -R "$MOGRIX/compat/include/mogrix-compat" "$STAGING/include/"
	# libgcc_s: GCC 9.5's runtime, cross-compiled by mogrix and committed there.
	# The agent needs it for _Unwind_*, and the package ships this very file.
	cp "$MOGRIX/cross/lib32/libgcc_s.so.1" "$STAGING/lib32/"
	ln -sfn libgcc_s.so.1 "$STAGING/lib32/libgcc_s.so"

	L="$STAGING/lib32"
	"$CC" -c "$MOGRIX/cross/crt/crtbeginT.S" -o "$L/crtbeginT.o"
	"$CC" -c "$MOGRIX/cross/crt/crtendT.S"   -o "$L/crtendT.o"
	"$CC" -c "$MOGRIX/cross/lib/dso_handle.c" -o "$L/dso_handle.o"
	# Compiled, not the eh_frame_reg.o mogrix commits: that prebuilt copy is
	# stale, and the one every build here has linked is this compile of the .c.
	"$CC" -c "$MOGRIX/cross/crt/eh_frame_reg.c" -o "$L/eh_frame_reg.o"
	# Raw clang: safe_mem.c is freestanding and conflicts with the headers
	# irix-cc force-includes.
	"$CROSS/clang" --target=mips-sgi-irix6.5 -mabi=n32 -march=mips3 -O2 \
		-fno-builtin -w -c "$MOGRIX/cross/lib/safe_mem.c" -o "$L/safe_mem.o"
	"$CC" -c -O2 -DHAVE_MORECORE=0 -DHAVE_MMAP=1 -DUSE_LOCKS=1 -DUSE_SPIN_LOCKS=1 \
		-DMMAP_CLEARS=1 -Dmalloc_getpagesize=16384 \
		"$MOGRIX/compat/malloc/dlmalloc.c" -o "$L/dlmalloc.o"
	# mogrix's copy is .gitignore'd, so a clone lacks it; see the file's header.
	"$CC" -c "$REPO/ports/toolchain/soft_float_stubs.c" -o "$SRC/soft_float_stubs.o"
	rm -f "$L/libsoft_float_stubs.a"
	"$AR" rcs "$L/libsoft_float_stubs.a" "$SRC/soft_float_stubs.o"
	mark staging
fi

# ---- 4. the five static libraries ---------------------------------------------------
# Recipes from RESUME.md's "Prerequisites" (recovered from the original build
# trees' config.status) and mogrix's rules/packages/*.yaml, which also hold the
# reasons for each flag. All static: nothing here should cost the agent an rld
# dependency, and a stock IRIX lacks all five (or, for zlib, has too old a one).
if ! done_ zlib; then
	say "zlib 1.3.2"
	d=$(unpack "$(fetch "$ZLIB_URL" "$ZLIB_SHA")")
	( cd "$d" && CC="$CC" ./configure --static --prefix="$STAGING" > "$LOGS/zlib.log" 2>&1 &&
	  make -j"$JOBS" libz.a >> "$LOGS/zlib.log" 2>&1 ) || { tail -30 "$LOGS/zlib.log" >&2; die "zlib failed"; }
	cp "$d/libz.a" "$STAGING/lib32/libz.a"
	mark zlib
fi

if ! done_ zstd; then
	say "zstd 1.5.6"
	d=$(unpack "$(fetch "$ZSTD_URL" "$ZSTD_SHA")")
	# -ffile-prefix-map: divsufsort.c's assert() records its own path, so
	# without it the archive differs by where this directory is. (The agent
	# does not link that object; the cache should not depend on it either.)
	( cd "$d/lib" && make -j"$JOBS" libzstd.a CC="$CC" AR=ar RANLIB=ranlib \
		CFLAGS="-O2 -DZSTD_MULTITHREAD=0 -ffile-prefix-map=$d=/zstd-1.5.6" > "$LOGS/zstd.log" 2>&1 ) ||
		{ tail -30 "$LOGS/zstd.log" >&2; die "zstd failed"; }
	cp "$d/lib/libzstd.a" "$STAGING/lib32/"
	cp "$d/lib/zstd.h" "$d/lib/zstd_errors.h" "$STAGING/include/"
	mark zstd
fi

if ! done_ sodium; then
	say "libsodium 1.0.18"
	d=$(unpack "$(fetch "$SODIUM_URL" "$SODIUM_SHA")")
	( cd "$d" && ./configure --host=mips-sgi-irix6.5 --prefix="$SRC/sodium-inst" \
		--disable-shared --enable-static --disable-pie --disable-ssp \
		CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS=-O2 > "$LOGS/sodium.log" 2>&1 &&
	  make -j"$JOBS" >> "$LOGS/sodium.log" 2>&1 && make install >> "$LOGS/sodium.log" 2>&1 ) ||
		{ tail -30 "$LOGS/sodium.log" >&2; die "libsodium failed"; }
	cp "$SRC/sodium-inst/lib/libsodium.a" "$STAGING/lib32/"
	rm -rf "$STAGING/include/sodium"
	cp -R "$SRC/sodium-inst/include/sodium" "$SRC/sodium-inst/include/sodium.h" "$STAGING/include/"
	mark sodium
fi

if ! done_ vpx; then
	say "libvpx 1.13.1, VP8 only, with two patches"
	d=$(unpack "$(fetch "$VPX_URL" "$VPX_SHA")")
	# A name collision with an IRIX header (mogrix), and the active-map early
	# exit that halves the cost of an unchanged frame (patches/, and why).
	( cd "$d" && patch -p1 -s < "$MOGRIX/patches/packages/libvpx/libvpx-irix-sync-name-collision.patch" &&
	  patch -p1 -s < "$REPO/patches/libvpx-vp8-active-map-early-out.patch" ) || die "a libvpx patch did not apply"
	# LD as well as CC: configure's link test otherwise uses the host's gcc and
	# fails on "relocations in generic ELF (EM: 8)".
	# --prefix is only RECORDED -- nothing here runs `make install` -- but
	# libvpx bakes the configure line into vpx_codec_build_config(), so it is
	# the development machine's value, which lets the two archives compare equal.
	( cd "$d" && CC="$CC" LD="$CC" AR="$AR" ./configure --target=generic-gnu --prefix=/opt/sgug-staging/usr/sgug \
		--disable-shared --enable-static \
		--enable-vp8-encoder --enable-vp8-decoder \
		--disable-vp9-encoder --disable-vp9-decoder \
		--disable-examples --disable-tools --disable-docs --disable-unit-tests \
		--disable-runtime-cpu-detect --disable-webm-io --disable-libyuv > "$LOGS/vpx.log" 2>&1 &&
	  make -j"$JOBS" libvpx.a >> "$LOGS/vpx.log" 2>&1 ) || { tail -30 "$LOGS/vpx.log" >&2; die "libvpx failed"; }
	cp "$d/libvpx.a" "$STAGING/lib32/"
	rm -rf "$STAGING/include/vpx"
	mkdir -p "$STAGING/include/vpx"
	for _h in vp8.h vp8cx.h vp8dx.h vpx_codec.h vpx_decoder.h vpx_encoder.h \
	          vpx_ext_ratectrl.h vpx_frame_buffer.h vpx_image.h vpx_integer.h; do
		cp "$d/vpx/$_h" "$STAGING/include/vpx/"
	done
	mark vpx
fi

if ! done_ mbedtls; then
	say "mbedTLS 3.6.2, with two patches"
	d=$(unpack "$(fetch "$MBEDTLS_URL" "$MBEDTLS_SHA")")
	for _p in mbedtls-irix-no-xopen-hides-getaddrinfo.patch mbedtls-irix-no-udbl-division.patch; do
		( cd "$d" && patch -p1 -s < "$MOGRIX/patches/packages/mbedtls/$_p" ) || die "$_p did not apply"
	done
	# NOT `make install`: it builds programs/, whose sub-make loses CC and links
	# with the host linker. The library, then its headers, by hand.
	( cd "$d" && make -j"$JOBS" lib CC="$CC" CFLAGS=-O2 > "$LOGS/mbedtls.log" 2>&1 ) ||
		{ tail -30 "$LOGS/mbedtls.log" >&2; die "mbedTLS failed"; }
	cp "$d/library/libmbedtls.a" "$d/library/libmbedx509.a" "$d/library/libmbedcrypto.a" "$STAGING/lib32/"
	rm -rf "$STAGING/include/mbedtls" "$STAGING/include/psa"
	cp -R "$d/include/mbedtls" "$d/include/psa" "$STAGING/include/"
	mark mbedtls
fi

# ---- 5. Rust ---------------------------------------------------------------------------
RH="$TC/rust"
export RUSTUP_HOME="$RH/rustup"
export CARGO_HOME="$RH/cargo"
# The whole point of the two variables above. Both patchers below rewrite, in
# place, whatever toolchain and registry they resolve to; aimed at ~/.rustup or
# ~/.cargo they would corrupt every other Rust effort on the machine.
case "$RUSTUP_HOME" in "$TC"/*) ;; *) die "RUSTUP_HOME escaped $TC: $RUSTUP_HOME" ;; esac
case "$CARGO_HOME"  in "$TC"/*) ;; *) die "CARGO_HOME escaped $TC: $CARGO_HOME" ;; esac
command -v rustup > /dev/null 2>&1 || die "no rustup on PATH"
mkdir -p "$RUSTUP_HOME" "$CARGO_HOME"

if ! done_ rustup; then
	say "Rust $RUST_TOOLCHAIN, as the toolchain named 'nightly'"
	rustup toolchain install "$RUST_TOOLCHAIN" --profile minimal --component rust-src \
		--no-self-update > "$LOGS/rustup.log" 2>&1 || { tail -20 "$LOGS/rustup.log" >&2; die "rustup failed"; }
	# Everything here says `cargo +nightly`, and the development tree's private
	# toolchain is a copy of a channel install named exactly that. Renaming the
	# dated install keeps the two layouts the same, so panic-location paths --
	# remapped, but still -- match between a local build and this one.
	_host=$(rustup show 2>/dev/null | sed -n 's/^Default host: //p')
	[ -n "$_host" ] || _host=x86_64-unknown-linux-gnu
	rm -rf "$RUSTUP_HOME/toolchains/nightly-$_host"
	mv "$RUSTUP_HOME/toolchains/$RUST_TOOLCHAIN-$_host" "$RUSTUP_HOME/toolchains/nightly-$_host"
	if [ -f "$RUSTUP_HOME/update-hashes/$RUST_TOOLCHAIN-$_host" ]; then
		mv "$RUSTUP_HOME/update-hashes/$RUST_TOOLCHAIN-$_host" "$RUSTUP_HOME/update-hashes/nightly-$_host"
	fi
	rustc +nightly -V | grep -q "$RUST_COMMIT" ||
		die "the toolchain is not rustc $RUST_COMMIT: $(rustc +nightly -V)"
	mark rustup
fi

if ! done_ rust-std; then
	say "patch-rust-sysroot.sh (mogrix): IRIX support in std"
	bash "$MOGRIX/scripts/patch-rust-sysroot.sh" > "$LOGS/std.log" 2>&1 ||
		{ tail -30 "$LOGS/std.log" >&2; die "patch-rust-sysroot.sh failed"; }
	# A pattern that no longer matches is reported and skipped rather than
	# fatal, which is how a std layout change would slip through -- so ask the
	# script's own check mode whether everything it wanted is in place.
	bash "$MOGRIX/scripts/patch-rust-sysroot.sh" --check > "$LOGS/std-check.log" 2>&1 ||
		{ tail -30 "$LOGS/std-check.log" >&2; die "std is not fully patched (log above)"; }
	mark rust-std
fi

if ! done_ crates; then
	say "cargo fetch, then mogrix's crate patcher"
	# The registry is filled from the agent's Cargo.lock AND std's own (via
	# -Zbuild-std in the crate's .cargo/config.toml): libc, for one, is patched
	# in both roles.
	( cd "$REPO/ports/rust/agent-portable" && cargo +nightly fetch ) > "$LOGS/fetch.log" 2>&1 ||
		{ tail -20 "$LOGS/fetch.log" >&2; die "cargo fetch failed"; }
	_reg=$(ls -d "$CARGO_HOME"/registry/src/index.crates.io-*/ 2>/dev/null | head -1)
	[ -n "$_reg" ] || die "cargo fetch left no registry sources in $CARGO_HOME"
	# Called directly rather than through `mogrix patch-crates`: the CLI pulls
	# in mogrix's whole dependency set, the patcher needs only PyYAML.
	PYTHONPATH="$MOGRIX" python3 - "$_reg" "$MOGRIX/rules" > "$LOGS/crates.log" 2>&1 <<'PY' ||
import sys
from mogrix.crate_patcher import patch_all_crates
stats = patch_all_crates(registry_dir=sys.argv[1], rules_dir=sys.argv[2])
print({k: v for k, v in stats.items() if k != "errors"})
errs = stats.get("errors", [])
for e in errs:
    print("PATTERN DID NOT MATCH:", e)
sys.exit(1 if ("error" in stats or errs) else 0)
PY
		{ tail -30 "$LOGS/crates.log" >&2; die "the crate patcher failed (log above)"; }
	mark crates
fi

if ! done_ compat; then
	say "librust_irix_compat.a"
	RD_COMPAT_OUT="$RH/compat" MOGRIX_ROOT="$MOGRIX" IRIX_CC="$CC" \
		sh "$REPO/ports/rust/build-compat.sh" > "$LOGS/compat.log" 2>&1 ||
		{ tail -20 "$LOGS/compat.log" >&2; die "build-compat.sh failed"; }
	mark compat
fi

# The sources and build trees are only inputs. Leaving them would triple what
# actions/cache has to carry for nothing.
rm -rf "$SRC"
say "toolchain ready: $TC"
