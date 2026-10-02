#!/bin/sh
# Build, link on the IRIX machine, and restart the agent there. For development.
set -e
cd "$(dirname "$0")/.."
make
python3 -u scripts/indigo.py link
sh scripts/restart-agent.sh "$@"
