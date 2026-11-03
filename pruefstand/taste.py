#!/usr/bin/env python3
"""
taste.py <geraet> <tasten...>  -- Tastendruecke per QMP an EIN Geraet.

Mit dem Monitorbefehl sendkey entscheidet QEMU, welche Tastatur sie
bekommt.  input-send-event mit "device" schickt sie an genau das
benannte Geraet -- damit ist belegt, welcher Treiberpfad sie traegt.

    taste.py ukbd ret
    taste.py ukbd ctrl alt delete    (gleichzeitig)
    taste.py ukbd -t hallo           (Zeichen fuer Zeichen)
"""
import socket, json, sys, time
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/qmp.sock')
f = s.makefile('rw')
def rufe(cmd, args=None):
    f.write(json.dumps({'execute': cmd, 'arguments': args or {}}) + '\n'); f.flush()
    while True:
        a = json.loads(f.readline())
        if 'return' in a or 'error' in a:
            return a
json.loads(f.readline()); rufe('qmp_capabilities')
geraet = sys.argv[1]
def druck(tasten, unten):
    return rufe('input-send-event', {'device': geraet, 'events': [
        {'type': 'key', 'data': {'down': unten, 'key': {'type': 'qcode', 'data': t}}}
        for t in tasten]})
if sys.argv[2] == '-t':
    for c in sys.argv[3]:
        druck([c], True); time.sleep(0.05); druck([c], False); time.sleep(0.05)
else:
    t = sys.argv[2:]
    a = druck(t, True); time.sleep(0.15); druck(list(reversed(t)), False)
    print(a)
