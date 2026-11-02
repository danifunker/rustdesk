#!/usr/bin/env bash
# Build C-Desk-Vint for PINT, Windows NT 4.0 for ARM (github.com/tenox7/PINT),
# as an app of PINT's own build: Microsoft C for ARM in tenox7/msvc-arm32,
# NT 4.0's C run-time (Visual C++ 4.2's libcmt) and import libraries made from
# the NT 4.0 tree. Only the parts of PINT's build the agent links are made, not
# NT itself; each is skipped when its output is there, so a kept WORK/nt40/RASPI/obj
# (the workflow caches it) goes straight to the agent.
#
#   scripts/pint-build.sh WORK [OUTDIR]     (OUTDIR default build/arm)
#
# WORK holds the trees as PINT's scripts expect them (they mount ../.. of
# RASPI/build as /work):
#   WORK/nt40/private, WORK/nt40/public   the NT 4.0 source
#   WORK/nt40/RASPI                       PINT
#   WORK/nt35/msvc                        Microsoft C, installed here by msvc-setup
#   WORK/nt35, WORK/XPSP1                 mounted by every step; not needed here, made empty
#
# The image is the tag PINT's scripts name, tenox7/msvc-arm32 (the workflow pulls
# a pinned digest and tags it so). Its tools run on 32-bit ARM Wine: a machine
# without AArch32 (Apple silicon, GitHub's ARM runners, any x86) needs QEMU's
# binfmt for linux/arm, which Docker Desktop has and a runner gets from
# docker/setup-qemu-action.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(cd "${1:?usage: pint-build.sh WORK [OUTDIR]}" && pwd)"
OUTDIR="${2:-$HERE/build/arm}"
NT40=$WORK/nt40 R=$WORK/nt40/RASPI
B=$R/build U=$R/obj/um
for d in $NT40/private $NT40/public $R/build; do [ -d $d ] || { echo "pint-build: no $d"; exit 1; }; done
mkdir -p $WORK/nt35/msvc $WORK/XPSP1 $OUTDIR

t0=$(date +%s)
say() { echo "== $(( ($(date +%s) - t0) / 60 ))m $*"; }
# have FILE... || run STEP: a step whose outputs are all there is done
have() { local f; for f; do [ -s "$f" ] || return 1; done; say "have $(basename "$1")"; }

# Microsoft C for ARM, once
if [ -z "$(ls -A $WORK/nt35/msvc)" ]; then
    say "msvc-setup"
    docker run --rm -e ACCEPT_MSVC_LICENSE=yes -v "$WORK/nt35/msvc":/opt/msvc tenox7/msvc-arm32 msvc-setup
fi

# the port's versions of NT files, from their edit scripts (build-update.sh does it first).
# Those over NT 3.5's or XP SP1's files are left out: the trees are not here, and what they
# make (the TCP/IP tools, the admin apps, RAS's DES, the hives, ARC's DOS shell, ntdbg.h of
# the debugger) is nothing the agent links.
say "ntdiff apply"
: > $R/obj/cdv-ntdiff.lst; : > $OUTDIR/ntdiff-skipped.txt
( cd $R && find . -name '*.ntdiff' -not -path './.git/*' | sort ) | while read -r f; do
    if head -1 "$R/$f" | grep -qE '\.\./(nt35|XPSP1)/'; then echo "${f#./}" >> $OUTDIR/ntdiff-skipped.txt
    else echo "/work/RASPI/${f#./}" >> $R/obj/cdv-ntdiff.lst; fi
done
say "  $(wc -l < $R/obj/cdv-ntdiff.lst) scripts, $(wc -l < $OUTDIR/ntdiff-skipped.txt) skipped (ntdiff-skipped.txt)"
# Every step applies them all again as it starts (msvc.sh, make-tools.sh): in this
# checkout of PINT, those apply the list instead.
for f in build/msvc.sh tools/make-tools.sh; do
    grep -qxF 'perl /work/RASPI/tools/ntdiff.pl apply /work /work/RASPI' $R/$f ||
        grep -qF 'cdv-ntdiff.lst' $R/$f || { echo "pint-build: $f no longer applies the scripts as expected"; exit 1; }
    sed -i.orig 's#^perl /work/RASPI/tools/ntdiff.pl apply /work /work/RASPI$#xargs perl /work/RASPI/tools/ntdiff.pl apply /work < /work/RASPI/obj/cdv-ntdiff.lst#' $R/$f
done
docker run --rm -v "$NT40":/work -v "$WORK/nt35":/nt35:ro -v "$WORK/XPSP1":/XPSP1:ro \
    -v "$NT40/private":/work/private:ro -v "$NT40/public":/work/public:ro \
    --entrypoint sh tenox7/msvc-arm32 -c 'xargs perl /work/RASPI/tools/ntdiff.pl apply /work < /work/RASPI/obj/cdv-ntdiff.lst'

# NT's build tools, the header pass, and the import libraries of kernel32, user32, advapi32
have $R/obj/host/RC.EXE $U/kernel32.lib $U/user32.lib $U/advapi32.lib || { say "tools"; bash $R/tools/make-tools.sh; }
have $R/obj/gen/ksarm.h || { say "ksarm.h"; bash $B/make-ksarm.sh; }

# the C run-time, in the order of build-update.sh's crt step, without the DLLs: libcmt
# (/MT, as on every other processor) and oldnames. The tools step leaves a libcmt.lib
# too, crt32's; Visual C++ 4.2's replaces it, as NT links its images with that one, so
# a stamp says this step ran, not the library.
have $R/obj/cdv-crt.done || {
    say "C run-time"
    ( cd $R/crt && ./make-crt32.sh ST && ./make-crt32.sh MT && ./make-crt.sh ST && ./make-crt.sh MT && ./make-crt.sh OLDNAMES )
    date > $R/obj/cdv-crt.done
}

# the import libraries of the DLLs the agent imports beyond those (build-nt.sh's implib)
implib() { have $U/$1.lib || { say "implib $1"; IMPLIB=1 ROOT=/work/private bash $B/make-dll.sh $2; }; }
implib gdi32 ntos/w32/ntgdi/client/daytona
implib ole32 ole32/dll/daytona
implib wsock32 net/sockets/winsock2/wsock32/dll

# the agent: staged, then built as PINT builds its apps (build-nt.sh: lib, exe)
say "stage"
python3 $HERE/scripts/stage-pint.py $HERE/build/pint
rm -rf $R/apps/cdv && cp -R $HERE/build/pint/cdv $R/apps/cdv
for c in sodium bssl zstd; do say "lib cdv/$c"; ROOT=/work/RASPI/apps bash $B/make-lib.sh cdv/$c; done
say "exe cdv"; ROOT=/work/RASPI/apps bash $B/make-exe.sh cdv/exe cdv
say "exe ntexec"; ROOT=/work/RASPI/apps bash $B/make-exe.sh cdv/ntexec ntexec

cp $R/obj/CDV.EXE $OUTDIR/C-Desk-Vint.exe
cp $R/obj/NTEXEC.EXE $OUTDIR/ntexec.exe
[ ! -f $R/obj/cdv.map ] || cp $R/obj/cdv.map $OUTDIR/C-Desk-Vint.map
say "done"; ls -l $OUTDIR
