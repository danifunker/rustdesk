#!/bin/sh
# Verify the coordinate scaling against a live agent.
#
# The peer asks for a pointer move at (100, 100) in the served space, the agent
# multiplies by the downscale before injecting, reads the pointer back at
# native resolution, and divides again before reporting. If the number that
# comes back is (100, 100), both halves agree; if it is (50, 50) or (200, 200),
# exactly one of them is missing.
DISPLAY=:0; export DISPLAY
LD_LIBRARYN32_PATH=/usr/sgug/lib32; export LD_LIBRARYN32_PATH
cd /tmp
./testpeer 127.0.0.1:21118 hunter2 12 move 100 100 2>&1 | grep -E "display|pointer|sent a"
echo "--- and where the pointer actually is, natively ---"
./rustdesk-agent --probe-live 2>&1 | grep -i "cursor before" | head -2
