#!/bin/sh
# PINT (Windows NT 4.0 for ARM, github.com/tenox7/PINT) in QEMU raspi2b, the ARM
# test machine: a PINT SD card image, booted by the card's own ARC firmware.
#
#   scripts/qemu-pint.sh DISK.img [DIR]
#
# DIR (default: the image's directory) gets arcfw.bin from the card (mtools), the
# monitor socket mon.sock (screendump, sendkey) and serial.log. ntexec (2370) and
# the agent's direct port (21118) are forwarded on 127.0.0.1.
#
# PINT's build/run.sh boots obj/arcfw.elf, which a card does not carry; its
# arcfw.bin is linked at 0x8000, where raspi2b loads a -bios image, so it boots
# as -bios. Traps (2026-11-02, QEMU 11.1.3):
#   - the frame buffer must be the card's config.txt size, 1920x1080: at
#     1024x768 NT draws nothing but its blue background, and seems hung;
#   - after NT restarts itself the screen stays blue (the -bios boot), though
#     NT runs: quit QEMU and start it again instead.
# Put programs on the card with mtools: mcopy -i DISK.img@@1M FILE ::/CDV/
set -e
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
D=${2:-$(dirname "$IMG")}
cd "$D"
[ -f arcfw.bin ] || mcopy -i "$IMG@@1M" ::/arcfw.bin arcfw.bin
rm -f mon.sock
exec qemu-system-arm -M raspi2b -m 1G -bios arcfw.bin \
  -drive if=sd,format=raw,file="$IMG" -serial file:serial.log \
  -monitor unix:mon.sock,server,nowait -display none \
  -device ds1338,bus=i2c-bus.1,address=0x68 -rtc base=localtime -device usb-mouse -device usb-kbd \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2370-:2370,hostfwd=tcp:127.0.0.1:21118-:21118 -device usb-net,netdev=n0 \
  -global bcm2835-fb.xres=1920 -global bcm2835-fb.yres=1080
