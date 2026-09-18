#!/bin/sh
# Resolve -- or download -- the IRIX boot image, printing the usable .chd path
# on stdout. One code path for a developer machine and for CI, because an image
# that is acquired differently in the two places is an image that works in one.
#
# Source resolution, first match wins:
#   1. $IRIX65_IMAGE       a local path. In Actions this is how the
#                          irix65_image dispatch input arrives (self-hosted:
#                          the image never leaves the runner).
#   2. ci/local.conf       per-machine, .gitignore'd.
#   3. $IRIX65_DISK_URL    a download URL -- a repository SECRET in Actions.
#                          These are licensed IRIX installs: host them
#                          privately. A bare .chd or a .zip containing one.
#
# The names are ../irixscsitb's names, deliberately. The same image and the
# same secret serve both repositories.
#
# Modes:
#   (default)     print the path, downloading to --dest if only a URL is
#                 available. IDEMPOTENT: a non-empty --dest is reused, which
#                 is what makes actions/cache a transparent win.
#   --check-only  verify a source exists without downloading. A URL is probed
#                 best-effort and a silent probe only warns -- some hosts
#                 reject HEAD -- but a typo'd URL usually shows up here rather
#                 than ten minutes into a build.
#   --cache-key   a short stable hash of the URL, for actions/cache. Prints
#                 "local" when a local path will be used, so a caller can skip
#                 caching entirely.
#
# Logs to stderr; stdout carries only the path or the key.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

DEST=""
MODE="fetch"

die() { echo "fetch-image: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--dest)       DEST="$2"; shift 2 ;;
		--check-only) MODE="check"; shift ;;
		--cache-key)  MODE="key"; shift ;;
		-h|--help)    sed -n '2,31p' "$0"; exit 0 ;;
		*)            die "unknown option: $1" ;;
	esac
done

load_local_conf
LOCAL=$(resolve_local_image)
URL=$(resolve_disk_url)
[ -n "$DEST" ] || DEST="$REPO/build/guest-disk.chd"

no_source() {
	# In Actions, also an annotation, so the reason is on the run's summary
	# page and not only in a step log.
	[ "${GITHUB_ACTIONS:-}" = true ] &&
		echo "::error title=No IRIX boot image::Set the repository secret IRIX65_DISK_URL to a PRIVATE download URL for an IRIX 6.5 development .chd (or a .zip holding one), or dispatch with irix65_image on a self-hosted runner."
	die "no boot image: set IRIX65_IMAGE to a local .chd (or the irix65_image
dispatch input), or IRIX65_DISK_URL to a private download URL. ci/local.conf is
the per-machine place for either -- copy ci/local.conf.example."
}

case "$MODE" in
key)
	if [ -n "$LOCAL" ]; then
		echo local
	elif [ -n "$URL" ]; then
		printf %s "$URL" | { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-16
	else
		no_source
	fi
	exit 0
	;;
check)
	if [ -n "$LOCAL" ]; then
		[ -f "$LOCAL" ] || die "IRIX65_IMAGE is set but the file is missing: $LOCAL"
		echo "fetch-image: local image present ($LOCAL)" >&2
	elif [ -n "$URL" ]; then
		if curl -sfIL --max-time 20 "$URL" > /dev/null 2>&1 ||
		   curl -sfL --max-time 20 -r 0-0 -o /dev/null "$URL" 2>/dev/null; then
			echo "fetch-image: the download URL answers" >&2
		else
			echo "fetch-image: WARNING: the download URL did not answer a HEAD or range probe -- the download may still work, but check it" >&2
		fi
	else
		no_source
	fi
	exit 0
	;;
esac

# ---- fetch --------------------------------------------------------------------
if [ -n "$LOCAL" ]; then
	[ -f "$LOCAL" ] || die "IRIX65_IMAGE is set but the file is missing: $LOCAL"
	printf '%s\n' "$LOCAL"
	exit 0
fi
[ -n "$URL" ] || no_source

mkdir -p "$(dirname "$DEST")"
if [ -s "$DEST" ]; then
	echo "fetch-image: reusing $DEST" >&2
	printf '%s\n' "$DEST"
	exit 0
fi

echo "fetch-image: downloading the boot image" >&2
tmp="$DEST.download"
curl -fsSL "$URL" -o "$tmp"
if unzip -l "$tmp" > /dev/null 2>&1; then
	xdir="$DEST.unzip"
	rm -rf "$xdir"
	unzip -q -o "$tmp" -d "$xdir"
	chd=$(find "$xdir" -iname '*.chd' | head -1)
	[ -n "$chd" ] || die "no .chd inside the downloaded zip"
	mv "$chd" "$DEST"
	rm -rf "$xdir" "$tmp"
else
	mv "$tmp" "$DEST"
fi
[ -s "$DEST" ] || die "the download produced an empty file"
echo "fetch-image: $DEST ($(wc -c < "$DEST" | tr -d ' ') bytes)" >&2
printf '%s\n' "$DEST"
