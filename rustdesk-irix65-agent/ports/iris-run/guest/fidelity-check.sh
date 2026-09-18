#!/bin/sh
# Does the peer's picture track a screen that is actually moving?
#
# Run a real session against a busy desktop, stop the movement, let the last
# frames catch up, then compare what the peer decoded against a fresh capture
# of the same screen. A macroblock the damage report missed -- or one an
# encoder patch wrongly skipped -- shows up here as a cluster of large
# differences and nowhere else.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
for p in `ps -e | grep -E 'xclock|xterm|xeyes|rustdesk' | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
sleep 2
xsetroot -solid gray30
/usr/bin/X11/xclock -update 1 -geometry 180x180+40+40 >/dev/null 2>&1 &
/usr/bin/X11/xterm -geometry 80x24+280+60 -sb -e /bin/sh -c 'i=0; while [ $i -lt 40 ]; do date; ls /usr/lib | head -6; i=`expr $i + 1`; sleep 2; done; sleep 600' >/dev/null 2>&1 &
sleep 6
cd /tmp
rm -f /tmp/decoded.ppm
nohup /tmp/r-deskvint-irix -v --listen 127.0.0.1 --port 21118 > /tmp/agent.log 2>&1 &
sleep 10
./testpeer 127.0.0.1:21118 hunter2 100 decode 2>&1 | tail -9
# The xterm stops after ~80 s; the clock keeps ticking, so its face is the only
# region allowed to differ.
for p in `ps -e | grep xclock | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
./perfprobe verify 2>&1 | tail -8
