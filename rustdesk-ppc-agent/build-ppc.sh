#!/usr/bin/env bash
# Cross-build the agent for 32-bit PowerPC Mac OS X, and optionally deploy it.
#
#   ./build-ppc.sh            build only
#   ./build-ppc.sh deploy     build, then install and restart on the G5
#
# Compilation is mrustc + minicargo; the C bits are compiled *on the G5* by the
# ppc-cc-remote.py / ppc-ar-remote.py wrappers, so the machine must be reachable
# and SSH_AUTH_SOCK must point at an agent holding the (passphrase-protected) key:
#
#   ssh-agent -a /tmp/ssh-agent-ppc.sock >/dev/null 2>&1
#   SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock ssh-add ~/.ssh/id_rsa
#
# OUT is kept outside /tmp on purpose: minicargo reuses what is already built
# there, which is the difference between ~90 seconds and ~14 minutes.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${PPC_OUT:-$HERE/target/ppc}"
HOST="${PPC_HOST:-ppctiger}"

export SSH_AUTH_SOCK="${SSH_AUTH_SOCK:-/tmp/ssh-agent-ppc.sock}"
export PPC_HOST="$HOST"   # the remote cc/ar wrappers read this, not $HOST
export PPC_CPU_FLAGS="${PPC_CPU_FLAGS:--mcpu=970 -maltivec}"

# The Rust standard library has to match the CPU the agent is built for, and
# this used to be a hardcoded `-g5` path.
#
# mrustc compiles libcore/liballoc to C and then to Mach-O objects, so the
# standard library carries the CPU flags it was built with. Measured: the g5
# liballoc has 863 `rldicl`, 698 `std` and 685 `ld` in it, and the g4 one has
# none. Linking the g5 copy into a G4 binary therefore drags in 64-bit
# instructions that trap on the target, and nothing in the build would say so --
# the cpusubtype would still read 7450, because that is set from the flags.
#
# Derived from PPC_CPU_FLAGS rather than fixed. Override with PPC_STDLIB.
case "$PPC_CPU_FLAGS" in
    *970*)                          STD_TAG=g5 ;;
    *7450*|*7455*|*7447*|*7400*|*G4*|*g4*) STD_TAG=g4 ;;
    *750*|*G3*|*g3*)                STD_TAG=g3 ;;
    *) STD_TAG="" ;;
esac
if [ -n "${PPC_STDLIB:-}" ]; then
    STDLIB="$PPC_STDLIB"
elif [ -n "$STD_TAG" ]; then
    STDLIB="$HOME/repos/mrustc/output-1.74.0-powerpc-apple-darwin-$STD_TAG"
else
    echo "error: cannot tell which standard library suits PPC_CPU_FLAGS='$PPC_CPU_FLAGS'." >&2
    echo "       Set PPC_STDLIB to the matching mrustc output directory." >&2
    exit 1
fi
[ -d "$STDLIB" ] || { echo "error: no standard library at $STDLIB" >&2; exit 1; }
export PPC_JOBS="${PPC_JOBS:-2}"
export PPC_SHIM="${PPC_SHIM:-$HOME/repos/rusty-backup/rb-cli-ppc/shim/ppc-compat.c}"
export PPC_LDFLAGS="${PPC_LDFLAGS:--L/opt/local/lib -L/Users/admin/ppc-libs/lib -latomic -lMacportsLegacySupport -lgcc_s.1 -lsodium -lvpx}"

mkdir -p "$OUT"

# One output directory holds objects for one CPU. minicargo's staleness check is
# "is the .o newer than the .c", which a flag change does not disturb, so a
# switch from G5 to G4 in the same directory silently links yesterday's G5
# objects. (The remote wrapper already namespaces the *shim* object by CPU flags
# for exactly this reason -- see `cpu_suffix` in ppc-cc-remote.py.) Refuse
# rather than produce a binary that is half one architecture.
STAMP="$OUT/.cpu-flags"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" != "$PPC_CPU_FLAGS $STDLIB" ]; then
    echo "error: $OUT was built with different settings:" >&2
    echo "         had:  $(cat "$STAMP")" >&2
    echo "         want: $PPC_CPU_FLAGS $STDLIB" >&2
    echo "       Use a separate PPC_OUT per CPU, or remove $OUT first." >&2
    exit 1
fi
printf '%s %s' "$PPC_CPU_FLAGS" "$STDLIB" > "$STAMP"

# minicargo caches the build script's output and does not honour
# cargo:rerun-if-changed, so an edited .c file links against a stale archive and
# the symbol simply comes up undefined. Drop the cache when a shim is newer.
for shim in "$HERE"/src/*.c; do
    if [ -n "$(find "$shim" -newer "$OUT/host/build_rustdesk-ppc-agent-0_1_0" 2>/dev/null)" ] \
       || [ ! -d "$OUT/host/build_rustdesk-ppc-agent-0_1_0" ]; then
        echo "shim changed ($(basename "$shim")) -- rebuilding the C shims"
        # The archives live in the build script's OUT_DIR, but minicargo decides
        # whether to re-run the script from the recorded-output .txt beside it.
        # Removing only the directory means the script is skipped and the link
        # then fails on -lvpxshim, so the marker has to go as well.
        rm -rf "$OUT/host/build_rustdesk-ppc-agent-0_1_0" \
               "$OUT/host/build_rustdesk-ppc-agent-0_1_0.txt" \
               "$OUT/librustdesk_ppc_agent-0_1_0.rlib" \
               "$OUT/rustdesk-agent"
        break
    fi
done

SODIUM_LIB_DIR=/Users/admin/ppc-libs/lib \
CC_powerpc_apple_darwin="$HOME/repos/rusty-backup/scripts/ppc-cc-remote.py" \
AR_powerpc_apple_darwin="$HOME/repos/rusty-backup/scripts/ppc-ar-remote.py" \
MRUSTC_TARGET_VER=1.74 \
  "$HOME/repos/mrustc/bin/minicargo" "$HERE" \
  --vendor-dir "$HERE/vendor" \
  --target powerpc-apple-darwin \
  -L "$STDLIB" \
  --output-dir "$OUT" \
  -j "$PPC_JOBS"

echo "built: $OUT/rustdesk-agent"

[ "${1:-}" = "deploy" ] || exit 0

# Kill by `comm`, never by matching the full command line: a pattern broad
# enough to match the agent also matches the ssh command running it.
ssh "$HOST" 'ps -axo pid,comm | awk "\$2 ~ /rustdesk-agent/ {print \$1}" \
  | while read p; do kill -9 $p; done; sleep 2'
scp -q "$OUT/rustdesk-agent" "$HOST:~/rustdesk-agent"
# Start it under `screen`, which is not cosmetic.
#
# A fully detached agent cannot reach the window server at all -- it fails with
# "On-demand launch of the Window Server is allowed for root user only" and then
# reports a 0x0 display and serves an input-only session. Backgrounding with
# `&` or nohup lands exactly there (and Darwin's nohup additionally dies with
# "can't migrate to background session" over a non-tty ssh). A detached screen
# keeps enough of the login session alive that capture still works once this
# connection closes -- verified by streaming frames afterwards.
ssh "$HOST" 'screen -S rdagent -X quit >/dev/null 2>&1; sleep 1
  rm -f ~/agent.log
  screen -dmS rdagent bash -c "~/rustdesk-agent --port 21118 -vv > ~/agent.log 2>&1"'
sleep 2
ssh "$HOST" 'ps -axo pid,comm | grep rustdesk-agent; head -5 ~/agent.log'
echo "deployed to $HOST"
