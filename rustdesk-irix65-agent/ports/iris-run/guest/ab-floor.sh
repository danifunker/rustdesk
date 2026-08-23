#!/bin/sh
# A/B the libvpx active-map early-out, interleaved.
#
# Interleaved rather than one after the other because this host is shared and
# its load moves: two runs minutes apart are not comparable, and the first
# attempt at this measured a 3x improvement at one scale and none at another,
# which is what noise looks like when you read it as a result.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
cd /tmp
i=1
while [ $i -le 3 ]; do
    echo "=== round $i: stock libvpx ==="
    ./perfprobe-stock floor 2>&1 | grep "profile 3"
    echo "=== round $i: patched libvpx ==="
    ./perfprobe floor 2>&1 | grep "profile 3"
    i=`expr $i + 1`
done
