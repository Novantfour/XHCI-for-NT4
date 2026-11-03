#!/usr/bin/env python3
"""
xpad.py -- ein Xbox-360-Controller (kabelgebunden) fuer QEMU, ueber usbredir.

QEMU sieht ein echtes USB-Geraet: Deskriptoren wie der echte Controller
(045E:028E, Schnittstelle FF/5D/01, Endpunkte 81 IN / 01 OUT, je 32 Byte),
Eingabeberichte im XInput-Format (20 Byte: 00 14 Knoepfe Trigger Sticks).

    python3 xpad.py            Xbox 360; lauscht auf /tmp/xpad.sock (fuer
                               QEMU) und /tmp/xpad_ctl.sock (Steuerung)
    python3 xpad.py --one      Xbox One S (045E:02EA, FF/47/D0, GIP):
                               schweigt bis zum Einschalten-Paket
                               (05 20 .. 01 00), Guide als Typ 07 mit
                               Quittungswunsch, Trigger 0..1023

In QEMU (Monitor):
    chardev-add socket,id=xpad,path=/tmp/xpad.sock
    device_add usb-redir,chardev=xpad,id=xpadr,bus=xhci.0

Steuerung (je Zeile, Antwort "ok" oder Fehler):
    setze a=1 b=0 lx=32767 ly=-32768 lt=255 hoch=1 ...
        Knoepfe: a b x y lb rb back start guide ls rs hoch runter links rechts
        Achsen:  lx ly rx ry (-32768..32767), lt rt (0..255)
    null                   alles loslassen, Sticks in die Mitte
    led                    zuletzt vom Treiber gesetzte LED-Meldung

usbredir-Protokoll: usbredir-protocol.txt (Hans de Goede).  Hier die
Seite "usb-host" (das Geraet); QEMU ist "usb-guest".  32-Bit-Kennungen
(die 64-Bit-Faehigkeit wird nicht angeboten).
"""
import os, socket, struct, sys, threading, time, select

SOCK, CTL, LOG = '/tmp/xpad.sock', '/tmp/xpad_ctl.sock', '/tmp/xpad.log'

# Nachrichtenarten
HELLO, DEV_CONNECT, DEV_DISCONNECT, RESET, IF_INFO, EP_INFO = 0, 1, 2, 3, 4, 5
SET_CONF, GET_CONF, CONF_STATUS, SET_ALT, GET_ALT, ALT_STATUS = 6, 7, 8, 9, 10, 11
START_INT, STOP_INT, INT_STATUS = 15, 16, 17
CANCEL, FILTER_REJECT, FILTER_FILTER, DISC_ACK = 21, 22, 23, 24
CONTROL, BULK, ISO, INTERRUPT = 100, 101, 102, 103
CAP_CONNECT_VERSION, CAP_EP_MPS = 1, 4
CAP_32BIT_BULK = 6     # QEMUs xHCI verlangt es, auch ohne Bulk-Endpunkte

# ---- Deskriptoren (wie der echte kabelgebundene Controller) ----
DEV = bytes([0x12, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0x08,
             0x5E, 0x04, 0x8E, 0x02, 0x14, 0x01, 0x01, 0x02, 0x03, 0x01])
_IF = bytes([0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00])
_UNBEKANNT = bytes([0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00,
                    0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00])
_EP_IN  = bytes([0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04])
_EP_OUT = bytes([0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x08])
_rest = _IF + _UNBEKANNT + _EP_IN + _EP_OUT
CONF = bytes([0x09, 0x02]) + struct.pack('<H', 9 + len(_rest)) + \
       bytes([0x01, 0x01, 0x00, 0xA0, 0xFA]) + _rest

def zeichenkette(s):
    b = s.encode('utf-16-le')
    return bytes([2 + len(b), 3]) + b

STRINGS = {0: bytes([4, 3, 0x09, 0x04]),
           1: zeichenkette('\u00a9Microsoft Corporation'),
           2: zeichenkette('Controller'),
           3: zeichenkette('08FEC93')}

EINS = '--one' in sys.argv
# --spaet N: die ersten N Einschalt-Nachrichten ignorieren (noch nicht bereit)
# --anmelden: nach dem Start des Interrupt-Empfangs eine Ankuendigung (02) schicken
SPAET = [int(sys.argv[sys.argv.index('--spaet') + 1])] if '--spaet' in sys.argv else [0]
ANMELDEN = '--anmelden' in sys.argv
angemeldet = [False]
if EINS:
    DEV = bytes([0x12, 0x01, 0x00, 0x02, 0xFF, 0x47, 0xD0, 0x40,
                 0x5E, 0x04, 0xEA, 0x02, 0x01, 0x01, 0x01, 0x02, 0x03, 0x01])
    _r = bytes([0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x47, 0xD0, 0x00]) + \
         bytes([0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x04]) + \
         bytes([0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x04])
    CONF = bytes([0x09, 0x02]) + struct.pack('<H', 9 + len(_r)) + \
           bytes([0x01, 0x01, 0x00, 0xA0, 0xFA]) + _r
    STRINGS[2] = zeichenkette('Controller')

# GIP-Anordnung (Xbox One): Byte 4 / Byte 5
KNOPF_BITS_EINS = {
    'start': (0, 2), 'back': (0, 3), 'a': (0, 4), 'b': (0, 5), 'x': (0, 6), 'y': (0, 7),
    'hoch': (1, 0), 'runter': (1, 1), 'links': (1, 2), 'rechts': (1, 3),
    'lb': (1, 4), 'rb': (1, 5), 'ls': (1, 6), 'rs': (1, 7)}

# ---- Zustand ----
KNOPF_BITS = {  # (Byte 2 / Byte 3 des Berichts), Bit
    'hoch': (0, 0), 'runter': (0, 1), 'links': (0, 2), 'rechts': (0, 3),
    'start': (0, 4), 'back': (0, 5), 'ls': (0, 6), 'rs': (0, 7),
    'lb': (1, 0), 'rb': (1, 1), 'guide': (1, 2),
    'a': (1, 4), 'b': (1, 5), 'x': (1, 6), 'y': (1, 7)}

zustand = {'knoepfe': [0, 0], 'lt': 0, 'rt': 0, 'lx': 0, 'ly': 0, 'rx': 0, 'ry': 0,
           'guide': 0}
eingeschaltet = [not EINS]
gip_seq = [0]
sperre = threading.Lock()
geaendert = threading.Event()
led = [None]

def log(*a):
    with open(LOG, 'a') as f:
        f.write(time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a) + '\n')

def bericht():
    with sperre:
        z = zustand
        if EINS:
            return bytes([0x20, 0x00, 0x00, 0x0E, z['knoepfe'][0], z['knoepfe'][1]]) + \
                   struct.pack('<HHhhhh', z['lt'], z['rt'], z['lx'], z['ly'], z['rx'], z['ry'])
        return bytes([0x00, 0x14, z['knoepfe'][0], z['knoepfe'][1], z['lt'], z['rt']]) + \
               struct.pack('<hhhh', z['lx'], z['ly'], z['rx'], z['ry']) + bytes(6)

# ---- Steuerung ----
def steuerung():
    try: os.unlink(CTL)
    except FileNotFoundError: pass
    s = socket.socket(socket.AF_UNIX); s.bind(CTL); s.listen(4)
    while True:
        c, _ = s.accept()
        f = c.makefile('rw')
        for zeile in f:
            w = zeile.split()
            antwort = 'ok'
            try:
                if not w:
                    continue
                if w[0] == 'null':
                    with sperre:
                        zustand.update({'knoepfe': [0, 0], 'lt': 0, 'rt': 0,
                                        'lx': 0, 'ly': 0, 'rx': 0, 'ry': 0, 'guide': 0})
                elif w[0] == 'setze':
                    with sperre:
                        for p in w[1:]:
                            k, v = p.split('=')
                            v = int(v, 0)
                            bits = KNOPF_BITS_EINS if EINS else KNOPF_BITS
                            if EINS and k == 'guide':
                                zustand['guide'] = 1 if v else 0
                            elif k in bits:
                                b, bit = bits[k]
                                if v: zustand['knoepfe'][b] |= 1 << bit
                                else: zustand['knoepfe'][b] &= ~(1 << bit)
                            elif k in ('lt', 'rt'):
                                zustand[k] = max(0, min(1023 if EINS else 255, v))
                            elif k in ('lx', 'ly', 'rx', 'ry'):
                                zustand[k] = max(-32768, min(32767, v))
                            else:
                                raise ValueError('unbekannt: ' + k)
                elif w[0] == 'led':
                    antwort = 'led ' + (led[0].hex() if led[0] else '-')
                elif w[0] == 'an':
                    antwort = 'eingeschaltet ' + str(eingeschaltet[0])
                else:
                    raise ValueError('unbekannt: ' + w[0])
                geaendert.set()
            except Exception as e:
                antwort = 'fehler ' + str(e)
            f.write(antwort + '\n'); f.flush()
        c.close()

# ---- usbredir ----
# Kopf: 12 Byte (Kennung 32 Bit), oder 16 Byte (Kennung 64 Bit), sobald
# BEIDE Seiten usb_redir_cap_64bits_ids (Bit 5) angeboten haben.  Das eigene
# HELLO und das HELLO von QEMU gehen noch mit 12 Byte -- die Faehigkeiten der
# Gegenseite sind da noch unbekannt; erst danach wird umgeschaltet, in beide
# Richtungen.  QEMUs xHCI verlangt 64-Bit-Kennungen, wenn das Geraet direkt
# an einem seiner Anschluesse haengt (hinter einem USB-1.1-Hub nicht).
CAP_64BIT_IDS = 5
kennung64 = [False]

def sende(c, art, nutz, kennung=0):
    if kennung64[0]:
        c.sendall(struct.pack('<IIQ', art, len(nutz), kennung) + nutz)
    else:
        c.sendall(struct.pack('<III', art, len(nutz), kennung) + nutz)

def lies_kopf(c):
    if kennung64[0]:
        return struct.unpack('<IIQ', lies_genau(c, 16))
    return struct.unpack('<III', lies_genau(c, 12))

def lies_genau(c, n):
    d = b''
    while len(d) < n:
        t = c.recv(n - len(d))
        if not t:
            raise ConnectionError('QEMU hat getrennt')
        d += t
    return d

def schnittstellen(c):
    cls, sub, prot, nr = [0]*32, [0]*32, [0]*32, [0]*32
    cls[0], sub[0], prot[0] = (0xFF, 0x47, 0xD0) if EINS else (0xFF, 0x5D, 0x01)
    sende(c, IF_INFO, struct.pack('<I', 1) + bytes(nr) + bytes(cls) + bytes(sub) + bytes(prot))
    art, iv, ifc, mps = [255]*32, [0]*32, [0]*32, [0]*32
    art[0] = art[16] = 0; mps[0] = mps[16] = 8                  # EP0
    groesse = 64 if EINS else 32
    art[1], iv[1], mps[1] = 3, (4 if EINS else 8), groesse      # 01 OUT
    art[17], iv[17], mps[17] = 3, 4, groesse                    # 81 IN
    sende(c, EP_INFO, bytes(art) + bytes(iv) + bytes(ifc) + struct.pack('<32H', *mps))

def steuerpaket(c, kennung, kopf, daten):
    ep, req, rtyp, st, wert, idx, laenge = struct.unpack('<BBBBHHH', kopf)
    antwort, status = b'', 0
    if rtyp == 0x80 and req == 6:                              # GET_DESCRIPTOR
        t, i = wert >> 8, wert & 0xFF
        antwort = DEV if t == 1 else CONF if t == 2 else STRINGS.get(i, b'') if t == 3 else b''
        if not antwort:
            status = 4
    elif rtyp == 0x80 and req == 0:                            # GET_STATUS
        antwort = b'\x00\x00'
    elif rtyp in (0x00, 0x01, 0x02) and req in (1, 3):          # CLEAR/SET_FEATURE
        pass
    elif rtyp == 0x81 and req == 0 or rtyp == 0x82 and req == 0:
        antwort = b'\x00\x00'
    else:
        log('Steuerpaket unbeantwortet', hex(rtyp), hex(req), hex(wert), hex(idx), laenge)
        status = 4                                             # stall
    antwort = antwort[:laenge]
    sende(c, CONTROL, struct.pack('<BBBBHHH', ep, req, rtyp, status, wert, idx, len(antwort)) + antwort, kennung)


def hallo_abwarten(c):
    """QEMUs HELLO lesen (noch 12-Byte-Kopf), dann umschalten.  Erst danach
    duerfen Schnittstellen und Geraeteanschluss hinaus -- sie brauchen schon
    die ausgehandelte Kopfgroesse."""
    while True:
        art, laenge, kennung = lies_kopf(c)
        nutz = lies_genau(c, laenge) if laenge else b''
        if art == HELLO:
            break
    caps = struct.unpack('<I', nutz[64:68])[0] if len(nutz) >= 68 else 0
    kennung64[0] = bool(caps & (1 << CAP_64BIT_IDS))
    log('QEMU:', nutz[:64].rstrip(b'\0').decode(errors='replace'),
        'Faehigkeiten %#x' % caps, '64-Bit-Kennungen' if kennung64[0] else '12-Byte-Kopf')
    return caps

def sitzung(c):
    konfig, empfang, alt = 1, set(), {}
    eingeschaltet[0] = not EINS         # je Verbindung neu: erst einschalten
    kennung64[0] = False                # eigenes HELLO noch mit 12 Byte
    sende(c, HELLO, b'xpad.py 1'.ljust(64, b'\0') +
          struct.pack('<I', (1 << CAP_CONNECT_VERSION) | (1 << CAP_EP_MPS) |
                            (1 << CAP_32BIT_BULK) | (1 << CAP_64BIT_IDS)))
    hallo_abwarten(c)
    schnittstellen(c)
    if EINS:
        sende(c, DEV_CONNECT, struct.pack('<BBBBHHH', 1, 0xFF, 0x47, 0xD0, 0x045E, 0x02EA, 0x0101))
    else:
        sende(c, DEV_CONNECT, struct.pack('<BBBBHHH', 1, 0xFF, 0xFF, 0xFF, 0x045E, 0x028E, 0x0114))
    log('verbunden, als Full-Speed-Geraet gemeldet')
    zuletzt = None
    guide_alt = 0
    while True:
        r, _, _ = select.select([c], [], [], 0.01)
        if r:
            art, laenge, kennung = lies_kopf(c)
            nutz = lies_genau(c, laenge) if laenge else b''
            if art == HELLO:
                log('zweites HELLO?')
            elif art == CONTROL:
                steuerpaket(c, kennung, nutz[:10], nutz[10:])
            elif art == SET_CONF:
                konfig = nutz[0]
                schnittstellen(c)
                sende(c, CONF_STATUS, bytes([0, konfig]), kennung)
                log('Konfiguration', konfig)
            elif art == GET_CONF:
                sende(c, CONF_STATUS, bytes([0, konfig]), kennung)
            elif art == SET_ALT:
                alt[nutz[0]] = nutz[1]
                sende(c, ALT_STATUS, bytes([0, nutz[0], nutz[1]]), kennung)
            elif art == GET_ALT:
                sende(c, ALT_STATUS, bytes([0, nutz[0], alt.get(nutz[0], 0)]), kennung)
            elif art == START_INT:
                empfang.add(nutz[0])
                sende(c, INT_STATUS, bytes([0, nutz[0]]), kennung)
                log('Interrupt-Empfang an', hex(nutz[0]))
                zuletzt = None                                 # ersten Bericht sofort
            elif art == STOP_INT:
                empfang.discard(nutz[0])
                sende(c, INT_STATUS, bytes([0, nutz[0]]), kennung)
            elif art == INTERRUPT:
                ep, st, n = struct.unpack('<BBH', nutz[:4])
                daten = nutz[4:]
                if ep == 0x01:
                    if daten[:2] == b'\x01\x03':
                        led[0] = daten
                    if EINS and len(daten) >= 5 and daten[0] == 0x05 and daten[3] == 0x01:
                        if SPAET[0] > 0:
                            SPAET[0] -= 1
                            log('GIP: Einschalten IGNORIERT (noch nicht bereit), Folgenummer', daten[2])
                        else:
                            eingeschaltet[0] = True
                            log('GIP: eingeschaltet, Folgenummer', daten[2])
                    if EINS and daten[:1] == b'\x01':
                        log('GIP: Quittung fuer Folgenummer', daten[2])
                    log('OUT an 01:', daten.hex())
                sende(c, INTERRUPT, struct.pack('<BBH', ep, 0, len(daten)), kennung)
            elif art in (RESET, CANCEL, FILTER_FILTER, DISC_ACK):
                pass
            else:
                log('unbehandelt: Art', art, 'Laenge', laenge)
        if 0x81 in empfang and EINS and ANMELDEN and not angemeldet[0]:
            angemeldet[0] = True
            time.sleep(0.3)
            a = bytes([0x02, 0x20, 0x01, 0x1C]) + bytes(28)
            sende(c, INTERRUPT, struct.pack('<BBH', 0x81, 0, len(a)) + a)
            log('GIP: Ankuendigung (02) geschickt')
        if 0x81 in empfang and EINS and eingeschaltet[0] and zustand['guide'] != guide_alt:
            guide_alt = zustand['guide']
            gip_seq[0] = (gip_seq[0] + 1) & 0xFF
            g = bytes([0x07, 0x30, gip_seq[0], 0x02, guide_alt, 0x5B])
            sende(c, INTERRUPT, struct.pack('<BBH', 0x81, 0, len(g)) + g)
            log('GIP: Guide', guide_alt, 'Folgenummer', gip_seq[0], '(Quittung erbeten)')
        if 0x81 in empfang and eingeschaltet[0]:
            b = bericht()
            if b != zuletzt or geaendert.is_set():
                geaendert.clear()
                sende(c, INTERRUPT, struct.pack('<BBH', 0x81, 0, len(b)) + b)
                zuletzt = b

def main():
    open(LOG, 'w').close()
    threading.Thread(target=steuerung, daemon=True).start()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
    s = socket.socket(socket.AF_UNIX); s.bind(SOCK); s.listen(1)
    log('wartet auf QEMU')
    while True:
        c, _ = s.accept()
        try:
            sitzung(c)
        except Exception as e:
            log('Sitzung beendet:', e)
        c.close()

if __name__ == '__main__':
    main()
