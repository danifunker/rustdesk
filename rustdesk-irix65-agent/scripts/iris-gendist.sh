#!/bin/sh
# Build the IRIX Software Manager product, in the guest, with the guest's own
# gendist.
#
# This is the one step of the pipeline that cannot happen on Linux. An inst
# product is three files -- a spec, an idb and a `.sw` archive in SGI's own
# format -- and `gendist`(1M) is the only thing that writes them. It ships in
# `inst_dev.sw` and it is already on the 6.5 image here. The same arrangement as
# ../irixscsitb: each OS packages its own build, so a 6.5 guest packages the n32
# binaries and (if an o32 flavor is ever built) a 5.3 guest would package those.
#
# It needs a RUNNING guest. Booting one is not this script's business -- the
# recipe is in RESUME.md §Operating the emulator, and one boot serves many
# packaging runs -- but the script checks, and says which of the two ways in is
# broken rather than hanging.
#
# THREE CHANNELS, each chosen because the others do not work for that job:
#   commands        the SERIAL console (iris-ci run). Telnet stalls after a few
#                   dozen sessions and the pipeline must not need a person; see
#                   guest_run in ci-lib.sh and docs/ISSUE-nat-inbound-stall.md.
#   host -> guest   HTTP. The guest has wget and reaches the host at
#                   192.168.0.1; that direction has never failed. A 10 MB binary
#                   down a serial line is not a thing you do twice. The server
#                   is temporary and dies with this script.
#   guest -> host   iris-ci get, over the console and the scratch volume. The
#                   guest cannot push a file anywhere, so this is the only way
#                   back.
#
# Usage:
#   scripts/iris-gendist.sh [--stage DIR] [--out DIR] [--version V]
#                           [--socket PATH] [--http-port N] [--abi n32|o32]
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

STAGE=""
OUT=""
VERSION=""
DISTVER=""
ABI="n32"
HTTP_PORT="8100"
SOCK="${IRIS_SOCKET:-/tmp/iris-rdagent.sock}"

die() { echo "iris-gendist: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--stage)        STAGE="$2"; shift 2 ;;
		--out)          OUT="$2"; shift 2 ;;
		--version)      VERSION="$2"; shift 2 ;;
		--dist-version) DISTVER="$2"; shift 2 ;;
		--abi)          ABI="$2"; shift 2 ;;
		--socket)       SOCK="$2"; shift 2 ;;
		--http-port)    HTTP_PORT="$2"; shift 2 ;;
		-h|--help)      sed -n '2,27p' "$0"; exit 0 ;;
		*)              die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$STAGE" ]   || STAGE="$REPO/build/stage"
[ -n "$OUT" ]     || OUT="$REPO/build/dist-$ABI"
[ -n "$VERSION" ] || VERSION=$(version_string)
[ -n "$DISTVER" ] || DISTVER=$(dist_version_from "$VERSION")
[ -n "$DISTVER" ] || die "cannot derive a numeric inst version from '$VERSION'; pass --dist-version"

[ -d "$STAGE/bin" ] || die "no staged tree at $STAGE -- run scripts/build.sh first"

IRIS_DIR="${IRIS_DIR:-$HOME/iris-upstream}"
CI_BIN="$IRIS_DIR/target/release/iris-ci"
[ -x "$CI_BIN" ] || die "no iris-ci at $CI_BIN (set IRIS_DIR in ci/local.conf)"
export IRIS_SOCKET="$SOCK"

# ---- 0. is there a guest, and does it answer on both channels? ----------------
echo ">>> checking the guest"
"$CI_BIN" ping > /dev/null 2>&1 || die "no iris on $SOCK.
Start one first -- RESUME.md \$Operating the emulator has the command -- and
remember 'iris-ci start', without which the CPU thread stays paused and a booted
guest is indistinguishable from a hung one."

IRIS_CI_BIN="$CI_BIN"
export IRIS_CI_BIN

# A shell on the console first. Without it every command below waits out its
# whole timeout against a login prompt, having done nothing.
guest_login || die "could not get a shell on the serial console"

guest_run 60 'test -x /usr/sbin/gendist && echo HAVE-GENDIST' 2>&1 | grep -q HAVE-GENDIST ||
	die "no /usr/sbin/gendist in the guest. It ships in the inst_dev.sw
subsystem ('Software Packager'); install it from the IRIX Development Option CD."

# ---- 1. stage the product description beside the files it describes -----------
echo ">>> staging the product description ($ABI, inst version $DISTVER)"
stage_inst_inputs "$ABI" "$DISTVER" "$STAGE" || die "unknown ABI: $ABI"

# The guest-side half, shipped in the tarball rather than typed at the shell:
# a command line long enough to name every gendist argument corrupts itself in
# the telnet tty and comes back as a syntax error on something nobody wrote.
cat > "$STAGE/gendist-run.sh" <<'GUEST'
#!/bin/sh
# Runs INSIDE the guest. Everything is already in /tmp/gd.
cd /tmp/gd || exit 1
rm -rf dist && mkdir dist || exit 1

# -all: build every subsystem in the spec. -sbase: where the idb's source paths
# start. The idb has already been version- and ABI-stamped on the host.
/usr/sbin/gendist -verbose \
    -sbase /tmp/gd \
    -idb   /tmp/gd/rustdesk-agent.idb \
    -spec  /tmp/gd/rustdesk-agent.spec \
    -dist  /tmp/gd/dist \
    -all
rc=$?
echo "gendist rc=$rc"
[ $rc -eq 0 ] || exit $rc
ls -l /tmp/gd/dist
echo GENDIST-OK
GUEST
chmod 755 "$STAGE/gendist-run.sh"

TARBALL="$REPO/build/gd-stage.tar"
mkdir -p "$REPO/build"
( cd "$STAGE" && tar cf "$TARBALL" bin lib chest rustdesk-agent.spec rustdesk-agent.idb gendist-run.sh )
echo "    $(ls -l "$TARBALL" | awk '{print $5}') bytes to move in"

# ---- 2. serve it, and pull it in from the guest -------------------------------
WGET=/usr/nekoware/bin/wget
URL="http://192.168.0.1:$HTTP_PORT/$(basename "$TARBALL")"
SERVEDIR=$(dirname "$TARBALL")
( cd "$SERVEDIR" && exec python3 -m http.server "$HTTP_PORT" > /dev/null 2>&1 ) &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null || true' EXIT INT TERM
sleep 1

echo ">>> moving the tree into the guest"
# One short command per line. The console mangles a long one -- a wget URL plus
# a tar plus an echo comes back as several hundred bytes of garbage and a bash
# syntax error on something nobody typed. The shell session persists between
# calls, so `cd` on its own line does what it looks like it does.
# `cd /` first. The console shell is long-lived and the last run left it in
# /tmp/gd, where `rm -rf /tmp/gd` fails with "Cannot remove the current working
# directory" -- which under `set -e` stops the whole pipeline on its second run
# and never on its first.
guest_run 60 "cd /" > /dev/null
guest_run 60 "rm -rf /tmp/gd" > /dev/null
guest_run 60 "mkdir /tmp/gd"  > /dev/null
guest_run 60 "cd /tmp/gd"     > /dev/null
guest_run 600 "$WGET -q $URL -O gd.tar" 2>&1 | sed 's/^/    /'
FETCH=$(guest_run 300 "tar xf gd.tar && rm gd.tar && echo STAGE-OK" 2>&1)
echo "$FETCH" | sed 's/^/    /'
echo "$FETCH" | grep -q STAGE-OK || die "the guest could not fetch or unpack the tree (output above)"

# ---- 3. gendist ---------------------------------------------------------------
echo ">>> gendist, in the guest"
GEN_OUT=$(guest_run 1200 'sh /tmp/gd/gendist-run.sh' 2>&1)
echo "$GEN_OUT" | sed 's/^/    /'
echo "$GEN_OUT" | grep -q GENDIST-OK || die "gendist failed in the guest (output above)"

# ---- 4. bring the product back ------------------------------------------------
echo ">>> pulling the product back over the serial console"
mkdir -p "$OUT"
for f in rustdesk_agent rustdesk_agent.idb rustdesk_agent.sw; do
	"$CI_BIN" get "/tmp/gd/dist/$f" --to "$OUT/$f" --timeout 900 ||
		die "could not fetch /tmp/gd/dist/$f. iris-ci get needs a shell on the
serial console: 'iris-ci login root' first, or it hangs for its whole timeout."
	[ -s "$OUT/$f" ] || die "$OUT/$f came back empty"
done

echo
echo ">>> Software Manager product ($ABI, packaged by the 6.5 guest):"
ls -la "$OUT" | sed 's/^/    /'
