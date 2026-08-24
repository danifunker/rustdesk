#!/bin/sh
# Cross-build everything the IRIX package ships, and stage it for gendist.
#
# The whole build happens on the Linux host -- clang-18 plus ld.lld-irix
# targeting mips-sgi-irix6.5. No emulator is involved and none is needed: iris
# is only for RUNNING what comes out, and scripts/iris-gendist.sh is the one
# step that needs a guest, because only IRIX can build an inst product.
#
# Two compilers, on purpose:
#   the agent    cargo, nightly with -Zbuild-std, from the PowerPC tree's own
#                main.rs -- so this is a build of the code that ships, not a
#                copy of it
#   the panel    irix-cc directly (gui/build-gui.sh). Motif is not a cargo
#                dependency and the agent must never gain one: a machine
#                without Motif development files still gets a working agent.
#
# Usage:
#   scripts/build.sh [--stage-only] [--no-agent] [--no-gui] [--out DIR]
#
#   --stage-only  skip compiling; stage whatever is already built
#   --out DIR     where the staged tree goes                     [build/stage]
#
# The staged tree is laid out the way inst/rustdesk-agent.idb expects, which is
# by SOURCE path under a gendist -sbase, not by destination:
#
#   bin/rustdesk-agent        -> /usr/sbin/rustdesk-agent
#   bin/rustdesk-agent-gui    -> /usr/sbin/rustdesk-agent-gui
#   lib/agent-helper.sh       -> /usr/lib/rustdesk-agent/agent-helper.sh
#   lib/libgcc_s.so.1         -> /usr/lib/rustdesk-agent/libgcc_s.so.1
#   chest/RustDesk.chest      -> /usr/lib/X11/app-chests/RustDesk.chest
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

OUT=""
DO_AGENT=1
DO_GUI=1
STAGE_ONLY=0

die() { echo "build: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--out)        OUT="$2"; shift 2 ;;
		--no-agent)   DO_AGENT=0; shift ;;
		--no-gui)     DO_GUI=0; shift ;;
		--stage-only) STAGE_ONLY=1; DO_AGENT=0; DO_GUI=0; shift ;;
		-h|--help)    sed -n '2,32p' "$0"; exit 0 ;;
		*)            die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$OUT" ] || OUT="$REPO/build/stage"

SGUG="${SGUG_STAGING:-/opt/sgug-staging/usr/sgug}"
REL="$REPO/ports/rust/agent-portable/target/mips-sgi-irix6.5/release"

# The portable modules are included from the PowerPC tree by #[path], so the two
# trees have to stay siblings. Say so here rather than letting cargo fail on a
# missing file three hundred lines into a build.
PPC="${PPC_AGENT_DIR:-$REPO/../rustdesk-ppc-agent}"
[ -f "$PPC/src/main.rs" ] || die "no PowerPC tree at $PPC.
The agent's portable modules are included from it by #[path]; the two trees must
stay siblings. Set PPC_AGENT_DIR in ci/local.conf if it lives somewhere else."

# ---- 1. the agent ------------------------------------------------------------
if [ "$DO_AGENT" = 1 ]; then
	echo ">>> cargo: rustdesk-agent (n32)"
	# env.sh resolves its own location, so this works from any directory and
	# on any machine. It also sets the -rpath the installed agent uses to
	# find its libgcc_s without an environment variable.
	# RD_RUST_DIR because this runs under /bin/sh, where a sourced file cannot
	# work out its own path: $BASH_SOURCE does not exist and $0 names this
	# script, not the file being sourced.
	RD_RUST_DIR="$REPO/ports/rust"
	export RD_RUST_DIR
	# shellcheck disable=SC1091
	. "$REPO/ports/rust/env.sh" > /dev/null
	( cd "$REPO/ports/rust/agent-portable" && cargo +nightly build --release --bin rustdesk-agent )
fi

# ---- 2. the panel ------------------------------------------------------------
if [ "$DO_GUI" = 1 ]; then
	echo ">>> irix-cc: rustdesk-agent-gui (Motif 1.2)"
	SGUG="$SGUG" "$REPO/gui/build-gui.sh"
fi

# ---- 3. stage ----------------------------------------------------------------
echo ">>> staging into $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/chest"

[ -f "$REL/rustdesk-agent" ] || die "no $REL/rustdesk-agent -- build without --stage-only first"
[ -f "$REPO/gui/rustdesk-agent-gui" ] || die "no gui/rustdesk-agent-gui -- build without --stage-only first"

cp "$REL/rustdesk-agent"           "$OUT/bin/rustdesk-agent"
cp "$REPO/gui/rustdesk-agent-gui"  "$OUT/bin/rustdesk-agent-gui"
cp "$REPO/gui/agent-helper.sh"     "$OUT/lib/agent-helper.sh"
cp "$REPO/desktop/RustDesk.chest"  "$OUT/chest/RustDesk.chest"

# libgcc_s.so.1 is the ONE library the agent needs that stock IRIX 6.5 does not
# ship. Everything else it links -- libX11, libXext, libz, libpthread, libm,
# libc -- is on the machine already, and libsodium, libvpx, mbedTLS and zstd are
# linked statically into the binary. Shipping this one file is what makes the
# package installable on an IRIX that has never heard of SGUG-RSE, which is the
# whole point of packaging it.
[ -f "$SGUG/lib32/libgcc_s.so.1" ] || die "no $SGUG/lib32/libgcc_s.so.1"
cp "$SGUG/lib32/libgcc_s.so.1"     "$OUT/lib/libgcc_s.so.1"

chmod 755 "$OUT/bin/"* "$OUT/lib/agent-helper.sh" "$OUT/lib/libgcc_s.so.1"
chmod 644 "$OUT/chest/RustDesk.chest"

# ---- 4. say what is in it, and check it ---------------------------------------
echo
echo ">>> staged tree:"
( cd "$OUT" && ls -lR . | sed 's/^/    /' )

# A cross-build that silently produced a host binary is the failure this catches.
# `file` on a MIPS ELF says "MIPS, N32" and on anything else it does not.
if command -v file > /dev/null 2>&1; then
	echo
	echo ">>> what the binaries actually are:"
	for f in "$OUT/bin/rustdesk-agent" "$OUT/bin/rustdesk-agent-gui" "$OUT/lib/libgcc_s.so.1"; do
		printf '    %-24s %s\n' "$(basename "$f")" "$(file -b "$f")"
		case "$(file -b "$f")" in
			*MIPS*N32*) ;;
			*) die "$f is not an n32 MIPS object -- the cross toolchain was not used" ;;
		esac
	done
fi

# What the agent will look for at run time. Anything here that is not on a stock
# 6.5 machine and not in the package is a bug that only shows up on someone
# else's computer, which is the worst place to find it.
if command -v readelf > /dev/null 2>&1; then
	echo
	echo ">>> run-time dependencies:"
	for f in "$OUT/bin/rustdesk-agent" "$OUT/bin/rustdesk-agent-gui"; do
		echo "    $(basename "$f"):"
		readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/      \1/p'
		readelf -d "$f" | sed -n 's/.*Library rpath: \[\(.*\)\]/      rpath: \1/p'
	done
fi

echo
echo "staged: $OUT"
