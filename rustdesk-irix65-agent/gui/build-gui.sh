#!/bin/sh
#
# Cross-build the Motif settings panel for IRIX n32.
#
# Separate from the agent's cargo build on purpose: this is a second binary and
# the agent must not gain a dependency on Motif. Nothing here is required to
# build or run the agent -- a machine without Motif development files simply
# does not get the panel.
#
# TWO THINGS THE STOCK CROSS ENVIRONMENT DOES NOT GIVE YOU:
#
# 1. The Motif headers were not in /opt/irix-sysroot. The libraries are
#    (libXm.so, libXt.so, libSgm.so -- all SHARED, which is what makes this
#    possible at all: LLD cannot read the static archives SGI ships, which is
#    why input_shim.c issues XTEST protocol requests by hand). The headers live
#    on the machine at /usr/Motif-1.2/include and /usr/include/Xm is a symlink
#    to them, so a plain `tar cf` of /usr/include/Xm stores a link and nothing
#    else. Copy the real directory:
#
#      # on IRIX
#      cd /usr/Motif-1.2/include && tar cf /tmp/motif-hdrs.tar Xm Sgm
#      # on the build host, with the guest halted or via iris-ci get
#      cd /opt/irix-sysroot/usr/include && tar xf motif-hdrs.tar
#
#    That is Motif 1.2.4, which is what 6.5 ships and 5.3 carries.
#
#    A sysroot made by scripts/make-sysroot.sh needs none of that: it takes
#    /usr/Motif-1.2 out of the image beside /usr/include, which is exactly
#    where the image's own Xm and Sgm symlinks point.
#
# 2. -D_XmConst= . Xm/XmStrDefs.h declares `externalref _XmConst char ...` for
#    SGI's keypad virtual keys inside a branch that never defines _XmConst.
#    MIPSpro lets it through; clang reports "unknown type name '_XmConst'" and
#    stops. Defining it empty on the command line is the whole fix and changes
#    nothing about the generated code.
set -e

SGUG=${SGUG:-/opt/sgug-staging/usr/sgug}
# irix-cc reads IRIX_SYSROOT itself; this is only so the check below looks in
# the same place the compiler will.
SYSROOT=${IRIX_SYSROOT:-/opt/irix-sysroot}
CC=${CC:-$SGUG/bin/irix-cc}
OUT=${OUT:-$(cd "$(dirname "$0")" && pwd)/r-deskvint-irix-gui}
SRC=$(cd "$(dirname "$0")" && pwd)/gui_motif.c

if [ ! -f "$SYSROOT/usr/include/Xm/Xm.h" ]; then
    echo "build-gui.sh: no Motif headers in $SYSROOT -- see the note at the top" >&2
    exit 1
fi

# Sgm before Xm before Xt before X11: the link order matters on IRIX. -lSgm is
# NOT here, deliberately -- gui_motif.c includes no <Sgm/...> header and calls
# no Sg* function, so linking it would only make the panel refuse to build on an
# IRIX that has Motif without SGI's extensions. Add it back alongside an actual
# use, not before. (The same reasoning, and the same wording, as irixscsitb's.)
# The version the panel shows, when the caller has one (scripts/build.sh passes
# version_string). A string literal, so only the characters version_string can
# produce -- digits, letters, dashes -- are let through.
VFLAG=""
if [ -n "${RD_VERSION:-}" ]; then
    case "$RD_VERSION" in
        *[!A-Za-z0-9.-]*) echo "build-gui.sh: ignoring an odd RD_VERSION: $RD_VERSION" >&2 ;;
        *) VFLAG="-DRD_VERSION=\"$RD_VERSION\"" ;;
    esac
fi
set -x
"$CC" -O2 -D_XmConst= $VFLAG -o "$OUT" "$SRC" -lXm -lXt -lXext -lX11 -lm
set +x
echo "built $OUT"
