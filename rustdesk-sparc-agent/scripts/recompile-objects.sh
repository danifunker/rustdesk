#!/bin/sh
#
# recompile-objects.sh OUTDIR -- rebuild every crate's .o from the C mrustc
# already emitted, without re-running the front end.
#
# mrustc leaves the exact compiler command for each crate beside its output, in
# `<crate>.rlib_cmd.txt`. Replaying those is how to change C compilers (4.9 ->
# 5.5, or the ssh wrapper -> a cross-gcc) for a tree that is otherwise current:
# the .rlib.hir metadata does not depend on the C compiler, so only the objects
# need to change. A full `make -f minicargo.mk LIBS` would redo the Rust front
# end as well, which is the slow half on the local machine.
#
#   SPARC_HOST=blade SPARC_CC=/opt/csw/gcc5/bin/gcc \
#     scripts/recompile-objects.sh ~/repos/mrustc/output-1.74.0-sparcv9-sun-solaris
#
# The link commands (`dump_cmd.txt` and any binary) are skipped: they consume
# these objects, so they have to come after, and minicargo drives them anyway.

set -e
OUTDIR="${1:?usage: recompile-objects.sh OUTDIR}"
CC_WRAPPER="${CC_WRAPPER:-$(dirname "$0")/sparc-cc-remote.py}"

n=0
for cmd in "$OUTDIR"/*.rlib_cmd.txt; do
    [ -e "$cmd" ] || { echo "no *_cmd.txt in $OUTDIR -- has it been built once?" >&2; exit 1; }
    crate=$(basename "$cmd" .rlib_cmd.txt)
    printf '%-48s' "$crate"
    start=$(date +%s)
    if "$CC_WRAPPER" "@$cmd"; then
        echo "  ok ($(( $(date +%s) - start ))s)"
        n=$((n + 1))
    else
        echo "  FAILED"
        exit 1
    fi
done
echo "$n objects rebuilt with ${SPARC_CC:-the default compiler}"
