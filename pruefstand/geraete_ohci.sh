#!/bin/sh
# Wie beim Anwender: USB-Maus am OHCI (AMD SB7x0: 00:12.1), zwei Prozessoren.
# Der Mausnachbau (lsgeraet.py) wird danach per device_add an ohci.0 gehaengt.
cd /home/claude/vm
./start.sh -smp 4,sockets=4,cores=1,threads=1 \
  -device pci-ohci,id=ohci,num-ports=4 \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4 \
  -device usb-kbd,bus=xhci.0,port=5,id=ukbd,display=vga0
