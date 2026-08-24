#!/bin/sh
# Boot a disposable IRIX guest for the pipeline, and take it away again.
#
# Until this existed, `iris-gendist.sh` and `iris-install-test.sh` needed a
# guest that a person had started by hand, on a machine where the disk image
# happened to live at a known path. That is fine for the developer who set it
# up and it is not a pipeline. This is the piece that makes those steps runnable
# by CI, or by anyone with the image.
#
# THREE PROPERTIES, each of which took a decision:
#
# 1. IT NEVER WRITES TO THE IMAGE. The config sets `overlay = true`, so iris
#    puts every write in a `.diff.chd` sidecar and the base is opened read-only.
#    `stop` deletes the sidecar. A licensed IRIX install that gets mutated by a
#    build is not something you can hand to a second machine, and the images
#    this is meant to run against are hosted privately and pulled down fresh.
#
#    The image is SYMLINKED into the work directory and iris is pointed at the
#    link, so the sidecar lands beside the link and not beside the original.
#
# 2. NO PORT FORWARDS. The packaging steps need none: commands go over the
#    serial console, files go IN over HTTP -- which is the guest dialling out
#    through NAT, not a forward -- and come OUT through `iris-ci get`. Without
#    forwards there are no host ports to collide over, so two runs of this on
#    one machine do not fight, and it cannot steal the telnet port from a
#    developer's own emulator. (`ci_socket` is per-run for the same reason:
#    iris DELETES and rebinds whatever socket path it is given.)
#
# 3. HEADLESS BY DEFAULT. gendist and inst never touch the framebuffer, and
#    REX3 is where the emulator's remaining X wedges live. `--graphics` maps it
#    for anything that drives the panel.
#
# Usage:
#   eval "$(scripts/iris-guest.sh start)"     # exports IRIS_SOCKET, IRIS_GUEST_DIR
#   scripts/iris-guest.sh stop
#   scripts/iris-guest.sh status
#
# `start` prints shell assignments on stdout and progress on stderr, so it can
# be eval'd. It leaves the guest logged in on the console, which `iris-ci get`
# and `guest_run` both require.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

CMD="${1:-}"
[ $# -gt 0 ] && shift

IMAGE=""
WORKDIR=""
GRAPHICS=0
BOOT_TIMEOUT=900
SOCK=""

die() { echo "iris-guest: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--image)    IMAGE="$2"; shift 2 ;;
		--workdir)  WORKDIR="$2"; shift 2 ;;
		--socket)   SOCK="$2"; shift 2 ;;
		# The guest answers a command about 110 s in on this host. 900
		# leaves a lot of room for a loaded machine without turning a
		# genuinely wedged guest into a fifteen-minute wait. (The console
		# LOGIN BANNER, which is not what this waits for, takes about seven
		# minutes -- see the note above.)
		--timeout)  BOOT_TIMEOUT="$2"; shift 2 ;;
		--graphics) GRAPHICS=1; shift ;;
		-h|--help)  sed -n '2,42p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$WORKDIR" ] || WORKDIR="$REPO/build/guest"
mkdir -p "$WORKDIR"
WORKDIR=$(cd "$WORKDIR" && pwd)
PIDFILE="$WORKDIR/iris.pid"
[ -n "$SOCK" ] || SOCK="$WORKDIR/ci.sock"

case "$CMD" in
status)
	if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
		echo "running, pid $(cat "$PIDFILE"), socket $SOCK"
		exit 0
	fi
	echo "not running"
	exit 1
	;;

start)
	if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
		echo "iris-guest: already running (pid $(cat "$PIDFILE"))" >&2
		echo "IRIS_SOCKET=$SOCK; export IRIS_SOCKET"
		echo "IRIS_GUEST_DIR=$WORKDIR; export IRIS_GUEST_DIR"
		exit 0
	fi

	[ -n "$IMAGE" ] || IMAGE=$(sh "$REPO/scripts/fetch-image.sh")
	[ -f "$IMAGE" ] || die "no such image: $IMAGE"
	IMAGE=$(cd "$(dirname "$IMAGE")" && pwd)/$(basename "$IMAGE")

	IRIS_DIR=$(sh "$REPO/scripts/fetch-iris.sh")
	IRIS="$IRIS_DIR/target/release/iris"
	CI_BIN="$IRIS_DIR/target/release/iris-ci"

	# FREE SPACE, before anything slow happens.
	#
	# The overlay grows with everything the run writes -- a packaging run plus
	# an install test reached about 180 MB -- and a write that cannot be
	# satisfied does not fail politely. IRIX takes an I/O error on its root
	# filesystem as fatal:
	#
	#   ALERT: I/O error in filesystem ("/") meta-data dev 0x38 ...
	#   PANIC: Fatal error on root filesystem
	#
	# which is a kernel panic ten minutes into a run, a console full of dump
	# errors, and no obvious connection to the actual cause. That happened
	# here on a host that had filled to 100% underneath the run. One `df` up
	# front turns it into a sentence.
	_free_mb=$(df -Pm "$WORKDIR" 2>/dev/null | awk 'NR==2 {print $4}')
	_need_mb="${IRIS_GUEST_MIN_MB:-2048}"
	if [ -n "$_free_mb" ] && [ "$_free_mb" -lt "$_need_mb" ]; then
		die "only ${_free_mb} MB free where the guest would write ($WORKDIR);
${_need_mb} MB is the floor. The guest writes every change to a copy-on-write
overlay there, and IRIX panics on a failed write to its root filesystem rather
than reporting it -- so this stops now instead of ten minutes in. Free some
space, or set IRIS_GUEST_MIN_MB if you know better."
	fi

	# The symlink is what keeps the sidecar out of the image's directory.
	ln -sf "$IMAGE" "$WORKDIR/boot.chd"
	# NVRAM is written by the guest, so the tracked copy is not handed over --
	# it carries console=d, without which the PROM never talks to the serial
	# line and the boot looks like a hang.
	cp -f "$REPO/ports/iris-run/nvram-irix65.bin" "$WORKDIR/nvram.bin"
	rm -f "$WORKDIR/console.log"

	cat > "$WORKDIR/iris.toml" <<EOF
# Generated by scripts/iris-guest.sh. Edits here are lost on the next start.
banks      = [128, 128, 0, 0]
no_audio   = true
nvram      = "nvram.bin"
serial_log = "console.log"
ci_socket  = "$SOCK"

# overlay = true: every write goes to boot.chd.diff.chd and the image itself is
# never modified. \`stop\` deletes the sidecar.
[scsi.1]
path    = "boot.chd"
cdrom   = false
overlay = true

# The scratch volume iris-ci's put/get move files through. Not part of the CHD.
[scsi.2]
path    = "scratch.raw"
cdrom   = false
overlay = false
scratch = true
size_mb = 64

# No port forwards, deliberately -- see the header.
EOF

	echo ">>> booting a guest from $(basename "$IMAGE")" >&2
	if [ "$GRAPHICS" = 1 ]; then
		( cd "$WORKDIR" && nohup "$IRIS" --config iris.toml --ci --ci-display \
			--cpu r5000 > iris.log 2>&1 & echo $! > "$PIDFILE" )
	else
		( cd "$WORKDIR" && nohup "$IRIS" --config iris.toml --ci --headless \
			--cpu r5000 > iris.log 2>&1 & echo $! > "$PIDFILE" )
	fi
	sleep 3
	kill -0 "$(cat "$PIDFILE")" 2>/dev/null || {
		tail -20 "$WORKDIR/iris.log" >&2
		die "iris exited immediately (log above)"
	}

	export IRIS_SOCKET="$SOCK"
	# `start` is not optional under --ci: the CPU thread is created PAUSED, and
	# a machine that was never started is indistinguishable from a slow boot.
	"$CI_BIN" start > /dev/null 2>&1 || die "iris-ci start failed"

	# READINESS IS "A COMMAND RUNS", not "a banner appeared".
	#
	# `iris-ci boot` waits for the console login prompt and is the obvious
	# thing to call here. It does not work on this image: the PROM autoboots
	# straight past the menu it watches for, so it sat through its whole
	# timeout while `IRIS console login:` was sitting in console.log the
	# entire time. RESUME.md has warned since the first session that the
	# serial banner is a bad readiness test on this image -- telnetd answers
	# well before it prints -- and this is the same lesson from the other
	# side.
	#
	# So ask the only question that matters. `login` is retried each round
	# because early in the boot there is nothing to log into, and typing at a
	# console that is not listening yet costs nothing.
	echo ">>> waiting for the guest to answer a command (up to ${BOOT_TIMEOUT}s)" >&2
	# WALL CLOCK, not a count of the sleeps. Each failed round is a `login`
	# plus a `run` that waits out its own 20 s timeout, so counting only the
	# sleeps undercounts by about eight to one -- a "900 s" budget that had
	# really been running for two hours would be a wedged guest nobody was
	# told about. This runs on the build host, where `date +%s` exists.
	_start=$(date +%s)
	_ready=0
	while :; do
		"$CI_BIN" login root > /dev/null 2>&1 || true
		if "$CI_BIN" run --shell sh --timeout 20 'echo GUEST-READY' 2>/dev/null |
		   grep -q GUEST-READY; then
			_ready=1
			break
		fi
		# A panic looks exactly like a slow boot to a poll that only asks
		# whether a command ran, and IRIX prints this and stops for ever.
		if grep -q 'PANIC:' "$WORKDIR/console.log" 2>/dev/null; then
			tail -12 "$WORKDIR/console.log" >&2
			die "the guest panicked while booting (console tail above).
The usual cause is the host running out of room for the copy-on-write overlay:
IRIX takes a failed write to its root filesystem as fatal. Check df."
		fi
		[ $(expr $(date +%s) - $_start) -lt "$BOOT_TIMEOUT" ] || break
		sleep 5
	done
	_took=$(expr $(date +%s) - $_start)
	[ "$_ready" = 1 ] || {
		tail -20 "$WORKDIR/console.log" >&2
		die "the guest never answered a command in ${_took}s (console tail above)"
	}
	echo ">>> answered after ${_took}s" >&2

	echo ">>> guest is up: $SOCK" >&2
	echo "IRIS_SOCKET=$SOCK; export IRIS_SOCKET"
	echo "IRIS_GUEST_DIR=$WORKDIR; export IRIS_GUEST_DIR"
	;;

stop)
	[ -f "$PIDFILE" ] || { echo "iris-guest: nothing to stop" >&2; exit 0; }
	PID=$(cat "$PIDFILE")
	IRIS_DIR=$(sh "$REPO/scripts/fetch-iris.sh" 2>/dev/null || echo "")
	CI_BIN="$IRIS_DIR/target/release/iris-ci"
	export IRIS_SOCKET="$SOCK"

	if kill -0 "$PID" 2>/dev/null; then
		echo ">>> halting the guest" >&2
		# /etc/halt ASKS a question and does nothing if the answer never
		# arrives, which is why this pipes yes into it. The command never
		# returns -- the shell it is running in goes away with the system --
		# so its timeout is expected to expire and is not an error.
		#
		# A clean halt is a courtesy here, not a requirement: everything this
		# guest wrote is in an overlay that is deleted below, so there is no
		# filesystem to protect. Hence the short wait -- a guest that never
		# got as far as a shell cannot be asked to halt at all, and waiting
		# three minutes to discover that is three minutes of nothing.
		[ -x "$CI_BIN" ] && "$CI_BIN" run --shell sh --timeout 45 \
			'echo yes | /etc/halt' > /dev/null 2>&1 || true
		_n=0
		while [ $_n -lt 90 ]; do
			grep -q 'Okay to power off' "$WORKDIR/console.log" 2>/dev/null && break
			sleep 3
			_n=$(expr $_n + 3)
		done
		grep -q 'Okay to power off' "$WORKDIR/console.log" 2>/dev/null ||
			echo "iris-guest: no clean power-off after ${_n}s -- killing anyway (the overlay is discarded either way)" >&2
		kill -TERM "$PID" 2>/dev/null || true
		sleep 4
		kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null || true
	fi
	rm -f "$PIDFILE"

	# The sidecar holds every write the run made. Nothing in it is wanted --
	# the artifacts are on the host by now -- and keeping it would mean the
	# next run started from a machine some earlier run had modified.
	if [ -f "$WORKDIR/boot.chd.diff.chd" ]; then
		echo ">>> discarding $(wc -c < "$WORKDIR/boot.chd.diff.chd" | tr -d ' ') bytes of overlay" >&2
		rm -f "$WORKDIR/boot.chd.diff.chd"
	fi
	rm -f "$SOCK"
	echo ">>> guest stopped; the image was not modified" >&2
	;;

*)
	die "usage: $0 start|stop|status [options]"
	;;
esac
