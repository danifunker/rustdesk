#!/bin/sh
# IRIX 5.3's headers, which the cross build compiles against, out of a disk
# image of an IRIX 5.3 system with the development option installed (the
# image is a MAME CHD or a raw disk; rb-cli reads either).
#
#   IRIX53_IMAGE=/path/to/irix53.chd scripts/make-sysroot.sh [DIR]
#
# THE RESULT IS SGI'S LICENSED MATERIAL: it stays under build/ (ignored by
# git) and is never committed or published. Only /usr/include is needed; the
# libraries are SGI's own, on the machine that links.
set -eu
DIR=${1:-$(cd "$(dirname "$0")/.." && pwd)/build/irix53-sysroot}
IMAGE=${IRIX53_IMAGE:?set IRIX53_IMAGE to an IRIX 5.3 disk image}
RB=${RB_CLI:-rb-cli}
rm -rf "$DIR"
mkdir -p "$DIR/usr"
"$RB" tar "$IMAGE" /usr/include "$DIR/include.tar"
tar -xf "$DIR/include.tar" -C "$DIR/usr"
rm -f "$DIR/include.tar"
test -f "$DIR/usr/include/X11/Xlib.h" || { echo "no X11 headers in the image" >&2; exit 1; }
test -f "$DIR/usr/include/X11/extensions/xtraplib.h" || echo "warning: no XTrap headers" >&2
echo "sysroot in $DIR"
