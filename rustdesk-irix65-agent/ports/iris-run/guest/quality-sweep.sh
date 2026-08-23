#!/bin/sh
# The peer's picture-size dial, measured end to end against the same screen.
#
# One run, three sessions, back to back: this host is shared and its load moves,
# so three numbers taken minutes apart are not comparable with each other.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
sh /tmp/run-agent.sh > /dev/null 2>&1
cd /tmp
for q in best balanced low; do
    echo "=== image_quality $q ==="
    ./testpeer 127.0.0.1:21118 hunter2 60 quality $q 2>&1 \
        | grep -E "switched the display|video frames|average"
    sleep 3
done
