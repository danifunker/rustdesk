#!/usr/bin/env bash
#
# Build, package and verify release artifacts. Run on the host; needs the G5.
#
#   ./deploy/release.sh                     # both CPUs, version from Cargo.toml
#   ./deploy/release.sh --arch g4           # just one
#   ./deploy/release.sh --version 0.2.0
#   ./deploy/release.sh --skip-build        # re-package what is already built
#
# Output lands in target/release/<version>/ : one tarball per CPU, SHA256SUMS,
# and MANIFEST.txt recording exactly what went in. Upload them by hand -- there
# is deliberately no GitHub job, because every build needs a real PowerPC Mac to
# compile the C on and no hosted runner has one.
#
# WHAT IT VERIFIES, and why each check exists rather than being assumed:
#
#   * the Mach-O cpusubtype matches the CPU asked for -- that is what decides
#     whether the target will load the binary at all;
#   * a G4 build contains NO 64-bit instructions. This is the check that
#     matters. `-mcpu=970` implies `-mpowerpc64`, and both the agent and
#     mrustc's standard library carry the flags they were built with, so a G4
#     release that picked up a G5 libcore would still be stamped 7450 and would
#     still trap on the target. Nothing else catches that;
#   * every bundled library resolves under @executable_path, checked by
#     bundle.sh, which also runs the binary before packing it.
#
# A failed check fails the release. That is the point of it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${PPC_HOST:-ppctiger}"
export SSH_AUTH_SOCK="${SSH_AUTH_SOCK:-/tmp/ssh-agent-ppc.sock}"

ARCHES="g5 g4"
VERSION=""
SKIP_BUILD=0
ALLOW_DIRTY=0

usage() {
    sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; $d'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)      [ $# -ge 2 ] || usage; ARCHES="$2"; shift 2 ;;
        --version)   [ $# -ge 2 ] || usage; VERSION="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        --help|-h)   usage ;;
        *) echo "unknown option: $1" >&2; usage ;;
    esac
done

# CPU flags per arch, and the cpusubtype each must produce. 100 is
# CPU_SUBTYPE_POWERPC_970, 10 is 7450. Measured, not guessed -- see
# deploy/check-cpu-compat.sh.
flags_for() {
    case "$1" in
        g5) echo "-mcpu=970 -maltivec" ;;
        g4) echo "-mcpu=7450 -maltivec" ;;
        g3) echo "-mcpu=750" ;;
        *)  echo "" ;;
    esac
}
subtype_for() {
    case "$1" in
        g5) echo 100 ;;
        g4) echo 10 ;;
        g3) echo 10 ;;
        *)  echo "" ;;
    esac
}

if [ -z "$VERSION" ]; then
    VERSION="$(sed -n 's/^version = "\(.*\)"/\1/p' "$HERE/Cargo.toml" | head -1)"
fi
[ -n "$VERSION" ] || { echo "error: no version; pass --version" >&2; exit 1; }

cd "$HERE"
GIT_REF="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if [ -n "$(git status --porcelain -- . 2>/dev/null)" ]; then
    if [ "$ALLOW_DIRTY" -eq 0 ]; then
        echo "error: the working tree has uncommitted changes." >&2
        echo "       A release names a commit, and this one would not be reproducible." >&2
        echo "       Commit them, or pass --allow-dirty to accept a 'git ref +dirty' label." >&2
        exit 1
    fi
    GIT_REF="$GIT_REF+dirty"
fi

REL="$HERE/target/release/$VERSION"
mkdir -p "$REL"

echo "releasing $VERSION from $GIT_REF, for: $ARCHES"
echo

# Host-side tests first. They cover the protocol decisions, and they are free
# compared with a 15-minute PowerPC build -- failing here costs nothing.
echo "--- host tests"
cargo test --quiet 2>&1 | tail -3
echo

for arch in $ARCHES; do
    CPU="$(flags_for "$arch")"
    WANT_SUBTYPE="$(subtype_for "$arch")"
    [ -n "$CPU" ] || { echo "error: unknown arch '$arch'" >&2; exit 1; }

    echo "=============================================================="
    echo "  $arch   ($CPU)"
    echo "=============================================================="

    OUT="$HERE/target/ppc-$arch"
    BIN="$OUT/rustdesk-agent"

    if [ "$SKIP_BUILD" -eq 0 ]; then
        # A separate output directory per CPU is not tidiness: minicargo decides
        # staleness by timestamp, so one directory shared between two CPUs links
        # whichever objects happen to be newer. build-ppc.sh stamps and refuses
        # a mismatch, but only the caller can give it somewhere else to go.
        PPC_CPU_FLAGS="$CPU" PPC_OUT="$OUT" ./build-ppc.sh
    fi
    [ -f "$BIN" ] || { echo "error: nothing built at $BIN" >&2; exit 1; }

    # Big-endian header read a byte at a time; the host is little-endian, so
    # `od -tu4` would report 100 as 1677721600 -- a wrong answer, not an error.
    GOT_SUBTYPE="$(od -An -tu1 -j8 -N4 "$BIN" \
                   | awk '{print $1 * 16777216 + $2 * 65536 + $3 * 256 + $4}')"
    if [ "$GOT_SUBTYPE" != "$WANT_SUBTYPE" ]; then
        echo "error: $arch built cpusubtype $GOT_SUBTYPE, expected $WANT_SUBTYPE" >&2
        exit 1
    fi
    echo "  cpusubtype $GOT_SUBTYPE as expected"

    # The check that would have caught a G5 standard library in a G4 build.
    echo "  scanning for instructions the target cannot run ..."
    scp -q "$BIN" "$HOST:/tmp/relscan-$arch"
    SCAN="$(ssh "$HOST" "sh -s /tmp/relscan-$arch" < "$HERE/deploy/check-cpu-compat.sh" 2>&1 \
            | sed -n '/=== requested files ===/,$p')"
    ssh "$HOST" "rm -f /tmp/relscan-$arch"
    echo "$SCAN" | sed 's/^/    /'

    if [ "$arch" != "g5" ]; then
        SIXTYFOUR="$(echo "$SCAN" | awk '
            $2=="x" && ($3=="std"||$3=="ld"||$3=="rldicl"||$3=="rldicr"||$3=="rldimi"||
                        $3=="mulld"||$3=="divd"||$3=="divdu"||$3=="cntlzd"||$3=="sld"||
                        $3=="srd"||$3=="srad"||$3=="sradi"||$3=="extsw"||$3=="fcfid"||
                        $3=="fctidz"||$3=="ldx"||$3=="stdx"||$3=="fsqrt") { n += $1 }
            END { print n+0 }')"
        if [ "$SIXTYFOUR" -gt 0 ]; then
            echo "error: the $arch build contains $SIXTYFOUR 64-bit instructions." >&2
            echo "       It would trap on the target. Most likely the standard" >&2
            echo "       library did not match: check PPC_STDLIB in build-ppc.sh." >&2
            exit 1
        fi
        echo "    no 64-bit instructions: this will run on a $arch"
    fi

    echo "  bundling ..."
    PPC_BIN="$BIN" PPC_BUNDLE_OUT="$HERE/target" ./deploy/bundle.sh | sed 's/^/    /'

    SRC_TAR="$HERE/target/rustdesk-agent-$arch.tar.gz"
    [ -f "$SRC_TAR" ] || { echo "error: bundle.sh produced no $SRC_TAR" >&2; exit 1; }
    mv "$SRC_TAR" "$REL/rustdesk-agent-$VERSION-$arch.tar.gz"
    echo "  -> $(basename "$REL/rustdesk-agent-$VERSION-$arch.tar.gz")"
    echo
done

# ---------------------------------------------------------------------------
# Manifest and checksums
# ---------------------------------------------------------------------------
cd "$REL"
sha256sum ./*.tar.gz > SHA256SUMS

{
    echo "rustdesk-ppc-agent $VERSION"
    echo "commit:   $GIT_REF"
    echo "built:    $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "builder:  $(uname -srm), C compiled on $HOST"
    echo
    echo "Artifacts"
    echo "---------"
    for f in *.tar.gz; do
        arch="${f##*-}"; arch="${arch%.tar.gz}"
        echo "  $f"
        echo "      for:        $(case $arch in g5) echo 'PowerPC G5 (970) only';;
                                              g4) echo 'PowerPC G4 (7450); also runs on a G5';;
                                              g3) echo 'PowerPC G3 (750); untested';; esac)"
        echo "      cpu flags:  $(flags_for "$arch")"
        echo "      cpusubtype: $(subtype_for "$arch")"
        echo "      size:       $(du -h "$f" | cut -f1)"
        echo "      sha256:     $(sha256sum "$f" | cut -d' ' -f1)"
    done
    echo
    echo "Install"
    echo "-------"
    echo "  tar xzf <file>.tar.gz && cd rustdesk-agent-<arch> && ./install.sh"
    echo
    echo "  Everything lands under \$HOME; no sudo. The agent starts at every"
    echo "  login and restarts if it dies. For a machine that must be reachable"
    echo "  after a reboot with nobody at it, turn on automatic login."
    echo
    echo "Verified for this build"
    echo "-----------------------"
    echo "  * host test suite"
    echo "  * Mach-O cpusubtype matches the CPU each artifact targets"
    echo "  * no 64-bit instructions in any non-G5 artifact (they would trap)"
    echo "  * every bundled library resolves under @executable_path, and the"
    echo "    packed binary was run on a real PowerPC Mac before packing"
    echo
    echo "Not verified"
    echo "------------"
    echo "  * no G4 or G3 hardware was available: the G4 artifact is verified by"
    echo "    instruction scan and has never been run on a G4."
    echo "  * lwsync appears in libvpx, libatomic and libgcc_s. It should"
    echo "    execute as a full sync on a 7450 by the reserved-bit rule, which"
    echo "    is correct if slower, but that is the architecture's promise"
    echo "    rather than a measurement. See deploy/check-cpu-compat.sh."
} > MANIFEST.txt

echo "=============================================================="
cat MANIFEST.txt
echo "=============================================================="
echo
echo "Artifacts in $REL"
ls -la "$REL"
