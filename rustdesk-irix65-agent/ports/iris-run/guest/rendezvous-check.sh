#!/bin/sh
# Does the agent's registration loop actually run on IRIX?
#
# --server is how a machine becomes reachable by ID through self-hosted
# infrastructure rather than only by IP, and nothing had ever exercised it from
# this build. It is also the loop most exposed to the platform: it is *built* on
# a UDP read timing out, which is what tells it to resend, and IRIX has no
# SO_RCVTIMEO.
#
# Points at a stand-in on the build host that logs every datagram and answers,
# so this checks the loop rather than anyone's real infrastructure.
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
DISPLAY=:0; export DISPLAY
cd /tmp
for p in `ps -e | grep rustdesk | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
sleep 2
echo "--- pointing it at the stand-in ---"
./r-deskvint-irix --server 192.168.0.1 2>&1 | tail -3
echo "--- what got persisted ---"
grep -E "^(id|id_server|relay_server|key|api_server)" /etc/r-deskvint-irix.conf
echo "--- running for 25 s ---"
nohup ./r-deskvint-irix -v --listen 0.0.0.0 --port 21118 > /tmp/agent.log 2>&1 &
sleep 25
grep -i rendezvous /tmp/agent.log | head -6
for p in `ps -e | grep rustdesk | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
