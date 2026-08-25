#!/bin/sh
#
# rd-session.sh -- the desktop the agent serves.
#
# The Blade's console is not usable for this yet: /dev/fbs/jfb0 is
# `crw------- root root` and Xsun is not setuid, so only root can drive the
# real screen, and while CDE sits at its greeter `dtgreet` holds a server grab
# that hangs every client inside XOpenDisplay. This starts a *separate* session
# instead -- a virtual screen with a real CDE window manager on it -- which any
# ordinary user can run, which survives logouts, and which is there whether or
# not anyone is at the machine.
#
#   ./rd-session.sh start     # Xvfb + window manager + a terminal
#   ./rd-session.sh status
#   ./rd-session.sh stop
#
# Settings, all overridable from the environment:
#   RD_DISPLAY   :2 by default. :0 is the console, :1 is left for ad-hoc probes.
#   RD_GEOMETRY  1280x1024x24. Depth 24 is what the capture path expects; it
#                reads 32-bit pixels and reports the byte order it found.
#   RD_WM        dtwm (CDE's, what someone at this machine would recognise)
#                or twm. Empty for no window manager at all.
#   RD_APPS      what to start in the session. dtterm gives the peer something
#                to type into, which is also how input is tested.
#
# Written for Solaris 10's /bin/sh: backticks, no `local`, no [[ ]].
#
# ACCESS. The server is started with no authorisation file, so any local user
# can connect to it -- the same footing as the machine's own console session,
# and simpler than managing a cookie for a headless display. It does not listen
# on TCP. If that is too loose, add `-auth <file>` below and hand the agent the
# same XAUTHORITY.

set -e

RD_DISPLAY="${RD_DISPLAY:-:2}"
RD_GEOMETRY="${RD_GEOMETRY:-1280x1024x24}"
RD_WM="${RD_WM-dtwm}"
RD_APPS="${RD_APPS-/usr/dt/bin/dtterm}"
XVFB="${XVFB:-/usr/X11/bin/Xvfb}"     # Sun's own /usr/openwin/bin/Xvfb has no -screen
RUNDIR="${RUNDIR:-$HOME/.rustdesk-session}"
PIDFILE="$RUNDIR/pids"

log() { echo "rd-session: $*"; }

start_one() {
    # $1 command, rest arguments. Records the pid so stop can find it again --
    # matching on the command line would also match the ssh command running
    # this script, which is a mistake the PowerPC port made once.
    cmd="$1"; shift
    if [ ! -x "$cmd" ]; then
        log "skipping $cmd (not installed)"
        return 0
    fi
    DISPLAY="$RD_DISPLAY" "$cmd" "$@" >> "$RUNDIR/log" 2>&1 &
    echo "$! $cmd" >> "$PIDFILE"
    log "started $cmd (pid $!)"
}

case "${1:-status}" in
start)
    mkdir -p "$RUNDIR"
    if [ -f "$PIDFILE" ] && "$0" status > /dev/null 2>&1; then
        log "already running on $RD_DISPLAY -- stop it first"
        exit 1
    fi
    : > "$PIDFILE"
    : > "$RUNDIR/log"

    log "starting $XVFB $RD_DISPLAY -screen 0 $RD_GEOMETRY"
    "$XVFB" "$RD_DISPLAY" -screen 0 "$RD_GEOMETRY" >> "$RUNDIR/log" 2>&1 &
    echo "$! $XVFB" >> "$PIDFILE"

    # The server needs to be answering before anything is pointed at it.
    i=0
    while [ $i -lt 20 ]; do
        if DISPLAY="$RD_DISPLAY" /usr/openwin/bin/xdpyinfo > /dev/null 2>&1; then
            break
        fi
        sleep 1
        i=`expr $i + 1`
    done
    if [ $i -ge 20 ]; then
        log "the server never came up -- see $RUNDIR/log"
        exit 1
    fi

    [ -n "$RD_WM" ] && start_one "/usr/dt/bin/$RD_WM" || true
    for app in $RD_APPS; do
        start_one "$app"
    done
    sleep 2
    log "session ready on $RD_DISPLAY"
    log "run the agent with DISPLAY=$RD_DISPLAY"
    ;;

stop)
    if [ ! -f "$PIDFILE" ]; then
        log "nothing recorded in $PIDFILE"
        exit 0
    fi
    # Reverse order: the clients first, the server last, so nothing is killed
    # while still talking to a server that has gone away.
    tail -r "$PIDFILE" 2>/dev/null || cat "$PIDFILE" | while read pid cmd; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            log "stopped $cmd (pid $pid)"
        fi
    done
    rm -f "$PIDFILE"
    ;;

status)
    if [ ! -f "$PIDFILE" ]; then
        log "no session recorded"
        exit 1
    fi
    alive=0
    while read pid cmd; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "  running  $cmd (pid $pid)"
            alive=`expr $alive + 1`
        else
            echo "  gone     $cmd (pid $pid)"
        fi
    done < "$PIDFILE"
    [ $alive -gt 0 ] || exit 1
    DISPLAY="$RD_DISPLAY" /usr/openwin/bin/xdpyinfo 2>/dev/null | \
        egrep "^name of display|dimensions" | sed 's/^/  /'
    ;;

*)
    echo "usage: $0 start|stop|status" >&2
    exit 2
    ;;
esac
