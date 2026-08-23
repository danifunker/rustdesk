#!/bin/sh
# Put something on the screen that actually changes, so the damage path has
# work to do. A blank root looks exactly like a broken agent.
DISPLAY=:0; export DISPLAY
for p in `ps -e | grep -E 'xclock|xterm|xeyes' | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
xsetroot -solid gray20
/usr/bin/X11/xclock -update 1 -geometry 180x180+40+40 >/dev/null 2>&1 &
sleep 2
/usr/bin/X11/xterm -geometry 80x24+280+60 -sb -e /bin/sh -c 'while true; do date; ls /usr/lib | head -8; sleep 2; done' >/dev/null 2>&1 &
sleep 4
echo "clients running:"
ps -e | grep -E 'xclock|xterm' | grep -v grep | wc -l
