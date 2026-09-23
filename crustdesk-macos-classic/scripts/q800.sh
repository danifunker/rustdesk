#!/usr/bin/env bash
# Boot C-Desk-Vint on QEMU's Quadra 800, System 7.5.5 with MacTCP.
#
#   scripts/q800.sh [start|stop|shot NAME|install]
#
#   install   copy the base system disk and put the app in Startup Items,
#             with a prefs file whose password is $CDV_PASSWORD (default
#             "classic")
#   start     install, then boot headless; the agent is on 127.0.0.1:$PORT
#   shot NAME save the screen to $ENV/NAME.png
#   stop      power off
#
# The workspace ($CDV_TESTENV, default ~/cdv-testenv) holds what cannot live in
# the repository: the Quadra ROM (f1acad13.rom, from MAME's macqd800 set), a
# System 7.5.5 disk with MacTCP set to Ethernet/BOOTP (sys755-net-base.hda),
# a PRAM image with 32-bit addressing on, and qm.py, the monitor client. See
# docs/TESTING.md for where each came from.
#
# ICOUNT=5 paces the CPU at roughly a real 33 MHz 68040 (one instruction per
# 32 ns), which is what makes timings mean anything; without it QEMU runs
# several times faster than the hardware.
#
# QEMU's user networking gives the Mac 10.0.2.15 and forwards the host's
# 127.0.0.1:$PORT to its 21118. SLIRP answers BOOTP, so MacTCP needs no setup.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
env=${CDV_TESTENV:-$HOME/cdv-testenv}
port=${PORT:-31119}
password=${CDV_PASSWORD:-classic}
app=${APP:-$here/build-m68k/C-Desk-Vint.bin}
qemu=${QEMU:-qemu-system-m68k}
rb() { rb-cli --progress never -q "$@"; }

install() {
    [ -f "$app" ] || { echo "build first: $app missing" >&2; exit 1; }
    cp "$env/sys755-net-base.hda" "$env/sys.hda"
    cp "$env/pram-32bit.img" "$env/pram.img"
    rb put-macbinary --dst-dir "/System Folder/Startup Items" "$env/sys.hda@1" "$app" >/dev/null
    printf 'password=%s\rport=21118\rquality=%s\r' "$password" "${CDV_QUALITY:-16}" \
        > "$env/C-Desk-Vint Prefs"
    rb put "$env/sys.hda@1" "$env/C-Desk-Vint Prefs" "/System Folder/Preferences/C-Desk-Vint Prefs" >/dev/null
    rb chmeta "$env/sys.hda@1" "/System Folder/Preferences/C-Desk-Vint Prefs" --type TEXT --creator ttxt >/dev/null
    echo "installed $(basename "$app") into Startup Items"
}

case "${1:-start}" in
install)
    install
    ;;
start)
    install
    cd "$env"
    "$qemu" -M q800 -m "${MEM:-64}" -bios f1acad13.rom -g "${GFX:-640x480x8}" \
        -drive file=pram.img,format=raw,if=mtd \
        -device scsi-hd,scsi-id=0,drive=hd0 \
        -drive file=sys.hda,media=disk,format=raw,if=none,id=hd0 \
        -nic user,model=dp83932,hostfwd=tcp:127.0.0.1:$port-:21118 \
        -display none -vnc 127.0.0.1:${VNC:-23} \
        -monitor unix:mon.sock,server,nowait ${ICOUNT:+-icount shift=$ICOUNT,align=off} \
        > qemu.log 2>&1 &
    echo $! > qemu.pid
    echo "booting (pid $(cat qemu.pid)); agent will be on 127.0.0.1:$port"
    ;;
shot)
    cd "$env" && ./qm.py "shot ${2:-screen}" >/dev/null && echo "$env/${2:-screen}.png"
    ;;
stop)
    cd "$env"
    [ -f qemu.pid ] && kill "$(cat qemu.pid)" 2>/dev/null || true
    rm -f qemu.pid
    ;;
*)
    echo "usage: $0 [start|stop|shot NAME|install]" >&2
    exit 1
    ;;
esac
