#!/bin/sh
# xHCI mit Stick, aber OHNE USB-Tastatur: Tasteneingaben ueber QEMU gehen
# dann an die PS/2-Tastatur -- zum Bedienen, solange noch kein Treiber da ist.
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4 \
  -drive if=none,id=stick,file=stick_ss.img,format=raw \
  -device usb-storage,bus=xhci.0,port=1,drive=stick
