#!/bin/sh
# geraete_iso.sh -- isochron an OHCI (wie SB7x0) und an xHCI (wie NEC-Karte)
cd /home/claude/vm
rm -f /tmp/qtrace.log
./start.sh -D /tmp/qtrace.log -trace usb_xhci_queue_event -trace usb_xhci_irq_intx -trace usb_xhci_runtime_write -trace usb_xhci_port_notify -trace usb_xhci_oper_write -trace usb_xhci_xfer_start -trace usb_xhci_xfer_success -trace usb_xhci_xfer_error -trace usb_xhci_ep_kick \
  -device ich9-usb-ehci1,id=ehci \
  -device pci-ohci,id=ohci0,masterbus=ehci.0,firstport=0,num-ports=3 \
  -device pci-ohci,id=ohci1,masterbus=ehci.0,firstport=3,num-ports=3 \
  -device nec-usb-xhci,id=xhci,msi=off,msix=off \
  -audiodev none,id=sndo,out.frequency=48000,out.channels=2,out.format=s16 \
  -device usb-audio,bus=ehci.0,port=1,audiodev=sndo \
  -audiodev none,id=sndx,out.frequency=48000,out.channels=2,out.format=s16 \
  -device usb-audio,bus=xhci.0,audiodev=sndx \
  -device usb-tablet,bus=xhci.0,id=utab \
  -chardev file,id=ser,path=/tmp/usbser.txt -device usb-serial,bus=ehci.0,port=2,chardev=ser
