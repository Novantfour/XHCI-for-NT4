#!/usr/bin/env python3
"""
udruck.py -- ein USB-Drucker fuer QEMU, ueber usbredir.

Meldet sich als HP LaserJet 1200 (03F0:0517), Schnittstelle 07/01/02
(Drucker, beide Richtungen), Bulk 01 hinaus / 82 herein, Full Speed.
Alles, was hinausgeht, landet in /tmp/udruck.bin.

    python3 udruck.py          lauscht auf /tmp/udruck.sock (QEMU)
                               und /tmp/udruck_ctl.sock (Steuerung)
In QEMU:
    chardev-add socket,id=druck,path=/tmp/udruck.sock
    device_add usb-redir,chardev=druck,id=rdruck,bus=xhci.0

Steuerung:
    papier 0|1        Papier leer / da  (GET_PORT_STATUS Bit 5)
    halt 1|0          keine Daten mehr annehmen (wie ohne Papier) / weiter
    zaehler           empfangene Byte
    leeren            /tmp/udruck.bin leeren
    antwort <text>    Text fuer den Rueckkanal bereitlegen (\\r \\n \\f gehen)

Rueckkanal: Leseanfragen (Bulk 82 herein) bleiben offen, bis etwas zu sagen
ist.  Steht "@PJL INFO STATUS" im Datenstrom, antwortet er wie ein HP-Drucker
mit einer PJL-Statusmeldung (CODE=10001, "00 READY"; ohne Papier 41000).

Betriebsarten:
    --dot4     Schnittstelle 0 mit zwei Einstellungen: 0 = Protokoll 03
               (IEEE 1284.4, "Dot4"), 1 = Protokoll 02 -- wie viele HP-
               Multifunktionsgeraete.  Der Treiber soll auf 1 umschalten.
    --nurdot4  nur Protokoll 03: der Treiber soll ihn NICHT als Drucker nehmen
    --hs       High Speed, Bulk-Endpunkte mit 512 Byte

Klassenanfragen: GET_DEVICE_ID (IEEE-1284-Kennung), GET_PORT_STATUS,
SOFT_RESET.  usbredir-Protokoll wie in xpad.py; dazu Bulk-Pakete mit
32-Bit-Laenge (die Faehigkeit wird angeboten).
"""
import os, socket, struct, sys, threading, time, select

SOCK, CTL, LOG, AUSGABE = '/tmp/udruck.sock', '/tmp/udruck_ctl.sock', '/tmp/udruck.log', '/tmp/udruck.bin'

HELLO, DEV_CONNECT, RESET, IF_INFO, EP_INFO = 0, 1, 3, 4, 5
SET_CONF, GET_CONF, CONF_STATUS, SET_ALT, GET_ALT, ALT_STATUS = 6, 7, 8, 9, 10, 11
CANCEL, FILTER_FILTER, DISC_ACK = 21, 23, 24
CONTROL, BULK = 100, 101
CAP_CONNECT_VERSION, CAP_EP_MPS, CAP_32BIT_BULK = 1, 4, 6

DOT4, NURDOT4, HS = '--dot4' in sys.argv, '--nurdot4' in sys.argv, '--hs' in sys.argv
# --ipp: IPP over USB wie ein neueres Multifunktionsgeraet --
#   Schnittstelle 0: Einstellung 0 = 07/01/02 (klassisch), 1 = 07/01/04;
#                    Endpunkte 01 hinaus, 82 herein
#   Schnittstelle 1: 07/01/04, Endpunkte 03 / 84
#   Schnittstelle 2: 07/01/04, Endpunkte 05 / 86
# Auf alles, was an eine 04-Schnittstelle geht, antwortet es mit einer
# kleinen HTTP-Antwort ueber deren Endpunkt herein.
IPP = '--ipp' in sys.argv
MPS = 512 if HS else 64

DEV = bytes([0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
             0xF0, 0x03, 0x17, 0x05, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01])

def _schnittstelle(alt, proto, nr=0, aus=0x01, ein=0x82):
    return bytes([0x09, 0x04, nr, alt, 0x02, 0x07, 0x01, proto, 0x00]) + \
           bytes([0x07, 0x05, aus, 0x02]) + struct.pack('<H', MPS) + bytes([0x00]) + \
           bytes([0x07, 0x05, ein, 0x02]) + struct.pack('<H', MPS) + bytes([0x00])

# Protokoll je Einstellung (Schnittstelle 0)
PROTO = [0x02, 0x04] if IPP else [0x03, 0x02] if DOT4 else [0x03] if NURDOT4 else [0x02]
_r = b''.join(_schnittstelle(a, p) for a, p in enumerate(PROTO))
if IPP:
    _r += _schnittstelle(0, 0x04, 1, 0x03, 0x84) + _schnittstelle(0, 0x04, 2, 0x05, 0x86)
N_IF = 3 if IPP else 1
CONF = bytes([0x09, 0x02]) + struct.pack('<H', 9 + len(_r)) + bytes([N_IF, 0x01, 0x00, 0xC0, 0x01]) + _r
IPP_EP = {0x03: (1, 0x84), 0x05: (2, 0x86)}     # hinaus -> (Schnittstelle, herein)
ipp_rueck = {0x84: bytearray(), 0x86: bytearray()}

def zk(s):
    b = s.encode('utf-16-le'); return bytes([2 + len(b), 3]) + b
STRINGS = {0: bytes([4, 3, 0x09, 0x04]), 1: zk('Hewlett-Packard'), 2: zk('HP LaserJet 1200'), 3: zk('00CNBB012345')}
KENNUNG = b'MFG:Hewlett-Packard;MDL:HP LaserJet 1200;CMD:PJL,MLC,PCL,PCLXL,POSTSCRIPT;CLS:PRINTER;DES:HP LaserJet 1200;'

zustand = {'papier': 1, 'halt': 0, 'bytes': 0, 'resets': 0}
rueck = bytearray()                       # Text fuer den Rueckkanal

def pjl_status():
    if zustand['papier']:
        return b'@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY="00 READY"\r\nONLINE=TRUE\r\n\x0c'
    return b'@PJL INFO STATUS\r\nCODE=41000\r\nDISPLAY="PAPIER EINLEGEN"\r\nONLINE=FALSE\r\n\x0c'
sperre = threading.Lock()

def log(*a):
    with open(LOG, 'a') as f:
        f.write(time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a) + '\n')

def steuerung():
    try: os.unlink(CTL)
    except FileNotFoundError: pass
    s = socket.socket(socket.AF_UNIX); s.bind(CTL); s.listen(4)
    while True:
        c, _ = s.accept(); f = c.makefile('rw')
        for zeile in f:
            w = zeile.split(); antwort = 'ok'
            try:
                with sperre:
                    if w[0] == 'papier': zustand['papier'] = int(w[1])
                    elif w[0] == 'halt': zustand['halt'] = int(w[1])
                    elif w[0] == 'zaehler': antwort = str(zustand['bytes'])
                    elif w[0] == 'resets': antwort = str(zustand['resets'])
                    elif w[0] == 'antwort':
                        text = zeile.split(' ', 1)[1].rstrip('\n')
                        text = text.replace('\\r', '\r').replace('\\n', '\n').replace('\\f', '\f')
                        rueck.extend(text.encode('latin-1'))
                    elif w[0] == 'leeren':
                        open(AUSGABE, 'wb').close(); zustand['bytes'] = 0
                    else: raise ValueError(w[0])
            except Exception as e:
                antwort = 'fehler ' + str(e)
            f.write(antwort + '\n'); f.flush()
        c.close()

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
        if not t: raise ConnectionError('QEMU hat getrennt')
        d += t
    return d

aktuelle_alt = [0]

def schnittstellen(c):
    cls, sub, prot, nr = [0]*32, [0]*32, [0]*32, [0]*32
    for i in range(N_IF):
        nr[i], cls[i], sub[i] = i, 0x07, 0x01
        prot[i] = PROTO[aktuelle_alt[0]] if i == 0 else 0x04
    sende(c, IF_INFO, struct.pack('<I', N_IF) + bytes(nr) + bytes(cls) + bytes(sub) + bytes(prot))
    art, iv, ifc, mps = [255]*32, [0]*32, [0]*32, [0]*32
    art[0] = art[16] = 0; mps[0] = mps[16] = 64
    art[1], mps[1] = 2, MPS           # 01 OUT bulk
    art[18], mps[18] = 2, MPS         # 82 IN bulk
    if IPP:
        for aus, (i, ein) in IPP_EP.items():
            art[aus], mps[aus], ifc[aus] = 2, MPS, i
            art[16 + (ein & 0x0F)], mps[16 + (ein & 0x0F)], ifc[16 + (ein & 0x0F)] = 2, MPS, i
    sende(c, EP_INFO, bytes(art) + bytes(iv) + bytes(ifc) + struct.pack('<32H', *mps))

def steuerpaket(c, kennung, kopf):
    ep, req, rtyp, st, wert, idx, laenge = struct.unpack('<BBBBHHH', kopf)
    antwort, status = b'', 0
    if rtyp == 0x80 and req == 6:
        t, i = wert >> 8, wert & 0xFF
        antwort = DEV if t == 1 else CONF if t == 2 else STRINGS.get(i, b'') if t == 3 else b''
        if not antwort: status = 4
    elif rtyp == 0x80 and req == 0:
        antwort = b'\x00\x00'
    elif rtyp == 0xA1 and req == 0:                      # GET_DEVICE_ID
        antwort = struct.pack('>H', len(KENNUNG) + 2) + KENNUNG
        log('GET_DEVICE_ID')
    elif rtyp == 0xA1 and req == 1:                      # GET_PORT_STATUS
        with sperre:
            s = 0x18 | (0x20 if not zustand['papier'] else 0)
        antwort = bytes([s]); log('GET_PORT_STATUS ->', hex(s))
    elif rtyp == 0x21 and req == 2:                      # SOFT_RESET
        with sperre: zustand['resets'] += 1
        log('SOFT_RESET')
    elif rtyp in (0x00, 0x01, 0x02) and req in (1, 3):
        pass
    else:
        log('Steuerpaket unbeantwortet', hex(rtyp), hex(req), hex(wert), hex(idx), laenge)
        status = 4
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
    konfig, alt, zurueck = 1, {}, []          # zurueck: angehaltene OUT-Pakete
    offen_ein = []                            # offene Leseanfragen (kennung, ep, laenge)
    aktuelle_alt[0] = 0
    kennung64[0] = False                      # eigenes HELLO noch mit 12 Byte
    sende(c, HELLO, b'udruck.py 1'.ljust(64, b'\0') +
          struct.pack('<I', (1 << CAP_CONNECT_VERSION) | (1 << CAP_EP_MPS) |
                            (1 << CAP_32BIT_BULK) | (1 << CAP_64BIT_IDS)))
    caps = hallo_abwarten(c)
    lang32 = bool(caps & (1 << CAP_32BIT_BULK))
    schnittstellen(c)
    sende(c, DEV_CONNECT, struct.pack('<BBBBHHH', 2 if HS else 1, 0x00, 0x00, 0x00, 0x03F0, 0x0517, 0x0100))
    log('verbunden')
    while True:
        r, _, _ = select.select([c], [], [], 0.01)
        if r:
            art, laenge, kennung = lies_kopf(c)
            nutz = lies_genau(c, laenge) if laenge else b''
            if art == HELLO:
                log('zweites HELLO?')
            elif art == CONTROL:
                steuerpaket(c, kennung, nutz[:10])
            elif art == SET_CONF:
                konfig = nutz[0]; schnittstellen(c); sende(c, CONF_STATUS, bytes([0, konfig]), kennung)
            elif art == GET_CONF:
                sende(c, CONF_STATUS, bytes([0, konfig]), kennung)
            elif art == SET_ALT:

                # Neue Einstellung: wie libusbredirhost erst Schnittstelle
                # und Endpunkte neu melden, dann den Status.
                if nutz[0] != 0 and nutz[1] == 0:
                    alt[nutz[0]] = 0                      # Schnittstelle 1/2 (IPP): nur Einstellung 0
                    schnittstellen(c)
                    sende(c, ALT_STATUS, bytes([0, nutz[0], 0]), kennung)
                    continue
                if nutz[0] == 0 and nutz[1] < len(PROTO):
                    alt[nutz[0]] = nutz[1]; aktuelle_alt[0] = nutz[1]
                    schnittstellen(c)
                    sende(c, ALT_STATUS, bytes([0, nutz[0], nutz[1]]), kennung)
                    log('SET_INTERFACE: Einstellung', nutz[1], 'Protokoll %02x' % PROTO[nutz[1]])
                else:
                    sende(c, ALT_STATUS, bytes([2, nutz[0], nutz[1]]), kennung)
            elif art == GET_ALT:
                sende(c, ALT_STATUS, bytes([0, nutz[0], alt.get(nutz[0], 0)]), kennung)
            elif art == BULK:
                kopf = 10 if lang32 else 8
                ep, st, n16, strom = struct.unpack('<BBHI', nutz[:8])
                n = n16 | ((struct.unpack('<H', nutz[8:10])[0] << 16) if lang32 else 0)
                daten = nutz[kopf:]
                if IPP and (ep in IPP_EP or (ep == 0x01 and PROTO[aktuelle_alt[0]] == 0x04)):
                    i, ein = IPP_EP.get(ep, (0, 0x82))
                    koerper = ('IPP Schnittstelle %d: %d Byte empfangen' % (i, len(daten))).encode()
                    antwort = (b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n'
                               % len(koerper)) + koerper
                    with sperre:
                        (ipp_rueck[ein] if ein in ipp_rueck else rueck).extend(antwort)
                    kopf2 = struct.pack('<BBHI', ep, 0, len(daten) & 0xFFFF, 0)
                    if lang32:
                        kopf2 += struct.pack('<H', len(daten) >> 16)
                    sende(c, BULK, kopf2, kennung)
                    log('IPP: Schnittstelle', i, len(daten), 'Byte -> Antwort bereit')
                elif ep == 0x01:
                    zurueck.append((kennung, daten))
                    if b'@PJL INFO STATUS' in daten:
                        with sperre:
                            rueck.extend(pjl_status())
                        log('PJL INFO STATUS -> Antwort bereit')
                else:
                    offen_ein.append((kennung, ep, n))     # Leseanfrage: offen, bis etwas da ist
            elif art == CANCEL:

                # Das Protokoll verlangt eine Antwort auf das abgebrochene
                # Paket, mit Status "cancelled" (1).  Die erste Fassung
                # verwarf es stumm -- QEMU wartete dann weiter darauf, und
                # nach einer Zeitueberschreitung kamen je Druck nur noch
                # zwei Pakete durch.
                weg = [o for o in offen_ein if o[0] == kennung]
                offen_ein = [o for o in offen_ein if o[0] != kennung]
                if weg:
                    kopf = struct.pack('<BBHI', weg[0][1], 1, 0, 0) + (struct.pack('<H', 0) if lang32 else b'')
                    sende(c, BULK, kopf, kennung)
                    log('Abbruch fuer Leseanfrage', kennung, '-> als abgebrochen beantwortet')
                    continue
                vorher = len(zurueck)
                zurueck = [(k, d) for (k, d) in zurueck if k != kennung]
                if len(zurueck) < vorher:
                    kopf = struct.pack('<BBHI', 0x01, 1, 0, 0) + (struct.pack('<H', 0) if lang32 else b'')
                    sende(c, BULK, kopf, kennung)
                    log('Abbruch fuer Paket', kennung, '-> als abgebrochen beantwortet')
                else:
                    log('Abbruch fuer Paket', kennung, '(nicht mehr offen)')
            elif art in (RESET, FILTER_FILTER, DISC_ACK):
                pass
            else:
                log('unbehandelt: Art', art)
        with sperre:
            bleibt = []
            for (k, ep, laenge) in offen_ein:
                q = ipp_rueck[ep] if ep in ipp_rueck else rueck
                if not q:
                    bleibt.append((k, ep, laenge)); continue
                stueck = bytes(q[:laenge]); del q[:laenge]
                kopf = struct.pack('<BBHI', ep, 0, len(stueck) & 0xFFFF, 0)
                if lang32:
                    kopf += struct.pack('<H', len(stueck) >> 16)
                sende(c, BULK, kopf + stueck, k)
                log('Rueckkanal %02x:' % ep, len(stueck), 'Byte')
            offen_ein = bleibt
            angehalten = zustand['halt']
        while zurueck and not angehalten:
            kennung, daten = zurueck.pop(0)
            with open(AUSGABE, 'ab') as f:
                f.write(daten)
            with sperre:
                zustand['bytes'] += len(daten)
            kopf = struct.pack('<BBHI', 0x01, 0, len(daten) & 0xFFFF, 0)
            if lang32:
                kopf += struct.pack('<H', len(daten) >> 16)
            sende(c, BULK, kopf, kennung)

def main():
    open(LOG, 'w').close()
    if not os.path.exists(AUSGABE): open(AUSGABE, 'wb').close()
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
