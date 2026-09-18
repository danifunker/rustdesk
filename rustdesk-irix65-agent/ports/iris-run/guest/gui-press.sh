#!/bin/sh
# Press the panel's buttons with injected input, and check what happened.
#
# The last unproven thing in this port: every field the window shows arrived
# through popen() from the helper, and the helper's write path is verified from
# a terminal, but no button had ever been pressed -- by a person or otherwise.
# This presses three, and it doubles as the first real exercise of keyboard
# injection, which until the panel existed had nothing focusable to type into.
#
# The coordinates are NOT read off a screenshot. The panel prints every widget's
# root-window position when RD_GUI_GEOM is set (see gui_motif.c) because the
# capture is at 1/2 scale, so a button read off one is +-2 native pixels before
# any arithmetic, and a click one pixel outside a PushButton does nothing at all
# and looks exactly like injection being broken.
#
# The typing test uses the RELAY field on purpose. It is empty on this machine,
# clearing it is verified, and nothing about the agent's behaviour depends on
# it -- unlike the ID server, which would make the next agent start try to
# register somewhere that does not exist.
#
# `ps -e -o pid,args`, never plain `ps -e`: IRIX truncates COMD to eight
# characters, so `grep r-deskvint-irix-gui` against it matches nothing.
#
# Runs against the /tmp build by default and against the INSTALLED one when
# asked, which is worth doing after a package install because it exercises the
# paths a person actually gets -- the panel finding its helper, and the helper
# finding the agent, neither of which is the same lookup as in /tmp:
#
#   RD_GUI=/usr/local/sbin/r-deskvint-irix-gui RD_HELPER= sh /tmp/gui-press.sh
#
# An empty RD_HELPER means "let the panel search", which is the whole point
# there.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
RD_GUI_GEOM=1; export RD_GUI_GEOM
GUI=${RD_GUI:-/tmp/r-deskvint-irix-gui}
if [ -n "${RD_HELPER+set}" ]; then
    export RD_HELPER
else
    RD_HELPER=/tmp/agent-helper.sh; export RD_HELPER
fi
# Whichever helper the panel is going to use is the one this script should drive
# too, or the "before" and "after" it prints describe a different machine.
HELPER=${RD_HELPER:-/usr/local/lib/r-deskvint-irix/agent-helper.sh}
[ -x "$HELPER" ] || HELPER=/tmp/agent-helper.sh
CONF=$HOME/.rustdesk-ppc-agent.conf
cd /tmp

TESTVAL=relay.press.test

# Prove the server answers SOMEONE before blaming anything below it. A wedged
# Xsgi accepts the connection and then never replies, so every client -- IRIX's
# own xdpyinfo included -- hangs for ever, with its CPU time frozen and no error
# in any log. The first run of this script hit exactly that and printed not one
# line, because `xsetroot` blocked before the first echo and the whole thing
# looked like a script that had never started. /root/restart-x.sh is the way
# back. /root/tmo is the guest's watchdog: IRIX has no timeout(1).
if /root/tmo 25 /usr/bin/X11/xdpyinfo > /dev/null 2>&1; then
    echo "X on :0 answers."
else
    echo "FAIL: X on :0 is not answering. It is wedged -- run /root/restart-x.sh."
    exit 1
fi

for p in `ps -e -o pid,args | grep r-deskvint-irix-gui | grep -v grep | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
for p in `ps -e -o pid,args | awk '{ c = $2; sub(/.*\//, "", c); if (c == "r-deskvint-irix") print $1 }'`; do
    kill -9 $p 2>/dev/null
done
sleep 2

/root/tmo 20 xsetroot -solid gray40

# Start from a known field.
#
# A click puts the insertion cursor exactly where it lands, which is correct and
# is itself part of what this proves -- but it means typing into a field that
# already holds the last run's value SPLICES the new text into the middle of the
# old one. The second run of this script produced
# `relay.press.tesrelay.press.testt` and it took a moment to see that as a pass
# rather than a corruption. Clearing through the helper, not through the
# keyboard, keeps the thing under test to one thing.
sh "$HELPER" set relay "" > /dev/null 2>&1

nohup "$GUI" > /tmp/gui.log 2>&1 &

# The geometry dump is on a 2.5 s timeout inside the panel, after the window
# manager has placed the shell; the panel's own first refresh forks the helper
# before that. Twenty seconds covers both on an emulated R5000.
sleep 20
echo "panel: $GUI, helper: $HELPER"
echo "panels running: `ps -e -o pid,args | grep r-deskvint-irix-gui | grep -v grep | wc -l`"
echo "--- geometry ---"
grep '^geom' /tmp/gui.log

# $7 $8 are the centre, which is what gets clicked. Last occurrence wins, so a
# second dump from a later run of the same log does not confuse it.
w() {
    awk -v n="$1" '$1=="geom" && $2==n { x=$7; y=$8 } END { if (x=="") exit 1; print x, y }' /tmp/gui.log
}

# How many agents are running, as a bare number. `expr` strips the leading
# whitespace `wc -l` leaves, which `test -eq` will not.
# Match the basename of argv[0] exactly, not the command line: `grep
# r-deskvint-irix` also matches the panel, and -- once the software is installed
# -- the helper's own shell, because the helper lives in /usr/local/lib/r-deskvint-irix.
agent_count() {
    expr `ps -e -o pid,args | awk '{ c = $2; sub(/.*\//, "", c); if (c == "r-deskvint-irix") print $1 }' | wc -l` + 0
}

# wait_agents COUNT SECONDS -- poll rather than sleep a guess.
#
# A fixed sleep was wrong here and looked like a broken button. Pressing Start
# runs the helper through popen(), which forks a shell, forks the agent, sleeps
# three seconds and then forks `ps` to check -- and the panel then refreshes,
# which forks the helper again. On an emulated R5000 that is comfortably more
# than the twelve seconds this used to wait, and the first run to exceed it
# reported "agents running: 0" about a Start button that had worked.
wait_agents() {
    _n=0
    while [ $_n -lt $2 ]; do
        _c=`agent_count`
        [ "$_c" -eq "$1" ] && break
        sleep 2
        _n=`expr $_n + 2`
    done
    echo "agents running: `agent_count`  (after ${_n}s, wanted $1)"
}

RELAY=`w relayField`   || { echo "FAIL: no geometry for relayField"; exit 1; }
APPLY=`w relayApply`   || { echo "FAIL: no geometry for relayApply"; exit 1; }
START=`w startBtn`     || { echo "FAIL: no geometry for startBtn"; exit 1; }
STOP=`w stopBtn`       || { echo "FAIL: no geometry for stopBtn"; exit 1; }
echo "relayField $RELAY / relayApply $APPLY / startBtn $START / stopBtn $STOP"

echo
echo "=== 1. click the relay field, type, click Apply ==="
echo "relay_server before: [`sed -n 's/^relay_server *= *//p' $CONF`]"
./xpoke click $RELAY type $TESTVAL sleep 500 click $APPLY
sleep 6
echo "relay_server after : [`sed -n 's/^relay_server *= *//p' $CONF`]"

echo
echo "=== 2. click Start ==="
./xpoke click $START
wait_agents 1 90

echo
echo "=== 3. click Stop ==="
./xpoke click $STOP
wait_agents 0 90

echo
echo "=== 4. put the machine back ==="
sh "$HELPER" set relay ""
echo "relay_server now   : [`sed -n 's/^relay_server *= *//p' $CONF`]"

echo
echo "--- gui.log tail ---"
tail -20 /tmp/gui.log
echo "--- a picture of it, /tmp/frame.ppm ---"
./perfprobe dump 2>&1 | tail -2
