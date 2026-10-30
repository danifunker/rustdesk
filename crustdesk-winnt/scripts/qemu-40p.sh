#!/bin/sh
# The PowerPC test machine: Windows NT 4.0 with Visual C++ 4 on QEMU's IBM
# 40P (PReP), from tenox7's nt40powerpc-qemu.tar.lz (its own QEMU build and
# firmware; github.com/tenox7/qemu, branch powerpc). Headless: VNC on
# 127.0.0.1:5941, QMP at $NTVM/run/qmp.sock (scripts/ntvm.py drives it).
#
# The image's disk stays as it came (read-only); everything goes to an
# overlay, $NTVM/work.qcow2, made on first use.
#
# Forwards on this machine's loopback: 2370 -> tools/nt/ntexec.c, and
# 21118 -> the agent's direct port. More at run time:
#   scripts/ntvm.py hmp 'hostfwd_add n0 tcp:127.0.0.1:PORT-:PORT'
#
# The image logs on as Administrator by itself; the password is "password"
# (its README says blank; Winlogon's DefaultPassword says otherwise).
NTVM=${NTVM:-/run/media/dani/BigDisk/winnt-ppc}
B=$NTVM/img/nt40powerpc-qemu
cd "$B" || exit 1
if [ ! -f "$NTVM/work.qcow2" ]; then
    qemu-img create -q -f qcow2 -b "$B/nt40.qcow2" -F qcow2 "$NTVM/work.qcow2"
    cp "$B/nvram.bin" "$NTVM/work-nvram.bin"
fi
mkdir -p "$NTVM/run"
exec bin/linux-x86_64/qemu-system-ppc \
 -M 40p -m 128 -bios fw/P12H0456.IMG \
 -drive file="$NTVM/work-nvram.bin",if=none,id=nvram,format=raw -global isa-m48t59.drive=nvram \
 -vga none -device s3-trio64,romfile=fw/gxt110p.bin \
 -display vnc=127.0.0.1:41 \
 -qmp unix:"$NTVM/run/qmp.sock",server,nowait \
 -nic none \
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2370-:2370,hostfwd=tcp:127.0.0.1:21118-:21118 \
 -device ne2k_isa,iobase=0x300,irq=5,netdev=n0 \
 -drive if=floppy,index=0,id=fd0 \
 -drive file="$NTVM/work.qcow2",if=none,id=hd0,format=qcow2 -device scsi-hd,drive=hd0,scsi-id=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on -device scsi-cd,drive=cd0,scsi-id=3 \
 "$@"
