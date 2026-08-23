#!/bin/sh
# Can the agent capture the screen when xdm owns it? See the long note below.
#
# Everything measured so far ran against a bare `Xsgi :0 -bs -c` started by
# restart-x.sh, which has no access control at all. A real machine runs xdm,
# which starts the server with -auth and a MIT-MAGIC-COOKIE living in xdm's
# authdir and copied into the *logged-in user's* ~/.Xauthority. The agent runs
# outside any session, so if the server insists on the cookie the agent goes
# blind the moment anyone logs in -- and blind at the login screen too, which is
# where remote access matters most.
#
# /etc/X0.hosts is SGI's host-based access list and already names localhost. X
# access control is a disjunction -- an allowed host OR a valid cookie -- so in
# theory that is enough. This finds out.
for p in `ps -e | grep rustdesk | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
sleep 1
echo "=== stopping the bare server so xdm can own :0 ==="
for p in `ps -e -o pid,args | grep Xsgi | grep -v grep | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
for p in `ps -e -o pid,args | grep "X11/xdm" | grep -v grep | awk '{print $1}'`; do
    kill -9 $p 2>/dev/null
done
sleep 3
echo "=== starting xdm ==="
/etc/init.d/xdm start 2>&1 | tail -3
sleep 60
echo "=== who owns :0 now ==="
ps -e -o pid,args | grep -E "Xsgi|xdm" | grep -v grep | head -4
echo "=== how the server was started (note -auth) ==="
ps -e -o args | grep Xsgi | grep -v grep | head -2
echo "=== the agent's own view of that display ==="
cd /tmp
DISPLAY=:0 LD_LIBRARYN32_PATH=/usr/sgug/lib32 /root/tmo 90 ./perfprobe bytes 2>&1 | head -5
