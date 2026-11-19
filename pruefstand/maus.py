#!/usr/bin/env python3
# maus.py dx dy [links|rechts]  -- relative USB-Maus (umaus) in 4-px-Schritten
import socket, json, sys, time
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/qmp.sock'); f=s.makefile('rw')
def rufe(c,a=None):
    f.write(json.dumps({'execute':c,'arguments':a or {}})+'\n'); f.flush()
    while True:
        r=json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
json.loads(f.readline()); rufe('qmp_capabilities')
dx,dy=int(sys.argv[1]),int(sys.argv[2])
while dx or dy:
    sx=max(-4,min(4,dx)); sy=max(-4,min(4,dy))
    rufe('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':sx}},{'type':'rel','data':{'axis':'y','value':sy}}]})
    dx-=sx; dy-=sy; time.sleep(0.012)
if len(sys.argv)>3:
    b='left' if sys.argv[3]=='links' else 'right'
    time.sleep(0.2)
    rufe('input-send-event',{'events':[{'type':'btn','data':{'down':True,'button':b}}]}); time.sleep(0.15)
    rufe('input-send-event',{'events':[{'type':'btn','data':{'down':False,'button':b}}]})
