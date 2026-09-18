#!/bin/sh
# Get the iris emulator and iris-ci, printing the directory that holds them on
# stdout in the layout the rest of the pipeline expects: <dir>/target/release/.
#
# A local build wins, because on a developer machine there already is one and
# it is the one being worked on. Failing that, the PREBUILT binaries from an
# iris release -- building iris from source is a Rust toolchain plus clang and
# libclang for the chd feature plus a full cargo build, which turns a CI job
# that should be seconds into minutes.
#
# Resolution, first match wins:
#   1. $IRIS_DIR                  a checkout that has been built
#   2. ~/iris-upstream, ../../iris  where a developer machine may keep one
#   3. a release download         $IRIS_RELEASE_REPO, default techomancer/iris,
#                                 at $IRIS_TAG or latest
#
# A LOCAL BUILD WITHOUT THE chd FEATURE IS SKIPPED, not used. The boot image is
# a .chd, and an iris built without `--features chd` starts, reads its config,
# and then refuses the disk with "CHD image support not compiled in" -- which is
# a failure three steps into a guest boot about a binary that was chosen here.
# The message is compiled in only when the feature is OFF, so its presence in
# the binary is the test. (2026-09-18: ~/repos/iris was exactly that build.)
#
# THE UPSTREAM EMULATOR, techomancer/iris. The old default, danifunker/iris,
# publishes no releases any more -- its releases API answers 404 (checked
# 2026-09-18) -- and ../irixscsitb moved to techomancer for the same reason.
# Since v2026-08-31-16-28 upstream ships ONE CLI build per platform,
# IRIS-cli-<os>-<arch>-<ver>.tar.gz, holding iris and iris-ci flat, with every
# build feature on (chd included) and the CPU a runtime option (--cpu).
#
# Usage:
#   scripts/fetch-iris.sh [--dir DIR] [--repo OWNER/NAME] [--tag TAG|latest]
#                         [--prebuilt]
#
#   --prebuilt  ignore local builds and use (or download) a release. What CI
#               wants, and what a developer wants to reproduce a CI run.
#
# Needs `gh` (authenticated) or `curl` for the download path. Nothing at all
# when a local build is present.
#
# NOTE ON VERSION. Anything older than upstream 02c4e155 ("fix hostr readback
# issues") wedges the X server within one frame of capture load. The packaging
# steps run headless and never touch the framebuffer, so they do not care --
# but anything that drives the panel does, and pinning IRIS_TAG to something
# ancient will produce a failure that looks like our bug and is not.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

DIR=""
GH_REPO=""
TAG=""
PREBUILT=0

die() { echo "fetch-iris: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--dir)     DIR="$2"; shift 2 ;;
		--repo)    GH_REPO="$2"; shift 2 ;;
		--tag)     TAG="$2"; shift 2 ;;
		--prebuilt) PREBUILT=1; shift ;;
		-h|--help) sed -n '2,/^set -eu$/{/^set -eu$/!p;}' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$GH_REPO" ] || GH_REPO="${IRIS_RELEASE_REPO:-techomancer/iris}"
[ -n "$TAG" ]     || TAG="${IRIS_TAG:-}"
# "latest" is what irixscsitb's workflow passes; here it means the same as blank.
[ "$TAG" = latest ] && TAG=""

have_both() { [ -x "$1/target/release/iris" ] && [ -x "$1/target/release/iris-ci" ]; }
# See the header: the refusal text exists in the binary only without the feature.
has_chd() { ! grep -q 'CHD image support not compiled in' "$1/target/release/iris"; }

# ---- 1 and 2: a local build ----------------------------------------------------
if [ "$PREBUILT" = 0 ]; then
	for _d in "${IRIS_DIR:-}" "$HOME/iris-upstream" "$REPO/../../iris"; do
		[ -n "$_d" ] || continue
		[ -d "$_d" ] || continue
		have_both "$_d" || continue
		if ! has_chd "$_d"; then
			echo "fetch-iris: skipping $_d -- built without the chd feature, so it cannot open the boot image" >&2
			continue
		fi
		echo "fetch-iris: using the local build in $_d" >&2
		( cd "$_d" && pwd )
		exit 0
	done
fi

# ---- 3: a release download -----------------------------------------------------
[ -n "$DIR" ] || DIR="$REPO/build/iris"
mkdir -p "$DIR/target/release"
# A download is reused only for the tag it was made for. `latest` is a moving
# target, so a download made as latest is reused as latest -- pin IRIS_TAG for
# a run that must not change underneath you.
STAMP="$DIR/target/release/.iris-release"
WANT="$GH_REPO@${TAG:-latest}"
if have_both "$DIR" && [ "$(cat "$STAMP" 2>/dev/null)" = "$WANT" ]; then
	echo "fetch-iris: reusing $DIR ($WANT)" >&2
	( cd "$DIR" && pwd )
	exit 0
fi

case "$(uname -s)" in
	Linux)  OS=linux ;;
	Darwin) OS=macos ;;
	*)      die "unsupported host $(uname -s); build iris yourself and set IRIS_DIR" ;;
esac
case "$(uname -m)" in
	x86_64|amd64) ARCH=x64 ;;
	aarch64|arm64) ARCH=arm64 ;;
	riscv64) ARCH=riscv64 ;;
	*) die "unsupported host arch $(uname -m); build iris yourself and set IRIS_DIR" ;;
esac

echo "fetch-iris: no local build -- downloading from $GH_REPO (${TAG:-latest}) for $OS-$ARCH" >&2
tmp=$(mktemp -d "${TMPDIR:-/tmp}/fetchiris.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

if command -v gh > /dev/null 2>&1; then
	# `gh` matches whatever the release actually named its CLI archive, which
	# has changed more than once; a hard-coded filename would rot.
	if [ -n "$TAG" ]; then
		gh release download "$TAG" --repo "$GH_REPO" --pattern "*cli*$OS*$ARCH*" --dir "$tmp" ||
			die "gh could not download a CLI archive for $OS-$ARCH at tag $TAG"
	else
		gh release download --repo "$GH_REPO" --pattern "*cli*$OS*$ARCH*" --dir "$tmp" ||
			die "gh could not download a CLI archive for $OS-$ARCH"
	fi
else
	command -v curl > /dev/null 2>&1 || die "need gh or curl to download iris"
	api="https://api.github.com/repos/$GH_REPO/releases/${TAG:+tags/$TAG}"
	[ -n "$TAG" ] || api="https://api.github.com/repos/$GH_REPO/releases/latest"
	url=$(curl -fsSL "$api" |
	      sed -n 's/.*"browser_download_url": *"\([^"]*cli[^"]*'"$OS"'[^"]*'"$ARCH"'[^"]*\)".*/\1/p' |
	      head -1)
	[ -n "$url" ] || die "no CLI asset for $OS-$ARCH in $GH_REPO ${TAG:-latest}"
	curl -fsSL "$url" -o "$tmp/iris-cli.tar.gz"
fi

# Layout-tolerant on purpose: releases have shipped these flat, ./-prefixed and
# nested under target/release at different times.
for f in "$tmp"/*; do
	case "$f" in
		*.tar.gz|*.tgz) ( cd "$tmp" && tar xzf "$f" ) ;;
		*.zip)          ( cd "$tmp" && unzip -q -o "$f" ) ;;
	esac
done
for b in iris iris-ci; do
	found=$(find "$tmp" -type f -name "$b" -perm -u+x 2>/dev/null | head -1)
	[ -n "$found" ] || found=$(find "$tmp" -type f -name "$b" 2>/dev/null | head -1)
	[ -n "$found" ] || die "the downloaded archive has no $b in it"
	cp "$found" "$DIR/target/release/$b"
	chmod 755 "$DIR/target/release/$b"
done

# --help exercises argument parsing and dynamic linking; iris has no --version.
# The prebuilt Linux CLI links ALSA, so on a bare runner this is where a missing
# libasound2 shows up -- here, not three steps into a boot.
"$DIR/target/release/iris" --help > /dev/null 2>&1 ||
	die "the downloaded iris does not run here (missing libasound2? try: ldd $DIR/target/release/iris)"
"$DIR/target/release/iris-ci" --help > /dev/null 2>&1 ||
	die "the downloaded iris-ci does not run here"
printf '%s\n' "$WANT" > "$STAMP"

echo "fetch-iris: $DIR/target/release/{iris,iris-ci} from $WANT" >&2
( cd "$DIR" && pwd )
