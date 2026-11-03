# boot4.sh <art1> <art2> -- Pruefgast auf dem fertigen Abbild, zwei Controller
sh halt.sh; [ -f /tmp/pad2.pid ] && kill $(cat /tmp/pad2.pid) 2>/dev/null
sleep 1; rm -f /tmp/com2.sock /tmp/mon.sock /tmp/pad.sock /tmp/pad2.sock /tmp/pad.ctl /tmp/pad2.ctl /tmp/com1.txt
setsid qemu-system-i386 -m 128 -cpu pentium2 -vga cirrus -drive file=test2.qcow2,format=qcow2,cache=writethrough -display none \
 -netdev user,id=n0 -device pcnet,netdev=n0,addr=3 \
 -device qemu-xhci,id=xhci,p2=8,p3=8 \
 -chardev socket,id=pad,path=/tmp/pad.sock,server=on,wait=off -device usb-redir,chardev=pad,bus=xhci.0 \
 -drive if=none,id=stick,file=stick.img,format=raw -device usb-storage,bus=xhci.0,drive=stick -chardev socket,id=pad2,path=/tmp/pad2.sock,server=on,wait=off -device usb-redir,chardev=pad2,bus=xhci.0 \
 -serial file:/tmp/com1.txt -serial unix:/tmp/com2.sock,server=on,wait=off \
 -monitor unix:/tmp/mon.sock,server=on,wait=off -rtc base=localtime \
 -pidfile /tmp/qemu.pid > /tmp/qemu.log 2>&1 < /dev/null &
sleep 2
setsid python3 usb/usb-nt4/pruefstand/pademu.py --art ${1:-360} > /tmp/pad.log 2>&1 < /dev/null &
echo $! > /tmp/pad.pid
setsid python3 usb/usb-nt4/pruefstand/pademu.py --art ${2:-one} --sock /tmp/pad2.sock --steuer /tmp/pad2.ctl > /tmp/pad2.log 2>&1 < /dev/null &
echo $! > /tmp/pad2.pid
