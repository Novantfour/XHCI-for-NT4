#!/bin/sh
# geraete.sh -- die VM mit einer ganzen Sammlung USB-Geraete am NEC-xHCI
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4 \
  -drive if=none,id=stick,file=stick.img,format=raw \
  -device usb-storage,bus=xhci.0,drive=stick \
  -device usb-kbd,bus=xhci.0,id=ukbd,display=vga0 \
  -device usb-mouse,bus=xhci.0,id=umaus \
  -device usb-tablet,bus=xhci.0,id=utab \
  -chardev file,id=ser,path=/tmp/usbser.txt -device usb-serial,bus=xhci.0,chardev=ser \
  -device usb-ccid,bus=xhci.0 \
  -audiodev none,id=snd -device usb-audio,bus=xhci.0,audiodev=snd
