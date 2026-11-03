[ -f /tmp/pad.pid ] && kill $(cat /tmp/pad.pid) 2>/dev/null
[ -f /tmp/qemu.pid ] && kill $(cat /tmp/qemu.pid) 2>/dev/null
rm -f /tmp/qemu.pid /tmp/pad.pid; true
