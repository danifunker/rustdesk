#!/usr/bin/env bash
#
# Build everything, end to end, from one command.
#
#   ./build-release.sh --host admin@192.168.99.116
#   ./build-release.sh -H ppctiger -i ~/.ssh/id_rsa
#   ./build-release.sh -H admin@10.0.0.5 --arch universal --version 0.2.0
#
# Produces, in target/release/<version>/ : a .dmg and a .tar.gz for each CPU
# variant, SHA256SUMS, and a MANIFEST.txt saying what was verified and what was
# not. Upload them by hand -- there is deliberately no CI job, because every
# build needs a real PowerPC Mac to compile the C on and no hosted runner has
# one.
#
# WHAT IT ACTUALLY DOES, since "one command" hides a lot:
#
#   host tests -> build G5 -> build G4 -> verify each against the CPU it claims
#   -> fuse the two into a universal binary -> for each: relocate the libraries,
#   compile the settings app, assemble the .app, tar it, build a disk image
#   -> checksums and manifest
#
# It calls the focused scripts in deploy/ rather than inlining them; each is
# independently runnable, and each is where its own reasoning is written down.
#
# THE MAC IS NOT OPTIONAL. mrustc emits C, and that C is compiled on the target
# by the ppc-cc-remote.py wrapper; `lipo`, `install_name_tool`, `hdiutil` and
# the Finder are all Darwin's. Everything below that talks about the remote is
# checked before any of the 15-minute work starts, because finding out that
# `lipo` is missing after two builds is a waste of half an hour.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

HOSTSPEC=""
IDENTITY=""
VERSION=""
ARCHES=""
SKIP_TESTS=0
ALLOW_DIRTY=""

usage() {
    sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; $d'
    cat <<'EOF'
OPTIONS
  -H, --host <ssh>     the PowerPC Mac: user@host, host, an IP, or an ssh
                       config alias. Required.
  -i, --identity <key> load this ssh key into a temporary agent first
      --arch <list>    which to build, quoted: "g5 g4 universal" (the default),
                       or e.g. "universal"
      --version <v>    override the version from Cargo.toml
      --skip-tests     skip the host test suite (it is 30 seconds; don't)
      --allow-dirty    release from a tree with uncommitted changes
  -h, --help           this
EOF
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        -H|--host)     [ $# -ge 2 ] || usage; HOSTSPEC="$2"; shift 2 ;;
        -i|--identity) [ $# -ge 2 ] || usage; IDENTITY="$2"; shift 2 ;;
        --arch)        [ $# -ge 2 ] || usage; ARCHES="$2"; shift 2 ;;
        --version)     [ $# -ge 2 ] || usage; VERSION="$2"; shift 2 ;;
        --skip-tests)  SKIP_TESTS=1; shift ;;
        --allow-dirty) ALLOW_DIRTY="--allow-dirty"; shift ;;
        -h|--help)     usage ;;
        *) echo "unknown option: $1" >&2; usage ;;
    esac
done

[ -n "$HOSTSPEC" ] || { echo "error: --host is required (the PowerPC Mac to build on)" >&2; usage; }

say()  { printf '  %s\n' "$*"; }
step() { printf '\n=== %s\n' "$*"; }
die()  { echo "error: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# ssh
#
# Everything downstream shells out to plain `ssh "$PPC_HOST"`, including the C
# compiler wrapper, so the connection has to work without a password prompt.
# An agent is how that is arranged here; --identity starts a throwaway one.
# ---------------------------------------------------------------------------
step "connecting to $HOSTSPEC"

if [ -n "$IDENTITY" ]; then
    [ -f "$IDENTITY" ] || die "no such key: $IDENTITY"
    AGENT_ENV="$(mktemp)"
    ssh-agent -s > "$AGENT_ENV"
    # shellcheck disable=SC1090
    . "$AGENT_ENV" >/dev/null
    trap 'ssh-agent -k >/dev/null 2>&1 || true; rm -f "$AGENT_ENV"' EXIT
    ssh-add "$IDENTITY" || die "could not add $IDENTITY to the agent"
    say "using a temporary agent holding $(basename "$IDENTITY")"
else
    export SSH_AUTH_SOCK="${SSH_AUTH_SOCK:-/tmp/ssh-agent-ppc.sock}"
    say "using SSH_AUTH_SOCK=$SSH_AUTH_SOCK"
fi

export PPC_HOST="$HOSTSPEC"

ssh -o BatchMode=yes -o ConnectTimeout=10 "$PPC_HOST" true 2>/dev/null || die \
"cannot reach $PPC_HOST without a prompt.

       Either add the key to an agent:
           ssh-agent -a /tmp/ssh-agent-ppc.sock >/dev/null
           SSH_AUTH_SOCK=/tmp/ssh-agent-ppc.sock ssh-add ~/.ssh/id_rsa
       or pass it here:
           $0 --host $PPC_HOST --identity ~/.ssh/id_rsa"
say "ssh works"

# ---------------------------------------------------------------------------
# Preflight: the Mac
# ---------------------------------------------------------------------------
step "checking the Mac"

REMOTE_INFO="$(ssh "$PPC_HOST" '
    echo "uname=$(uname -s)"
    echo "cpu=$(machine 2>/dev/null || echo unknown)"
    echo "osver=$(sw_vers -productVersion 2>/dev/null || echo unknown)"
    for t in gcc otool install_name_tool lipo hdiutil plutil defaults osascript screen; do
        command -v "$t" >/dev/null 2>&1 && echo "have=$t" || echo "missing=$t"
    done
    [ -d /System/Library/Frameworks/Cocoa.framework ] && echo "have=Cocoa" || echo "missing=Cocoa"
    echo "tmpfree=$(df -k /tmp | awk "NR==2 {print int(\$4/1024)}")"
' 2>/dev/null)" || die "could not query $PPC_HOST"

remote_val() { echo "$REMOTE_INFO" | sed -n "s/^$1=//p" | head -1; }

[ "$(remote_val uname)" = "Darwin" ] || die "$PPC_HOST is not a Mac (uname says $(remote_val uname))"
case "$(remote_val cpu)" in
    ppc*) ;;
    *) die "$PPC_HOST is not a PowerPC Mac (machine says $(remote_val cpu))" ;;
esac
say "$(remote_val cpu), Mac OS X $(remote_val osver)"

MISSING="$(echo "$REMOTE_INFO" | sed -n 's/^missing=//p' | tr '\n' ' ')"
[ -z "$MISSING" ] || die "$PPC_HOST is missing: $MISSING
       gcc and Cocoa come with Xcode; lipo, otool and install_name_tool are
       cctools; hdiutil and osascript ship with the OS."
say "gcc, cctools, hdiutil, Cocoa: all present"

FREE="$(remote_val tmpfree)"
[ "${FREE:-0}" -ge 400 ] || die "only ${FREE}MB free in /tmp on $PPC_HOST; the fat binary alone is ~40MB and the build stages several copies"
say "${FREE}MB free in /tmp"

# The static libraries the agent links live on the Mac and are not built here.
LIBS_DIR="${PPC_LIBS_DIR:-/Users/admin/ppc-libs/lib}"
for lib in libsodium.a libvpx.a libyuv.a; do
    ssh "$PPC_HOST" "test -f '$LIBS_DIR/$lib'" 2>/dev/null \
        || die "no $LIBS_DIR/$lib on $PPC_HOST.
       These are built once, by hand, on the Mac. Set PPC_LIBS_DIR if they
       live somewhere else."
done
say "static libraries in $LIBS_DIR"

# ---------------------------------------------------------------------------
# Preflight: this machine
# ---------------------------------------------------------------------------
step "checking the toolchain here"

MRUSTC_DIR="${MRUSTC_DIR:-$HOME/repos/mrustc}"
PPC_TOOLS_DIR="${PPC_TOOLS_DIR:-$HOME/repos/rusty-backup}"
export MRUSTC_DIR PPC_TOOLS_DIR PPC_LIBS_DIR="$LIBS_DIR"

[ -x "$MRUSTC_DIR/bin/minicargo" ] || die "no minicargo at $MRUSTC_DIR/bin/minicargo (set MRUSTC_DIR)"
for w in ppc-cc-remote.py ppc-ar-remote.py; do
    [ -x "$PPC_TOOLS_DIR/scripts/$w" ] || die "no $w in $PPC_TOOLS_DIR/scripts (set PPC_TOOLS_DIR)"
done
say "mrustc at $MRUSTC_DIR"

# One standard library per CPU, and they are not interchangeable: a G5 libcore
# is full of 64-bit instructions that trap on a G4. Check the ones this run
# needs before spending fifteen minutes finding out.
# `universal` is a fuse of the g4 and g5 builds, so it needs both standard
# libraries even though it is one artifact.
WANT=""
for a in ${ARCHES:-universal}; do
    case "$a" in
        universal) WANT="$WANT g4 g5" ;;
        *)         WANT="$WANT $a" ;;
    esac
done
for a in $WANT; do
    case "$a" in
        g5|g4|g3)
            d="$MRUSTC_DIR/output-1.74.0-powerpc-apple-darwin-$a"
            [ -d "$d" ] || die "no $a standard library at $d
       mrustc builds one per CPU; see docs/BACKLOG.md item 13." ;;
    esac
done
say "standard libraries present for: $WANT"

command -v cargo >/dev/null || die "cargo is not on PATH (the host tests need it)"

# Said here as well as in release.sh, because here it is still cheap to stop.
if [ -n "$(git status --porcelain -- . 2>/dev/null)" ]; then
    say ""
    say "WARNING: this tree has uncommitted changes."
    say "         The build will be stamped +dirty and will say so in the app's"
    say "         About window, and it will not be reproducible from its commit."
    git status --short -- . 2>/dev/null | sed 's/^/           /'
    say ""
else
    say "tree is clean at $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
fi

# The committed artwork. Regenerating it needs Pillow; using it does not.
for art in deploy/app.icns deploy/dmg-background.png; do
    [ -f "$art" ] || die "missing $art -- run ./deploy/make-icns.py or ./deploy/make-dmg-bg.py"
done
say "icon and disk-image artwork present"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
step "building"
say "this takes roughly 15 minutes per CPU that is not already built,"
say "most of it gcc on the Mac. Nothing below is cached across CPUs."

ARGS=""
[ -n "$ARCHES" ]     && ARGS="$ARGS --arch $ARCHES"
[ -n "$VERSION" ]    && ARGS="$ARGS --version $VERSION"
[ -n "$ALLOW_DIRTY" ] && ARGS="$ARGS $ALLOW_DIRTY"
[ "$SKIP_TESTS" -eq 1 ] && export RELEASE_SKIP_TESTS=1

# shellcheck disable=SC2086
./deploy/release.sh $ARGS

step "done"
V="${VERSION:-$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)}"
say "artifacts are in target/release/$V"
