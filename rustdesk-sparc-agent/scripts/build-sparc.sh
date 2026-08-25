#!/usr/bin/env bash
#
# build-sparc.sh -- build the agent for sparcv9-sun-solaris.
#
# Rust goes through mrustc to C here, and the C is compiled on the Blade over
# ssh by sparc-cc-remote.py, so a build needs the machine reachable. What it
# does *not* need is a cross toolchain, which is why this is the path of least
# resistance rather than a stopgap.
#
#   ./scripts/build-sparc.sh                 # build
#   ./scripts/build-sparc.sh run captest --frames 5   # build, copy over, run
#
# Prerequisites: the stdlib for this target, built once in the mrustc tree --
#   make -f minicargo.mk LIBS RUSTC_VERSION=1.74.0 \
#        MRUSTC_TARGET=sparcv9-sun-solaris OVERRIDE_SUFFIX=-solaris \
#        STD_ENV_ARCH=sparc64 PARLEVEL=2
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
MRUSTC_DIR="${MRUSTC_DIR:-$HOME/repos/mrustc}"
RUSTC_VERSION="${RUSTC_VERSION:-1.74.0}"
TARGET=sparcv9-sun-solaris
STDLIB="${STDLIB:-$MRUSTC_DIR/output-$RUSTC_VERSION-$TARGET}"
# The portable modules and the vendored crates both come from the PowerPC tree.
PPC_DIR="${PPC_DIR:-$HERE/../rustdesk-ppc-agent}"
OUT="${OUT:-$HERE/target/$TARGET}"
# Built from source on the Blade by scripts/build-deps.sh, and named here by
# the path they have *there*.
REMOTE_DEPS="${REMOTE_DEPS:-$HOME/sparc-deps}"

export SPARC_HOST="${SPARC_HOST:?set SPARC_HOST, e.g. dani@192.168.99.176}"
# gcc 4.9 compiles all of this and then fails to link: __builtin_add_overflow
# and its siblings arrived in gcc 5, and mrustc emits them for Rust's checked
# arithmetic.
export SPARC_CC="${SPARC_CC:-/opt/csw/bin/gcc-5.5}"
export MRUSTC_TARGET_VER="${MRUSTC_TARGET_VER:-1.74}"
export CC_sparcv9_sun_solaris="$HERE/scripts/sparc-cc-remote.py"
# Solaris' default 64-bit search path has neither X prefix on it, so the
# runpaths have to be linked in or the binary will not start.
# -lssp, statically: libsodium is built with the stack protector, whose runtime
# (__stack_chk_fail, __stack_chk_guard) is in gcc's libssp here rather than in
# libc as it would be on Solaris 11 or Linux. Static so the agent does not
# depend on OpenCSW being installed wherever it ends up running.
export SPARC_LDFLAGS="${SPARC_LDFLAGS:--lsocket -lnsl -lrt -lpthread -R/usr/openwin/lib/sparcv9 -R/usr/openwin/sfw/lib/sparcv9 -L/opt/csw/lib/sparcv9 -Wl,-Bstatic -lssp -Wl,-Bdynamic}"
# Where the C libraries built on the Blade live. These are *remote* paths: they
# do not exist on this machine, and the wrapper passes a directory it cannot
# find through untouched for exactly that reason.
export SODIUM_LIB_DIR="${SODIUM_LIB_DIR:-$REMOTE_DEPS/lib}"

[ -d "$STDLIB" ] || { echo "no stdlib at $STDLIB -- see the header of this script" >&2; exit 1; }
[ -d "$PPC_DIR/vendor" ] || { echo "no vendor tree at $PPC_DIR/vendor" >&2; exit 1; }

# minicargo caches a build script's output and does not honour
# cargo:rerun-if-changed, so an edited .c file otherwise links against a stale
# archive and the change simply does not appear -- silently, with a successful
# build. Drop the cache when a shim is newer than the recorded run. Removing
# only the directory is not enough: minicargo decides whether to re-run the
# script from the .txt beside it, so that has to go too, and with it anything
# built from the old archive.
BUILD_MARK="$OUT/host/build_rustdesk-sparc-agent-0_1_0"
for shim in "$HERE"/src/*.c "$HERE"/src/*.h; do
    [ -e "$shim" ] || continue
    if [ ! -d "$BUILD_MARK" ] || [ -n "$(find "$shim" -newer "$BUILD_MARK" 2>/dev/null)" ]; then
        echo "shim changed ($(basename "$shim")) -- rebuilding the C shims"
        rm -rf "$BUILD_MARK" "$BUILD_MARK.txt" \
               "$OUT"/librustdesk_sparc_agent-*.rlib* "$OUT"/captest
        break
    fi
done

mkdir -p "$OUT"
"$MRUSTC_DIR/bin/minicargo" "$HERE" \
    --vendor-dir "$PPC_DIR/vendor" \
    --target "$TARGET" \
    -L "$STDLIB" \
    --output-dir "$OUT" \
    "${MINICARGO_FLAGS:-}"

echo "built: $OUT"
ls -la "$OUT"/captest 2>/dev/null || true

[ "${1:-}" = "run" ] || exit 0
shift
BIN="${1:-captest}"; shift || true
scp -q "$OUT/$BIN" "$SPARC_HOST:/tmp/$BIN"
# shellcheck disable=SC2029  # the argument list is meant to expand here
ssh "$SPARC_HOST" "/tmp/$BIN $*"
