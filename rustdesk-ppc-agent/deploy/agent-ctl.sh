#!/bin/sh
#
# Start, stop and check the agent as a LaunchAgent, from the G5 itself.
#
# It works from anywhere, including ssh, because of `launchctl -S Aqua`.
#
# That flag is the whole trick and it is **not in Leopard's `launchctl load`
# usage text**, which is why the obvious command fails: plain `launchctl load`
# from an ssh login filters by the caller's own session type, finds a plist
# marked `LimitLoadToSessionType Aqua`, matches nothing, and says "nothing found
# to load". `-S Aqua` names the session type to load into, and the job then
# starts inside the GUI session with the window server *and the pasteboard*.
# Verified: a probe started this way reports `PasteboardCreate = 0`, and the
# clipboard round-trips both directions.
#
# `unload` needs `-S Aqua` for the same reason, or it says "nothing found to
# unload" and leaves the agent running.
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
# Use, from anywhere -- ssh is fine:
#     ~/rustdesk-ctl            # start it, and say whether the clipboard works
#     ~/rustdesk-ctl status
#     ~/rustdesk-ctl stop
#
# `stop` matters more than it looks: the plist has KeepAlive, so killing the
# process is not enough -- launchd restarts it. Unload it before deploying a new
# binary from the host, or the two take turns failing to bind port 21118.

LABEL=com.rustdesk.ppc-agent
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
# Overridable because `deploy/install.sh` puts the agent in its own directory
# (the bundled libraries have to sit beside it), while the development G5 has
# it loose in $HOME. Defaults to the latter so an existing setup is unaffected.
BIN="${RUSTDESK_AGENT_BIN:-$HOME/rustdesk-agent}"
LOG="${RUSTDESK_AGENT_LOG:-$HOME/agent.log}"

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

    # Which session *this shell* is in no longer decides anything -- `-S Aqua`
    # does -- but it is worth showing, because it is the difference between what
    # this shell can do by hand and what the agent can do.
    if in_aqua; then
        say "this shell   : IS the GUI session (pbpaste works here)"
    else
        say "this shell   : is not the GUI session, which is fine: -S Aqua"
        say "               loads the agent into it regardless"
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
    # No session check here any more. `-S Aqua` names the session to load
    # into, so this works from ssh as well as from a GUI terminal -- which was
    # the whole difficulty, and cost an evening of "nothing found to load".
    if ! in_aqua; then
        say "(running from outside the GUI session; -S Aqua handles that)"
    fi
    stop_screen_agent
    launchctl unload -S Aqua "$PLIST" >/dev/null 2>&1
    kill_stragglers
    sleep 1
    if launchctl load -w -S Aqua "$PLIST"; then
        say "loaded"
    else
        say "launchctl load failed"
        exit 1
    fi
    sleep 3
    report
    if is_loaded && [ -n "`agent_pids`" ]; then
        echo "  Ready. Connect with 192.168.99.116 in the client's ID field."
        echo "  Copy something on the G5 and paste it on the client, and the other"
        echo "  way round, to check the clipboard."
    fi
    ;;
stop)
    echo "--- stopping the agent ---"
    launchctl unload -S Aqua "$PLIST" >/dev/null 2>&1 && say "unloaded from launchd"
    stop_screen_agent
    kill_stragglers
    sleep 1
    report
    ;;
status)
    report
    ;;
diag)
    # Everything needed to tell why `launchctl load` did what it did, in one
    # paste. `nothing found to load` has several causes that look identical from
    # the outside, and guessing between them costs a round trip each.
    echo "--- diag ---"
    say "user         : `whoami`   HOME=$HOME"
    say "tty          : `tty 2>/dev/null`"
    say "console owner: `ls -l /dev/console | awk '{print $3}'`"

    if in_aqua; then
        say "pbpaste      : works -> this shell can reach the pasteboard"
    else
        say "pbpaste      : FAILS  -> this shell cannot reach the pasteboard"
    fi

    # A second, independent read on which launchd this is. The GUI session's
    # launchd has the desktop's own agents in it; a login shell's has a handful.
    n=`launchctl list 2>/dev/null | wc -l | tr -d ' '`
    say "launchctl list: $n entries"
    # Not a session test: on Leopard an ssh login's launchctl lists the GUI
    # applications too, with Carbon PSN labels like [0x0-0xd70d7].com.apple.dock.
    # Believing otherwise sent one investigation down the wrong path. pbpaste
    # above is the test that actually distinguishes them.
    say "  gui apps    : `launchctl list 2>/dev/null | grep -c '^\[0x'` with PSN labels" 
    say "ours loaded  : `launchctl list 2>/dev/null | grep -c "$LABEL"`"

    if [ -f "$PLIST" ]; then
        say "plist        : `ls -l "$PLIST" | awk '{print $5" bytes, "$6" "$7" "$8}'`"
        say "  parses     : `plutil -lint "$PLIST" 2>&1 | sed 's|.*: ||'`"
        say "  session    : `grep -A1 LimitLoadToSessionType "$PLIST" | tail -1 | sed 's/[<>]/ /g' | awk '{print $2}'`"
        say "  Disabled   : `grep -c Disabled "$PLIST"` (want 0; 1 means launchctl -w turned it off)"
    else
        say "plist        : MISSING at $PLIST"
    fi
    say "binary       : `ls -l "$BIN" 2>/dev/null | awk '{print $5" bytes"}' || echo MISSING`"

    echo
    say "what launchctl actually says:"
    launchctl load -w -S Aqua "$PLIST" 2>&1 | sed 's/^/    /'
    say "(exit $?)"
    echo
    say "Paste all of the above back."
    ;;
*)
    echo "usage: $0 [start|stop|status|diag]"
    exit 2
    ;;
esac
