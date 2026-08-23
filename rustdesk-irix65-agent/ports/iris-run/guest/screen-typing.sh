#!/bin/sh
# The workload a remote desktop is actually used for: somebody typing.
#
# `setup-screen.sh` is close to the worst case -- an 80x24 xterm that repaints
# its whole window twice a second -- and a frame rate averaged over that is a
# number about scrolling, not about using the machine. This is the other end:
# characters appearing one at a time, which dirties a few macroblocks.
DISPLAY=:0; export DISPLAY
for p in `ps -e | grep -E 'xclock|xterm|xeyes' | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
sleep 1
xsetroot -solid gray30
/usr/bin/X11/xterm -geometry 80x24+280+60 -e /bin/sh -c '
    while true; do
        for w in the quick brown fox jumps over the lazy dog and then keeps typing; do
            /bin/echo "$w \c"
            sleep 1
        done
        echo
    done' >/dev/null 2>&1 &
sleep 4
ps -e | grep -c xterm
