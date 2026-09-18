#!/bin/sh
# Print the path of a usable rb-cli on STDOUT, downloading a release build if
# none is installed. Taken from ../irixscsitb's script of the same name, which
# is the one rb-cli-provisioning code path there too.
#
#   RB=$(scripts/ensure-rbcli.sh) && "$RB" --version
#
# rb-cli (danifunker/rusty-backup) reads the IRIX disk image's XFS root
# directly, with no emulator, no root and no mount. scripts/make-sysroot.sh
# uses it to take the cross-compile sysroot out of the same image the guest
# boots, which is what lets a hosted runner build with nothing licensed on it
# but that one privately-hosted file.
#
# Order: $RB_CLI env > rb-cli on PATH > a previous download in --install-dir
# (default build/rb-cli) > download the release asset for this OS/arch. All
# progress goes to stderr; stdout carries ONLY the resulting path.
#
# Needs `gh` (authenticated, or $GH_TOKEN) for the release lookup, curl, and
# tar/unzip depending on the asset type.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

INSTALL_DIR="$REPO/build/rb-cli"
RB_REPO="danifunker/rusty-backup"

die() { echo "ensure-rbcli: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--install-dir) INSTALL_DIR="$2"; shift 2 ;;
		--repo)        RB_REPO="$2"; shift 2 ;;
		-h|--help)     sed -n '2,/^set -eu$/{/^set -eu$/!p;}' "$0"; exit 0 ;;
		*)             die "unknown option: $1" ;;
	esac
done

load_local_conf   # ci/local.conf may set RB_CLI

# Already provided or installed?
if [ -n "${RB_CLI:-}" ]; then
	command -v "$RB_CLI" >/dev/null 2>&1 || [ -x "$RB_CLI" ] || die "\$RB_CLI is set but not executable: $RB_CLI"
	printf '%s\n' "$RB_CLI"
	exit 0
fi
if command -v rb-cli >/dev/null 2>&1; then
	printf '%s\n' "rb-cli"
	exit 0
fi
if [ -x "$INSTALL_DIR/rb-cli" ]; then
	printf '%s\n' "$INSTALL_DIR/rb-cli"
	exit 0
fi

# Download the release build for this host.
case "$(uname -s)-$(uname -m)" in
	Linux-x86_64)   pat='rb-cli-linux-x64-.*\.tar\.gz'     ;;
	Linux-aarch64)  pat='rb-cli-linux-arm64-.*\.tar\.gz'   ;;
	Linux-riscv64)  pat='rb-cli-linux-riscv64-.*\.tar\.gz' ;;
	Darwin-arm64)   pat='rb-cli-macos-arm64-.*\.zip'       ;;
	Darwin-x86_64)  pat='rb-cli-macos-x64-.*\.zip'         ;;
	*) die "no rb-cli release mapping for $(uname -s)-$(uname -m) — install rb-cli yourself (or set \$RB_CLI)" ;;
esac

command -v gh >/dev/null 2>&1 || die "need gh to locate the rb-cli release (or install rb-cli / set \$RB_CLI)"
echo "ensure-rbcli: fetching $pat from $RB_REPO" >&2
# jq string literals need \\ where the regex has \ — newer jq hard-errors on
# "\." as an invalid string escape (older jq merely tolerated it).
jqpat=$(printf '%s' "$pat" | sed 's/\\/\\\\/g')
url=$(gh api "repos/$RB_REPO/releases/latest" \
	--jq ".assets[] | select(.name|test(\"^$jqpat$\")) | .browser_download_url" | head -1)
[ -n "$url" ] || die "no asset matching $pat in $RB_REPO's latest release"

mkdir -p "$INSTALL_DIR"
tmp="$INSTALL_DIR/.rb-cli-download"
curl -fsSL "$url" -o "$tmp"
# Dispatch on the resolved URL, not $pat — the pattern spells its dots as \.
# so a *tar.gz* glob never matched it and the tarball went to unzip.
case "$url" in
	*.tar.gz) tar -C "$INSTALL_DIR" -xzf "$tmp" rb-cli ;;
	*.zip)    command -v unzip >/dev/null 2>&1 || die "need unzip for the macOS rb-cli asset"
	          unzip -q -o "$tmp" rb-cli -d "$INSTALL_DIR" ;;
	*)        die "unexpected rb-cli asset type: $url" ;;
esac
rm -f "$tmp"
chmod +x "$INSTALL_DIR/rb-cli"
"$INSTALL_DIR/rb-cli" --version >&2

printf '%s\n' "$INSTALL_DIR/rb-cli"
