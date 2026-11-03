#!/bin/sh
# Maus anstecken, MAUSLATENZ starten, 6 Klicks WAEHREND dauernder Bewegung.
cd /home/claude/vm
for t in 1 2 3 4 5 6; do timeout 10 python3 agent.py PING 2>/dev/null | grep -q PONG && echo "Agent da" && break; sleep 10; done
timeout 8 python3 mon.py chardev-add socket,id=ls,path=/tmp/ls.sock >/dev/null 2>&1; timeout 8 python3 mon.py device_add usb-redir,chardev=ls,id=rls,bus=xhci.0,port=4 >/dev/null 2>&1; sleep 12
( timeout 100 python3 agent.py 'IA C:\TEMP\MAUSLATENZ.EXE' > /tmp/ml_ia.txt 2>&1 & ); sleep 8
setsid nohup timeout 40 python3 /tmp/bewegen.py 14 >/dev/null 2>&1 &
sleep 2
for k in 1 2 3 4 5 6; do python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('/tmp/ls_ctl.sock'); s.sendall(b'knopf 1 1\n'); s.recv(50)"; sleep 0.15; python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('/tmp/ls_ctl.sock'); s.sendall(b'knopf 1 0\n'); s.recv(50)"; sleep 1.5; done
sleep 30; cat /tmp/bewegen.txt 2>/dev/null
