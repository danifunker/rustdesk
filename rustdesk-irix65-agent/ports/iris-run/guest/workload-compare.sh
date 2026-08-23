#!/bin/sh
# The agent against three points on the workload range, back to back.
#
# One frame rate for "the agent" is close to meaningless: the ends differ by an
# order of magnitude and a remote desktop spends its life at the cheap one.
#
#   pointer   a moving cursor, small and continuous -- this measures the
#             agent's *capability* for small changes, which the fixed-content
#             workloads cannot: a screen that changes once a second reports one
#             frame a second however fast the agent is.
#   typing    a word a second, which is what the rate of change looks like
#   scrolling an 80x24 xterm repainting its whole window twice a second, very
#             nearly the worst case this design can meet
#
# Checks X before and after each session: a dead server produces "0 frames" and
# looks exactly like a performance collapse.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
cd /tmp

xalive() {
    n=`ps -e | grep -c Xsgi`
    if [ "$n" -lt 1 ]; then
        echo "  !! no X server -- restarting"
        /root/restart-x.sh > /tmp/xrestart.log 2>&1
    fi
}

run_one() {
    xalive
    sh /tmp/$1 > /dev/null 2>&1
    xalive
    sh /tmp/run-agent.sh > /dev/null 2>&1
    ./testpeer 127.0.0.1:21118 hunter2 $2 $3 2>&1 | grep -E "video frames|average|pointer moves"
}

echo "=== pointer: small continuous change (capability) ==="
run_one screen-typing.sh 60 "spin 40"
echo "=== typing: a word a second (rate of change) ==="
run_one screen-typing.sh 60
echo "=== scrolling: a whole window twice a second (worst case) ==="
run_one setup-screen.sh 60
