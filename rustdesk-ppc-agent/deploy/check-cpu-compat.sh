#!/bin/sh
#
# Will this build run on a G4? Scan Mach-O PowerPC binaries for instructions a
# 74xx does not have. Run it ON a PowerPC Mac -- it needs `otool`.
#
#   ssh ppctiger 'sh -' < deploy/check-cpu-compat.sh
#   ./check-cpu-compat.sh ~/rustdesk-ppc-agent/rustdesk-agent   # specific files
#
# WHY: the Mach-O `cpusubtype` is not the answer, and believing it was is what
# this script exists to prevent. The header says which CPU the *linker* stamped;
# it says nothing about the instructions inside, and every library here is
# stamped generic `ppc` while three of them contain 970-era instructions.
#
# What it looks for, all absent from 74xx:
#   * 64-bit integer ops   ld/std/rld*/mulld/divd/cntlzd/fcfid/...
#     -- the decisive group. `-mcpu=970` implies `-mpowerpc64`, so a G5 build is
#     full of these and a G4 build has none. This is what tells the two apart.
#   * fsqrt / fsqrts       the "general purpose" FP optional group
#   * popcntb, mfocrf/mtocrf, lwsync, dcbzl   970-era additions
#
# On the last group, read the result rather than just the verdict: `mtocrf` and
# `lwsync` are defined so that a pre-2.01 processor executes the base form
# (`mtcrf`, `sync`) instead, which is correct if slower. Apple's own Leopard
# libSystem.B.dylib contains 498 `mtocrf` and shipped to every G4, which settles
# that one. `lwsync` is not in any Apple library scanned here, so it rests on
# the architecture's reserved-bit rule and has not been run on real hardware.
set -u

G5ONLY="ld ldu ldx ldux lwa lwax lwaux std stdu stdx stdux sld srd srad sradi rldicl rldicr rldic rldimi rldcl rldcr cntlzd mulld mulhd mulhdu divd divdu extsw cmpd cmpld td tdi fcfid fctid fctidz fsqrt fsqrts popcntb mfocrf mtocrf lwsync dcbzl"

# The list is one line on purpose. The G5's awk cannot take a newline inside
# -v: it warns and leaves the match set EMPTY, so every file reports clean.
# That is what the first version of this did, and it is why the controls below
# are not optional -- a scanner that finds nothing looks identical to a clean
# result.
scan() {
    f="$1"
    [ -f "$f" ] || { echo "  $(basename "$f"): not found"; return 0; }
    dis=$(otool -tV "$f" 2>/dev/null \
          | awk -F'\t' 'NF>1 && $1 ~ /^[0-9a-f]+$/ {m=$2; sub(/\.$/,"",m); print m}')
    total=$(echo "$dis" | grep -c .)
    hits=$(echo "$dis" | sort | uniq -c | sort -rn \
           | awk -v list="$G5ONLY" '
               BEGIN { n=split(list, a, " "); for (i=1;i<=n;i++) want[a[i]]=1 }
               want[$2] { print "        " $1 " x " $2 }')
    if [ -n "$hits" ]; then
        echo "  $(basename "$f"): G5-only instructions ($total insns)"
        echo "$hits"
    else
        echo "  $(basename "$f"): clean ($total insns)"
    fi
}

CC=/opt/local/libexec/gcc10-bootstrap/bin/gcc
if [ -x "$CC" ]; then
    echo "=== controls: one source, two -mcpu settings ==="
    T=${TMPDIR:-/tmp}/cpucompat.$$
    mkdir -p "$T"
    cat > "$T/c.c" <<'EOF'
double sq(double x) { return __builtin_sqrt(x); }
long long mul(long long a, long long b) { return a * b; }
int main(void) { return (int)sq(2.0) + (int)mul(3, 4); }
EOF
    $CC -O2 -mcpu=970  -o "$T/c970"  "$T/c.c" 2>/dev/null && scan "$T/c970"
    $CC -O2 -mcpu=7450 -o "$T/c7450" "$T/c.c" 2>/dev/null && scan "$T/c7450"
    echo "  (970 must report hits and 7450 must be clean, or the scan is broken)"
    rm -rf "$T"
    echo
fi

if [ $# -gt 0 ]; then
    echo "=== requested files ==="
    for f in "$@"; do scan "$f"; done
    exit 0
fi

echo "=== the installed agent ==="
scan "$HOME/rustdesk-ppc-agent/rustdesk-agent"
echo
echo "=== bundled dynamic libraries ==="
for d in "$HOME"/rustdesk-ppc-agent/lib/*.dylib; do scan "$d"; done
echo
if [ -d "$HOME/ppc-libs/lib" ]; then
    echo "=== static dependencies, if this is the build machine ==="
    for a in "$HOME"/ppc-libs/lib/*.a; do scan "$a"; done
    echo
fi
echo "=== reference: Apple's own Leopard libraries, which shipped to G4s ==="
scan /usr/lib/libSystem.B.dylib
