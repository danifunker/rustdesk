#!/bin/sh
# The x86 test machine: Windows NT 4.0 Workstation (no service pack), from
# tenox's nt4-vanilla.rar (winnt/VMs/qemu-x86), on QEMU's i440FX PC with a
# Pentium, Cirrus VGA and a PCnet card. Visual C++ 4.0 is its CD (the x86
# edition, Disc 1, from the winworld collection), run from there.
# The distribution's QEMU (/usr/bin): the one in /usr/local has no user-mode
# networking. No KVM: NT 4.0 hung at a blank desktop under it, at 100% CPU,
# with QEMU logging slirp send failures. Headless: VNC on 127.0.0.1:5942, QMP at $NTVM/run/qmp.sock.
#
# The image stays as it came (read-only); everything goes to an overlay,
# $NTVM/work.qcow2, made on first use.
#
# Forwards on this machine's loopback, beside the PowerPC machine's:
# 2371 -> tools/nt/ntexec.c, 21128 -> the agent's direct port 21118.
# A second CD drive (cd1) takes the ISO that carries ntexec.c in.
NTVM=${NTVM:-/run/media/dani/BigDisk/winnt-x86}
B=$NTVM/img
cd "$B" || exit 1
[ -f "$NTVM/work.qcow2" ] ||
    qemu-img create -q -f qcow2 -b "$B/nt4-vanilla.qcow2" -F qcow2 "$NTVM/work.qcow2"
mkdir -p "$NTVM/run"
exec /usr/bin/qemu-system-i386 -M pc -cpu pentium -m 256 -vga cirrus \
 -display vnc=127.0.0.1:42 \
 -qmp unix:"$NTVM/run/qmp.sock",server,nowait \
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2371-:2370,hostfwd=tcp:127.0.0.1:21128-:21118 \
 -device pcnet,netdev=n0 \
 -drive file="$NTVM/work.qcow2",if=ide,index=0,format=qcow2 \
 -drive file="$NTVM/iso/MSVC40.iso",if=ide,index=2,media=cdrom,readonly=on \
 -drive if=ide,index=3,media=cdrom,id=cd1 \
 "$@"
