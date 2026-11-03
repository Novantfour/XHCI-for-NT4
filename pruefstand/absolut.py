#!/usr/bin/env python3
# absolut.py <x> <y>  -- Zeiger ueber das Tablett genau setzen (Bildschirm 640x480)
import socket, json, sys
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/qmp.sock'); f=s.makefile('rw')
def rufe(c,a=None):
    f.write(json.dumps({'execute':c,'arguments':a or {}})+'\n'); f.flush()
    while True:
        r=json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
json.loads(f.readline()); rufe('qmp_capabilities')
x=int(sys.argv[1])*32767//640; y=int(sys.argv[2])*32767//480
print(rufe('input-send-event',{'events':[{'type':'abs','data':{'axis':'x','value':x}},{'type':'abs','data':{'axis':'y','value':y}}]}))
