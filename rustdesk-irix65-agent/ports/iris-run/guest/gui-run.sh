#!/bin/sh
# Start the Motif settings panel on the guest's own display.
#
# `ps -e -o pid,args`, never plain `ps -e`: IRIX truncates COMD to eight
# characters, so `ps -e | grep r-deskvint-irix-gui` matches NOTHING and five
# consecutive launches looked like crashes when the panel had been running the
# whole time. `grep rustdesk` matches the agent as well, so a kill loop written
# that way takes the panel down with it.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
RD_HELPER=/tmp/agent-helper.sh; export RD_HELPER
cd /tmp
for p in `ps -e -o pid,args | grep r-deskvint-irix-gui | grep -v grep | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
sleep 1
nohup /tmp/r-deskvint-irix-gui > /tmp/gui.log 2>&1 &
sleep 8
echo "panels running: `ps -e -o pid,args | grep r-deskvint-irix-gui | grep -v grep | wc -l`"
tail -5 /tmp/gui.log
