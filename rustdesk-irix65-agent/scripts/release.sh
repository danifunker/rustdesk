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
#   scripts/release.sh [--boot] [--version V] [--no-inst] [--no-build]
#                      [--outdir DIR] [--install-test]
#
# --install-test is OPT-IN and is not part of a release. See the note beside it
# below.
#
# --boot starts a disposable guest for the emulator steps and takes it away
# again, from an image resolved by scripts/fetch-image.sh -- a local path, or a
# private URL in CI. Without it they attach to a guest that is already running,
# which is what you want while iterating.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

VERSION=""
OUTDIR=""
DO_BUILD=1
DO_INST=1
DO_TEST=0
BOOT=""

die() { echo "release: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--version)  VERSION="$2"; shift 2 ;;
		--outdir)   OUTDIR="$2"; shift 2 ;;
		--no-build) DO_BUILD=0; shift ;;
		--no-inst)  DO_INST=0; shift ;;
		--boot)     BOOT="--boot"; shift ;;
		--install-test) DO_TEST=1; shift ;;
		-h|--help)  sed -n '2,19p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$VERSION" ] || VERSION=$(version_string)
[ -n "$OUTDIR" ]  || OUTDIR="$REPO/dist"

echo "=== r-deskvint-irix for IRIX, $VERSION ==="
echo

if [ "$DO_BUILD" = 1 ]; then
	sh "$REPO/scripts/build.sh"
	echo
fi

# ONE guest for the whole release, not one per step.
#
# Both emulator steps take --boot and each would start and stop its own, which
# is correct when either is run alone and wasteful here: a boot is five minutes
# on an emulated R5000 and the two steps want the same machine. So the guest is
# started here, both steps attach to it through IRIS_SOCKET, and the trap stops
# it however this exits.
if [ -n "$BOOT" ]; then
	trap 'sh "$REPO/scripts/iris-guest.sh" stop > /dev/null 2>&1 || true' EXIT INT TERM
	_guest_env=$(sh "$REPO/scripts/iris-guest.sh" start) ||
		die "could not start a guest"
	eval "$_guest_env"
	[ -n "${IRIS_SOCKET:-}" ] || die "the guest started but told us no socket"
	echo
fi

if [ "$DO_INST" = 1 ]; then
	sh "$REPO/scripts/iris-gendist.sh" --version "$VERSION"
	echo
fi

sh "$REPO/scripts/package.sh" --version "$VERSION" --outdir "$OUTDIR"

# NOT part of a release, and off everywhere by default.
#
# It roughly doubles a run -- ten minutes of twenty, nearly all of it inside
# inst on an emulated R5000 -- and shipping a package should not cost that every
# time. Run it when the PACKAGING has changed: the file list, the install paths,
# what the helper looks for. That is the only kind of change it has ever caught
# anything on, and when it does catch something the something is invisible
# everywhere else (see docs/PACKAGING.md). For an ordinary code change it tells
# you nothing you did not already know.
if [ "$DO_TEST" = 1 ]; then
	echo
	sh "$REPO/scripts/iris-install-test.sh"
fi
