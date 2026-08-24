#!/bin/sh
# Turn a staged tree and an inst product into the things a person downloads.
#
#   rustdesk-agent-VER-n32.tardist   Software Manager package. Copy it to the
#                                    IRIX box and open it with swmgr, or
#                                    `inst -f <dir>` after untarring it. This is
#                                    the normal way to install.
#   rustdesk-agent-VER-n32.tar.gz    the same files raw, plus install.sh, for a
#                                    machine where inst is not available or the
#                                    files want to go somewhere else.
#   SHA256SUMS                       so a download can be checked.
#
# A .tardist is nothing clever: it is a plain tar of the three files gendist
# wrote, which is what swmgr knows how to open. The pipeline that produces it is
# build.sh -> iris-gendist.sh -> here, and each step leaves its output on disk so
# any one of them can be re-run on its own.
#
# Usage:
#   scripts/package.sh [--stage DIR] [--inst DIR] [--version V] [--outdir DIR]
#                      [--abi n32|o32] [--no-tardist] [--no-tar]
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

STAGE=""
INST=""
OUTDIR=""
VERSION=""
ABI="n32"
DO_TARDIST=1
DO_TAR=1

die() { echo "package: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--stage)       STAGE="$2"; shift 2 ;;
		--inst)        INST="$2"; shift 2 ;;
		--outdir)      OUTDIR="$2"; shift 2 ;;
		--version)     VERSION="$2"; shift 2 ;;
		--abi)         ABI="$2"; shift 2 ;;
		--no-tardist)  DO_TARDIST=0; shift ;;
		--no-tar)      DO_TAR=0; shift ;;
		-h|--help)     sed -n '2,22p' "$0"; exit 0 ;;
		*)             die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$STAGE" ]   || STAGE="$REPO/build/stage"
[ -n "$INST" ]    || INST="$REPO/build/dist-$ABI"
[ -n "$OUTDIR" ]  || OUTDIR="$REPO/dist"
[ -n "$VERSION" ] || VERSION=$(version_string)

[ -d "$STAGE/bin" ] || die "no staged tree at $STAGE -- run scripts/build.sh"
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)

BASE="rustdesk-agent-$VERSION-$ABI"

# ---- the Software Manager package --------------------------------------------
if [ "$DO_TARDIST" = 1 ]; then
	if [ -f "$INST/rustdesk_agent.sw" ]; then
		echo ">>> tardist: $OUTDIR/$BASE.tardist"
		( cd "$INST" && tar cf "$OUTDIR/$BASE.tardist" \
			rustdesk_agent rustdesk_agent.idb rustdesk_agent.sw )
	else
		# Not fatal. The cross-build works on any machine; the inst product
		# needs a running IRIX guest, so a host without one still gets a
		# usable tarball rather than a failed build.
		echo ">>> no inst product in $INST -- skipping the tardist."
		echo "    Run scripts/iris-gendist.sh against a running guest to make one."
	fi
fi

# ---- the plain tarball --------------------------------------------------------
if [ "$DO_TAR" = 1 ]; then
	echo ">>> tarball: $OUTDIR/$BASE.tar.gz"
	WORK=$(mktemp -d "${TMPDIR:-/tmp}/rdpkg.XXXXXX")
	trap 'rm -rf "$WORK"' EXIT INT TERM
	TOP="$WORK/$BASE"
	mkdir -p "$TOP"
	# Three named directories rather than the whole staged tree, because the
	# stage also holds the gendist inputs -- the spec, the idb and the guest
	# script. Those describe how to BUILD the package, not how to use it, and a
	# spec file in a download only invites someone to run gendist on a machine
	# that has no idea what it is.
	cp -r "$STAGE/bin" "$STAGE/lib" "$STAGE/chest" "$TOP/"
	cp "$REPO/scripts/install.sh" "$TOP/install.sh"
	chmod 755 "$TOP/install.sh"
	cat > "$TOP/README.txt" <<EOF
RustDesk agent for IRIX -- $VERSION ($ABI)

  sh install.sh              install into /usr (as root)
  sh install.sh -p /opt/rd   install somewhere else
  sh install.sh -u           remove it again

What lands where, and what it needs, is in the header of install.sh.

The agent needs an X server on :0 and nothing else: libsodium, libvpx, mbedTLS
and zstd are linked in, and the one library IRIX 6.5 does not ship --
libgcc_s.so.1 -- is in lib/ and is installed beside the agent.

  rustdesk-agent --show-id         this machine's ID
  rustdesk-agent-gui               the settings panel (Motif; needs a display)
  agent-helper.sh start|stop|status

If you would rather use the Software Manager, install the .tardist instead --
same files, and swmgr then knows how to remove them.
EOF
	( cd "$WORK" && tar cf - "$BASE" ) | gzip -9 > "$OUTDIR/$BASE.tar.gz"
	rm -rf "$WORK"
	trap - EXIT INT TERM
fi

# ---- checksums ----------------------------------------------------------------
( cd "$OUTDIR" && ls "$BASE".* > /dev/null 2>&1 ) || die "nothing was produced"
if command -v sha256sum > /dev/null 2>&1; then
	( cd "$OUTDIR" && sha256sum "$BASE".* > SHA256SUMS )
fi

echo
echo "Packaged rustdesk-agent $VERSION ($ABI):"
ls -la "$OUTDIR" | sed 's/^/    /'
