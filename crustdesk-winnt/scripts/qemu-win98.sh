#!/bin/sh
# The Windows 98 test machine: the user's own VMware Windows 98 SE install
# (Win98VM: "Windows 98.vmdk", an 8 GB monolithic IDE disk), booted by QEMU
# with the hardware VMware gave it: IDE, an AMD PCnet card (VMware's "vlance"
# for a 98 guest), a Pentium II class CPU. The .vmx also names a backup disk,
# a second partition disk, a CD and a floppy that are not here; QEMU needs
# none of them, and the .vmx is left as it is.
#
# The disk stays as it came (read-only); the machine runs on a raw copy of
# it, $W98VM/work.raw (qemu-img convert, made on first use), so that with
# the machine stopped its files can be read and changed with mtools
# (the partition starts at byte 1048576): BOOTLOG.TXT, MSDOS.SYS.
# VNC 127.0.0.1:5943, QMP at $W98VM/run/qmp.sock.
# Forwards: 2372 -> tools/nt/ntexec.c, 21138 -> the agent's direct port.
# The second CD drive (cd1) is for the ISO that carries files in.
W98VM=${W98VM:-/run/media/dani/BigDisk/win98vm}
[ -f "$W98VM/work.raw" ] ||
    qemu-img convert -O raw "$W98VM/img/Windows 98.vmdk" "$W98VM/work.raw"
mkdir -p "$W98VM/run"
exec /usr/bin/qemu-system-i386 -M pc -cpu pentium2 -m 256 -vga std \
 -display vnc=127.0.0.1:43 \
 -qmp unix:"$W98VM/run/qmp.sock",server,nowait \
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2372-:2370,hostfwd=tcp:127.0.0.1:21138-:21118 \
 -device pcnet,netdev=n0,mac=00:0c:29:ee:73:cd \
 -drive file="$W98VM/work.raw",if=ide,index=0,format=raw \
 -drive if=ide,index=2,media=cdrom,id=cd1 \
 -rtc base=localtime \
 "$@"
