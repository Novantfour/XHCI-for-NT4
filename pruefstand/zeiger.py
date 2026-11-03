#!/usr/bin/env python3
# zeiger.py -- Monitorbefehle ueber EINE Verbindung, in Schritten unter der
# NT-Beschleunigungsschwelle (6 Punkte), damit die Position genau stimmt.
import socket, sys, time
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/mon.sock'); s.settimeout(0.3)
def cmd(c, pause=0.06):
    s.send((c+'\n').encode()); time.sleep(pause)
    try:
        while s.recv(65536): pass
    except socket.timeout: pass
def gehe(dx, dy):
    while dx or dy:
        sx = max(-5, min(5, dx)); sy = max(-5, min(5, dy))
        cmd('mouse_move %d %d' % (sx, sy)); dx -= sx; dy -= sy
time.sleep(0.3)
for a in sys.argv[1:]:
    if a == 'ecke':   gehe(-700, -500)
    elif a == 'ab':   cmd('mouse_button 1', 0.3)
    elif a == 'auf':  cmd('mouse_button 0', 0.3)
    elif a == 'doppel':
        cmd('mouse_button 1',0.05); cmd('mouse_button 0',0.05); cmd('mouse_button 1',0.05); cmd('mouse_button 0',0.3)
    else:
        dx, dy = map(int, a.split(',')); gehe(dx, dy)
