#!/usr/bin/env bash
# Build and run the video-performance probes on the G5.
#
# These are C rather than Rust on purpose: they establish the *hardware* floor
# without mrustc's codegen in the way, so the agent's own numbers can be
# compared against a hand-written -O2 baseline. See ../docs/videoperformance.md.
#
#   PPC_HOST=ppctiger probes/run.sh [probe ...]
#
# Needs SSH_AUTH_SOCK pointing at an agent holding the key (the key is
# passphrase-protected):
#   ssh-agent -a /tmp/ssh-agent-ppc.sock >/dev/null 2>&1
#   SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock ssh-add ~/.ssh/id_rsa
set -euo pipefail

HOST="${PPC_HOST:-ppctiger}"
GCC="${PPC_CC:-/opt/local/libexec/gcc10-bootstrap/bin/gcc}"
CPU="${PPC_CPU_FLAGS:--mcpu=970}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

probes=("$@")
if [ ${#probes[@]} -eq 0 ]; then
  probes=(display-info vram-vs-ram read-scaling)
fi

ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ppc-probes' >/dev/null

for p in "${probes[@]}"; do
  printf '\n\033[1;36m==== %s ====\033[0m\n' "$p"
  scp -q "$HERE/$p.c" "$HOST:~/ppc-probes/"
  ssh -o BatchMode=yes "$HOST" \
    "$GCC -O2 $CPU ~/ppc-probes/$p.c -o ~/ppc-probes/$p -framework ApplicationServices && ~/ppc-probes/$p"
done
