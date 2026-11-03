#!/usr/bin/env python3
# mon.py "befehl" -- einen Befehl an den QEMU-Monitor, Antwort ausgeben
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/mon.sock'); s.settimeout(3)
time.sleep(0.2)
try: s.recv(65536)
except: pass
s.send((' '.join(sys.argv[1:]) + '\n').encode())
time.sleep(0.8)
out=b''
try:
    while True:
        b=s.recv(65536)
        if not b: break
        out+=b
except: pass
print(out.decode(errors='ignore').replace('\r',''))
