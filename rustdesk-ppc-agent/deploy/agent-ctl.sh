#!/bin/sh
#
# Start, stop and check the agent as a LaunchAgent, from the G5 itself.
#
# This exists because one step cannot be done over ssh: an ssh login reaches a
# different launchd from the logged-in GUI ("Aqua") session, and the plist is
# LimitLoadToSessionType Aqua. Loading it from the wrong place appears to work
# and starts nothing.
#
# Why bother, when `build-ppc.sh deploy` starts the agent perfectly well over
# ssh: **the clipboard only works from the Aqua session.** PasteboardCreate
# returns -4960 from ssh and from a detached `screen`, and pbcopy and pbpaste
# fail there too, so it is the session and not the API. Capture is no guide --
# that works over ssh, which is exactly the wrong thing to conclude from.
#
# Install (from the host):
#     scp rustdesk-ppc-agent/deploy/agent-ctl.sh ppctiger:~/rustdesk-ctl
#     ssh ppctiger 'chmod +x ~/rustdesk-ctl'
#
# Use, in Terminal.app **on the G5**:
#     ~/rustdesk-ctl            # start it, and say whether the clipboard works
#     ~/rustdesk-ctl status
#     ~/rustdesk-ctl stop
#
# `stop` matters more than it looks: the plist has KeepAlive, so killing the
# process is not enough -- launchd restarts it. Unload it before deploying a new
# binary from the host, or the two take turns failing to bind port 21118.

LABEL=com.rustdesk.ppc-agent
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
BIN="$HOME/rustdesk-agent"
LOG="$HOME/agent.log"

say() { echo "  $*"; }

# Is this session the one that can reach the pasteboard? pbpaste is part of the
# OS and fails outside Aqua, which is exactly the test -- measured, not assumed.
in_aqua() {
    pbpaste >/dev/null 2>&1
}

agent_pids() {
    ps -axo pid,comm | awk '$2 ~ /rustdesk-agent/ { print $1 }'
}

is_loaded() {
    launchctl list 2>/dev/null | grep -q "$LABEL"
}

# Everything holding port 21118 that launchd does not know about: the detached
# `screen` that `build-ppc.sh deploy` leaves behind. Stopped before loading,
# because only one agent can bind the port.
stop_screen_agent() {
    if screen -ls 2>/dev/null | grep -q rdagent; then
        say "stopping the screen-started agent (only one may hold port 21118)"
        screen -S rdagent -X quit >/dev/null 2>&1
        sleep 1
    fi
}

kill_stragglers() {
    for p in `agent_pids`; do
        kill -9 "$p" 2>/dev/null
    done
}

report() {
    echo
    echo "--- status ---"
    if is_loaded; then
        say "launchd      : loaded ($LABEL)"
    else
        say "launchd      : NOT loaded"
    fi

    pids=`agent_pids`
    if [ -n "$pids" ]; then
        say "process      : running (pid$pids)"
    else
        say "process      : not running"
    fi

    # Only trust the log while the process is alive: the file outlives it, and a
    # stale "agent listening" line reads as a running agent when there is none.
    if [ -n "$pids" ] && [ -f "$LOG" ]; then
        last=`grep 'agent listening' "$LOG" 2>/dev/null | tail -1`
        if [ -n "$last" ]; then
            say "listening    : $last"
        else
            say "listening    : nothing in $LOG says so yet"
        fi
    elif [ -f "$LOG" ]; then
        say "listening    : no (the lines in $LOG are from an earlier run)"
    fi

    if in_aqua; then
        say "this shell   : IS the GUI (Aqua) session -- launchctl will work here"
    else
        say "this shell   : is NOT the GUI session (over ssh, or a detached"
        say "               screen). launchctl load would find nothing to load,"
        say "               and a clipboard started from here would not work."
    fi
    # Whether the *agent* has the clipboard is a different question from whether
    # this shell does, and the answer is in its own log -- it says so once per
    # session, when a peer connects.
    if [ -f "$LOG" ] && grep -q "clipboard unavailable" "$LOG" 2>/dev/null; then
        say "agent said   : clipboard unavailable (it was not started in Aqua)"
    fi

    # The same code the agent links, if the probes were copied across.
    if [ -x "$HOME/ppc-probes/clipshim" ]; then
        line=`"$HOME/ppc-probes/clipshim" 2>/dev/null | head -1`
        say "clipshim     : $line"
    fi
    echo
}

case "${1:-start}" in
start)
    echo "--- starting the agent in this session ---"
    [ -x "$BIN" ] || { say "no agent binary at $BIN -- deploy one first"; exit 1; }
    [ -f "$PLIST" ] || {
        say "no plist at $PLIST"
        say "copy it over first:"
        say "  scp rustdesk-ppc-agent/deploy/$LABEL.plist ppctiger:~/Library/LaunchAgents/"
        exit 1
    }
    # Refuse rather than warn. Going on from here produces launchctl's
    # "nothing found to load", which is true and unhelpful: it means the plist
    # is LimitLoadToSessionType Aqua and this session is not Aqua, and nothing
    # in that sentence says what to do about it.
    if ! in_aqua; then
        say "This shell is not the GUI (Aqua) session, so launchctl here would"
        say "load nothing and say \"nothing found to load\". Not attempting it."
        echo
        say "Two ways on, in order of how little they disturb:"
        say ""
        say "  1. Run this script from a Terminal window on the G5's own screen"
        say "     -- physically, or through screen sharing. Not over ssh."
        say ""
        say "  2. Log out of the G5 and log back in. The plist is already"
        say "     installed and enabled, and RunAtLoad starts it at every"
        say "     login, so no launchctl command is needed at all."
        echo
        say "Either way, check afterwards with:  ~/rustdesk-ctl status"
        say "The agent still runs perfectly over ssh without any of this --"
        say "the clipboard is the only thing that needs the Aqua session."
        exit 1
    fi
    stop_screen_agent
    launchctl unload "$PLIST" >/dev/null 2>&1
    kill_stragglers
    sleep 1
    if launchctl load -w "$PLIST"; then
        say "loaded"
    else
        say "launchctl load failed"
        exit 1
    fi
    sleep 3
    report
    if is_loaded && [ -n "`agent_pids`" ] && in_aqua; then
        echo "  Ready. Connect with 192.168.99.116 in the client's ID field."
        echo "  Copy something on the G5 and paste it on the client to check the clipboard."
    fi
    ;;
stop)
    echo "--- stopping the agent ---"
    launchctl unload "$PLIST" >/dev/null 2>&1 && say "unloaded from launchd"
    stop_screen_agent
    kill_stragglers
    sleep 1
    report
    ;;
status)
    report
    ;;
*)
    echo "usage: $0 [start|stop|status]"
    exit 2
    ;;
esac
