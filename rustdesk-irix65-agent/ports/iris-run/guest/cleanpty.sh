#!/bin/sh
# Reap stale telnet logins. See gsh.py's teardown for why they accumulate.
for p in `ps -e -o pid,args | grep telnetd | grep -v grep | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
sleep 2
echo "sessions left:"; who | wc -l
