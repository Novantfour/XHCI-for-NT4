#!/bin/sh
# NEC-xHCI mit freien Anschluessen: usbredir-Geraete (udruck.py, xpad.py)
# haengen DIREKT am xHCI -- QEMU verlangt dann von ihnen 64-Bit-Kennungen,
# 32-Bit-Bulk-Laengen und ep_info_max_packet_size.  (Sind die Anschluesse
# fast voll, schiebt QEMU selbst einen USB-1.1-Hub dazwischen; dahinter
# prueft es das nicht -- so war es in geraete.sh.)
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4 \
  -device usb-kbd,bus=xhci.0,id=ukbd,display=vga0
