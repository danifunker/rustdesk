#!/bin/sh
# Put unmistakable colour on the screen, then dump what the converter makes of
# it. Red on the left, green in the middle, blue on the right: a channel swap
# is then a matter of looking rather than of arithmetic.
DISPLAY=:0; export DISPLAY
for p in `ps -e | grep rustdesk | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
for p in `ps -e | grep -E 'xclock|xterm|xeyes' | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
xsetroot -solid black
/usr/bin/X11/xterm -geometry 20x10+40+60  -bg red   -fg white -e /bin/sh -c 'echo RED; sleep 600' >/dev/null 2>&1 &
/usr/bin/X11/xterm -geometry 20x10+400+60 -bg green -fg black -e /bin/sh -c 'echo GREEN; sleep 600' >/dev/null 2>&1 &
/usr/bin/X11/xterm -geometry 20x10+760+60 -bg blue  -fg white -e /bin/sh -c 'echo BLUE; sleep 600' >/dev/null 2>&1 &
sleep 8
cd /tmp && DISPLAY=:0 LD_LIBRARYN32_PATH=/usr/sgug/lib32 ./perfprobe dump
