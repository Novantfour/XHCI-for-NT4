#!/usr/bin/env python3
# klick.py <x> <y> [doppel] -- Zeiger setzen und links klicken (640x480)
import socket, json, sys, time
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/qmp.sock'); f=s.makefile('rw')
def rufe(c,a=None):
    f.write(json.dumps({'execute':c,'arguments':a or {}})+'\n'); f.flush()
    while True:
        r=json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
json.loads(f.readline()); rufe('qmp_capabilities')
x=int(sys.argv[1])*32767//640; y=int(sys.argv[2])*32767//480
rufe('input-send-event',{'device':'utab','events':[{'type':'abs','data':{'axis':'x','value':x}},{'type':'abs','data':{'axis':'y','value':y}}]})
time.sleep(0.2)
for n in range(2 if len(sys.argv)>3 else 1):
    rufe('input-send-event',{'device':'utab','events':[{'type':'btn','data':{'down':True,'button':'left'}}]}); time.sleep(0.1)
    rufe('input-send-event',{'device':'utab','events':[{'type':'btn','data':{'down':False,'button':'left'}}]}); time.sleep(0.1)
