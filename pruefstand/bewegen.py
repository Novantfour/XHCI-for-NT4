# Maus dauernd bewegen: alle 10 ms ein Schritt, so lange wie angegeben.
import socket, sys, time
dauer = float(sys.argv[1]); t0 = time.time(); n = 0
while time.time() - t0 < dauer:
    s = socket.socket(socket.AF_UNIX); s.connect('/tmp/ls_ctl.sock')
    s.sendall(b'bewege 1 1\n'); s.recv(50); s.close(); n += 1; time.sleep(0.01)
open('/tmp/bewegen.txt', 'w').write('%d Bewegungen\n' % n)
