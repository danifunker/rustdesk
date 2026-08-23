#!/bin/sh
# Put :0 back under a bare server after an xdm experiment.
for p in `ps -e -o pid,args | grep -E "X11/xdm|Xsgi" | grep -v grep | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
sleep 3
/root/restart-x.sh 2>&1 | tail -3
