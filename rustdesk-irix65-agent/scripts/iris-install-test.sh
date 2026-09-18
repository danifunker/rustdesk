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
H=/usr/local/lib/r-deskvint-irix/agent-helper.sh
A=/usr/local/sbin/r-deskvint-irix
SYS=/etc/r-deskvint-irix.conf

# FIRST, before anything else runs the agent: the boot path, and the move of
# the settings into /etc. Both have to happen on a machine that has never had
# $SYS, and every later step would create it.
echo "--- the boot start, as rc2 runs it ---"
RH=`awk -F: '$3 == 0 { print $6; exit }' /etc/passwd`
OLD="$RH/.rustdesk-ppc-agent.conf"
[ "$RH" = / ] && OLD=/.rustdesk-ppc-agent.conf
OLD_ID=`sed -n 's/^id *= *//p' "$OLD" 2>/dev/null | head -1`
echo "root's home: $RH; old settings: $OLD; old id: ${OLD_ID:-none}"
[ -f $SYS ] && echo "PRE-EXISTING $SYS -- the move cannot be tested on this guest"
# rc2's environment: no HOME, no USER, a short PATH. The helper stopped on an
# unset HOME here until 2026-09-18, and nothing started at boot.
env -i PATH=/usr/sbin:/usr/bsd:/sbin:/usr/bin:/etc:/usr/etc:/usr/bin/X11 DISPLAY=:0 \
    /sbin/sh $H start 2>&1
sleep 3
ps -e -o pid,args | awk '{ c = $2; sub(/.*\//, "", c); if (c == "r-deskvint-irix") print "BOOT-START-RUNNING pid " $1 }'
ls -l $SYS 2>&1
NEW_ID=`sed -n 's/^id *= *//p' $SYS 2>/dev/null | head -1`
echo "id in $SYS: ${NEW_ID:-none}"
[ -n "$OLD_ID" ] && [ "$NEW_ID" = "$OLD_ID" ] && echo "BOOT-MOVE-KEPT-ID"
ls -l $SYS | awk '{ print "SYS-MODE " $1 " " $3 }'
$H stop > /dev/null 2>&1

# And the agent's own move, for a root shell that has a HOME: take $SYS away,
# ask for the id, and it must come back -- same id, written now.
echo "--- the agent's own move, from a root shell ---"
mv $SYS $SYS.test-aside
$A --show-id 2>&1 | head -3
[ -f $SYS ] && [ "`sed -n 's/^id *= *//p' $SYS | head -1`" = "$OLD_ID" ] && echo "AGENT-MOVE-KEPT-ID"
rm -f $SYS
mv $SYS.test-aside $SYS

echo "--- what landed ---"
ls -l /usr/local/sbin/r-deskvint-irix /usr/local/sbin/r-deskvint-irix-gui 2>&1
ls -l /usr/local/lib/r-deskvint-irix 2>&1
ls -l /usr/lib/X11/app-chests/R-DeskVint.chest 2>&1
echo "--- inst's own inventory ---"
versions -n r_deskvint_irix 2>&1 | head -8
echo "--- the installed agent runs ---"
/usr/local/sbin/r-deskvint-irix --show-id 2>&1 | head -3
echo "--- the installed helper answers ---"
/usr/local/lib/r-deskvint-irix/agent-helper.sh status 2>&1 | head -6
echo "--- the panel finds the installed helper, not a stray one ---"
# RD_HELPER unset on purpose: this is the search that a person double-clicking
# the Toolchest entry gets. The panel is /usr/local/sbin/r-deskvint-irix-gui, so
# "beside argv[0]" finds nothing and the packaged path is what has to answer.
unset RD_HELPER
/usr/local/sbin/r-deskvint-irix-gui -help > /dev/null 2>&1
ls -l /usr/local/lib/r-deskvint-irix/agent-helper.sh > /dev/null 2>&1 && echo "helper is where the panel looks"
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

# The settings must have moved to /etc with the machine's identity intact, by
# both routes, and the boot start must actually start the agent.
for _m in BOOT-START-RUNNING BOOT-MOVE-KEPT-ID AGENT-MOVE-KEPT-ID; do
	echo "$OUT" | grep -q "$_m" || die "$_m missing: see the boot-start block above"
done
echo "$OUT" | grep -q 'SYS-MODE -rw------- root' ||
	die "/etc/r-deskvint-irix.conf is not mode 600 and owned by root (see above)"

# Each of these is a way the package can be wrong that still leaves inst happy.
echo "$OUT" | grep -q '/usr/local/sbin/r-deskvint-irix$\|/usr/local/sbin/r-deskvint-irix ' ||
	die "the agent is not at /usr/local/sbin/r-deskvint-irix"
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
echo "$OUT" | grep -q 'agent=/usr/local/sbin/r-deskvint-irix' ||
	die "the installed helper is not using /usr/local/sbin/r-deskvint-irix (see 'agent=' above)"

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
cd r-deskvint-irix-* || exit 1
sh install.sh -p /opt/rdtest || exit 1
echo "--- the wrapper runs, with no environment set ---"
unset LD_LIBRARYN32_PATH
/opt/rdtest/sbin/r-deskvint-irix --show-id
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
	# A script, and its output to a FILE, for a reason that cost a ten-minute
	# timeout to find. `iris-ci run` recognises the end of a command by
	# "\nIRIS-CI-RC=" -- the marker at the start of a line. `versions` ends by
	# ringing the terminal bell three times with no newline after it, so the
	# marker arrives as "\a\a\aIRIS-CI-RC=0", is never matched, and the run
	# waits out its whole timeout on a command that finished in seconds.
	# nekoware's `ls` does the same with a trailing "\033[m" colour reset. Both
	# printed fine on the console; neither returned. So nothing chatty runs on
	# the console directly: it writes a log, the bells are stripped, and the
	# last thing printed is a line of our own.
	cat > "$WORK/remove-run.sh" <<'GUEST'
#!/bin/sh
# Runs INSIDE the guest.
versions remove r_deskvint_irix > /tmp/vremove.log 2>&1
echo "versions rc=$?"
tr -d '\007' < /tmp/vremove.log | tail -4
if [ -f /usr/local/sbin/r-deskvint-irix ]; then
	echo "STILL-THERE: /usr/local/sbin/r-deskvint-irix"
fi
versions -n r_deskvint_irix 2>&1 | tr -d '\007' | grep r_deskvint_irix
echo REMOVE-DONE
GUEST
	chmod 755 "$WORK/remove-run.sh"

	echo
	echo ">>> versions remove r_deskvint_irix"
	guest_run 120 "$WGET -q $BASE/remove-run.sh -O /tmp/remove-run.sh" > /dev/null
	OUT=$(guest_run 900 'sh /tmp/remove-run.sh' 2>&1)
	echo "$OUT" | sed 's/^/    /'
	echo "$OUT" | grep -q REMOVE-DONE || die "the removal did not finish (output above)"
	if echo "$OUT" | grep -q STILL-THERE; then
		die "versions remove left the agent installed"
	fi
	if echo "$OUT" | grep -q '^I  *r_deskvint_irix'; then
		die "inst still lists r_deskvint_irix as installed"
	fi
fi

echo
echo "install test finished."
