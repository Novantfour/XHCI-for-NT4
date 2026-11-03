#!/bin/sh
# FAT-Stick am NEC-xHCI; ein zweiter Stick (superstick) steht zum Anstecken bereit
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off \
  -drive if=none,id=fstick,file=fatstick.img,format=raw \
  -device usb-storage,bus=xhci.0,drive=fstick,id=ustick \
  -drive if=none,id=sstick,file=superstick.img,format=raw \
  -device usb-kbd,bus=xhci.0,id=ukbd,display=vga0 \
  -device usb-mouse,bus=xhci.0,id=umaus
