#!/bin/sh
# geraete_ich9.sh -- Intel-Aufbau: EHCI mit drei UHCI-Begleitern, dazu ein Hub
cd /home/claude/vm
./start.sh \
  -device ich9-usb-ehci1,id=ehci \
  -device ich9-usb-uhci1,masterbus=ehci.0,firstport=0 \
  -device ich9-usb-uhci2,masterbus=ehci.0,firstport=2 \
  -device ich9-usb-uhci3,masterbus=ehci.0,firstport=4 \
  -device usb-hub,bus=ehci.0,port=1,id=hub \
  -device usb-kbd,bus=ehci.0,port=1.1,id=ukbd,display=vga0,usb_version=1 \
  -device usb-tablet,bus=ehci.0,port=1.2,id=utab \
  -chardev file,id=ser,path=/tmp/usbser.txt -device usb-serial,bus=ehci.0,port=1.3,chardev=ser \
  -device usb-mouse,bus=ehci.0,port=3,id=umaus,usb_version=1 \
  -drive if=none,id=stick,file=stick.img,format=raw \
  -device usb-storage,bus=ehci.0,port=5,drive=stick \
  -audiodev none,id=snd,out.frequency=48000,out.channels=2,out.format=s16 \
  -device usb-audio,bus=ehci.0,port=4,audiodev=snd
