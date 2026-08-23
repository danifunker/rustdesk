#!/bin/sh
# Restart the agent against the current build and leave it listening.
for p in `ps -e | grep rustdesk | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
sleep 2
cd /tmp
DISPLAY=:0 LD_LIBRARYN32_PATH=/usr/sgug/lib32 nohup /tmp/rustdesk-agent -v \
    --listen 127.0.0.1 --port 21118 > /tmp/agent.log 2>&1 &
sleep 10
tail -3 /tmp/agent.log
