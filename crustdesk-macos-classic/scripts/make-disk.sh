#!/usr/bin/env bash
# Package C-Desk-Vint as a SCSI hard-disk image for a real Mac.
#
#   scripts/make-disk.sh [-o OUT.hda] [-z OUT.sit.hqx] [-m OUT.mar] [-s SIZE] [-b BUILD-PARENT]
#
# The result is an Apple Partition Map disk with an Apple SCSI driver and one
# HFS volume, "C-Desk-Vint", holding the installer, the application, the
# Control Strip module and the Read Me (-z also packs them as .sit.hqx, -m as a
# stored MAR archive). A
# BlueSCSI (or any SCSI emulator) presents it as an ordinary hard disk, and a
# Quadra's ROM mounts it next to the system disk; open Install C-Desk-Vint.
#
# Needs rb-cli (rusty-backup) on the PATH.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
b68=$here/build-m68k
bppc=$here/build-ppc
out=""
sit=""
mar=""
size=8M

while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=$2; shift 2 ;;
        -z) sit=$2; shift 2 ;;
        -m) mar=$2; shift 2 ;;
        -s) size=$2; shift 2 ;;
        -b) b68=$2/build-m68k; bppc=$2/build-ppc; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done
[ -n "$out" ] || out=$b68/C-Desk-Vint.hda

# The fat application if the PowerPC half is built, else the 68k one.
app="$b68/C-Desk-Vint.bin"
if [ -f "$bppc/C-Desk-Vint.bin" ]; then
    python3 "$here/tools/fatmerge.py" "$b68/C-Desk-Vint.bin" "$bppc/C-Desk-Vint.bin" \
        "$b68/C-Desk-Vint-fat.bin"
    app="$b68/C-Desk-Vint-fat.bin"
fi
for f in "$app" "$b68/C-Desk-Vint-Installer.bin" "$b68/C-Desk-Vint-Strip.bin"; do
    [ -f "$f" ] || { echo "build first ($f missing)" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rb() { rb-cli --progress never -q "$@"; }
# A blank HFS volume: rb-cli releases since mid-2026 spell it `new volume hfs`,
# older ones `new --fs hfs`.
new_hfs() { rb new volume hfs "$@" 2>/dev/null || rb new --fs hfs "$@"; }

# The four items, named as the Mac will show them.
mkdir "$work/items"
python3 "$here/tools/mbrename.py" "$app" "$work/items/C-Desk-Vint.bin" "C-Desk-Vint" --bundle
python3 "$here/tools/mbrename.py" "$b68/C-Desk-Vint-Installer.bin" \
    "$work/items/Install C-Desk-Vint.bin" "Install C-Desk-Vint" --bundle
python3 "$here/tools/mbrename.py" "$b68/C-Desk-Vint-Strip.bin" \
    "$work/items/C-Desk-Vint Strip.bin" "C-Desk-Vint Strip"
# The Read Me, as a SimpleText document: Mac line endings, Mac Roman.
python3 - "$here/docs/READ-ME-MAC.txt" "$work/Read Me" <<'EOF2'
import sys
text = open(sys.argv[1], encoding='utf-8').read()
open(sys.argv[2], 'wb').write(text.replace('\n', '\r').encode('mac_roman'))
EOF2

flat="$work/flat.hfs"
new_hfs --size "$size" --name C-Desk-Vint "$flat"
for f in "$work/items/"*.bin; do
    rb put-macbinary "$flat" "$f"
done
rb put "$flat" "$work/Read Me" "/Read Me"
rb chmeta "$flat" "/Read Me" --type TEXT --creator ttxt

rb expand --size "$size" --output "$out" "$flat"
rb mac-scsi-bless "$out"
echo "wrote $out"
rb ls "$out@1" /

# And the same four as archives, for a Mac on the network: StuffIt (.sit.hqx)
# and MAR. Each item goes in as BinHex from the volume: the archiver takes
# .hqx with both forks and Finder info, where it would store a .bin as plain
# data.
if [ -n "$sit" ] || [ -n "$mar" ]; then
    mkdir "$work/hqx"
    for f in "C-Desk-Vint" "Install C-Desk-Vint" "C-Desk-Vint Strip" "Read Me"; do
        rb-cli get-binhex --progress never -q "$flat" "/$f" "$work/hqx/$f.hqx"
    done
    for a in "$sit" "$mar"; do
        [ -n "$a" ] || continue
        rm -f "$a"
        rb-cli archive create --progress never -q "$a" "$work/hqx/"*.hqx
        echo "wrote $a"
    done
fi
