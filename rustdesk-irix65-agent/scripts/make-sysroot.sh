#!/bin/sh
# Build the n32 cross-compile sysroot FROM THE IRIX DISK IMAGE, printing its
# directory on stdout.
#
# This is what lets a hosted runner build. Everything else the cross build needs
# -- clang-18, mogrix's patched LLD, the static C libraries, the Rust nightly --
# is open source and can be downloaded or cached. The sysroot cannot: it is
# SGI's headers and libraries, copied off an IRIX install. The runner downloads
# the image anyway, to boot the guest that runs gendist, so the sysroot comes
# out of the same privately-hosted file and nothing else licensed is needed.
#
# THE RESULT IS LICENSED MATERIAL. It lives under build/ for the life of one
# job and must never be cached, uploaded as an artifact, or committed -- this
# fork is public, so actions/cache is readable by anyone who opens a pull
# request, and artifacts by anyone at all. It takes about fifteen seconds to
# make, which is why there is no reason to keep it.
#
# How, and what it was checked against (2026-09-18):
#
#   ../irixscsitb/scripts/make-irix-sysroot.sh --direct --abi n32 was run
#   against ~/Indy-IRIX65_dev.chd and diffed with the /opt/irix-sysroot this
#   project had always built against. Every file it extracted was byte-identical
#   and every symlink pointed the same way. It was short by exactly two things,
#   both of which this script adds:
#
#   1. THE MOTIF HEADERS. On IRIX /usr/include/Xm and /usr/include/Sgm are
#      relative symlinks into /usr/Motif-1.2/include, so extracting
#      /usr/include alone leaves them dangling and the panel does not build.
#      /usr/Motif-1.2 is extracted beside it, where the links already point.
#
#   2. usr/lib32/mips3/fixed/{crt1,crtn}.o, which mogrix's irix-ld links every
#      executable with. They are SGI's own crt objects with the .MIPS.events*
#      sections stripped, because LLD cannot process those. llvm-objcopy with
#      the three -R flags below reproduces the ones mogrix made BYTE FOR BYTE --
#      for crtn.o too, which mogrix's setup guide says is a plain copy and is not.
#
#   It uses `rb-cli tar` rather than irixscsitb's `rb-cli get`, because tar
#   keeps IRIX symlinks as symlinks (libc.so -> libc.so.1) instead of writing
#   them as text files to be recreated afterwards, and preserves names that
#   differ only in case. Same extraction, less repair.
#
# The build was then run against it and against /opt/irix-sysroot; see
# RESUME.md for how the two builds compare.
#
# Usage:
#   scripts/make-sysroot.sh [--image CHD] [--out DIR] [--rb-cli PATH]
#
# --image defaults to whatever scripts/fetch-image.sh resolves. --out defaults
# to build/irix-sysroot and is replaced if it exists.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

IMAGE=""
OUT=""
RB=""

die() { echo "make-sysroot: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--image)  IMAGE="$2"; shift 2 ;;
		--out)    OUT="$2"; shift 2 ;;
		--rb-cli) RB="$2"; shift 2 ;;
		-h|--help) sed -n '2,/^set -eu$/{/^set -eu$/!p;}' "$0"; exit 0 ;;
		*)        die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$IMAGE" ] || IMAGE=$(sh "$REPO/scripts/fetch-image.sh")
[ -f "$IMAGE" ] || die "no such image: $IMAGE"
[ -n "$OUT" ] || OUT="$REPO/build/irix-sysroot"
[ -n "$RB" ] || RB=$(sh "$REPO/scripts/ensure-rbcli.sh")

# llvm-objcopy: the one in /opt/cross if mogrix's layout is there, otherwise
# the distribution's llvm-18 (apt's llvm-18 on an Ubuntu runner).
OBJCOPY=""
for _o in "${IRIX_CROSS:-/opt/cross}/bin/llvm-objcopy" /usr/lib/llvm-18/bin/llvm-objcopy \
          llvm-objcopy-18 llvm-objcopy; do
	if [ -x "$_o" ] || command -v "$_o" > /dev/null 2>&1; then OBJCOPY="$_o"; break; fi
done
[ -n "$OBJCOPY" ] || die "no llvm-objcopy (install llvm-18)"

# The image's root is partition 1 in rb-cli's numbering (dvh slot 0, XFS).
SRC="$IMAGE@1"

rm -rf "$OUT"
mkdir -p "$OUT/usr"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/mksysroot.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

# rb-cli tar archives a subtree under its own basename, so /usr/lib32 and /lib32
# both come out as lib32/ -- hence one archive each, unpacked where it belongs.
take() {	# IMAGE-PATH HOST-PARENT
	_n=$(printf %s "$1" | tr / _)
	"$RB" --progress never tar --no-compress --force "$SRC" "$1" "$WORK/$_n.tar" >&2 ||
		die "rb-cli could not archive $1 from $IMAGE"
	tar -xf "$WORK/$_n.tar" -C "$2"
	rm -f "$WORK/$_n.tar"
}
echo ">>> extracting the n32 sysroot from $(basename "$IMAGE")" >&2
take /usr/include   "$OUT/usr"
take /usr/lib32     "$OUT/usr"
take /usr/Motif-1.2 "$OUT/usr"
take /lib32         "$OUT"

echo ">>> stripping .MIPS.events from the crt objects (mogrix's mips3/fixed)" >&2
M3="$OUT/usr/lib32/mips3"
mkdir -p "$M3/fixed"
for _o in crt1 crtn; do
	[ -f "$M3/$_o.o" ] || die "the image has no /usr/lib32/mips3/$_o.o -- not an IRIX 6.5 development install?"
	"$OBJCOPY" -R .MIPS.events.text -R .MIPS.events.init -R .MIPS.events \
		"$M3/$_o.o" "$M3/fixed/$_o.o"
done

# Every one of these is something the build reads, and each is a way a partial
# image or a bad extraction would otherwise fail thirty lines into a compile.
for _f in usr/include/stdio.h usr/include/X11/Xlib.h usr/include/Xm/Xm.h \
          usr/lib32/libc.so usr/lib32/libX11.so.1 usr/lib32/libXext.so \
          usr/lib32/libXm.so usr/lib32/libpthread.so lib32/rld \
          usr/lib32/mips3/fixed/crt1.o usr/lib32/mips3/fixed/crtn.o; do
	[ -e "$OUT/$_f" ] || die "the sysroot has no $_f -- the image lacks the
IRIX development headers/libraries (IDO, or Motif's development subsystem for
Xm/Xm.h), or the extraction was cut short"
done

echo ">>> sysroot: $OUT ($(du -sm "$OUT" | cut -f1) MB) -- licensed: never cache or upload it" >&2
printf '%s\n' "$OUT"
