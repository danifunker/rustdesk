#!/usr/bin/env bash
#
# Reproduce backlog item 1d: an agent whose video dies after it has been up a
# while, leaving every later peer with a working mouse and no picture.
#
# Runs on the **host**, not the G5, because the symptom is only visible from a
# peer: `Video::new` fails when a session starts, the agent logs "video
# unavailable -- serving input only", and carries on. Nothing on the G5 notices.
#
# Two hypotheses have already died, and both died because the probe testing them
# could not reproduce the symptom:
#
#   - display sleep. Suggested only by the failure being at eleven minutes and
#     `pmset` saying `displaysleep 10`. `fb-vigil` polling every 15 s reset
#     HIDIdleTime and kept the display awake for 33 minutes, so it could never
#     have seen it either way.
#   - plain idleness. `fb-idle` reproduces the agent's exact sequence -- read the
#     geometry, do nothing at all, then ask for the base address -- and is clean
#     at 2, 5, 11 and 20 minutes.
#
# So stop theorising and reproduce it, with a peer, against the real agent.
#
# **Cold start, then wait, then one peer.** That ordering is the experiment and
# it took a wrong version to see why. The first draft connected every ten
# minutes from t=0, which cannot reproduce the report however long it runs: the
# agent is never idle from a cold start, because the connection at t=0 has
# already exercised CoreGraphics and every one after it keeps the connection
# warm. The same mistake as fb-vigil's polling keeping the display awake, one
# level up. What was reported is an agent that started, sat with **no peer at
# all** for eleven minutes, and failed on the first one to arrive.
#
# So each round restarts the agent for a clean t=0, waits the given number of
# minutes touching nothing, and then connects exactly once. Each round also
# runs a *fresh* one-shot check on the G5 at that moment, because the report's
# sharpest detail is that a new process read the framebuffer fine while the old
# one could not -- if that holds here the fault is the process, not the machine.
#
#   PPC_HOST=ppctiger probes/soak-video.sh 11 25 45 90 ...   # idle minutes
#
# Watch it with:  tail -f /tmp/soak-video.log
set -u

HOST="${PPC_HOST:-ppctiger}"
ADDR="${PPC_ADDR:-192.168.99.116:21118}"
PASS="${PPC_PASS:-ppctest123}"
WAITS="${@:-11 25 45 90 180}"
LOG="${SOAK_LOG:-/tmp/soak-video.log}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT="$HERE/target/debug/examples/probe_client"

[ -x "$CLIENT" ] || { echo "build it first: cargo build --example probe_client" >&2; exit 1; }

# How the agent is started, which is a variable and not a detail.
#
# The original incident predates the LaunchAgent, so it happened to an agent
# started by `build-ppc.sh deploy` under a detached `screen` whose ssh had since
# closed. That is a different session lifecycle from the Aqua session, and this
# project has been caught twice by exactly that difference -- item 9, and the
# clipboard's -4960. With the reported case (cold start, 11 minutes, first peer)
# coming back clean twice under launchd, the launch method is the next thing to
# change rather than the idle time.
#
#   SOAK_LAUNCH=launchd   the Aqua session, how the agent runs now
#   SOAK_LAUNCH=screen    detached screen from a closing ssh, how it ran then
LAUNCH="${SOAK_LAUNCH:-launchd}"

restart_agent() {
    if [ "$LAUNCH" = "screen" ]; then
        # launchd first, or KeepAlive restarts its copy and the two fight over
        # the port. The ssh closing at the end of this is deliberate: it is what
        # the original agent's session did.
        ssh "$HOST" 'launchctl unload -S Aqua ~/Library/LaunchAgents/com.rustdesk.ppc-agent.plist >/dev/null 2>&1
                     screen -S rdagent -X quit >/dev/null 2>&1; sleep 2
                     ps -axo pid,comm | awk "\$2 ~ /rustdesk-agent/ {print \$1}" | while read p; do kill -9 $p; done
                     rm -f ~/agent.log
                     screen -dmS rdagent bash -c "~/rustdesk-agent --port 21118 -vv > ~/agent.log 2>&1"
                     sleep 4'
    else
        ssh "$HOST" 'screen -S rdagent -X quit >/dev/null 2>&1
                     launchctl unload -S Aqua ~/Library/LaunchAgents/com.rustdesk.ppc-agent.plist >/dev/null 2>&1
                     sleep 3; rm -f ~/agent.log
                     launchctl load -w -S Aqua ~/Library/LaunchAgents/com.rustdesk.ppc-agent.plist >/dev/null 2>&1
                     sleep 4'
    fi
}

{
    echo "# soak-video: cold-start rounds via $LAUNCH, idle minutes: $WAITS"
    echo "# each round: restart the agent, touch nothing for N minutes, connect once"
    echo "# looking for: a blind session -- frames=0, or \"video unavailable\" in the agent log"
    echo "# columns: time  idle(min)  pid  frames  keyframes  first-frame  fresh-base-address"
} >> "$LOG"

for wait_min in $WAITS; do
    restart_agent || { echo "restart failed" >> "$LOG"; continue; }
    FIRST_PID=$(ssh "$HOST" 'ps -axo pid,comm | awk "\$2 ~ /rustdesk-agent/ {print \$1; exit}"')

    # Nothing at all for the whole wait. No ssh, no connection, no probe: the
    # point is an agent that nobody has spoken to since it started.
    sleep $(( wait_min * 60 ))

    now=$(date +%H:%M:%S)
    up="$wait_min"

    out=$(timeout 40 "$CLIENT" "$ADDR" "$PASS" 2>/dev/null)
    frames=$(echo "$out" | sed -n 's/.*video frames *: \([0-9]*\).*/\1/p')
    keys=$(echo "$out" | sed -n 's/.*video frames *: [0-9]* (\([0-9]*\) keyframes.*/\1/p')
    firstf=$(echo "$out" | sed -n 's/.*first frame *: \(.*\)/\1/p')
    [ -n "$frames" ] || frames="CONNECT-FAILED"

    # The comparison the original report turns on: a brand new process, at this
    # same moment. `fb-vigil 0` is one tick and exit.
    fresh=$(ssh -o ConnectTimeout=15 "$HOST" '~/ppc-probes/fb-vigil 0 2>&1 | sed -n "3p"' 2>/dev/null \
            | awk '{print $4}')
    pid=$(ssh -o ConnectTimeout=15 "$HOST" 'ps -axo pid,comm | awk "\$2 ~ /rustdesk-agent/ {print \$1; exit}"' 2>/dev/null)
    [ -n "$pid" ] || pid="GONE"
    # launchd's KeepAlive restarts a crashed agent, which silently resets the
    # clock this whole experiment is measuring. Worth seeing rather than
    # averaging over.
    restarted=""
    [ "$pid" != "$FIRST_PID" ] && restarted="  <-- AGENT RESTARTED (was $FIRST_PID)"

    line=$(printf "%s  %4d  %-6s  %-14s %-3s %-22s %s%s" \
        "$now" "$up" "$pid" "$frames" "${keys:-?}" "${firstf:-?}" "${fresh:-?}" "$restarted")

    # The agent's own account is the better detector now that it retries every
    # five seconds: a transient failure recovers mid-session, so the peer may
    # still see frames and `frames=0` would miss it entirely. "video unavailable"
    # is logged when the session starts blind, whether or not it recovers.
    blind=$(ssh -o ConnectTimeout=15 "$HOST" 'grep -c "video unavailable" ~/agent.log 2>/dev/null' 2>/dev/null)
    [ -n "$blind" ] || blind=0
    [ "$blind" -gt 0 ] 2>/dev/null && line="$line  <-- $blind BLIND SESSION(S)"
    echo "$line" >> "$LOG"

    # The thing being hunted. Grab everything that might explain it, at once,
    # while the agent is still in the failed state -- a second chance may be
    # hours away.
    if [ "$frames" = "0" ] || [ "$frames" = "CONNECT-FAILED" ] || [ "$blind" -gt 0 ] 2>/dev/null; then
        {
            echo "### REPRODUCED at $now, up ${up} min"
            ssh "$HOST" 'echo "-- agent log, last 40 --"; tail -40 ~/agent.log
                         echo "-- fresh process --"; ~/ppc-probes/fb-vigil 0
                         echo "-- display asleep? --"; ~/ppc-probes/fb-vigil 0 | sed -n "3p"
                         echo "-- HID idle --"; ioreg -c IOHIDSystem | grep -m1 HIDIdleTime
                         echo "-- who has the display --"; ps -axo pid,comm | grep -iE "screensaver|VNC|vnc" | grep -v grep
                         echo "-- uptime --"; uptime' 2>&1
            echo "### end"
        } >> "$LOG"
    fi

done
echo "# soak finished" >> "$LOG"
