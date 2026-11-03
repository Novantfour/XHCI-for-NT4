#!/usr/bin/env python3
# bewege.py <geraet> -- absolute Zeigerbewegungen und einen Klick an EIN Geraet
import socket, json, sys, time
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/qmp.sock'); f = s.makefile('rw')
def rufe(c, a=None):
    f.write(json.dumps({'execute': c, 'arguments': a or {}}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
json.loads(f.readline()); rufe('qmp_capabilities')
g = sys.argv[1]
for x, y in ((1000, 2000), (8000, 9000), (16000, 16000), (30000, 25000)):
    print(rufe('input-send-event', {'events': [
        {'type': 'abs', 'data': {'axis': 'x', 'value': x}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': y}}]}))
    time.sleep(1.2)
for unten in (True, False):
    rufe('input-send-event', {'events': [
        {'type': 'btn', 'data': {'down': unten, 'button': 'left'}}]})
    time.sleep(1.2)
