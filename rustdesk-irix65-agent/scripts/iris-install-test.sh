#!/bin/sh
# Install the .tardist in the guest with inst(1M), then run what it installed.
#
# Building a package and shipping it is not the same as knowing it installs. The
# things this catches, none of which are visible from the host:
#
#   - a file landing in the wrong place, or not landing at all
#   - the agent failing to start because its libgcc_s.so.1 is not found -- the
#     whole reason the binary carries an rpath, and the one thing that would
#     work on the build machine and fail on everybody else's
#   - the Toolchest fragment not being where the desktop reads it
#   - inst rejecting the product outright
#
# It runs the installed binary WITHOUT LD_LIBRARYN32_PATH set, deliberately.
# Every other script here sets it, so an rpath that silently did nothing would
# never show up.
#
# Usage:
#   scripts/iris-install-test.sh [--boot] [--tardist FILE] [--http-port N]
#                                [--tarball] [--remove]
#
#   --boot     start a guest for this run and take it away again afterwards,
#              from an image resolved by scripts/fetch-image.sh. Without it,
#              attaches to a guest that is already running.
#   --tarball  also install the .tar.gz with install.sh under a NON-DEFAULT
#              prefix, which is the one path in install.sh that writes a
#              wrapper -- and therefore the one most likely to have rotted
#   --remove   also test `versions remove`, leaving the guest as it started
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

TARDIST=""
HTTP_PORT="8101"
DO_REMOVE=0
DO_TARBALL=0
BOOT=0

die() { echo "install-test: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--tardist)   TARDIST="$2"; shift 2 ;;
		--http-port) HTTP_PORT="$2"; shift 2 ;;
		--boot)      BOOT=1; shift ;;
		--tarball)   DO_TARBALL=1; shift ;;
		--remove)    DO_REMOVE=1; shift ;;
		-h|--help)   sed -n '2,22p' "$0"; exit 0 ;;
		*)           die "unknown option: $1" ;;
	esac
done

load_local_conf
if [ -z "$TARDIST" ]; then
	TARDIST=$(ls -t "$REPO"/dist/*.tardist 2>/dev/null | head -1)
	[ -n "$TARDIST" ] || die "no .tardist in $REPO/dist -- run scripts/release.sh"
fi
[ -f "$TARDIST" ] || die "no such file: $TARDIST"

IRIS_DIR=$(sh "$REPO/scripts/fetch-iris.sh")
IRIS_CI_BIN="$IRIS_DIR/target/release/iris-ci"
export IRIS_CI_BIN
[ -x "$IRIS_CI_BIN" ] || die "no iris-ci at $IRIS_CI_BIN"

# One cleanup handler, set before anything is started -- a second `trap ... EXIT`
# further down would replace it, and this is what stops a guest we booted.
cleanup() {
	[ -n "${HTTP_PID:-}" ] && kill "$HTTP_PID" 2>/dev/null
	[ "${BOOT:-0}" = 1 ] && sh "$REPO/scripts/iris-guest.sh" stop > /dev/null 2>&1
	return 0
}
trap cleanup EXIT INT TERM

if [ "$BOOT" = 1 ]; then
	_guest_env=$(sh "$REPO/scripts/iris-guest.sh" start) ||
		die "could not start a guest"
	eval "$_guest_env"
	[ -n "${IRIS_SOCKET:-}" ] || die "the guest started but told us no socket"
else
	export IRIS_SOCKET="${IRIS_SOCKET:-/tmp/iris-rdagent.sock}"
fi

"$IRIS_CI_BIN" ping > /dev/null 2>&1 ||
	die "no iris on $IRIS_SOCKET -- pass --boot to have one started for you"
guest_login || die "could not get a shell on the serial console"

# The guest half. Shipped rather than typed: an inst session driven from a
# command line long enough to hold a here-document is exactly the kind of line
# the serial console mangles.
WORK="$REPO/build/insttest"
rm -rf "$WORK"; mkdir -p "$WORK"
cp "$TARDIST" "$WORK/rd.tardist"
cat > "$WORK/inst-run.sh" <<'GUEST'
#!/bin/sh
# Runs INSIDE the guest.
set -u
rm -rf /tmp/rdinst && mkdir /tmp/rdinst || exit 1
cd /tmp/rdinst || exit 1
tar xf /tmp/rd.tardist || exit 1
ls -l

# `install standard` then `go`. inst reads its commands from stdin, and this is
# the whole non-interactive session -- there is nothing to answer because the
# product replaces itself and conflicts with nothing.
#
# `quit` is sent last and is the one part worth knowing about: inst's quit path
# runs a machine-compatibility check that has been seen, on an odd hinv, to
# propose removing unrelated software. Nothing here would confirm such a
# removal -- stdin has ended by then -- but that is why the log is kept and
# printed rather than thrown away.
inst -f /tmp/rdinst > /tmp/inst.log 2>&1 <<EOF
install standard
go
quit
EOF
echo "inst rc=$?"
tail -25 /tmp/inst.log
echo INST-DONE
GUEST

cat > "$WORK/verify.sh" <<'GUEST'
#!/bin/sh
# Runs INSIDE the guest, AFTER the install. No LD_LIBRARYN32_PATH anywhere in
# here -- if the agent needs one, this is where that shows up.
unset LD_LIBRARYN32_PATH
echo "--- what landed ---"
ls -l /usr/sbin/rustdesk-agent /usr/sbin/rustdesk-agent-gui 2>&1
ls -l /usr/lib/rustdesk-agent 2>&1
ls -l /usr/lib/X11/app-chests/RustDesk.chest 2>&1
echo "--- inst's own inventory ---"
versions -n rustdesk_agent 2>&1 | head -8
echo "--- the installed agent runs ---"
/usr/sbin/rustdesk-agent --show-id 2>&1 | head -3
echo "--- the installed helper answers ---"
/usr/lib/rustdesk-agent/agent-helper.sh status 2>&1 | head -6
echo "--- the panel finds the installed helper, not a stray one ---"
# RD_HELPER unset on purpose: this is the search that a person double-clicking
# the Toolchest entry gets. The panel is /usr/sbin/rustdesk-agent-gui, so
# "beside argv[0]" finds nothing and the packaged path is what has to answer.
unset RD_HELPER
/usr/sbin/rustdesk-agent-gui -help > /dev/null 2>&1
ls -l /usr/lib/rustdesk-agent/agent-helper.sh > /dev/null 2>&1 && echo "helper is where the panel looks"
echo VERIFY-DONE
GUEST
chmod 755 "$WORK"/*.sh

( cd "$WORK" && exec python3 -m http.server "$HTTP_PORT" > /dev/null 2>&1 ) &
HTTP_PID=$!
sleep 1

WGET=/usr/nekoware/bin/wget
BASE="http://192.168.0.1:$HTTP_PORT"

echo ">>> moving the package into the guest"
guest_run 60 "cd /tmp" > /dev/null
for f in rd.tardist inst-run.sh verify.sh; do
	guest_run 600 "$WGET -q $BASE/$f -O /tmp/$f" > /dev/null
done
guest_run 60 "chmod 755 /tmp/inst-run.sh /tmp/verify.sh" > /dev/null

echo ">>> inst, in the guest"
OUT=$(guest_run 1200 'sh /tmp/inst-run.sh' 2>&1)
echo "$OUT" | sed 's/^/    /'
echo "$OUT" | grep -q INST-DONE || die "the inst session did not finish (output above)"

echo
echo ">>> what the install produced"
OUT=$(guest_run 600 'sh /tmp/verify.sh' 2>&1)
echo "$OUT" | sed 's/^/    /'
echo "$OUT" | grep -q VERIFY-DONE || die "the verification did not finish (output above)"

# Each of these is a way the package can be wrong that still leaves inst happy.
echo "$OUT" | grep -q '/usr/sbin/rustdesk-agent$\|/usr/sbin/rustdesk-agent ' ||
	die "the agent is not at /usr/sbin/rustdesk-agent"
# An `if`, not `grep ... && die`. The latter is safe under `set -e` -- a failing
# non-last command of an AND-OR list does not abort -- but it reads exactly like
# the bug it is not, and this file is meant to be read.
if echo "$OUT" | grep -qi 'rld:\|not found\|cannot open'; then
	die "the installed agent could not resolve a library -- the rpath did not take"
fi
echo "$OUT" | grep -q 'agent id\|^[a-z0-9]\{9\}$' ||
	echo "    NOTE: --show-id printed nothing recognisable; read the block above."

# The helper must resolve the INSTALLED agent. It used to default to a SGUG
# path and fall back to /tmp, so on a development machine it reported the /tmp
# copy after a successful install -- correct-looking output about the wrong
# binary, and invisible anywhere but here.
echo "$OUT" | grep -q 'agent=/usr/sbin/rustdesk-agent' ||
	die "the installed helper is not using /usr/sbin/rustdesk-agent (see 'agent=' above)"

if [ "$DO_TARBALL" = 1 ]; then
	TARBALL=$(ls -t "$REPO"/dist/*.tar.gz 2>/dev/null | head -1)
	[ -n "$TARBALL" ] || die "no .tar.gz in $REPO/dist"
	cp "$TARBALL" "$WORK/rd.tar.gz"
	cat > "$WORK/tarball-run.sh" <<'GUEST'
#!/bin/sh
# Runs INSIDE the guest. A non-default prefix on purpose: /usr is what inst
# already owns, and the interesting code is the wrapper install.sh writes when
# the prefix does not match the rpath the binary carries.
set -u
rm -rf /tmp/rdtar /opt/rdtest && mkdir /tmp/rdtar || exit 1
cd /tmp/rdtar || exit 1
gunzip -c /tmp/rd.tar.gz | tar xf - || exit 1
cd rustdesk-agent-* || exit 1
sh install.sh -p /opt/rdtest || exit 1
echo "--- the wrapper runs, with no environment set ---"
unset LD_LIBRARYN32_PATH
/opt/rdtest/sbin/rustdesk-agent --show-id
echo "--- and then goes away again ---"
sh install.sh -p /opt/rdtest -u
ls /opt/rdtest/sbin 2>&1
echo TARBALL-DONE
GUEST
	chmod 755 "$WORK/tarball-run.sh"

	echo
	echo ">>> the .tar.gz, installed by hand under /opt/rdtest"
	for f in rd.tar.gz tarball-run.sh; do
		guest_run 600 "$WGET -q $BASE/$f -O /tmp/$f" > /dev/null
	done
	guest_run 60 "chmod 755 /tmp/tarball-run.sh" > /dev/null
	OUT=$(guest_run 900 'sh /tmp/tarball-run.sh' 2>&1)
	echo "$OUT" | sed 's/^/    /'
	echo "$OUT" | grep -q TARBALL-DONE || die "the tarball install did not finish"
fi

if [ "$DO_REMOVE" = 1 ]; then
	echo
	echo ">>> versions remove rustdesk_agent"
	guest_run 600 'versions remove rustdesk_agent' 2>&1 | sed 's/^/    /'
	guest_run 60 'ls /usr/sbin/rustdesk-agent 2>&1' | sed 's/^/    /'
fi

echo
echo "install test finished."
