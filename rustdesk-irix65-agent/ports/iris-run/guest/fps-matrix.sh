#!/bin/sh
# Frame rate by picture size and by workload -- the only honest way to answer
# "how fast is it", because the two axes matter more than any single number.
#
# The pointer runs at one move every 150 ms, which is a person moving a mouse
# deliberately rather than flinging it. Driving it faster measures input
# injection (an XTEST round trip each, on a one-processor machine) as much as
# it measures video.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
cd /tmp

xalive() {
    n=`ps -e | grep -c Xsgi`
    if [ "$n" -lt 1 ]; then echo "  !! restarting X"; /root/restart-x.sh > /tmp/xrestart.log 2>&1; fi
}

for q in balanced low; do
    echo "=== image_quality $q, pointer moving ==="
    xalive; sh /tmp/screen-typing.sh > /dev/null 2>&1
    xalive; sh /tmp/run-agent.sh > /dev/null 2>&1
    ./testpeer 127.0.0.1:21118 hunter2 45 quality $q spin 150 2>&1 \
        | grep -E "switched the display|video frames|average"
    echo "=== image_quality $q, xterm scrolling ==="
    xalive; sh /tmp/setup-screen.sh > /dev/null 2>&1
    xalive; sh /tmp/run-agent.sh > /dev/null 2>&1
    ./testpeer 127.0.0.1:21118 hunter2 45 quality $q 2>&1 \
        | grep -E "switched the display|video frames|average"
done
