#!/usr/bin/env python3
"""
lsgeraet.py -- Low-Speed-Maus oder -Tastatur fuer QEMU, ueber usbredir.

Echte Maeuse und Tastaturen sind fast immer Low-Speed-Geraete (1,5 Mbit/s):
Steuerendpunkt 8 Byte, Interrupt-Endpunkt hoechstens 8 Byte, Intervall
~10 ms.  QEMUs eigene usb-kbd/usb-mouse sind Full Speed -- ueber xHCI lief
deshalb nie ein Low-Speed-Geraet.  Dieser Nachbau schliesst die Luecke.

    python3 lsgeraet.py --maus       Logitech 046D:C077, 8 Tasten, 4-Byte-Berichte
    python3 lsgeraet.py --tastatur   Dell 413C:2003, Boot-Tastatur, 8 Byte

    Sockets: /tmp/ls.sock (QEMU), /tmp/ls_ctl.sock (Steuerung)
    QEMU:    chardev-add socket,id=ls,path=/tmp/ls.sock
             device_add usb-redir,chardev=ls,id=rls,bus=xhci.0

Steuerung:
    Maus:      bewege dx dy     knopf n 0|1 (n = 1..8)     rad n
    Tastatur:  taste <usage> 0|1   (HID-Usage hex, z. B. 04 = A, 39 = Caps)
               tippe <usage>       druecken, 50 ms, loslassen
    beide:     zaehler             gesendete Berichte
               led                 zuletzt per SET_REPORT gesetzte LEDs

Das usbredir-Protokoll wie in udruck.py/xpad.py: 64-Bit-Kennungen, erst
nach QEMUs HELLO melden.
"""
import os, socket, struct, sys, threading, time, select

SOCK, CTL, LOG = '/tmp/ls.sock', '/tmp/ls_ctl.sock', '/tmp/ls.log'
TASTATUR = '--tastatur' in sys.argv
# --joystick: HID-Flugknueppel (Klasse 03/00/00): X, Y, Drehung (Rz),
# Schubregler (Slider), Coolie-Hat (8 Richtungen), 12 Knoepfe.
# Befehl: joy <x> <y> <rz> <schub> <hut 0..7 oder 8=Mitte> <knoepfe hex>
JOYSTICK = '--joystick' in sys.argv
# --medien (mit --tastatur): zweite HID-Schnittstelle wie viele echte
# Tastaturen -- Consumer Control (Bericht 1, Liste mit 16-Bit-Nummer) und
# System Control (Bericht 2, Bitfeld 0x81..0x83).  Befehle:
#   medien <nutzung hex> 1|0     system <nutzung hex> 1|0
MEDIEN = '--medien' in sys.argv and '--tastatur' in sys.argv
# --voll: Full Speed, Intervall 1 ms (wie viele echte Maeuse an USB 2.0)
# --dauer N: alle N ms einen LEEREN Bericht senden -- wie eine Maus, die
#            SET_IDLE 0 nicht beachtet und dauernd meldet
VOLL  = '--voll' in sys.argv
DAUER = 0
if '--dauer' in sys.argv:
    DAUER = int(sys.argv[sys.argv.index('--dauer') + 1])

HELLO, DEV_CONNECT, RESET, IF_INFO, EP_INFO = 0, 1, 3, 4, 5
SET_CONF, GET_CONF, CONF_STATUS, SET_ALT, GET_ALT, ALT_STATUS = 6, 7, 8, 9, 10, 11
START_INT, STOP_INT, INT_STATUS = 15, 16, 17
CANCEL, FILTER_FILTER, DISC_ACK = 21, 23, 24
CONTROL, INTERRUPT = 100, 103
CAP_CONNECT_VERSION, CAP_EP_MPS, CAP_64BIT_IDS, CAP_32BIT_BULK = 1, 4, 5, 6

if TASTATUR:
    VID, PID, NAME, PROTO, MPS = 0x413C, 0x2003, 'USB Keyboard', 1, 8
    BERICHT = bytes([
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,             # Keyboard
        0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x08, 0x81, 0x02,             # 8 Umschalter
        0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             # reserviert
        0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
        0x91, 0x02,                                     # 5 LEDs (Ausgabe)
        0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
        0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00,
        0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x81, 0x00, # 6 Tasten (auch F13..F24)
        0xC0])
else:
    VID, PID, NAME, PROTO, MPS = 0x046D, 0xC077, 'USB Optical Mouse', 2, 4
    BERICHT = bytes([
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x08, 0x75, 0x01, 0x81, 0x02,             # 8 Knoepfe
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
        0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,  # X Y Rad relativ
        0xC0, 0xC0])

DEV = bytes([0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x08]) + struct.pack('<HHH', VID, PID, 0x0100) + bytes([0x01, 0x02, 0x00, 0x01])
_hid = bytes([0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22]) + struct.pack('<H', len(BERICHT))
_r = bytes([0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, PROTO, 0x00]) + _hid + \
     bytes([0x07, 0x05, 0x81, 0x03]) + struct.pack('<H', MPS) + bytes([0x01 if VOLL else 0x0A])
JOYBERICHT = bytes([
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01,                         # Joystick
    0x09, 0x01, 0xA1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0xC0,
    0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,   # Rz
    0x09, 0x36, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,   # Schub
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,                         # Hut, Mitte = ausserhalb
    0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0C, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x0C, 0x81, 0x02,                         # 12 Knoepfe
    0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
    0xC0])
joy_zustand = {'x': 128, 'y': 128, 'rz': 128, 'schub': 0, 'hut': 8, 'knoepfe': 0}
if JOYSTICK:
    # Eigene Kennung und Name, und 8 Byte Paketgroesse -- die Maus hat 4,
    # und QEMU verwarf die 7-Byte-Berichte des Knueppels als zu lang
    # (der Treiber sah null Berichte und startete die Leitung neu).
    VID, PID, NAME, MPS = 0x06A3, 0x0836, 'USB Flugknueppel', 8
    BERICHT = JOYBERICHT
    _hid = bytes([0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22]) + struct.pack('<H', len(BERICHT))
    _r = bytes([0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00]) + _hid + \
         bytes([0x07, 0x05, 0x81, 0x03]) + struct.pack('<H', 8) + bytes([0x0A])
    CONF = bytes([0x09, 0x02]) + struct.pack('<H', 9 + len(_r)) + bytes([0x01, 0x01, 0x00, 0xA0, 0x32]) + _r
    PROTO = 0
def joy_bericht():
    z = joy_zustand
    return bytes([z['x'], z['y'], z['rz'], z['schub'], z['hut'] & 0x0F,
                  z['knoepfe'] & 0xFF, (z['knoepfe'] >> 8) & 0x0F])
BERICHT2 = bytes([
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01,             # Consumer Control, Bericht 1
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0,                   # 1 x 16 Bit, Liste
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x02,             # System Control, Bericht 2
    0x19, 0x81, 0x29, 0x83, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
    0x95, 0x05, 0x81, 0x01, 0xC0])
_hid2 = bytes([0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22]) + struct.pack('<H', len(BERICHT2))
if MEDIEN:
    _r += bytes([0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00]) + _hid2 + \
          bytes([0x07, 0x05, 0x82, 0x03]) + struct.pack('<H', 8) + bytes([0x0A])
N_IF = 2 if MEDIEN else 1
CONF = bytes([0x09, 0x02]) + struct.pack('<H', 9 + len(_r)) + bytes([N_IF, 0x01, 0x00, 0xA0, 0x32]) + _r
bereit2 = []                                  # Berichte der zweiten Schnittstelle
medien_zustand = {'consumer': 0, 'system': 0}

def zk(s):
    b = s.encode('utf-16-le'); return bytes([2 + len(b), 3]) + b
STRINGS = {0: bytes([4, 3, 0x09, 0x04]), 1: zk('Logitech' if not TASTATUR else 'Dell'), 2: zk(NAME)}

sperre = threading.Lock()
zustand = {'knoepfe': 0, 'dx': 0, 'dy': 0, 'rad': 0, 'mods': 0, 'tasten': [], 'gesendet': 0,
           'led': None, 'protokoll': 1}
bereit = []                                 # Berichte, die hinaus sollen

def log(*a):
    with open(LOG, 'a') as f:
        f.write(time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a) + '\n')

def bericht_bauen():
    """Unter sperre aufrufen."""
    if TASTATUR:
        t = (zustand['tasten'] + [0] * 6)[:6]
        return bytes([zustand['mods'], 0] + t)
    def s8(v): return v & 0xFF
    b = bytes([zustand['knoepfe'], s8(zustand['dx']), s8(zustand['dy']), s8(zustand['rad'])])
    zustand['dx'] = zustand['dy'] = zustand['rad'] = 0
    return b

def schicke():
    with sperre:
        bereit.append(bericht_bauen())

def steuerung():
    try: os.unlink(CTL)
    except FileNotFoundError: pass
    s = socket.socket(socket.AF_UNIX); s.bind(CTL); s.listen(4)
    while True:
        c, _ = s.accept(); f = c.makefile('rw')
        for zeile in f:
            w = zeile.split(); antwort = 'ok'
            try:
                if w[0] == 'bewege':
                    dx, dy = int(w[1]), int(w[2])
                    while dx or dy:                       # in Schritten von hoechstens 127
                        sx = max(-127, min(127, dx)); sy = max(-127, min(127, dy))
                        with sperre: zustand['dx'], zustand['dy'] = sx, sy
                        schicke(); dx -= sx; dy -= sy
                elif w[0] == 'knopf':
                    bit = 1 << (int(w[1]) - 1)
                    with sperre:
                        zustand['knoepfe'] = (zustand['knoepfe'] | bit) if int(w[2]) else (zustand['knoepfe'] & ~bit)
                    schicke()
                elif w[0] == 'rad':
                    with sperre: zustand['rad'] = int(w[1])
                    schicke()
                elif w[0] == 'joy' and JOYSTICK:
                    for k, v in zip(('x', 'y', 'rz', 'schub', 'hut'), w[1:6]):
                        joy_zustand[k] = int(v)
                    if len(w) > 6:
                        joy_zustand['knoepfe'] = int(w[6], 16)
                    with sperre:
                        bereit.append(joy_bericht())
                elif w[0] in ('medien', 'system') and MEDIEN:
                    u, an = int(w[1], 16), bool(int(w[2]))
                    with sperre:
                        if w[0] == 'medien':
                            medien_zustand['consumer'] = u if an else 0
                            bereit2.append(bytes([0x01]) + struct.pack('<H', medien_zustand['consumer']))
                        else:
                            bit = 1 << (u - 0x81)
                            medien_zustand['system'] = (medien_zustand['system'] | bit) if an else (medien_zustand['system'] & ~bit)
                            bereit2.append(bytes([0x02, medien_zustand['system']]))
                elif w[0] in ('taste', 'tippe'):
                    u = int(w[1], 16)
                    def setze(an):
                        with sperre:
                            if 0xE0 <= u <= 0xE7:
                                m = 1 << (u - 0xE0)
                                zustand['mods'] = (zustand['mods'] | m) if an else (zustand['mods'] & ~m)
                            elif an and u not in zustand['tasten']:
                                zustand['tasten'].append(u)
                            elif not an and u in zustand['tasten']:
                                zustand['tasten'].remove(u)
                        schicke()
                    if w[0] == 'tippe':
                        setze(True); time.sleep(0.05); setze(False)
                    else:
                        setze(bool(int(w[2])))
                elif w[0] == 'zaehler':
                    antwort = str(zustand['gesendet'])
                elif w[0] == 'led':
                    antwort = 'led ' + ('-' if zustand['led'] is None else '%02x' % zustand['led'])
                else:
                    raise ValueError(w[0])
            except Exception as e:
                antwort = 'fehler ' + str(e)
            f.write(antwort + '\n'); f.flush()
        c.close()

kennung64 = [False]

def sende(c, art, nutz, kennung=0):
    if kennung64[0]: c.sendall(struct.pack('<IIQ', art, len(nutz), kennung) + nutz)
    else:            c.sendall(struct.pack('<III', art, len(nutz), kennung) + nutz)

def lies_genau(c, n):
    d = b''
    while len(d) < n:
        t = c.recv(n - len(d))
        if not t: raise ConnectionError('QEMU hat getrennt')
        d += t
    return d

def lies_kopf(c):
    return struct.unpack('<IIQ', lies_genau(c, 16)) if kennung64[0] else struct.unpack('<III', lies_genau(c, 12))

def schnittstellen(c):
    cls, sub, prot, nr = [0]*32, [0]*32, [0]*32, [0]*32
    cls[0], sub[0], prot[0] = 0x03, (0x00 if JOYSTICK else 0x01), PROTO
    if MEDIEN:
        nr[1], cls[1], sub[1], prot[1] = 1, 0x03, 0x00, 0x00
    sende(c, IF_INFO, struct.pack('<I', N_IF) + bytes(nr) + bytes(cls) + bytes(sub) + bytes(prot))
    art, iv, ifc, mps = [255]*32, [0]*32, [0]*32, [0]*32
    art[0] = art[16] = 0; mps[0] = mps[16] = 8                  # Low Speed: EP0 8 Byte
    art[17], iv[17], mps[17] = 3, (1 if VOLL else 10), MPS      # 81 IN
    if MEDIEN:
        art[18], iv[18], mps[18], ifc[18] = 3, 10, 8, 1          # 82 IN, Schnittstelle 1
    sende(c, EP_INFO, bytes(art) + bytes(iv) + bytes(ifc) + struct.pack('<32H', *mps))

def steuerpaket(c, kennung, kopf, daten):
    ep, req, rtyp, st, wert, idx, laenge = struct.unpack('<BBBBHHH', kopf)
    antwort, status = b'', 0
    if rtyp == 0x80 and req == 6:
        t, i = wert >> 8, wert & 0xFF
        antwort = DEV if t == 1 else CONF if t == 2 else STRINGS.get(i, b'') if t == 3 else b''
        if not antwort: status = 4
    elif rtyp == 0x81 and req == 6 and (wert >> 8) == 0x22:      # Berichtsdeskriptor
        antwort = BERICHT2 if (MEDIEN and idx == 1) else BERICHT
    elif rtyp == 0x81 and req == 6 and (wert >> 8) == 0x21:      # HID-Deskriptor
        antwort = _hid2 if (MEDIEN and idx == 1) else _hid
    elif rtyp == 0x21 and req == 0x0A:                           # SET_IDLE
        log('SET_IDLE', hex(wert))
    elif rtyp == 0x21 and req == 0x0B:                           # SET_PROTOCOL
        zustand['protokoll'] = wert; log('SET_PROTOCOL', wert, '(0 = Boot, 1 = Bericht)')
    elif rtyp == 0xA1 and req == 0x03:                           # GET_PROTOCOL
        antwort = bytes([zustand['protokoll']])
    elif rtyp == 0x21 and req == 0x09:                           # SET_REPORT (LEDs)
        if daten: zustand['led'] = daten[0]
        log('SET_REPORT', daten.hex())
    elif rtyp == 0xA1 and req == 0x01:                           # GET_REPORT
        with sperre: antwort = bericht_bauen()
    elif rtyp == 0x80 and req == 0:
        antwort = b'\x00\x00'
    elif rtyp in (0x00, 0x01, 0x02) and req in (1, 3):
        pass
    else:
        log('Steuerpaket unbeantwortet', hex(rtyp), hex(req), hex(wert), hex(idx), laenge)
        status = 4
    antwort = antwort[:laenge]
    sende(c, CONTROL, struct.pack('<BBBBHHH', ep, req, rtyp, status, wert, idx, len(antwort)) + antwort, kennung)

def sitzung(c):
    konfig, empfang = 1, set()
    kennung64[0] = False
    sende(c, HELLO, b'lsgeraet.py 1'.ljust(64, b'\0') +
          struct.pack('<I', (1 << CAP_CONNECT_VERSION) | (1 << CAP_EP_MPS) |
                            (1 << CAP_32BIT_BULK) | (1 << CAP_64BIT_IDS)))
    while True:
        art, laenge, kennung = lies_kopf(c)
        nutz = lies_genau(c, laenge) if laenge else b''
        if art == HELLO: break
    caps = struct.unpack('<I', nutz[64:68])[0] if len(nutz) >= 68 else 0
    kennung64[0] = bool(caps & (1 << CAP_64BIT_IDS))
    schnittstellen(c)
    sende(c, DEV_CONNECT, struct.pack('<BBBBHHH', 1 if VOLL else 0, 0x00, 0x00, 0x00, VID, PID, 0x0100))  # 0 Low, 1 Full
    log('verbunden als Low-Speed-' + ('Tastatur' if TASTATUR else 'Maus'))
    while True:
        r, _, _ = select.select([c], [], [], 0.001 if DAUER else 0.01)
        if r:
            art, laenge, kennung = lies_kopf(c)
            nutz = lies_genau(c, laenge) if laenge else b''
            if art == CONTROL:
                steuerpaket(c, kennung, nutz[:10], nutz[10:])
            elif art == SET_CONF:
                konfig = nutz[0]; schnittstellen(c); sende(c, CONF_STATUS, bytes([0, konfig]), kennung)
                log('Konfiguration', konfig)
            elif art == GET_CONF:
                sende(c, CONF_STATUS, bytes([0, konfig]), kennung)
            elif art == SET_ALT:
                sende(c, ALT_STATUS, bytes([0, nutz[0], nutz[1]]), kennung)
            elif art == GET_ALT:
                sende(c, ALT_STATUS, bytes([0, nutz[0], 0]), kennung)
            elif art == START_INT:
                empfang.add(nutz[0]); sende(c, INT_STATUS, bytes([0, nutz[0]]), kennung)
                log('Interrupt-Empfang an', hex(nutz[0]))
            elif art == STOP_INT:
                empfang.discard(nutz[0]); sende(c, INT_STATUS, bytes([0, nutz[0]]), kennung)
            elif art in (RESET, CANCEL, FILTER_FILTER, DISC_ACK, INTERRUPT):
                pass
            else:
                log('unbehandelt: Art', art)
        if 0x81 in empfang:
            with sperre:
                raus = bereit[:]; del bereit[:]
                if DAUER and not raus and not TASTATUR:
                    jetzt = time.time()
                    if jetzt - zustand.get('leer_t', 0) >= DAUER / 1000.0:
                        zustand['leer_t'] = jetzt
                        raus = [bytes([zustand['knoepfe'], 0, 0, 0])]
            for b in raus:
                sende(c, INTERRUPT, struct.pack('<BBH', 0x81, 0, len(b)) + b)
                zustand['gesendet'] += 1
        if MEDIEN and 0x82 in empfang:
            with sperre:
                raus2 = bereit2[:]; del bereit2[:]
            for b in raus2:
                sende(c, INTERRUPT, struct.pack('<BBH', 0x82, 0, len(b)) + b)
                log('Medienbericht', b.hex())

def main():
    open(LOG, 'w').close()
    threading.Thread(target=steuerung, daemon=True).start()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
    s = socket.socket(socket.AF_UNIX); s.bind(SOCK); s.listen(1)
    log('wartet auf QEMU')
    while True:
        c, _ = s.accept()
        try: sitzung(c)
        except Exception as e: log('Sitzung beendet:', e)
        c.close()

if __name__ == '__main__':
    main()
