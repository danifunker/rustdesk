#!/usr/bin/env bash
# Boot Mac OS 9.2.2 on QEMU's mac99 (G4) with C-Desk-Vint on a CD image.
#
#   scripts/mac99.sh [start|stop|shot NAME|launch]
#
#   start     a fresh overlay on the OS 9 disk (never written itself), the
#             application on an HFS CD-ROM image, and boot; the agent will be
#             on 127.0.0.1:$PORT once launched
#   launch    open the CD and the application from the Finder by keystrokes
#   shot NAME screenshot to $CDV_TESTENV/NAME.png
#   stop      power off
#
# $OS9_DISK is the Mac OS 9.2.2 disk (default ~/MacOS9-2-2 UTM.qcow2, a UTM
# install). Open Transport there is set to DHCP, which QEMU's SLIRP answers.
# APP picks the application (default: the fat binary).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
env=${CDV_TESTENV:-$HOME/cdv-testenv}
port=${PORT:-31129}
os9=${OS9_DISK:-$HOME/MacOS9-2-2 UTM.qcow2}
app=$(realpath "${APP:-$here/build-m68k/C-Desk-Vint-fat.bin}")
rb() { rb-cli --progress never -q "$@"; }
mon() { python3 "$here/scripts/qmon.py" "$env/mon9.sock" "$@"; }

case "${1:-start}" in
start)
    [ -f "$app" ] || { echo "no $app: build it (see README)" >&2; exit 1; }
    cd "$env"
    rm -f os9-overlay.qcow2 cd.hfs
    qemu-img create -q -f qcow2 -b "$os9" -F qcow2 os9-overlay.qcow2
    rb new --fs hfs --size 4M --name C-Desk-Vint cd.hfs >/dev/null
    rb put-macbinary cd.hfs "$app" >/dev/null
    qemu-system-ppc -M mac99,via=pmu -cpu g4 -m 512 -boot c \
        -drive file=os9-overlay.qcow2,format=qcow2,if=none,id=d0 \
        -device ide-hd,drive=d0,bus=ide.0 -cdrom cd.hfs \
        -nic user,model=sungem,hostfwd=tcp:127.0.0.1:$port-:21118 \
        -g "${GFX:-800x600x32}" -display none -vnc 127.0.0.1:${VNC:-24} \
        -monitor unix:mon9.sock,server,nowait > qemu9.log 2>&1 &
    echo $! > qemu9.pid
    echo "booting Mac OS 9 (pid $(cat qemu9.pid)); give it two minutes, then: $0 launch"
    ;;
launch)
    # The CD is named C-Desk-Vint too: type-select it, open it, then the app.
    # Every start is a fresh overlay, so the Setup Assistant is up: quit it
    # (and confirm), then type-select the CD and the application.
    mon "sendkey meta_l-q" "sleep 2" "sendkey ret" "sleep 3" "sendkey c" "sendkey minus" "sleep 1" "sendkey meta_l-o" "sleep 4" \
        "sendkey c" "sleep 1" "sendkey meta_l-o"
    ;;
shot)
    mon "shot $env/${2:-screen}"
    echo "$env/${2:-screen}.png"
    ;;
stop)
    cd "$env"
    [ -f qemu9.pid ] && kill "$(cat qemu9.pid)" 2>/dev/null || true
    rm -f qemu9.pid
    ;;
*)
    echo "usage: $0 [start|stop|shot NAME|launch]" >&2
    exit 1
    ;;
esac
