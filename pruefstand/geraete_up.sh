#!/bin/sh
# Mehrprozessor: vier SOCKEL, nicht vier Kerne -- SeaBIOS traegt in die
# MPS-Tabelle einen Prozessor je Sockel ein, und NT 4.0 (halmps) kennt nur
# diese Tabelle.  Bei "-smp 4" macht QEMU 8 einen Sockel mit vier Kernen,
# und NT sieht einen Prozessor.  (Mit MP=1 aus einbau.py: ntkrnlmp + halmps.)
# xHCI mit Tastatur, Maus, Tablett und Stick -- viel gleichzeitige Last.
cd /home/claude/vm
./start.sh \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4 \
  -device usb-kbd,bus=xhci.0,port=5,id=ukbd,display=vga0 \
  -device usb-mouse,bus=xhci.0,port=6,id=umaus \
  -device usb-tablet,bus=xhci.0,port=7,id=utab \
  -drive if=none,id=stick,file=stick_ss.img,format=raw \
  -device usb-storage,bus=xhci.0,port=1,drive=stick,id=stickdev
