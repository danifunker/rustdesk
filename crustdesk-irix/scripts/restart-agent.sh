#!/bin/sh
# Restart the agent on the IRIX machine from the linked build directory,
# with a test prefs file and a verbose log. For development.
#   scripts/restart-agent.sh [extra agent args]
cd "$(dirname "$0")/.." || exit 1
DIR=${INDIGO_DIR:-/usr/tmp/cdv-build}
exec python3 scripts/indigo.py spawn 10 "/etc/killall C-Desk-Vint 2>/dev/null; sleep 1; rm -f /usr/tmp/cdv.log; cd $DIR && DISPLAY=:0 && export DISPLAY && (./C-Desk-Vint -v -c /usr/tmp/cdv-prefs -l /usr/tmp/cdv.log $* < /dev/null > /usr/tmp/cdv.out 2>&1 &) ; sleep 6; cat /usr/tmp/cdv.out"
