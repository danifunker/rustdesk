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
#                    [--isa mips3|mips4|both]
#
#   --stage-only  skip compiling; stage whatever is already built
#   --out DIR     where the staged tree goes                     [build/stage]
#   --isa         which agents: MIPS III runs on every IRIX 6.5 machine, MIPS IV
#                 on R5000, R8000, R10000 and later, a few percent faster.
#                 Default both when there is a MIPS IV staging tree (the
#                 toolchain layout has one: scripts/toolchain.sh), else mips3.
#
# The staged tree is laid out the way inst/r-deskvint-irix.idb expects, which is
# by SOURCE path under a gendist -sbase, not by destination:
#
#   bin/r-deskvint-irix        -> /usr/local/sbin/r-deskvint-irix  (MIPS III)
#   bin/r-deskvint-irix-mips4  -> /usr/local/sbin/r-deskvint-irix  (MIPS IV;
#                                inst installs one of the two, by CPU -- see
#                                inst/r-deskvint-irix.idb and install.sh)
#   bin/r-deskvint-irix-gui    -> /usr/local/sbin/r-deskvint-irix-gui
#   bin/cacert.pem            -> /usr/local/sbin/cacert.pem
#   lib/agent-helper.sh       -> /usr/local/lib/r-deskvint-irix/agent-helper.sh
#   lib/libgcc_s.so.1         -> /usr/local/lib/r-deskvint-irix/libgcc_s.so.1
#   lib/r_deskvint_irix.init   -> /usr/local/lib/r-deskvint-irix/r_deskvint_irix.init
#   chest/R-DeskVint.chest      -> /usr/lib/X11/app-chests/R-DeskVint.chest
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

OUT=""
DO_AGENT=1
DO_GUI=1
STAGE_ONLY=0
ISA=""

die() { echo "build: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--out)        OUT="$2"; shift 2 ;;
		--no-agent)   DO_AGENT=0; shift ;;
		--no-gui)     DO_GUI=0; shift ;;
		--stage-only) STAGE_ONLY=1; DO_AGENT=0; DO_GUI=0; shift ;;
		--isa)        ISA="$2"; shift 2 ;;
		-h|--help)    sed -n '2,41p' "$0"; exit 0 ;;
		*)            die "unknown option: $1" ;;
	esac
done

load_local_conf
toolchain_env || die "the toolchain named by IRIX_TOOLCHAIN is not usable"
[ -n "$OUT" ] || OUT="$REPO/build/stage"

SGUG="${SGUG_STAGING:-/opt/sgug-staging/usr/sgug}"
REL="${CARGO_TARGET_DIR:-$REPO/ports/rust/agent-portable/target}/mips-sgi-irix6.5/release"
REL4="${CARGO_TARGET_DIR:-$REPO/ports/rust/agent-portable/target}/mips-sgi-irix6.5-mips4/release"

# The MIPS IV staging tree: the same as SGUG with the ISA raised in its
# compiler and its five static libraries. See ports/rust/env.sh (RD_ISA).
if [ -n "${IRIX_TOOLCHAIN:-}" ]; then
	SGUG4="$IRIX_TOOLCHAIN/sgug-mips4"
else
	SGUG4="${SGUG_STAGING_MIPS4:-/opt/sgug-staging/usr/sgug-mips4}"
fi
case "$ISA" in
	"")
		if [ -x "$SGUG4/bin/irix-cc" ]; then ISA=both; else
			ISA=mips3
			echo ">>> no MIPS IV staging tree at $SGUG4 -- building for MIPS III only."
			echo "    scripts/toolchain.sh makes one; IRIX_TOOLCHAIN then selects it."
		fi ;;
	mips3|mips4|both) ;;
	*) die "--isa is mips3, mips4 or both, not $ISA" ;;
esac
case "$ISA" in mips3) DO3=1 DO4=0 ;; mips4) DO3=0 DO4=1 ;; both) DO3=1 DO4=1 ;; esac
[ "$DO4" = 0 ] || [ "$STAGE_ONLY" = 1 ] || [ -x "$SGUG4/bin/irix-cc" ] ||
	die "no MIPS IV staging tree at $SGUG4 -- run scripts/toolchain.sh"

# The portable modules are included from the PowerPC tree by #[path], so the two
# trees have to stay siblings. Say so here rather than letting cargo fail on a
# missing file three hundred lines into a build.
PPC="${PPC_AGENT_DIR:-$REPO/../rustdesk-ppc-agent}"
[ -f "$PPC/src/main.rs" ] || die "no PowerPC tree at $PPC.
The agent's portable modules are included from it by #[path]; the two trees must
stay siblings. Set PPC_AGENT_DIR in ci/local.conf if it lives somewhere else."

# ---- 1. the agent ------------------------------------------------------------
if [ "$DO_AGENT" = 1 ]; then
	# env.sh resolves its own location, so this works from any directory and
	# on any machine. It also sets the -rpath the installed agent uses to
	# find its libgcc_s without an environment variable.
	# RD_RUST_DIR because this runs under /bin/sh, where a sourced file cannot
	# work out its own path: $BASH_SOURCE does not exist and $0 names this
	# script, not the file being sourced.
	RD_RUST_DIR="$REPO/ports/rust"
	export RD_RUST_DIR
	if [ "$DO3" = 1 ]; then
		echo ">>> cargo: r-deskvint-irix (n32, MIPS III)"
		# shellcheck disable=SC1091
		( . "$REPO/ports/rust/env.sh" > /dev/null &&
		  cd "$REPO/ports/rust/agent-portable" && cargo +nightly build --release --bin r-deskvint-irix ) ||
			die "the MIPS III agent did not build"
	fi
	if [ "$DO4" = 1 ]; then
		echo ">>> cargo: r-deskvint-irix (n32, MIPS IV)"
		# Its own target, so its own std: the spec differs from the MIPS III
		# one only in "cpu", and -Zbuild-std compiles std for it. The C shims
		# and the static libraries come from SGUG4, whose irix-cc raises the ISA.
		# shellcheck disable=SC1091
		( RD_ISA=mips4; export RD_ISA
		  [ -n "${IRIX_TOOLCHAIN:-}" ] || { SGUG_STAGING="$SGUG4"; export SGUG_STAGING; }
		  . "$REPO/ports/rust/env.sh" > /dev/null &&
		  cd "$REPO/ports/rust/agent-portable" &&
		  cargo +nightly build --release --target mips-sgi-irix6.5-mips4.json --bin r-deskvint-irix ) ||
			die "the MIPS IV agent did not build"
	fi
fi

# ---- 2. the panel ------------------------------------------------------------
if [ "$DO_GUI" = 1 ]; then
	echo ">>> irix-cc: r-deskvint-irix-gui (Motif 1.2)"
	SGUG="$SGUG" "$REPO/gui/build-gui.sh"
fi

# ---- 3. stage ----------------------------------------------------------------
echo ">>> staging into $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/chest"

[ "$DO3" = 0 ] || [ -f "$REL/r-deskvint-irix" ] ||
	die "no $REL/r-deskvint-irix -- build without --stage-only first"
[ "$DO4" = 0 ] || [ -f "$REL4/r-deskvint-irix" ] ||
	die "no $REL4/r-deskvint-irix -- build without --stage-only first"
[ -f "$REPO/gui/r-deskvint-irix-gui" ] || die "no gui/r-deskvint-irix-gui -- build without --stage-only first"

# The MIPS III agent is the one every package must have: it is what an R4x00
# runs, and what the package falls back on. --isa mips4 alone is for trying the
# MIPS IV build, not for shipping.
if [ "$DO3" = 1 ]; then
	cp "$REL/r-deskvint-irix"       "$OUT/bin/r-deskvint-irix"
else
	echo ">>> --isa mips4: staging the MIPS IV agent under BOTH names; this tree"
	echo "    is for a MIPS IV machine only and must not be packaged."
	cp "$REL4/r-deskvint-irix"      "$OUT/bin/r-deskvint-irix"
fi
[ "$DO4" = 0 ] || cp "$REL4/r-deskvint-irix" "$OUT/bin/r-deskvint-irix-mips4"
cp "$REPO/gui/r-deskvint-irix-gui"  "$OUT/bin/r-deskvint-irix-gui"
cp "$REPO/gui/agent-helper.sh"     "$OUT/lib/agent-helper.sh"
# The init script ships beside the helper rather than straight into /etc/init.d:
# a package that writes to /etc on install decides for the admin that this
# machine runs the agent at boot. `agent-helper.sh service install` puts it
# there when someone asks for it.
cp "$REPO/init/r_deskvint_irix"     "$OUT/lib/r_deskvint_irix.init"
cp "$REPO/desktop/R-DeskVint.chest"  "$OUT/chest/R-DeskVint.chest"

# libgcc_s.so.1 is the ONE library the agent needs that stock IRIX 6.5 does not
# ship. Everything else it links -- libX11, libXext, libpthread, libm, libc --
# is on the machine already, and libsodium, libvpx, mbedTLS, zstd and zlib are
# linked statically into the binary. Shipping this one file is what makes the
# package installable on an IRIX that has never heard of SGUG-RSE, which is the
# whole point of packaging it.
#
# zlib is in that static list for a reason found the hard way: IRIX 6.5 does
# ship a zlib, but 6.5 shipped more than one over its life and the older one
# predates compressBound (zlib 1.2.0). Linking it dynamically produced an agent
# that ran on the build image and died on a stock O2 with "rld: unresolvable
# symbol: compressBound" -- written to SYSLOG, not stderr, so it presented as a
# silent exit 1 from every invocation. See docs/PACKAGING.md.
[ -f "$SGUG/lib32/libgcc_s.so.1" ] || die "no $SGUG/lib32/libgcc_s.so.1"
cp "$SGUG/lib32/libgcc_s.so.1"     "$OUT/lib/libgcc_s.so.1"

# Certificate roots for an https console. IRIX 6.5 predates every root in use
# today and ships no bundle at all, so without this the console is refused with
# "no CA bundle found" and the machine never appears in a device list.
# Registration is unaffected -- that is UDP to hbbs, not TLS.
#
# It goes BESIDE THE BINARY because that is where the agent looks first
# (http.rs CaBundle::search_paths, the directory of current_exe), which is the
# same place the Mac bundle puts it. A .pem in sbin is odd; one path that works
# on every platform is worth more than tidiness.
#
# Taken from the build host, where a current bundle already lives, so there is
# nothing to download and nothing to keep in git. Not fatal: a host without one
# still produces a working agent for everything but an https console, and
# --ca-bundle overrides it either way.
CA_SRC="${RD_CA_BUNDLE:-}"
if [ -z "$CA_SRC" ]; then
	for _c in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt \
	          /usr/share/ssl/certs/ca-bundle.crt /etc/ssl/cert.pem; do
		[ -f "$_c" ] && { CA_SRC="$_c"; break; }
	done
fi
if [ -n "$CA_SRC" ] && [ -f "$CA_SRC" ]; then
	cp "$CA_SRC" "$OUT/bin/cacert.pem"
	echo ">>> CA bundle: $CA_SRC (`grep -c 'BEGIN CERTIFICATE' "$CA_SRC"` roots)"
else
	echo ">>> WARNING: no CA bundle on this host -- an https console will need"
	echo "    --ca-bundle. Set RD_CA_BUNDLE to one to include it."
fi

chmod 755 "$OUT/bin/"* "$OUT/lib/agent-helper.sh" "$OUT/lib/libgcc_s.so.1" \
          "$OUT/lib/r_deskvint_irix.init"
chmod 644 "$OUT/chest/R-DeskVint.chest"
if [ -f "$OUT/bin/cacert.pem" ]; then chmod 644 "$OUT/bin/cacert.pem"; fi

# ---- 4. say what is in it, and check it ---------------------------------------
echo
echo ">>> staged tree:"
( cd "$OUT" && ls -lR . | sed 's/^/    /' )

# A cross-build that silently produced a host binary is the failure this catches.
# `file` on a MIPS ELF says "MIPS, N32" and on anything else it does not.
if command -v file > /dev/null 2>&1; then
	echo
	echo ">>> what the binaries actually are:"
	for f in "$OUT/bin/r-deskvint-irix" "$OUT/bin/r-deskvint-irix-mips4" \
	         "$OUT/bin/r-deskvint-irix-gui" "$OUT/lib/libgcc_s.so.1"; do
		# cacert.pem is deliberately not in this list: it is text, not an object.
		[ -f "$f" ] || continue
		printf '    %-24s %s\n' "$(basename "$f")" "$(file -b "$f")"
		case "$(file -b "$f")" in
			*MIPS*N32*) ;;
			*) die "$f is not an n32 MIPS object -- the cross toolchain was not used" ;;
		esac
	done
	# And the right one each: an ELF header says the highest ISA of anything
	# linked in, so one MIPS IV object in the MIPS III agent shows up here.
	if [ "$DO3" = 1 ]; then
		case "$(file -b "$OUT/bin/r-deskvint-irix")" in
			*MIPS-III*) ;;
			*) die "bin/r-deskvint-irix is not marked MIPS-III -- something MIPS IV was linked in" ;;
		esac
	fi
	if [ "$DO4" = 1 ]; then
		case "$(file -b "$OUT/bin/r-deskvint-irix-mips4")" in
			*MIPS-IV*) ;;
			*) die "bin/r-deskvint-irix-mips4 is not marked MIPS-IV -- the MIPS IV build did not happen" ;;
		esac
	fi
fi

# The header is a claim; this is the check. Disassemble the MIPS III agent as
# MIPS IV and look for anything only MIPS IV has -- the conditional moves, the
# indexed FP loads and stores, prefetch, the fused FP multiply-adds, and FP
# condition codes other than $fcc0. One of those on an R4400 is a SIGILL in
# the middle of a session.
OBJDUMP=""
for _o in "${IRIX_TOOLCHAIN:-/nonexistent}/cross/bin/llvm-objdump" /opt/cross/bin/llvm-objdump; do
	[ -x "$_o" ] && { OBJDUMP="$_o"; break; }
done
if [ "$DO3" = 1 ] && [ -n "$OBJDUMP" ]; then
	_n=$("$OBJDUMP" -d --mcpu=mips4 "$OUT/bin/r-deskvint-irix" |
		grep -cE '[[:space:]](movn|movz|movf|movt|pref|prefx|lwxc1|ldxc1|swxc1|sdxc1|madd\.[sd]|msub\.[sd]|nmadd\.[sd]|nmsub\.[sd]|recip\.[sd]|rsqrt\.[sd])[[:space:]]|\$fcc[1-7]' || true)
	echo
	echo ">>> MIPS IV instructions in the MIPS III agent: $_n"
	[ "$_n" = 0 ] || die "the MIPS III agent contains MIPS IV instructions; it would not run on an R4x00"
fi

# What the agent will look for at run time. Anything here that is not on a stock
# 6.5 machine and not in the package is a bug that only shows up on someone
# else's computer, which is the worst place to find it.
#
# Note what this does NOT catch: a symbol left undefined because no library was
# named for it at all prints nothing here. That is exactly how the compressBound
# bug above reached a real machine, so read the list as necessary, not sufficient.
if command -v readelf > /dev/null 2>&1; then
	echo
	echo ">>> run-time dependencies:"
	for f in "$OUT/bin/r-deskvint-irix" "$OUT/bin/r-deskvint-irix-gui"; do
		echo "    $(basename "$f"):"
		readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/      \1/p'
		readelf -d "$f" | sed -n 's/.*Library rpath: \[\(.*\)\]/      rpath: \1/p'
	done
fi

echo
echo "staged: $OUT"
