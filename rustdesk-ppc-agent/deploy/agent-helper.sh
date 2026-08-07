#!/bin/sh
#
# The non-interactive half of "RustDesk Agent.app", in Contents/Resources.
#
# The UI is a compiled AppleScript applet (deploy/app.applescript) which calls
# each command below with `do shell script`. Nothing here displays anything, so
# all of it is testable from a terminal -- and bundle.sh does exactly that.
#
# WHY AN APPLET AND NOT A SHELL-SCRIPT .APP, both measured on 10.5.8:
#
#   * a shell script as CFBundleExecutable is refused by LaunchServices with
#     -10810 (kLSUnknownErr). A trivial one launches; this one does not, and no
#     amount of trimming Info.plist keys, Resources or the bundle name changed
#     it;
#   * even when such an app does launch, only its FIRST osascript is user
#     interactive. Every later one fails with -1713 "No user interaction
#     allowed", because a script app never becomes a foreground application.
#
# A compiled applet has Apple's own Mach-O as its executable and is a real
# foreground app, so it launches and can show as many dialogs as it likes --
# verified: three in a row, including `with hidden answer`.
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

is_installed() { [ -x "$BIN" ] && [ -f "$PLIST" ]; }
is_running()   { launchctl list 2>/dev/null | grep -q "$LABEL"; }

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
    printf 'Status:      %s\nID:          %s\nThis Mac:    %s\nPassword:    %s\nID server:   %s\nRelay:       %s\nServer key:  %s' \
        "$st" "$id" "$ip" "$pw" "$srv" "$rly" "$key"
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
stop)    ctl stop >/dev/null 2>&1; sleep 1; is_running && echo "It is still running." || echo "Stopped." ;;
conf)    conf_get "$2" ;;
showkey) "$(agent_bin)" --show-key 2>/dev/null || echo "(no key yet)" ;;
showlog) tail -25 "$PREFIX/agent.log" 2>/dev/null || echo "No log yet - the agent has not run." ;;
set)
    # $2 = field, $3 = value. Every one of these goes through the agent binary
    # rather than editing the config file, so validation lives in one place.
    A="$(agent_bin)"
    field="$2"; value="${3:-}"
    case "$field" in
        password)
            if [ "${#value}" -lt 6 ]; then echo "The password must be at least 6 characters."; exit 0; fi
            "$A" --password "$value" >/dev/null 2>&1 && echo "Password set." || echo "Could not save it." ;;
        server)
            if [ -z "$value" ]; then "$A" --no-server >/dev/null 2>&1 && echo "Registration turned off; this Mac is reachable by IP only."
            else "$A" --server "$value" >/dev/null 2>&1 && echo "ID server set to $value." || echo "Could not save it."; fi ;;
        key)   "$A" --key "$value" >/dev/null 2>&1 && { [ -n "$value" ] && echo "Server key set." || echo "Server key cleared."; } ;;
        relay) "$A" --relay-server "$value" >/dev/null 2>&1 && { [ -n "$value" ] && echo "Relay server set to $value." || echo "Relay override cleared."; } ;;
    esac
    # A running agent reads its config at startup, so a change means a restart.
    if is_running; then
        ctl stop >/dev/null 2>&1
        ctl >/dev/null 2>&1
    fi
    ;;
*) echo "unknown command: $1" >&2; exit 2 ;;
esac
