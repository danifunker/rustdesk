#!/usr/bin/env bash
#
# package-sol9.sh -- build the R-DeskVint SVR4 package for SPARC Solaris.
#
#   scripts/package-sol9.sh                 build, then package
#   scripts/package-sol9.sh --no-build      package what is already built
#   scripts/package-sol9.sh --version 1.0   override the version string
#   scripts/package-sol9.sh --install       ...and pkgadd it on the host too
#
# The build happens here on Linux; the packaging happens on a Solaris machine,
# because pkgmk and pkgtrans only exist there. That split is the same one the
# IRIX port makes with gendist, and for the same reason: each OS packages its
# own build, and neither tool has a usable substitute off its own platform.
#
# SOL9_HOST names the Solaris machine to package on. It needs no toolchain --
# only pkgmk, pkgtrans and somewhere to write -- so any Solaris 9 or 10 box will
# do, including the one you are going to install on.
#
# Output: dist/RDVTagent-<version>-sparc.pkg, a package datastream that installs
# with `pkgadd -d`.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
TARGET=sparcv9-sun-solaris2.9
SOL9_HOST="${SOL9_HOST:-user@192.168.99.176}"
TOOLCHAIN="${TOOLCHAIN:-$HOME/sol9-toolchain}"
OUT="${OUT:-$HERE/target/$TARGET}"
DIST="${DIST:-$HERE/dist}"
PKG=RDVTagent
DO_BUILD=1
DO_INSTALL=0
VERSION=""

die() { echo "package-sol9: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0; shift ;;
        --install)  DO_INSTALL=1; shift ;;
        --version)  VERSION="$2"; shift 2 ;;
        --host)     SOL9_HOST="$2"; shift 2 ;;
        -h|--help)  sed -n '2,20p' "$0"; exit 0 ;;
        *)          die "unknown option: $1" ;;
    esac
done

# Numeric and date-led, so pkgadd's own comparison orders releases correctly.
[ -n "$VERSION" ] || VERSION="0.1.0,REV=$(date +%Y.%m.%d)"
PKGFILE="$PKG-$(echo "$VERSION" | tr ',=' '--')-sparc.pkg"

if [ "$DO_BUILD" = 1 ]; then
    echo "==> Building"
    "$HERE/scripts/build-sol9.sh"
fi

[ -x "$OUT/rdeskvint" ] || die "no agent at $OUT/rdeskvint -- build first"

# libgcc_s.so.1 comes out of whichever toolchain built the binary; the container
# and the native toolchain hold the same one.
LIBGCC="${LIBGCC:-$TOOLCHAIN/opt/$TARGET/lib/sparcv9/libgcc_s.so.1}"
[ -f "$LIBGCC" ] || die "no libgcc_s.so.1 at $LIBGCC (set LIBGCC=)"

echo "==> Staging"
STAGE="$HERE/target/pkgstage"
rm -rf "$STAGE"
mkdir -p "$STAGE/root/bin" "$STAGE/root/lib" "$STAGE/root/doc"

cp "$OUT/rdeskvint"                "$STAGE/root/bin/"
cp "$HERE/pkg/rdeskvint-enable"         "$STAGE/root/bin/"
cp "$HERE/pkg/rdeskvint-disable"        "$STAGE/root/bin/"
cp "$HERE/scripts/sol9-console-test.sh" "$STAGE/root/bin/rdeskvint-console-test"
cp "$LIBGCC"                            "$STAGE/root/lib/"
cp "$HERE/pkg/rdeskvint.init"           "$STAGE/root/lib/"
cp "$HERE/pkg/rdeskvint.xsession"       "$STAGE/root/lib/"
cp "$HERE/pkg/README.txt"               "$STAGE/root/doc/"
cp "$HERE/pkg/prototype" "$HERE/pkg/postinstall" \
   "$HERE/pkg/preremove" "$HERE/pkg/postremove" "$STAGE/"
chmod 755 "$STAGE/root/bin/"* "$STAGE/postinstall" "$STAGE/preremove" "$STAGE/postremove"

# Strip: the binary is 13 MB of which most is debug information that no Solaris
# 9 machine has a debugger for anyway.
"$TOOLCHAIN/opt/bin/$TARGET-strip" "$STAGE/root/bin/rdeskvint" 2>/dev/null || true

sed -e "s|@VERSION@|$VERSION|" \
    -e "s|@PSTAMP@|$(date -u +%Y%m%d%H%M%S)|" \
    "$HERE/pkg/pkginfo.tmpl" > "$STAGE/pkginfo"

echo "==> Packaging on $SOL9_HOST (pkgmk/pkgtrans live only on Solaris)"
REMOTE="/tmp/rdeskvint-pkg.$$"
# shellcheck disable=SC2029  # $REMOTE is meant to expand here
# pkgmk will not create its own -d device directory; it fails with
# "unable to determine or access output filesystem" if it is missing.
ssh "$SOL9_HOST" "rm -rf $REMOTE && mkdir -p $REMOTE/build"
tar cf - -C "$STAGE" . | ssh "$SOL9_HOST" "cd $REMOTE && tar xf -"
ssh "$SOL9_HOST" "cd $REMOTE && \
    pkgmk -o -r $REMOTE/root -d $REMOTE/build -f prototype > pkgmk.log 2>&1 && \
    pkgtrans -s $REMOTE/build $REMOTE/$PKGFILE $PKG > pkgtrans.log 2>&1" \
    || { ssh "$SOL9_HOST" "cat $REMOTE/pkgmk.log $REMOTE/pkgtrans.log 2>/dev/null"; \
         die "packaging failed on $SOL9_HOST"; }

mkdir -p "$DIST"
scp -q "$SOL9_HOST:$REMOTE/$PKGFILE" "$DIST/$PKGFILE"

if [ "$DO_INSTALL" = 1 ]; then
    echo "==> Installing on $SOL9_HOST"
    ssh "$SOL9_HOST" "pkginfo $PKG > /dev/null 2>&1 && sudo pkgrm -n $PKG || true"
    ssh "$SOL9_HOST" "sudo pkgadd -n -d $REMOTE/$PKGFILE $PKG"
fi

ssh "$SOL9_HOST" "rm -rf $REMOTE"
echo
echo "==> $DIST/$PKGFILE"
ls -l "$DIST/$PKGFILE"
