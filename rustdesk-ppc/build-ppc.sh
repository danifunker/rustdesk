#!/usr/bin/env bash
#
# build-ppc.sh -- transpile the RustDesk *agent* to C via mrustc, toward a
# native PowerPC Mac OS X 10.4/10.5 `rustdesk --server`.
#
# Two-machine model (same as rusty-backup's; see docs/powerpc-mrustc-scope.md):
#
#     This machine (fast)                 PowerPC Mac (over ssh)
#     -------------------                 ----------------------
#     Rust --mrustc--> C99         --->   C99 --gcc10--> PowerPC Mach-O
#
# scripts/ppc-cc-remote.py from the rusty-backup tree *is* the C compiler as
# far as mrustc is concerned: point CC_powerpc_apple_darwin at it and every
# codegen step ships its .c over ssh, runs gcc there, copies the .o back.
#
# Without a reachable PowerPC Mac you can still exercise the whole Rust
# front-end -- set PPC_STUB_CC=1 and the C compile is replaced by a stub that
# emits an empty object. mrustc errors still surface; only codegen is fake.
#
# Usage:
#   rustdesk-ppc/build-ppc.sh <stage>
#   stages:
#     vendor   re-resolve and re-vendor (reverts the git-dep patch around it)
#     hbb      transpile libs/hbb_common only  (protocol core: tokio+protobuf+sodium)
#     agent    transpile the whole `--features cli` agent
#     clean    remove the output tree
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MRUSTC_DIR="${MRUSTC_DIR:-$HOME/repos/mrustc}"
VENDOR="$REPO/rustdesk-ppc/vendor"
PATCHES="$REPO/rustdesk-ppc/patches"

RUSTC_VERSION="${RUSTC_VERSION:-1.74.0}"
export MRUSTC_TARGET_VER="${MRUSTC_TARGET_VER:-1.74}"
PPC_TARGET="powerpc-apple-darwin"
PPC_CPU="${PPC_CPU:-g5}"                 # g3 | g4 | g5
FEATURES="${FEATURES:-cli}"

PPC_LIBS="$MRUSTC_DIR/output-${RUSTC_VERSION}-${PPC_TARGET}-${PPC_CPU}"
OUT="${OUT:-$MRUSTC_DIR/output-rustdesk-ppc-${PPC_CPU}}"
JOBS="$(nproc 2>/dev/null || echo 4)"

banner() { printf '\n\033[1;36m==== %s ====\033[0m\n' "$*"; }
note()   { printf '\033[33m%s\033[0m\n' "$*"; }
die()    { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- the C compiler for the target ------------------------------------------
# Real builds need a PowerPC Mac. PPC_STUB_CC=1 swaps in a no-op so the Rust
# front-end can be exercised on this machine alone.
setup_cc() {
  if [ "${PPC_STUB_CC:-0}" = "1" ]; then
    local stub="$REPO/rustdesk-ppc/stub-cc.sh"
    [ -x "$stub" ] || die "PPC_STUB_CC=1 but $stub is missing/not executable"
    export CC_powerpc_apple_darwin="$stub"
    export AR_powerpc_apple_darwin="$(command -v ar)"
    note "PPC_STUB_CC=1 -- C codegen is STUBBED. Objects are junk; only the"
    note "Rust front-end result is meaningful."
  else
    [ -n "${CC_powerpc_apple_darwin:-}" ] || die \
      "CC_powerpc_apple_darwin is not set. Point it at rusty-backup's
       scripts/ppc-cc-remote.py (with PPC_HOST set), or re-run with
       PPC_STUB_CC=1 to test the Rust front-end only."
  fi
}

stage_vendor() {
  banner "re-vendor dependencies"
  cd "$REPO"
  # cargo needs the real git URLs back to re-resolve.
  "$PATCHES/git-deps.py" --revert
  cargo generate-lockfile
  rm -rf "$VENDOR"
  cargo vendor "$VENDOR" > /dev/null
  "$PATCHES/git-deps.py"
  note "vendored $(ls "$VENDOR" | wc -l | tr -d ' ') crates."
}

# Re-apply the manifest patch before every transpile: it is idempotent, and
# this makes the stages safe to run in any order.
prepare() {
  [ -d "$VENDOR" ] || die "no vendor dir -- run '$0 vendor' first"
  [ -d "$PPC_LIBS" ] || die "no PowerPC stdlib at $PPC_LIBS (build it in mrustc first)"
  "$PATCHES/git-deps.py" > /dev/null
  setup_cc
}

transpile() {
  local pkg="$1" out="$2"
  mkdir -p "$out"
  cd "$MRUSTC_DIR"
  MINICARGO_DEFER_CODEGEN=1 ./bin/minicargo "$pkg" \
    --vendor-dir "$VENDOR" \
    --target "$PPC_TARGET" \
    -L "$PPC_LIBS" \
    --output-dir "$out" \
    -j "$JOBS" "${@:3}"
}

stage_hbb() {
  banner "transpile libs/hbb_common for $PPC_TARGET ($PPC_CPU)"
  prepare
  transpile "$REPO/libs/hbb_common" "$OUT-hbb"
}

stage_agent() {
  banner "transpile the agent (--features $FEATURES) for $PPC_TARGET ($PPC_CPU)"
  prepare
  transpile "$REPO" "$OUT" --no-default-features --features "$FEATURES"
}

main() {
  case "${1:-}" in
    vendor) stage_vendor ;;
    hbb)    stage_hbb ;;
    agent)  stage_agent ;;
    clean)  rm -rf "$OUT" "$OUT-hbb"; note "removed $OUT{,-hbb}" ;;
    *) die "usage: $0 <vendor|hbb|agent|clean>" ;;
  esac
}
main "$@"
