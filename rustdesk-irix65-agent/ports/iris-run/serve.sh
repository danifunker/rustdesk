#!/bin/sh
# Serve the current build and the guest-side helper scripts on port 8099, which
# is where the guest's wget fetches them from (192.168.0.1 is the host as the
# NAT gateway sees it).
#
# The scripts live in git beside this file and are copied into the build
# directory rather than living there: target/ is an artifact and gets deleted,
# and losing the harness with it costs an afternoon.
set -e
here=$(cd "$(dirname "$0")" && pwd)
rel="$here/../rust/agent-portable/target/mips-sgi-irix6.5/release"
cp "$here"/guest/*.sh "$rel"/
cd "$rel"
echo "serving $rel on :8099"
exec python3 -m http.server 8099
