#!/bin/sh
# Wie geraete_direkt.sh, dazu ein USB-Stick -- fuer Pruefungen, bei denen
# auch die Massenspeicher-Einrichtung durch den xHCI laufen soll.
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4 \
  -device usb-kbd,bus=xhci.0,id=ukbd,display=vga0 \
  -drive if=none,id=stick,file=stick.img,format=raw \
  -device usb-storage,bus=xhci.0,drive=stick
