#!/bin/sh
# NEC-xHCI: Tastatur an QEMU-Anschluss 5 (nur USB 2), Stick an Anschluss 2
# (QEMU-Anschluesse 1..4 sind USB 2 UND 3; ein SuperSpeed-Geraet landet dort
# auf der USB-3-Seite, beim NEC also an seinem Anschluss 1..4).  QEMUs
# usb-storage bietet SuperSpeed-Deskriptoren an und laeuft dort mit 5 Gbit/s.
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4 \
  -device usb-kbd,bus=xhci.0,port=5,id=ukbd,display=vga0 \
  -drive if=none,id=stick,file=stick_ss.img,format=raw \
  -device usb-storage,bus=xhci.0,port=2,drive=stick
