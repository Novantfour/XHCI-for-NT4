#!/bin/sh
# Kombi-Kartenleser am NEC-xHCI: ein Bulk-Only-Geraet, zwei Schaechte (LUN 0 und 1)
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off \
  -device usb-bot,bus=xhci.0,id=leser \
  -drive if=none,id=l0,file=fatstick.img,format=raw \
  -device scsi-hd,bus=leser.0,scsi-id=0,lun=0,drive=l0,removable=on \
  -drive if=none,id=l1,file=karte2.img,format=raw,readonly=on \
  -device scsi-hd,bus=leser.0,scsi-id=0,lun=1,drive=l1,removable=on \
  -device usb-kbd,bus=xhci.0,id=ukbd,display=vga0
