#!/bin/sh
# Put three unmistakable colours on the screen, run a real session, and read
# pixels out of the frame the peer decoded. This covers the whole chain --
# capture, byte order, downscale, colour conversion, VP8 encode, VP8 decode --
# which is the only thing that catches a swap the two halves agree on.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
for p in `ps -e | grep -E 'xclock|xterm|xeyes|rustdesk' | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
sleep 2
xsetroot -solid black
/usr/bin/X11/xterm -geometry 20x10+40+60  -bg red   -fg white -e /bin/sh -c 'echo RED;   sleep 600' >/dev/null 2>&1 &
/usr/bin/X11/xterm -geometry 20x10+400+60 -bg green -fg black -e /bin/sh -c 'echo GREEN; sleep 600' >/dev/null 2>&1 &
/usr/bin/X11/xterm -geometry 20x10+760+60 -bg blue  -fg white -e /bin/sh -c 'echo BLUE;  sleep 600' >/dev/null 2>&1 &
sleep 8
cd /tmp
rm -f /tmp/decoded.ppm
nohup /tmp/rustdesk-agent -v --listen 127.0.0.1 --port 21118 > /tmp/agent.log 2>&1 &
sleep 10
./testpeer 127.0.0.1:21118 hunter2 40 decode 2>&1 | tail -10
echo "--- decoded pixels: left should be RED, middle GREEN, right BLUE ---"
for off in 96195 96705 97245; do
    dd if=/tmp/decoded.ppm bs=1 skip=$off count=3 2>/dev/null | od -An -tu1
done
