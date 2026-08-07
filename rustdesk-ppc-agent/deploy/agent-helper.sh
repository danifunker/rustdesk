#!/bin/sh
#
# The non-interactive half of "Agent for RustDesk PPC.app", in Contents/Resources.
#
# The window is a small Cocoa app (deploy/app-ui.m) which shells out to each
# command below. Nothing here displays anything, so all of it is testable from a
# terminal -- and bundle.sh runs it on every build.
#
# Keeping the two apart is deliberate: the interface is the part that can only
# be exercised by a person at the machine, so it should contain as little
# behaviour as possible. Everything that decides anything lives here.
#
# WHY NOT A SHELL SCRIPT AS CFBundleExecutable, measured on 10.5.8: it is
# refused by LaunchServices with -10810 (kLSUnknownErr) once it is any bigger
# than trivial, and even when such an app launches only its FIRST osascript is
# user-interactive -- every later one fails -1713 "No user interaction allowed",
# because a script app never becomes a foreground application.
set -u

# This script lives in Contents/Resources beside the payload it installs.
RES="$(cd "$(dirname "$0")" && pwd)"

LABEL=com.rustdesk.ppc-agent
PREFIX="$HOME/rustdesk-ppc-agent"
BIN="$PREFIX/rustdesk-agent"
CONF="$HOME/.rustdesk-ppc-agent.conf"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

# Before installation the only binary is the one inside the bundle. After it,
# prefer the installed copy so what the UI reports is what is actually running.
agent_bin() {
    if [ -x "$BIN" ]; then echo "$BIN"; else echo "$RES/rustdesk-agent"; fi
}

# One key out of the provenance file bundle.sh wrote.
info_get() {
    [ -f "$RES/BUILD-INFO" ] || { echo "?"; return 0; }
    v="$(sed -n "s/^$1=//p" "$RES/BUILD-INFO" | head -1)"
    [ -n "$v" ] && echo "$v" || echo "?"
}

conf_get() {
    [ -f "$CONF" ] || return 0
    sed -n "s/^$1 *= *//p" "$CONF" | head -1
}

# The installed wrapper knows where the agent went; before installation, point
# the bundled control script at the bundle's own copy.
ctl() {
    if [ -x "$HOME/rustdesk-ctl" ]; then
        "$HOME/rustdesk-ctl" "$@"
    else
        RUSTDESK_AGENT_BIN="$(agent_bin)" RUSTDESK_AGENT_LOG="$PREFIX/agent.log" \
            sh "$RES/rustdesk-ctl" "$@"
    fi
}

# Which slice of a universal binary this Mac executes. A plain function, not a
# case nested inside a command substitution inside a string: Leopard's bash
# fails to parse that, and the error lands in the middle of the status block.
this_slice() {
    case "$(machine 2>/dev/null)" in
        ppc970) echo "the G5 build" ;;
        ppc74*) echo "the G4 build" ;;
        *)      echo "whichever slice matches" ;;
    esac
}

is_installed() { [ -x "$BIN" ] && [ -f "$PLIST" ]; }
# Exact match on the label, not a substring. `launchctl list` prints
# PID<TAB>status<TAB>label, and the settings app registers itself as
# "[0x0-...].com.rustdesk.ppc-agent.settings" -- which CONTAINS the agent's
# label, so `grep -q "$LABEL"` is true whenever the window is merely open. That
# made Stop report "It is still running" every time, and the Start/Stop buttons
# read the wrong state. The app's bundle id being a prefix of the job label is
# allowed; code that cannot tell them apart is not.
is_running()   { launchctl list 2>/dev/null | awk -v l="$LABEL" '$NF == l { f = 1 } END { exit !f }' ; }

# ---------------------------------------------------------------------------
# Commands invoked from the AppleScript by `do shell script`.
# Each prints one thing and exits; none of them is interactive.
# ---------------------------------------------------------------------------
case "${1:-status}" in
status)
    A="$(agent_bin)"
    if is_installed; then
        if is_running; then st="Installed and running"; else st="Installed, not running"; fi
    else
        st="Not installed"
    fi
    id="$("$A" --show-id 2>/dev/null || echo '?')"
    srv="$(conf_get rendezvous_server)"; [ -n "$srv" ] || srv="(none - direct IP only)"
    rly="$(conf_get relay_server)";      [ -n "$rly" ] || rly="(whichever the ID server names)"
    key="$(conf_get server_key)";        [ -n "$key" ] && key="(set)" || key="(none)"
    if [ -n "$(conf_get password)" ]; then pw="(set)"; else pw="NOT SET - nobody can connect"; fi
    ip="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo '?')"
    # Which CPU this copy was built for. The G4 and G5 downloads carry the same
    # app name, so without this there is nothing on screen to tell them apart.
    arch="$(info_get arch)"
    # A universal copy carries both, and the kernel grades them at exec time --
    # so name the one this machine will actually run, which is the question
    # someone reads this line to answer.
    case "$arch" in
        *and*) arch="$arch (this Mac runs $(this_slice))" ;;
    esac
    printf 'Status:      %s\nID:          %s\nThis Mac:    %s\nPassword:    %s\nID server:   %s\nRelay:       %s\nServer key:  %s\nBuilt for:   %s\nBuild:       %s (%s)' \
        "$st" "$id" "$ip" "$pw" "$srv" "$rly" "$key" "$arch" "$(info_get version)" "$(info_get commit)"
    ;;
menu)
    # The action list, which depends on what is installed. Newline separated;
    # the AppleScript splits it into `choose from list` items.
    if is_installed; then
        if is_running; then printf 'Stop the agent\n'; else printf 'Start the agent\n'; fi
        printf 'Set the password\nSet the ID server\nSet the server key\nSet the relay server\n'
        printf 'Show my public key\nShow the log\nWhy is there no websocket or API setting?\nUninstall\n'
    else
        printf 'Install\nSet the password\nSet the ID server\nSet the server key\n'
        printf 'Why is there no websocket or API setting?\n'
    fi
    ;;
install)
    # install.sh does the work: it is the same script the command-line install
    # uses, so there is one implementation and the UI cannot drift from it.
    log="${TMPDIR:-/tmp}/rustdesk-install.log"
    sh "$RES/install.sh" --yes >"$log" 2>&1 \
        && echo "Installed. The agent will start at every login." \
        || { echo "Install failed:"; tail -6 "$log"; }
    ;;
uninstall)
    sh "$RES/install.sh" --uninstall >/dev/null 2>&1 \
        && echo "Removed. Your ID and password were kept, so reinstalling keeps the same ID." \
        || echo "Uninstall reported a problem."
    ;;
start)   ctl >/dev/null 2>&1; sleep 2; is_running && echo "Started." || echo "Could not start it. Try Show the log." ;;
restart) ctl stop >/dev/null 2>&1; sleep 1; ctl >/dev/null 2>&1; sleep 2
         is_running && echo "Restarted." || echo "It did not come back. Try Show the log." ;;
stop)    ctl stop >/dev/null 2>&1; sleep 1; is_running && echo "It is still running." || echo "Stopped." ;;
conf)    conf_get "$2" ;;
info)    info_get "$2" ;;
showkey) "$(agent_bin)" --show-key 2>/dev/null || echo "(no key yet)" ;;
showlog) tail -25 "$PREFIX/agent.log" 2>/dev/null || echo "No log yet - the agent has not run." ;;
set)
    # $2 = field, $3 = value. Every one of these goes through the agent binary
    # rather than editing the config file, so validation lives in one place.
    #
    # The output and exit status are BOTH used, because they used to not be:
    # this ran the binary with `>/dev/null 2>&1 && echo ...`, so a command that
    # failed printed nothing and exited 0, and the window reported "Nothing was
    # changed" for a setting that had silently not been applied. A failure has
    # to look different from a no-op.
    A="$(agent_bin)"
    field="$2"; value="${3:-}"
    case "$field" in
        password)
            if [ "${#value}" -lt 6 ]; then echo "The password must be at least 6 characters."; exit 0; fi
            out="$("$A" --password "$value" 2>&1)"; rc=$? ;;
        server)
            if [ -z "$value" ]; then out="$("$A" --no-server 2>&1)"; rc=$?
            else out="$("$A" --server "$value" 2>&1)"; rc=$?; fi ;;
        key)   out="$("$A" --key "$value" 2>&1)"; rc=$? ;;
        relay) out="$("$A" --relay-server "$value" 2>&1)"; rc=$? ;;
        *)     echo "Unknown setting: $field"; exit 0 ;;
    esac
    if [ "$rc" -ne 0 ]; then
        # Usage text is long and unhelpful in a dialog; the first line plus the
        # likely cause is what someone can act on.
        echo "Could not set $field."
        echo "The agent refused it (exit $rc). This usually means the installed"
        echo "agent is older than this app and does not know that setting yet;"
        echo "reinstall from the same download as this app."
        exit 0
    fi
    case "$field" in
        password) echo "Password set." ;;
        server)   [ -n "$value" ] && echo "ID server set to $value." || echo "Registration turned off; this Mac is reachable by IP only." ;;
        key)      [ -n "$value" ] && echo "Server key set." || echo "Server key cleared." ;;
        relay)    [ -n "$value" ] && echo "Relay server set to $value." || echo "Relay override cleared." ;;
    esac
    # A running agent reads its config at startup, so a change means a restart.
    if is_running; then
        ctl stop >/dev/null 2>&1
        ctl >/dev/null 2>&1
    fi
    ;;
*) echo "unknown command: $1" >&2; exit 2 ;;
esac
