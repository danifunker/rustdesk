#!/usr/bin/env bash
# Package C-Desk-Vint as a SCSI hard-disk image for a real Mac.
#
#   scripts/make-disk.sh [-o OUT.hda] [-s SIZE]
#
# The result is an Apple Partition Map disk with an Apple SCSI driver and one
# HFS volume, "C-Desk-Vint", holding the application and its Read Me. A
# BlueSCSI (or any SCSI emulator) presents it as an ordinary hard disk, and a
# Quadra's ROM mounts it next to the system disk; drag the application into
# the System Folder's Startup Items to have it start at boot.
#
# Needs rb-cli (rusty-backup) on the PATH.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
app="$here/build-m68k/C-Desk-Vint.bin"
out="$here/build-m68k/C-Desk-Vint.hda"
size=8M

while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=$2; shift 2 ;;
        -s) size=$2; shift 2 ;;
        -a) app=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done
[ -f "$app" ] || { echo "build the app first ($app missing)" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rb() { rb-cli --progress never -q "$@"; }

flat="$work/flat.hfs"
rb new --fs hfs --size "$size" --name C-Desk-Vint "$flat"
rb put-macbinary "$flat" "$app"

# The Read Me, as a SimpleText document: Mac line endings, Mac Roman.
python3 - "$here/docs/READ-ME-MAC.txt" "$work/Read Me" <<'EOF'
import sys
text = open(sys.argv[1], encoding='utf-8').read()
open(sys.argv[2], 'wb').write(text.replace('\n', '\r').encode('mac_roman'))
EOF
rb put "$flat" "$work/Read Me" "/Read Me"
rb chmeta "$flat" "/Read Me" --type TEXT --creator ttxt

rb expand --size "$size" --output "$out" "$flat"
rb mac-scsi-bless "$out"
echo "wrote $out"
rb ls "$out@1" /
