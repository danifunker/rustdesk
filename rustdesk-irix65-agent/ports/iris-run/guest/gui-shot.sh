#!/bin/sh
# Start one panel and capture the screen with the agent's own capture path.
#
# NOTE ON MATCHING PROCESSES. IRIX `ps -e` truncates COMD to eight characters,
# so `ps -e | grep r-deskvint-irix-gui` matches NOTHING and `grep rustdesk`
# matches the agent AND the panel. Both are traps: the first made five
# consecutive launches look like crashes when the panel had been running the
# whole time, and the second means a careless kill loop takes the panel down
# with the agent. Use `ps -e -o pid,args`, which carries the full command line.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
RD_HELPER=/tmp/agent-helper.sh; export RD_HELPER
cd /tmp
for p in `ps -e -o pid,args | grep r-deskvint-irix-gui | grep -v grep | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
for p in `ps -e -o pid,args | grep -E 'xclock|xterm' | grep -v grep | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
sleep 2
xsetroot -solid gray40
nohup ./r-deskvint-irix-gui > /tmp/gui.log 2>&1 &
sleep 15
echo "panels running: `ps -e -o pid,args | grep r-deskvint-irix-gui | grep -v grep | wc -l`"
echo "--- gui.log ---"; cat /tmp/gui.log
./perfprobe dump 2>&1 | tail -2
