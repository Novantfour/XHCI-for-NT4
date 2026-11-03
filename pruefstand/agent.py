#!/usr/bin/env python3
"""
agent.py "befehl"   -- einen Befehl an den NT-4.0-Gastagenten (COM2).
agent.py -w datei ziel  -- eine Datei in den Gast schreiben (base64 ueber cmd)

Der Wirt oeffnet den Unix-Socket, an dem QEMUs COM2 haengt, schickt eine
Zeile und liest, bis die Schlusszeile mit 0x04 EXIT kommt.
"""
import socket, sys, time, os, base64

SOCK = '/tmp/com2.sock'

def verbinde():
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    s.settimeout(2)
    return s

def leere(s):
    try:
        while s.recv(4096): pass
    except: pass

def ruf(s, befehl, gesamt_timeout=60):
    s.send((befehl + '\n').encode())
    out = b''
    ende = time.time() + gesamt_timeout
    while time.time() < ende:
        try:
            b = s.recv(4096)
        except socket.timeout:
            continue
        if not b:
            break
        out += b
        # Schlusszeile?  0x04 EXIT <code> \n
        i = out.rfind(b'\x04EXIT ')
        if i >= 0 and out.find(b'\n', i) >= 0:
            code = out[i+6:out.find(b'\n', i)].decode(errors='ignore').strip()
            text = out[:i].replace(b'\x04', b'')
            return text.decode(errors='ignore'), code
    return out.replace(b'\x04', b'').decode(errors='ignore'), '?'

def put(quelle, ziel):
    """Datei roh in den Gast: PUT <pfad> <laenge>, dann die Bytes."""
    d = open(quelle, 'rb').read()
    s = verbinde(); leere(s)
    s.send(('PUT %s %d\n' % (ziel, len(d))).encode())
    time.sleep(0.2)
    # Ohne Zeitlimit senden: die serielle Leitung schafft rund 11 kB/s und
    # nimmt nur so schnell an, wie der Gast liest -- 1 MB dauert 90 s.  Mit
    # dem Lese-Zeitlimit von 2 s brach sendall ab, und der Agent im Gast
    # wartete dann auf den Rest der Datei (so geschehen mit 1 MB).
    s.settimeout(None)
    s.sendall(d)
    s.settimeout(2)
    text, code = ruf(s, '', 30) if False else (None, None)
    # auf die Schlusszeile warten
    out = b''; ende = time.time() + 30
    while time.time() < ende:
        try: b = s.recv(4096)
        except socket.timeout: continue
        if not b: break
        out += b
        if b'\x04EXIT' in out and out.endswith(b'\n'): break
    ok = b'EXIT 0' in out
    print('%s -> %s  %d Byte  %s' % (quelle, ziel, len(d), 'ok' if ok else out.decode(errors='ignore')))

def get(quelle, ziel):
    """Datei roh aus dem Gast: GET <pfad> -> LEN n, Bytes, Schluss."""
    s = verbinde(); leere(s)
    s.send(('GET %s\n' % quelle).encode())
    out = b''; ende = time.time() + 30
    while time.time() < ende:
        try: b = s.recv(65536)
        except socket.timeout: continue
        if not b: break
        out += b
        i = out.find(b'\n')
        if out.startswith(b'LEN ') and i > 0:
            n = int(out[4:i]); rest = out[i+1:]
            if len(rest) >= n + len(b'\x04EXIT 0\n'):
                open(ziel, 'wb').write(rest[:n])
                print('%s -> %s  %d Byte' % (quelle, ziel, n)); return
    print('GET unvollstaendig')

def main():
    if sys.argv[1] == 'put':
        put(sys.argv[2], sys.argv[3]); return
    if sys.argv[1] == 'get':
        get(sys.argv[2], sys.argv[3]); return
    if sys.argv[1] == '-w':
        quelle, ziel = sys.argv[2], sys.argv[3]
        d = open(quelle, 'rb').read()
        b64 = base64.b64encode(d).decode()
        s = verbinde(); leere(s)
        # in Haeppchen, damit die Befehlszeile nicht platzt
        ruf(s, 'del ' + ziel + '.b64 2>nul')
        for i in range(0, len(b64), 400):
            ruf(s, 'echo ' + b64[i:i+400] + '>>' + ziel + '.b64')
        # dekodieren -- NT4 hat kein base64; wir schieben es per debug? Nein:
        # einfacher ueber den Agenten selbst spaeter.  Vorerst nur .b64 ablegen.
        print("uebertragen als", ziel + '.b64', "(', len(d), 'Byte)")
        return
    s = verbinde(); leere(s)
    text, code = ruf(s, ' '.join(sys.argv[1:]))
    sys.stdout.write(text)
    if not text.endswith('\n'):
        sys.stdout.write('\n')
    sys.stderr.write('[EXIT %s]\n' % code)

if __name__ == '__main__':
    main()
