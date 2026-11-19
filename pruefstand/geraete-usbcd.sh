#!/bin/sh
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4 \
  -drive if=none,id=stick,file=stick.img,format=raw \
  -device usb-storage,bus=xhci.0,drive=stick \
  -device usb-kbd,bus=xhci.0,id=ukbd,display=vga0 \
  -device usb-tablet,bus=xhci.0,id=utab \
  -device ich9-usb-ehci1,id=ehci \
  -device ich9-usb-uhci1,masterbus=ehci.0,firstport=0 \
  -device ich9-usb-uhci2,masterbus=ehci.0,firstport=2 \
  -device ich9-usb-uhci3,masterbus=ehci.0,firstport=4 \
  -device usb-hub,bus=ehci.0,port=1,id=hub \
  -device usb-kbd,bus=ehci.0,port=1.1,id=ukbd2,display=vga0,usb_version=1 \
  -device usb-mouse,bus=ehci.0,port=3,id=umaus,usb_version=1 \
  -drive if=none,id=ucd,media=cdrom,file=/tmp/test.iso -device usb-bot,bus=xhci.0,id=bot -device scsi-cd,bus=bot.0,drive=ucd -device pci-ohci,id=ohci -device usb-mouse,bus=ohci.0,id=om
