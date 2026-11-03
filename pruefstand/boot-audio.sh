# boot8.sh <uhci|ohci|xhci> -- Pruefgast mit QEMU usb-audio (UAC1), Ausgabe nach /tmp/ton.wav
sh halt.sh; sleep 1; rm -f /tmp/com2.sock /tmp/mon.sock /tmp/com1.txt /tmp/ton.wav
case "${1:-xhci}" in
 uhci) HC="-device piix3-usb-uhci,id=hc";;
 ohci) HC="-device pci-ohci,id=hc";;
 xhci) HC="-device qemu-xhci,id=hc,p2=8,p3=8";;
esac
setsid qemu-system-i386 -m 128 -cpu pentium2 -vga cirrus -drive file=test2.qcow2,format=qcow2,cache=writethrough -display none \
 -netdev user,id=n0 -device pcnet,netdev=n0,addr=3 $HC \
 -audiodev wav,id=snd0,path=/tmp/ton.wav -device usb-audio,audiodev=snd0,bus=hc.0 \
 -serial file:/tmp/com1.txt -serial unix:/tmp/com2.sock,server=on,wait=off -monitor unix:/tmp/mon.sock,server=on,wait=off \
 -rtc base=localtime -pidfile /tmp/qemu.pid > /tmp/qemu.log 2>&1 < /dev/null &
