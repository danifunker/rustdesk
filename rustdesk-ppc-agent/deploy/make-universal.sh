#!/usr/bin/env bash
#
# Fuse the G4 and G5 agents into one universal binary, so a single .app runs on
# either -- including after a disk is moved between machines.
#
#   ./deploy/make-universal.sh target/ppc-g4/rustdesk-agent \
#                              target/ppc-g5/rustdesk-agent \
#                              target/ppc-universal/rustdesk-agent
#
# WHY THIS WORKS. Mach-O's fat format keys slices on cputype AND cpusubtype, not
# just cputype, so two PowerPC builds can live in one file and the kernel grades
# them at exec time. Measured on the G5 rather than assumed: a fat binary whose
# two slices print their own name prints the G5 one, and each slice extracted
# with `lipo -thin` prints what it claims. That is the whole mechanism -- there
# is no launcher script and nothing to choose at install time.
#
# `lipo` is cctools and lives on the Mac, not here, which is why this shells out
# the same way build-ppc.sh does for the C compiler.
#
# Worth knowing: `-mcpu=7450` produces cpusubtype 10, which lipo names ppc7400,
# not ppc7450 (that is 11). 7400 is the *broader* of the two -- it runs on every
# G4 -- so this is the better outcome, and it is why the slice is labelled
# ppc7400 everywhere below.
set -euo pipefail

G4="${1:?usage: make-universal.sh <g4-binary> <g5-binary> <output>}"
G5="${2:?}"
OUT="${3:?}"
HOST="${PPC_HOST:-ppctiger}"
export SSH_AUTH_SOCK="${SSH_AUTH_SOCK:-/tmp/ssh-agent-ppc.sock}"

for f in "$G4" "$G5"; do
    [ -f "$f" ] || { echo "error: no binary at $f" >&2; exit 1; }
done

REMOTE="/tmp/rd-universal-$$"
mkdir -p "$(dirname "$OUT")"

ssh "$HOST" "mkdir -p '$REMOTE'"
scp -q "$G4" "$HOST:$REMOTE/g4"
scp -q "$G5" "$HOST:$REMOTE/g5"

ssh "$HOST" "REMOTE='$REMOTE' bash -s" <<'REMOTE_SH'
set -euo pipefail
cd "$REMOTE"

# Refuse to fuse two slices of the same subtype: lipo would reject it anyway,
# but the message here says which inputs were wrong.
s4="$(lipo -info g4 | sed 's/.*: //')"
s5="$(lipo -info g5 | sed 's/.*: //')"
if [ "$s4" = "$s5" ]; then
    echo "error: both inputs are $s4 -- one of them is the wrong build" >&2
    exit 1
fi

lipo -create g4 g5 -output fat
lipo -info fat

# Both slices must survive, and each must still be the architecture it was.
for want in ppc7400 ppc970; do
    lipo -info fat | grep -q "$want" || { echo "error: $want missing from the fat binary" >&2; exit 1; }
done

# And the fused file must still run here. This machine is a G5, so this only
# exercises the ppc970 slice -- the G4 one has never been run on a G4, which the
# release manifest says out loud.
./fat --config "$REMOTE/probe.conf" --show-id >/dev/null || {
    echo "error: the universal binary does not run" >&2; exit 1; }
rm -f "$REMOTE/probe.conf"
REMOTE_SH

scp -q "$HOST:$REMOTE/fat" "$OUT"
ssh "$HOST" "rm -rf '$REMOTE'"
chmod +x "$OUT"

echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
