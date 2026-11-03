#!/bin/sh
# start.sh <usb-geraete...>  -- NT 4.0 unter QEMU, ohne Bildschirm
# Bildschirm per Monitor: screendump.  Serielle Ausgabe: com1.log
cd /home/claude/vm
rm -f /tmp/mon.sock /tmp/qmp.sock /tmp/com2.sock com1.log
setsid nohup qemu-system-i386 -M pc -cpu pentium3 -m 128 \
  -drive file=nt4_smal.img,format=raw,if=ide,index=0 \
  -vga none -device VGA,id=vga0 -display none \
  -serial file:com1.log \
  -serial unix:/tmp/com2.sock,server,nowait \
  -monitor unix:/tmp/mon.sock,server,nowait \
  -qmp unix:/tmp/qmp.sock,server,nowait \
  -rtc base=localtime -no-reboot -parallel none \
  "$@" > qemu.out 2>&1 &
echo $! > qemu.pid
