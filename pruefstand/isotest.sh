#!/bin/sh
# isotest.sh <AC-UsbDev> <AS-UsbDev> <audiodev> <wav>
#   Lautstaerke auf Faktor 1, Alternative 1, 100 Rahmen Pruefmuster,
#   Mitschnitt auf dem Wirt, Vergleich Byte fuer Byte.
cd /home/claude/vm
A="timeout 30 python3 agent.py"
for k in 1 2; do $A "usbgeraet $1 steuer 21 01 020$k 0200 2 -x 00 08" >/dev/null 2>&1; done
$A "usbgeraet $2 alt 1" 2>/dev/null | tr -d '\r' | grep -o "Alternative.*"
rm -f $4; timeout 8 python3 mon.py wavcapture $4 $3 48000 16 2 >/dev/null; sleep 1
$A "usbgeraet $2 iso 01 100 192" 2>/dev/null | tr -d '\r' | grep -o "isochron.*"
sleep 2; N=$(timeout 8 python3 mon.py info capture | grep -oE "^\[[0-9]+\]" | tail -1 | tr -d '[]')
timeout 8 python3 mon.py info capture | grep -oE "Capturing.*" | tail -1
timeout 8 python3 mon.py stopcapture ${N:-0} >/dev/null; sleep 1
python3 - "$4" <<'PY'
import struct, sys
d = open(sys.argv[1],'rb').read()
i = d.find(b'data'); n = struct.unpack_from('<I', d, i+4)[0]; pcm = d[i+8:i+8+n]
soll = b''.join(struct.pack('<H', (0x1000*(k & 7) + j) & 0xFFFF) for k in range(100) for j in range(96))
f = sum(1 for x in range(min(len(pcm), len(soll))) if pcm[x] != soll[x])
print("  Vergleich: %d Byte angekommen, %d abweichend%s" % (len(pcm), f, "  -> BITGENAU" if f == 0 and len(pcm) == len(soll) else ""))
PY
