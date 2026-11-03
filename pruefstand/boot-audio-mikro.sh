# boot9.sh -- Pruefgast mit xHCI und emuliertem USB-Mikrofon (pademu --art mikro)
sh halt.sh; sleep 1; rm -f /tmp/ton.wav /tmp/com2.sock /tmp/mon.sock /tmp/com1.txt /tmp/pad.sock /tmp/pad.ctl
setsid qemu-system-i386 -m 128 -cpu pentium2 -vga cirrus -drive file=test2.qcow2,format=qcow2,cache=writethrough -display none \
 -netdev user,id=n0 -device pcnet,netdev=n0,addr=3 -device qemu-xhci,id=hc,p2=8,p3=8 \
 -audiodev wav,id=snd0,path=/tmp/ton.wav -device usb-audio,audiodev=snd0,bus=hc.0 -chardev socket,id=pad,path=/tmp/pad.sock,server=on,wait=off -device usb-redir,chardev=pad,bus=hc.0 \
 -serial file:/tmp/com1.txt -serial unix:/tmp/com2.sock,server=on,wait=off -monitor unix:/tmp/mon.sock,server=on,wait=off \
 -rtc base=localtime -pidfile /tmp/qemu.pid > /tmp/qemu.log 2>&1 < /dev/null &
sleep 2
setsid python3 usb/usb-nt4/pruefstand/pademu.py --art mikro > /tmp/pad.log 2>&1 < /dev/null &
echo $! > /tmp/pad.pid
