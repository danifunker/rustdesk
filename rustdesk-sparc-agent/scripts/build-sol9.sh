#!/usr/bin/env bash
#
# build-sol9.sh -- build the agent for sparcv9-sun-solaris2.9.
#
# Unlike build-sparc.sh, which compiles its C on the Blade over ssh because
# Solaris 10 had the only usable compiler, this cross-compiles: see
# mrustc's docker/sol9-cross for the gcc 4.9.4 toolchain it wants on PATH.
#
# Prerequisites:
#   - the stdlib for this target, built in the mrustc tree:
#       make -f minicargo.mk LIBS RUSTC_VERSION=1.74.0 \
#            MRUSTC_TARGET=sparcv9-sun-solaris2.9 OVERRIDE_SUFFIX=-solaris \
#            STD_ENV_ARCH=sparc64
#   - the C libraries, cross-built by build-deps-cross.sh
#   - a Solaris 9 sysroot, for the X headers and libraries
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
TARGET=sparcv9-sun-solaris2.9
MRUSTC_DIR="${MRUSTC_DIR:-$HOME/repos/mrustc}"
TOOLCHAIN="${TOOLCHAIN:-$HOME/sol9-toolchain}"
SYSROOT="${SYSROOT:-$TOOLCHAIN/sysroot}"
STDLIB="${STDLIB:-$MRUSTC_DIR/output-1.74.0-$TARGET}"
DEPS="${DEPS:-$HOME/sol9-deps/prefix}"
# The portable modules come from the PowerPC tree, as they do for every port;
# the vendored crates come from wherever that tree actually is.
PPC_DIR="${PPC_DIR:-$HERE/../rustdesk-ppc-agent}"
VENDOR="${VENDOR:-$PPC_DIR/vendor}"
OUT="${OUT:-$HERE/target/$TARGET}"
# Runpaths are the paths on the *target*, not in the sysroot: neither X prefix is
# on Solaris' default 64-bit search path.
SPARC_LDFLAGS="${SPARC_LDFLAGS:--R/usr/openwin/lib/sparcv9}"

export PATH="$TOOLCHAIN/opt/bin:$PATH"
export MRUSTC_TARGET_VER="${MRUSTC_TARGET_VER:-1.74}"

# mrustc's own C compiler for the target, behind a wrapper that adds the runpath.
# build-sparc.sh put SPARC_LDFLAGS on the ssh compiler wrapper's command line;
# there is no wrapper now, and mrustc has no hook for per-application link flags,
# so this is where they go. Only on the link: `-R` on a -c compile is just a
# warning, but a noisy one, once per crate.
mkdir -p "$OUT"
CCWRAP="$OUT/cc-link-wrapper.sh"
cat > "$CCWRAP" <<WRAP
#!/bin/sh
for a in "\$@"; do
    [ "\$a" = "-c" ] && exec $TARGET-gcc "\$@"
done
exec $TARGET-gcc "\$@" ${SPARC_LDFLAGS:--R/usr/openwin/lib/sparcv9}
WRAP
chmod +x "$CCWRAP"
export CC_sparcv9_sun_solaris2_9="$CCWRAP"

# The `cc` crate builds the shims; it runs on the host, so it has to be told
# which compiler and flags to use for the target explicitly.
# -D__EXTENSIONS__ because Solaris hides fd_set and much of POSIX behind it
# whenever __STDC__ is 1, which -std=gnu11 makes it.
export TARGET_CC="$TARGET-gcc"
export TARGET_AR="$TARGET-ar"
export TARGET_CFLAGS="-std=gnu11 -m64 -mcpu=v9 -fPIC -D__EXTENSIONS__"
export CC_sparcv9_sun_solaris2_9_cc="$TARGET-gcc"
export AR_sparcv9_sun_solaris2_9="$TARGET-ar"
export CFLAGS_sparcv9_sun_solaris2_9="$TARGET_CFLAGS"

export SPARC_DEPS="$DEPS"
export SODIUM_LIB_DIR="$DEPS/lib"
# X lives under /usr/openwin here, and only the base prefix has anything: this
# release has no Xfixes or Xdamage at all, which build.rs detects and gates on.
export X11_INCLUDE_DIR="$SYSROOT/usr/openwin/include"
export X11_LIB_DIR="$SYSROOT/usr/openwin/lib/sparcv9"
export X11_SFW_LIB_DIR="$SYSROOT/usr/openwin/sfw/lib/sparcv9"
# Runpaths are the paths on the *target*, not in the sysroot: neither X prefix
# is on Solaris' default 64-bit search path.

for d in "$STDLIB" "$DEPS/lib" "$VENDOR" "$X11_INCLUDE_DIR"; do
    [ -d "$d" ] || { echo "missing: $d -- see the header of this script" >&2; exit 1; }
done

mkdir -p "$OUT"
exec "$MRUSTC_DIR/bin/minicargo" "$HERE" \
    --vendor-dir "$VENDOR" \
    --target "$TARGET" \
    -L "$STDLIB" \
    --output-dir "$OUT" \
    "$@"
