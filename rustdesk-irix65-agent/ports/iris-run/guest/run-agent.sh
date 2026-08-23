#!/bin/sh
# Restart the agent against the current build and leave it listening.
#
# Binds 0.0.0.0, not 127.0.0.1. iris NATs a connection from the host in, so it
# arrives at the guest from 192.168.0.1 rather than from loopback -- binding
# loopback only serves a peer running on the guest itself, which is every test
# in this repo and none of the ways a person would actually use it. With
# iris.toml's 21118 forward in place, a real RustDesk client on the host
# connects to 127.0.0.1:21118.
for p in `ps -e | grep rustdesk | awk '{print $1}'`; do kill -9 $p 2>/dev/null; done
sleep 2
cd /tmp
DISPLAY=:0 LD_LIBRARYN32_PATH=/usr/sgug/lib32 nohup /tmp/rustdesk-agent -v \
    --listen 0.0.0.0 --port 21118 > /tmp/agent.log 2>&1 &
sleep 10
grep -E "agent listening|capture path|password" /tmp/agent.log | tail -3
