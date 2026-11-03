#!/bin/sh
# geraete_sb7.sh -- wie SB700: EHCI mit sechs Buchsen, zwei OHCI-Begleiter
cd /home/claude/vm
./start.sh \
  -device ich9-usb-ehci1,id=ehci \
  -device pci-ohci,id=ohci0,masterbus=ehci.0,firstport=0,num-ports=3 \
  -device pci-ohci,id=ohci1,masterbus=ehci.0,firstport=3,num-ports=3 \
  -drive if=none,id=stick,file=stick.img,format=raw \
  -device usb-storage,bus=ehci.0,port=1,drive=stick \
  -device usb-kbd,bus=ehci.0,port=2,id=ukbd,display=vga0,usb_version=1 \
  -device usb-mouse,bus=ehci.0,port=3,id=umaus,usb_version=1 \
  -device usb-tablet,bus=ehci.0,port=4,id=utab \
  -chardev file,id=ser,path=/tmp/usbser.txt -device usb-serial,bus=ehci.0,port=5,chardev=ser
