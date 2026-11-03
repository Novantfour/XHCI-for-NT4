# Eingabeflut ueber QMP: Mausbewegungen, alle 10 eine Umschalttaste.
# Laeuft als eigener Prozess; schreibt Zaehler nach /tmp/flut.txt.
import socket, json, time, random, sys
dauer = int(sys.argv[1]) if len(sys.argv) > 1 else 60
NUR_MAUS = 'maus' in sys.argv[2:]          # ohne Tastendruecke
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/qmp.sock'); s.settimeout(5)
f = s.makefile('rw'); f.readline()
def qmp(c, a=None):
    f.write(json.dumps({"execute": c, "arguments": a} if a else {"execute": c}) + "\n"); f.flush()
    while True:                                   # Ereignisse ueberspringen
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
qmp("qmp_capabilities"); n = 0; t0 = time.time()
while time.time() - t0 < dauer:
    qmp("input-send-event", {"events": [{"type": "rel", "data": {"axis": "x", "value": random.randint(-20, 20)}},
                                        {"type": "rel", "data": {"axis": "y", "value": random.randint(-20, 20)}}]})
    if n % 10 == 0 and not NUR_MAUS:
        for down in (True, False):
            qmp("input-send-event", {"device": "vga0", "events": [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": "shift"}}}]})
    n += 1; time.sleep(0.02)
open('/tmp/flut.txt', 'w').write("Mausbewegungen %d, Tastendruecke %d\n" % (n, n // 10))
