# boot7.sh <uhci|ohci|ehci|xhci> -- Pruefgast mit genau einem Controllertyp
sh halt.sh; sleep 1; rm -f /tmp/com2.sock /tmp/mon.sock /tmp/com1.txt
case "$1" in
 uhci) HC="-device piix3-usb-uhci,id=hc"; GER="-device usb-kbd,bus=hc.0 -device usb-mouse,bus=hc.0 -device usb-storage,bus=hc.0,drive=stick";;
 ohci) HC="-device pci-ohci,id=hc";       GER="-device usb-kbd,bus=hc.0 -device usb-mouse,bus=hc.0 -device usb-storage,bus=hc.0,drive=stick";;
 ehci) HC="-device usb-ehci,id=hc";       GER="-device usb-storage,bus=hc.0,drive=stick";;
 xhci) HC="-device qemu-xhci,id=hc,p2=8,p3=8"; GER="-device usb-kbd,bus=hc.0 -device usb-mouse,bus=hc.0 -device usb-storage,bus=hc.0,drive=stick";;
esac
setsid qemu-system-i386 -m 128 -cpu pentium2 -vga cirrus -drive file=test2.qcow2,format=qcow2,cache=writethrough -display none \
 -netdev user,id=n0 -device pcnet,netdev=n0,addr=3 $HC -drive if=none,id=stick,file=stick.img,format=raw $GER \
 -serial file:/tmp/com1.txt -serial unix:/tmp/com2.sock,server=on,wait=off -monitor unix:/tmp/mon.sock,server=on,wait=off \
 -rtc base=localtime -pidfile /tmp/qemu.pid > /tmp/qemu.log 2>&1 < /dev/null &
