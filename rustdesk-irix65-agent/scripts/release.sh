#!/bin/sh
# The whole pipeline, in one command.
#
#   scripts/build.sh          cross-build + stage      (Linux, no emulator)
#   scripts/iris-gendist.sh   gendist                  (needs a running guest)
#   scripts/package.sh        .tardist + .tar.gz       (Linux)
#
# Each step leaves its output on disk and can be re-run alone, which is what you
# want when only one of them failed -- and the one that fails is nearly always
# the middle one, because it is the only one that needs a live IRIX.
#
# Without a guest, --no-inst gives you everything except the Software Manager
# package: the binaries and a tarball with install.sh in it, which is a complete
# and installable release for anyone who does not want to use swmgr.
#
# Usage:
#   scripts/release.sh [--version V] [--no-inst] [--no-build] [--outdir DIR]
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

VERSION=""
OUTDIR=""
DO_BUILD=1
DO_INST=1

die() { echo "release: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--version)  VERSION="$2"; shift 2 ;;
		--outdir)   OUTDIR="$2"; shift 2 ;;
		--no-build) DO_BUILD=0; shift ;;
		--no-inst)  DO_INST=0; shift ;;
		-h|--help)  sed -n '2,19p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$VERSION" ] || VERSION=$(version_string)
[ -n "$OUTDIR" ]  || OUTDIR="$REPO/dist"

echo "=== rustdesk-agent for IRIX, $VERSION ==="
echo

if [ "$DO_BUILD" = 1 ]; then
	sh "$REPO/scripts/build.sh"
	echo
fi

if [ "$DO_INST" = 1 ]; then
	sh "$REPO/scripts/iris-gendist.sh" --version "$VERSION"
	echo
fi

sh "$REPO/scripts/package.sh" --version "$VERSION" --outdir "$OUTDIR"
