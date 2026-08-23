#!/bin/sh
#
# The non-interactive half of the IRIX settings panel.
#
# `gui_motif.c` is the window; this is everything that decides anything. The
# split is copied from the Mac bundle's deploy/agent-helper.sh, and its reason
# holds here too: the interface is the part that can only be exercised by a
# person sitting at the machine, so it should contain as little behaviour as
# possible. Everything below runs from a terminal, which is how it gets tested.
#
# It is also what keeps the panel honest about the CLI. Every `set` verb is one
# `rustdesk-agent --flag` invocation, so the panel cannot drift into having its
# own idea of what a setting means, and anything it can do can be done without
# it.
#
# Bourne shell, not bash: /bin/sh on IRIX 6.5 is the SVR4 shell.
set -u

AGENT="${RD_AGENT:-/usr/sgug/bin/rustdesk-agent}"
[ -x "$AGENT" ] || AGENT=/tmp/rustdesk-agent
CONF="${RD_CONF:-$HOME/.rustdesk-ppc-agent.conf}"
LOG="${RD_LOG:-/tmp/agent.log}"
PORT="${RD_PORT:-21118}"

# The agent needs libgcc_s from the sgug tree at run time; see RESUME.
LD_LIBRARYN32_PATH="${LD_LIBRARYN32_PATH:-/usr/sgug/lib32}"
export LD_LIBRARYN32_PATH

conf_get() {
    [ -f "$CONF" ] || return 0
    sed -n "s/^$1 *= *//p" "$CONF" | head -1
}

agent_pids() {
    ps -e | grep rustdesk-agent | grep -v grep | awk '{print $1}'
}

is_running() {
    [ -n "`agent_pids`" ]
}

# Start it detached and NOT owned by this shell's terminal. Without nohup the
# agent dies when the panel's session ends, which on a machine driven over
# telnet is every time.
start_agent() {
    ( cd /tmp && DISPLAY="${DISPLAY:-:0}" nohup "$AGENT" -v \
        --listen 0.0.0.0 --port "$PORT" >> "$LOG" 2>&1 & ) 
    sleep 3
}

stop_agent() {
    for p in `agent_pids`; do kill -9 "$p" 2>/dev/null; done
    sleep 1
}

case "${1:-}" in

# Everything the panel shows in one call, one `key=value` per line, so the
# window makes a single round trip when it opens or refreshes rather than one
# per field.
status)
    echo "agent=$AGENT"
    echo "present=`[ -x \"$AGENT\" ] && echo yes || echo no`"
    echo "running=`is_running && echo yes || echo no`"
    echo "id=`conf_get id`"
    echo "password=`conf_get password`"
    echo "server=`conf_get rendezvous_server`"
    echo "relay=`conf_get relay_server`"
    echo "key=`conf_get server_key`"
    echo "api=`conf_get api_server`"
    echo "ca=`conf_get ca_bundle`"
    echo "port=$PORT"
    # Reported rather than assumed: an agent that came up on the ReadDisplay
    # fallback works and is slow enough to look broken, and this is the only
    # place a person would find that out. See RESUME's note on the capture path.
    echo "capture=`sed -n 's/.*capture path: //p' \"$LOG\" 2>/dev/null | tail -1`"
    ;;

showkey)  "$AGENT" --show-key 2>/dev/null || echo "(no key yet)" ;;
showid)   "$AGENT" --show-id  2>/dev/null || echo "(no id yet)"  ;;
showlog)  tail -30 "$LOG" 2>/dev/null || echo "No log yet - the agent has not run." ;;

start)    start_agent; is_running && echo "Started." || echo "Could not start it. Try Show the log." ;;
stop)     stop_agent;  is_running && echo "It is still running." || echo "Stopped." ;;
restart)  stop_agent; start_agent
          is_running && echo "Restarted." || echo "Could not start it. Try Show the log." ;;

# set FIELD VALUE. An empty value is meaningful for most of these -- it is how
# a setting is cleared -- so it is passed through rather than rejected.
set)
    field="${2:-}"
    value="${3:-}"
    case "$field" in
    password) out=`"$AGENT" --password "$value" 2>&1`;      rc=$? ;;
    server)   if [ -n "$value" ]; then out=`"$AGENT" --server "$value" 2>&1`; rc=$?
              else out=`"$AGENT" --no-server 2>&1`; rc=$?; fi ;;
    relay)    out=`"$AGENT" --relay-server "$value" 2>&1`;  rc=$? ;;
    key)      out=`"$AGENT" --key "$value" 2>&1`;           rc=$? ;;
    api)      if [ -n "$value" ]; then out=`"$AGENT" --api-server "$value" 2>&1`; rc=$?
              else out=`"$AGENT" --no-api-server 2>&1`; rc=$?; fi ;;
    ca)       out=`"$AGENT" --ca-bundle "$value" 2>&1`;     rc=$? ;;
    *)        echo "unknown field: $field"; exit 2 ;;
    esac

    if [ $rc -ne 0 ]; then
        echo "$out"
        exit $rc
    fi

    # Say what changed in the terms a person set it in, not the terms the CLI
    # answered in. The CLI is talking to a script; this is talking to whoever
    # just pressed the button.
    case "$field" in
    password) echo "Password set." ;;
    server)   [ -n "$value" ] && echo "ID server set to $value. Restart the agent for it to register." \
                              || echo "Registration turned off; this machine is reachable by IP only." ;;
    relay)    [ -n "$value" ] && echo "Relay set to $value." \
                              || echo "Relay override cleared; whichever relay the ID server names will be used." ;;
    key)      [ -n "$value" ] && echo "Server key set." || echo "Server key cleared." ;;
    api)      [ -n "$value" ] && echo "Console set to $value. This machine will appear in its device list within about 15 seconds." \
                              || echo "Console cleared; this machine will not appear in any device list." ;;
    ca)       [ -n "$value" ] && echo "CA bundle set to $value." \
                              || echo "CA bundle cleared; the usual places will be searched." ;;
    esac
    ;;

*)
    echo "usage: $0 status | showid | showkey | showlog | start | stop | restart"
    echo "       $0 set password|server|relay|key|api|ca VALUE"
    exit 2
    ;;
esac
