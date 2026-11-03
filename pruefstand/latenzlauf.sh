#!/bin/sh
# Ein Latenzlauf: Treiber $1 einbauen und starten (Messung danach mit latenzmessen.sh).
cd /home/claude/vm
for p in $(pgrep -x qemu-system-i38); do kill $p; done; [ -f /tmp/ls.pid ] && kill $(cat /tmp/ls.pid) 2>/dev/null; sleep 2
setsid nohup python3 /home/claude/vm/lsgeraet.py --maus >/dev/null 2>&1 &
echo $! > /tmp/ls.pid
mkdir -p /tmp/lauf && cp "$1" /tmp/lauf/usbnt.sys
DATEIEN="/tmp/mauslatenz.exe=/TEMP/MAUSLATENZ.EXE" MP=${MPLAUF:-} NT4_BASIS=neu timeout 200 python3 einbau.py /tmp/lauf/usbnt.sys 1 2>&1 | grep -E "Abbild fertig"
rm -f /tmp/usbser.txt; if [ "$MPLAUF" = 1 ]; then timeout 20 ./geraete_mp.sh >/dev/null; else timeout 20 ./geraete_up.sh >/dev/null; fi; echo gestartet
