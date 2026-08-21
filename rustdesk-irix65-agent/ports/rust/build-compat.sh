#!/bin/sh
# Build librust_irix_compat.a, the C stubs every Rust binary here links.
#
# The target spec's late-link-args name it (-lrust_irix_compat), so without it
# every link fails on an unresolved symbol that names the archive but not the
# reason. It is a build artifact, so it is not in git; this script is how you
# get it back on a fresh clone.
#
# The sources come from mogrix: IRIX has no dirfd, fdopendir, preadv, pwritev,
# dup3, posix_fadvise, posix_fallocate, cfmakeraw or flock, and its errno lives
# behind __oserror() rather than __errno_location().
set -e

MOGRIX="${MOGRIX_ROOT:-$HOME/repos/mogrix}"
CC="${IRIX_CC:-/opt/sgug-staging/usr/sgug/bin/irix-cc}"
OUT="$(cd "$(dirname "$0")" && pwd)/hello/compat"

if [ ! -d "$MOGRIX/compat" ]; then
    echo "no mogrix compat sources at $MOGRIX/compat" >&2
    echo "set MOGRIX_ROOT to your mogrix checkout" >&2
    exit 1
fi

mkdir -p "$OUT"
for src in rust/rust_compat.c rust/errno_location.c dicl/openat-compat.c \
           string/strnlen.c stdlib/setenv.c error/strerror_r.c; do
    obj="$OUT/$(basename "$src" .c).o"
    "$CC" -c "$MOGRIX/compat/$src" -o "$obj" \
        -I/opt/sgug-staging/usr/sgug/include \
        -I"$MOGRIX/compat/include"
done
ar rcs "$OUT/librust_irix_compat.a" "$OUT"/*.o
echo "built $OUT/librust_irix_compat.a"

# agent-portable links the same archive through a symlink rather than a second
# copy. A fresh clone has the symlink but not its target until now.
ln -sfn ../hello/compat "$(dirname "$OUT")/agent-portable/compat" 2>/dev/null || true
