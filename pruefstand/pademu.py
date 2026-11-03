#!/usr/bin/env python3
"""pademu.py -- spielt einem QEMU-Gast einen USB-Gamepad vor (usbredir).

    pademu.py [--art 360|one|hid] [--sock /tmp/pad.sock] [--steuer /tmp/pad.ctl]

QEMU-Seite (QEMU ist der Server, pademu verbindet sich):
    -chardev socket,id=pad,path=/tmp/pad.sock,server=on,wait=off
    -device usb-redir,chardev=pad,bus=xhci.0

Arten
    360   Xbox-360-Controller (045e:028e), Klasse FF/5D/01, eigenes Protokoll
          (Bericht 00 14 ...), samt Headset-, Erweiterungs- und
          Sicherheitsschnittstelle wie das Original
    one   Xbox-One-S-Controller (045e:02ea; mit --pid 0b12 Series X|S), GIP: meldet sich mit Typ 02 an,
          schweigt bis zum Einschalten (05 20 .. 01 00), Eingaben als Typ 20,
          Guide als Typ 07 mit Quittung; Schnittstelle 1 = Audio (wie echt)
    hid   gewoehnliches HID-Gamepad (4 Achsen, Hut, 12 Knoepfe)
    mikro USB-Mikrofon, UAC1 (Aufnahme): 48 kHz, 16 Bit, Mono, isochron IN
          0x82, liefert einen fortlaufenden 1-kHz-Sinus (Amplitude 10000)
          -- zum Pruefen der Aufnahme; QEMUs usb-audio kann nur abspielen
    headset  USB-Headset wie HyperX Cloud Stinger Core (aus dem Anwender-Log):
          Mikrofon EP 0x81 (IN, Mono) und Lautsprecher EP 0x02 (OUT, Alt 1
          Mono, Alt 2 Stereo); ZWEI Feature Units, die des Mikrofons (3)
          steht vor der des Lautsprechers (2).  Der Lautsprecher kommt
          STUMM und auf Minimum hoch.  Was ankommt, geht nach
          /tmp/headset_aus.raw (stumm/Minimum: als Stille), Format in
          /tmp/headset_aus.txt

Steuerung: Zeilen an den Steuer-Socket (Unix, Datagramm-frei, je Verbindung
eine oder mehrere Zeilen), z. B. mit  echo "A 1" | nc -U /tmp/pad.ctl
    A B X Y LB RB BACK START LS RS GUIDE  <0|1>   Knopf
    HOCH RUNTER LINKS RECHTS <0|1>               Steuerkreuz
    LX LY RX RY <-32768..32767>                  Sticks (oben = positiv)
    LT RT <0..255>                               Trigger (one: 0..1023)
    LOS                                          alles loslassen
    ZUSTAND                                      aktuellen Zustand melden
Alles, was der Gasttreiber schickt (LED, Rumble, GIP), steht im Protokoll.

usbredir: Nachrichtenformat aus usbredirproto.h (spice/usbredir), ohne
64-Bit-Kennungen und ohne Bulk-Streams -- beides kuendigen wir nicht an.
"""
import argparse, os, socket, struct, sys, threading, time
STUMM = 0.0

# ---------------------------------------------------------------- usbredir
HELLO, DEV_CONNECT, DEV_DISCONNECT, RESET, IF_INFO, EP_INFO = 0, 1, 2, 3, 4, 5
SET_CONF, GET_CONF, CONF_STATUS, SET_ALT, GET_ALT, ALT_STATUS = 6, 7, 8, 9, 10, 11
START_ISO, STOP_ISO, ISO_STATUS = 12, 13, 14
START_INT, STOP_INT, INT_STATUS = 15, 16, 17
CANCEL, FILTER_REJECT, FILTER_FILTER, DISC_ACK = 21, 22, 23, 24
START_BULK_RECV, STOP_BULK_RECV = 25, 26
CONTROL, BULK, ISO, INTERRUPT = 100, 101, 102, 103
NAMEN = {0: 'hello', 3: 'reset', 6: 'set_configuration', 7: 'get_configuration',
         9: 'set_alt_setting', 10: 'get_alt_setting', 12: 'start_iso',
         13: 'stop_iso', 15: 'start_interrupt_receiving',
         16: 'stop_interrupt_receiving', 21: 'cancel', 23: 'filter',
         24: 'disconnect_ack', 100: 'control', 101: 'bulk', 103: 'interrupt'}

CAP_BULK_STREAMS = 0
CAP_CONNECT_DEVICE_VERSION = 1
CAP_EP_INFO_MAX_PACKET_SIZE = 4
CAP_64BITS_IDS = 5
CAP_32BITS_BULK_LENGTH = 6
# QEMU verlangt am xHCI Bulk-Streams und 32-Bit-Bulk-Laengen; mit Bulk-
# Streams waechst ep_info um max_streams[32], mit 32-Bit-Laengen der
# Bulk-Kopf um length_high.
OK, CANCELLED, INVAL, IOERROR, STALL = 0, 1, 2, 3, 4
EP_CONTROL, EP_ISO, EP_BULK, EP_INT, EP_INVALID = 0, 1, 2, 3, 255

def ep_index(ep):
    return ((ep & 0x80) >> 3) | (ep & 0x0F)

def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)

# ---------------------------------------------------------------- Deskriptoren
def dev_desc(cls, sub, proto, mps, vid, pid, bcd):
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x0200, cls, sub, proto, mps,
                       vid, pid, bcd, 1, 2, 3, 1)

def iface(num, alt, neps, cls, sub, proto, extra=b''):
    return struct.pack('<BBBBBBBBB', 9, 4, num, alt, neps, cls, sub, proto, 0) + extra

def ep(addr, typ, mps, interval):
    return struct.pack('<BBBBHB', 7, 5, addr, typ, mps, interval)

def config(nifaces, body, attr=0xA0, power=0xFA):
    total = 9 + len(body)
    return struct.pack('<BBHBBBBB', 9, 2, total, nifaces, 1, 0, attr, power) + body

def strdesc(s):
    b = s.encode('utf-16-le')
    return bytes([2 + len(b), 3]) + b

class Geraet:
    """Grundlage: Deskriptoren, Endpunkte, Antworten auf Standardanfragen."""
    speed = 1                     # full speed
    def __init__(self):
        self.konf = 0
        self.alts = {}
        self.zust = dict(A=0, B=0, X=0, Y=0, LB=0, RB=0, BACK=0, START=0, LS=0,
                         RS=0, GUIDE=0, HOCH=0, RUNTER=0, LINKS=0, RECHTS=0,
                         LX=0, LY=0, RX=0, RY=0, LT=0, RT=0)
        self.senden = None        # wird gesetzt: f(ep, daten)
        self.stumm_bis = 0.0      # --stumm: bis dahin keine Eingabeberichte
        self.empfang = set()      # IN-Endpunkte, auf denen der Gast liest
        self.sperre = threading.Lock()

    # Endpunkt-Tabelle fuer ep_info: {adresse: (typ, intervall, schnittstelle, mps)}
    def endpunkte(self):
        raise NotImplementedError

    def schnittstellen(self):     # [(nummer, klasse, unterklasse, protokoll)]
        raise NotImplementedError

    def strings(self, i):
        return {0: b'\x04\x03\x09\x04', 1: strdesc(self.hersteller),
                2: strdesc(self.produkt), 3: strdesc(self.seriennr)}.get(i)

    def steuer(self, rt, req, wert, index, laenge, daten):
        """Standardanfragen. -> (status, antwortdaten)"""
        typ, nr = wert >> 8, wert & 0xFF
        if rt == 0x80 and req == 6:                     # GET_DESCRIPTOR
            if typ == 1: return OK, self.dd[:laenge]
            if typ == 2: return OK, self.cd[:laenge]
            if typ == 3:
                s = self.strings(nr)
                return (OK, s[:laenge]) if s else (STALL, b'')
            if typ == 6: return STALL, b''              # kein device_qualifier
            return STALL, b''
        if rt == 0x80 and req == 0: return OK, b'\x00\x00'[:laenge]   # GET_STATUS
        if rt == 0x81 and req == 0: return OK, b'\x00\x00'[:laenge]
        if rt == 0x82 and req == 0: return OK, b'\x00\x00'[:laenge]
        if rt == 0x02 and req in (1, 3): return OK, b''  # CLEAR/SET_FEATURE (Endpunkt)
        if rt == 0x00 and req in (1, 3): return OK, b''
        return self.klasse(rt, req, wert, index, laenge, daten)

    def klasse(self, rt, req, wert, index, laenge, daten):
        return STALL, b''

    def eingang_out(self, ep, daten):
        log('  Gast -> %02x:' % ep, daten.hex(' '))

    def gesetzt(self, name, wert):
        with self.sperre:
            self.zust[name] = wert
        self.bericht()

    def bericht(self):
        pass

# ---------------------------------------------------------------- Xbox 360
class Xbox360(Geraet):
    hersteller, produkt, seriennr = '\xa9Microsoft Corporation', 'Controller', '0D3E7A1'
    def __init__(self):
        super().__init__()
        self.dd = dev_desc(0xFF, 0xFF, 0xFF, 8, 0x045E, 0x028E, 0x0114)
        # Wie ein echter kabelgebundener 360-Controller (Deskriptor nach
        # lsusb): 0 Eingabe, 1 Headset (Audio), 2 Erweiterung, 3 Sicherheit.
        body = (iface(0, 0, 2, 0xFF, 0x5D, 0x01,
                      bytes.fromhex('1121000101258114000000001301080000'))
                + ep(0x81, 3, 32, 4) + ep(0x01, 3, 32, 8)
                + iface(1, 0, 4, 0xFF, 0x5D, 0x03,
                        bytes.fromhex('1b2100010101824001022016830000000000001603000000000000'))
                + ep(0x82, 3, 32, 2) + ep(0x02, 3, 32, 4)
                + ep(0x83, 3, 32, 64) + ep(0x03, 3, 32, 16)
                + iface(2, 0, 1, 0xFF, 0x5D, 0x02, bytes.fromhex('092100010122840700'))
                + ep(0x84, 3, 32, 16)
                + iface(3, 0, 0, 0xFF, 0xFD, 0x13, bytes.fromhex('064100010103')))
        self.cd = config(4, body)

    def schnittstellen(self):
        return [(0, 0xFF, 0x5D, 0x01), (1, 0xFF, 0x5D, 0x03),
                (2, 0xFF, 0x5D, 0x02), (3, 0xFF, 0xFD, 0x13)]

    def endpunkte(self):
        return {0x81: (EP_INT, 4, 0, 32), 0x01: (EP_INT, 8, 0, 32),
                0x82: (EP_INT, 2, 1, 32), 0x02: (EP_INT, 4, 1, 32),
                0x83: (EP_INT, 64, 1, 32), 0x03: (EP_INT, 16, 1, 32),
                0x84: (EP_INT, 16, 2, 32)}

    def eingang_out(self, ep, d):
        if ep == 0x01 and len(d) >= 3 and d[0] == 0x01:
            log('  Gast: LED-Muster %d' % d[2])
        elif ep == 0x01 and len(d) >= 5 and d[0] == 0x00 and d[1] == 0x08:
            log('  Gast: Rumble gross %d klein %d' % (d[3], d[4]))
        else:
            super().eingang_out(ep, d)

    def bericht(self):
        z = self.zust
        b2 = (z['HOCH'] | z['RUNTER'] << 1 | z['LINKS'] << 2 | z['RECHTS'] << 3 |
              z['START'] << 4 | z['BACK'] << 5 | z['LS'] << 6 | z['RS'] << 7)
        b3 = (z['LB'] | z['RB'] << 1 | z['GUIDE'] << 2 |
              z['A'] << 4 | z['B'] << 5 | z['X'] << 6 | z['Y'] << 7)
        r = struct.pack('<BBBBBBhhhh6x', 0x00, 0x14, b2, b3, z['LT'] & 255, z['RT'] & 255,
                        z['LX'], z['LY'], z['RX'], z['RY'])
        if time.time() >= self.stumm_bis and self.senden and 0x81 in self.empfang:
            self.senden(0x81, r)

# ---------------------------------------------------------------- Xbox One (GIP)
class XboxOne(Geraet):
    hersteller, produkt, seriennr = 'Microsoft', 'Controller', '3032363030303'
    pid = 0x02EA                  # --pid: 02ea One S, 0b00 Elite 2, 0b12 Series X|S
    def __init__(self):
        super().__init__()
        self.dd = dev_desc(0xFF, 0x47, 0xD0, 64, 0x045E, self.pid, 0x0408)
        # 0 Eingaben (GIP), 1 Audio (Headset-Buchse; Alt 1 = isochron),
        # 2 weitere GIP-Schnittstelle (Bulk).  Wie Xbox One S, Modell 1708.
        body = (iface(0, 0, 2, 0xFF, 0x47, 0xD0) + ep(0x02, 3, 64, 4) + ep(0x82, 3, 64, 4)
                + iface(1, 0, 0, 0xFF, 0x47, 0xD0)
                + iface(1, 1, 2, 0xFF, 0x47, 0xD0) + ep(0x03, 1, 228, 1) + ep(0x83, 1, 64, 1)
                + iface(2, 0, 0, 0xFF, 0x47, 0xD0)
                + iface(2, 1, 2, 0xFF, 0x47, 0xD0) + ep(0x04, 2, 64, 0) + ep(0x84, 2, 64, 0))
        self.cd = config(3, body)
        self.an = False
        self.seq = 0

    def schnittstellen(self):
        return [(0, 0xFF, 0x47, 0xD0), (1, 0xFF, 0x47, 0xD0), (2, 0xFF, 0x47, 0xD0)]

    def endpunkte(self):
        e = {0x02: (EP_INT, 4, 0, 64), 0x82: (EP_INT, 4, 0, 64)}
        if self.alts.get(1) == 1:
            e.update({0x03: (EP_ISO, 1, 1, 228), 0x83: (EP_ISO, 1, 1, 64)})
        if self.alts.get(2) == 1:
            e.update({0x04: (EP_BULK, 0, 2, 64), 0x84: (EP_BULK, 0, 2, 64)})
        return e

    def _seq(self):
        self.seq = (self.seq + 1) & 0xFF or 1
        return self.seq

    def ankuendigen(self):
        # Typ 02 "Ankuendigung": VID/PID und Firmware wie xpad sie erwartet
        nutz = bytearray(0x1c)
        nutz[0:6] = bytes.fromhex('7eed8257a4c0')          # Kennung des Controllers
        struct.pack_into('<HH', nutz, 8, 0x045E, self.pid)
        a = bytes([0x02, 0x20, self._seq(), 0x1c]) + bytes(nutz)
        if self.senden and 0x82 in self.empfang:
            self.senden(0x82, a[:4 + 0x1c])
            log('  Controller: Ankuendigung (02) gesendet')

    def eingang_out(self, ep, d):
        if ep != 0x02 or len(d) < 4:
            return super().eingang_out(ep, d)
        typ = d[0]
        if typ == 0x05:
            log('  Gast: GIP Einschalten (05) Modus %02x' % (d[4] if len(d) > 4 else 0))
            if len(d) > 4 and d[4] == 0x00:
                self.an = True
                self.bericht()
        elif typ == 0x09:
            log('  Gast: GIP Rumble  motoren %02x  LT %d RT %d stark %d schwach %d' %
                tuple(d[5:10]) if len(d) >= 10 else d.hex(' '))
        elif typ == 0x0A:
            log('  Gast: GIP LED', d.hex(' '))
        elif typ == 0x01:
            log('  Gast: GIP Quittung', d.hex(' '))
        elif typ == 0x06:
            log('  Gast: GIP Anmeldeantwort', d.hex(' '))
        else:
            super().eingang_out(ep, d)

    def bericht(self):
        if not self.an:
            return
        z = self.zust
        b4 = z['START'] << 2 | z['BACK'] << 3 | z['A'] << 4 | z['B'] << 5 | z['X'] << 6 | z['Y'] << 7
        b5 = (z['HOCH'] | z['RUNTER'] << 1 | z['LINKS'] << 2 | z['RECHTS'] << 3 |
              z['LB'] << 4 | z['RB'] << 5 | z['LS'] << 6 | z['RS'] << 7)
        r = struct.pack('<BBBBBBHHhhhh', 0x20, 0x00, self._seq(), 0x0e, b4, b5,
                        z['LT'] & 1023, z['RT'] & 1023, z['LX'], z['LY'], z['RX'], z['RY'])
        if time.time() >= self.stumm_bis and self.senden and 0x82 in self.empfang:
            self.senden(0x82, r)

    def gesetzt(self, name, wert):
        if name == 'GUIDE':
            with self.sperre:
                self.zust['GUIDE'] = wert
            if self.senden and 0x82 in self.empfang:
                self.senden(0x82, bytes([0x07, 0x30, self._seq(), 0x02, wert & 1, 0x5b]))
            return
        super().gesetzt(name, wert)

# ---------------------------------------------------------------- HID-Gamepad
HID_BERICHT = bytes.fromhex(
    '05010905a101'                                  # Desktop, Gamepad, Application
    '05091901290c150025017501950c8102'              # 12 Knoepfe
    '750195048103'                                  # 4 Bit Fuellung
    '05010930093109320935150026ff007508950481 02'.replace(' ', '') +  # X Y Z Rz
    '0939150025073500463b016514750495018142'        # Hut, Nullzustand
    '750495018103'                                  # 4 Bit Fuellung
    'c0')

class HidPad(Geraet):
    hersteller, produkt, seriennr = 'pademu', 'USB Gamepad', '0001'
    def __init__(self):
        super().__init__()
        self.dd = dev_desc(0, 0, 0, 8, 0x0079, 0x0011, 0x0106)
        hid = struct.pack('<BBHBBBH', 9, 0x21, 0x0110, 0, 1, 0x22, len(HID_BERICHT))
        self.cd = config(1, iface(0, 0, 1, 3, 0, 0, hid) + ep(0x81, 3, 8, 10), attr=0x80, power=50)

    def schnittstellen(self):
        return [(0, 3, 0, 0)]

    def endpunkte(self):
        return {0x81: (EP_INT, 10, 0, 8)}

    def klasse(self, rt, req, wert, index, laenge, daten):
        if rt == 0x81 and req == 6 and wert >> 8 == 0x22:
            return OK, HID_BERICHT[:laenge]
        if rt == 0x81 and req == 6 and wert >> 8 == 0x21:
            return OK, self.cd[18:27][:laenge]
        if rt == 0x21 and req in (0x0A, 0x0B, 0x09):   # SET_IDLE, SET_PROTOCOL, SET_REPORT
            return OK, b''
        if rt == 0xA1 and req == 1:                    # GET_REPORT
            return OK, self._r()[:laenge]
        return STALL, b''

    def _r(self):
        z = self.zust
        k = (z['A'] | z['B'] << 1 | z['X'] << 2 | z['Y'] << 3 | z['LB'] << 4 | z['RB'] << 5 |
             z['BACK'] << 6 | z['START'] << 7 | z['LS'] << 8 | z['RS'] << 9 | z['GUIDE'] << 10)
        o, u, l, r = z['HOCH'], z['RUNTER'], z['LINKS'], z['RECHTS']
        hut = {(1,0,0,0): 0, (1,0,0,1): 1, (0,0,0,1): 2, (0,1,0,1): 3, (0,1,0,0): 4,
               (0,1,1,0): 5, (0,0,1,0): 6, (1,0,1,0): 7}.get((o, u, l, r), 8)
        ax = lambda v: max(0, min(255, (v + 32768) >> 8))
        return struct.pack('<HBBBBB', k, ax(z['LX']), ax(-z['LY'] - 1), ax(z['RX']),
                           ax(-z['RY'] - 1), hut)

    def bericht(self):
        if time.time() >= self.stumm_bis and self.senden and 0x81 in self.empfang:
            self.senden(0x81, self._r())

# ---------------------------------------------------------------- Mikrofon (UAC1)
import math

class Mikrofon(Geraet):
    hersteller, produkt, seriennr = 'pademu', 'USB Mikrofon', '0001'
    RATE = 48000
    def __init__(self):
        super().__init__()
        self.dd = dev_desc(0, 0, 0, 64, 0x0d8c, 0x0014, 0x0100)
        ac = (bytes.fromhex('0c240201010200010000 0000'.replace(' ', ''))      # IT 1: Mikrofon 0x0201, 1 Kanal
              + bytes.fromhex('092406020101030000')                           # FU 2: Master Stumm+Laut, K1 nichts
              + bytes.fromhex('092403030101000200'))                          # OT 3: USB-Strom 0x0101, Quelle 2
        hdr = struct.pack('<BBBHHBB', 9, 0x24, 0x01, 0x0100, 9 + len(ac), 1, 1)
        body = (iface(0, 0, 0, 1, 1, 0, hdr + ac)
                + iface(1, 0, 0, 1, 2, 0)
                + iface(1, 1, 1, 1, 2, 0,
                        bytes.fromhex('07240103010100')                       # AS: Terminal 3, PCM
                        + bytes.fromhex('0b2402010102100180bb00'))            # Typ I, 1 Kanal, 16 Bit, 48000
                + struct.pack('<BBBBHBBB', 9, 5, 0x82, 0x05, 98, 1, 0, 0)    # ISO IN, asynchron
                + bytes.fromhex('07250101000000'))                            # Rate einstellbar
        self.cd = config(2, body, attr=0x80, power=50)
        self.phase = 0.0
        self.stumm = 0
        self.laut = 0
        self.rate = self.RATE
        self.gesendet = 0

    def schnittstellen(self):
        return [(0, 1, 1, 0), (1, 1, 2, 0)]

    def endpunkte(self):
        if self.alts.get(1) == 1:
            return {0x82: (EP_ISO, 1, 1, 98)}
        return {}

    def klasse(self, rt, req, wert, index, laenge, daten):
        if rt == 0x22 and req == 0x01:                    # SET_CUR Rate am Endpunkt
            self.rate = daten[0] | daten[1] << 8 | daten[2] << 16
            log('  Gast: Abtastrate %d Hz' % self.rate)
            return OK, b''
        if rt == 0xA2 and req == 0x81:
            return OK, struct.pack('<I', self.rate)[:3]
        if rt == 0x21 and req == 0x01:
            if wert >> 8 == 1: self.stumm = daten[0]; log('  Gast: stumm %d' % self.stumm)
            if wert >> 8 == 2: self.laut = struct.unpack('<h', daten[:2])[0]; log('  Gast: Lautstaerke %d' % self.laut)
            return OK, b''
        if rt == 0xA1 and req in (0x81, 0x82, 0x83, 0x84):
            if wert >> 8 == 1: return OK, bytes([self.stumm])[:laenge]
            v = {0x81: self.laut, 0x82: -10240, 0x83: 1536, 0x84: 256}[req]
            return OK, struct.pack('<h', v)[:laenge]
        return STALL, b''

    def iso_paket(self):
        """48 Abtastungen = 1 ms, fortlaufender Sinus (1 kHz)."""
        n = self.rate // 1000
        st = 2 * math.pi * 1000 / self.rate
        out = bytearray()
        for _ in range(n):
            v = 0 if self.stumm else int(10000 * math.sin(self.phase))
            out += struct.pack('<h', v)
            self.phase += st
            if self.phase > math.pi: self.phase -= 2 * math.pi
        self.gesendet += 1
        return bytes(out)

# ---------------------------------------------------------------- Headset (UAC1)
class Headset(Mikrofon):
    hersteller, produkt, seriennr = 'pademu', 'Headset (wie HyperX)', '0002'
    def __init__(self):
        Geraet.__init__(self)
        self.dd = dev_desc(0, 0, 0, 64, 0x0951, 0x170b, 0x0100)
        ac = (bytes.fromhex('0c240201010100020300 0000'.replace(' ', ''))   # IT 1: USB-Strom, 2 Kanaele
              + bytes.fromhex('0c240206010200010000 0000'.replace(' ', ''))   # IT 6: Mikrofon, 1 Kanal
              + bytes.fromhex('0824060306010300')                           # FU 3 <- 6: Master Stumm+Laut (vorn!)
              + bytes.fromhex('0a240602010103000000')                       # FU 2 <- 1: Master, K1, K2
              + bytes.fromhex('092403040204000200')                         # OT 4: Headset 0x0402 <- 2
              + bytes.fromhex('092403070101000300'))                        # OT 7: USB-Strom <- 3
        hdr = struct.pack('<BBBHHBBB', 10, 0x24, 0x01, 0x0100, 10 + len(ac), 2, 1, 2)
        def fmt(kan, raten):
            r = b''.join(struct.pack('<I', x)[:3] for x in raten)
            return bytes([8 + len(r), 0x24, 0x02, 0x01, kan, 2, 16, len(raten)]) + r
        body = (iface(0, 0, 0, 1, 1, 0, hdr + ac)
                + iface(1, 0, 0, 1, 2, 0)
                + iface(1, 1, 1, 1, 2, 0, bytes.fromhex('07240107010100') + fmt(1, [8000, 16000, 32000, 44100, 48000]))
                + struct.pack('<BBBBHBBB', 9, 5, 0x81, 0x0D, 144, 1, 0, 0) + bytes.fromhex('07250101000000')
                + iface(2, 0, 0, 1, 2, 0)
                + iface(2, 1, 1, 1, 2, 0, bytes.fromhex('07240101010100') + fmt(1, [44100, 48000]))
                + struct.pack('<BBBBHBBB', 9, 5, 0x02, 0x0D, 144, 1, 0, 0) + bytes.fromhex('07250101000000')
                + iface(2, 2, 1, 1, 2, 0, bytes.fromhex('07240101010100') + fmt(2, [44100, 48000]))
                + struct.pack('<BBBBHBBB', 9, 5, 0x02, 0x0D, 288, 1, 0, 0) + bytes.fromhex('07250101000000'))
        self.cd = config(3, body, attr=0x80, power=100)
        self.phase = 0.0; self.rate = 48000; self.gesendet = 0
        # je Feature Unit: stumm, laut; Lautsprecher (2) stumm und auf Minimum
        self.fu = {2: {'stumm': 1, 'laut': -12288}, 3: {'stumm': 0, 'laut': 0}}
        self.stumm = 0                      # fuer iso_paket (Mikrofon)
        self.empf = 0
        open('/tmp/headset_aus.raw', 'wb').close()

    def schnittstellen(self):
        return [(0, 1, 1, 0), (1, 1, 2, 0), (2, 1, 2, 0)]

    def endpunkte(self):
        e = {}
        if self.alts.get(1) == 1:
            e[0x81] = (EP_ISO, 1, 1, 144)
        if self.alts.get(2) in (1, 2):
            e[0x02] = (EP_ISO, 1, 2, 144 if self.alts.get(2) == 1 else 288)
        return e

    def klasse(self, rt, req, wert, index, laenge, daten):
        if rt == 0x22 and req == 0x01:
            r = daten[0] | daten[1] << 8 | daten[2] << 16
            log('  Gast: Abtastrate %d Hz an EP %02x' % (r, index & 0xFF))
            if index & 0x80: self.rate = r
            else: self.rate_aus = r
            return OK, b''
        fu = self.fu.get(index >> 8)
        if fu is None:
            return STALL, b''
        if rt == 0x21 and req == 0x01:
            if wert >> 8 == 1: fu['stumm'] = daten[0]; log('  Gast: FU %d stumm %d' % (index >> 8, daten[0]))
            if wert >> 8 == 2: fu['laut'] = struct.unpack('<h', daten[:2])[0]; log('  Gast: FU %d Kanal %d Lautstaerke %d' % (index >> 8, wert & 0xFF, fu['laut']))
            if index >> 8 == 3: self.stumm = fu['stumm']
            return OK, b''
        if rt == 0xA1 and req in (0x81, 0x82, 0x83, 0x84):
            if wert >> 8 == 1: return OK, bytes([fu['stumm']])[:laenge]
            v = {0x81: fu['laut'], 0x82: -12288, 0x83: 0, 0x84: 256}[req]
            return OK, struct.pack('<h', v)[:laenge]
        return STALL, b''

    def iso_aus(self, ep, daten):
        """Wiedergabe: was ankommt, mitschreiben -- stumm/Minimum als Stille."""
        fu = self.fu[2]
        hoerbar = not fu['stumm'] and fu['laut'] > -12288
        with open('/tmp/headset_aus.raw', 'ab') as f:
            f.write(daten if hoerbar else bytes(len(daten)))
        self.empf += 1
        if self.empf == 1:
            kan = 1 if self.alts.get(2) == 1 else 2
            open('/tmp/headset_aus.txt', 'w').write('%d %d\n' % (getattr(self, 'rate_aus', 48000), kan))
            log('  erstes Wiedergabepaket (%d Byte), Alt %d, %s' % (len(daten), self.alts.get(2), 'hoerbar' if hoerbar else 'STUMM/Minimum'))

# ---------------------------------------------------------------- Verbindung
class Redir:
    def __init__(self, sock, geraet):
        self.s, self.g = sock, geraet
        self.wlock = threading.Lock()
        self.eigene_id = 1 << 30
        self.id64 = False          # nach beiden hello: 16-Byte-Kopf
        self.iso_an = False
        geraet.senden = self.int_senden

    def schreibe(self, typ, kennung, nutz=b''):
        kopf = (struct.pack('<IIQ', typ, len(nutz), kennung) if self.id64
                else struct.pack('<III', typ, len(nutz), kennung))
        with self.wlock:
            self.s.sendall(kopf + nutz)

    def lies_genau(self, n):
        b = b''
        while len(b) < n:
            t = self.s.recv(n - len(b))
            if not t:
                raise EOFError
            b += t
        return b

    def hello(self):
        caps = ((1 << CAP_BULK_STREAMS) | (1 << CAP_CONNECT_DEVICE_VERSION) |
                (1 << CAP_EP_INFO_MAX_PACKET_SIZE) | (1 << CAP_64BITS_IDS) |
                (1 << CAP_32BITS_BULK_LENGTH))
        self.schreibe(HELLO, 0, b'pademu 1.0'.ljust(64, b'\0') + struct.pack('<I', caps))

    def melde_geraet(self):
        g = self.g
        n = g.schnittstellen()
        ii = struct.pack('<I', len(n))
        for feld in range(4):
            ii += bytes([x[feld] for x in n]).ljust(32, b'\0')
        self.schreibe(IF_INFO, 0, ii)
        typ, iv, ifc, mps = [EP_INVALID] * 32, [0] * 32, [0] * 32, [0] * 32
        typ[0] = typ[16] = EP_CONTROL
        mps[0] = mps[16] = g.dd[7]
        for adr, (t, i, f, m) in g.endpunkte().items():
            k = ep_index(adr)
            typ[k], iv[k], ifc[k], mps[k] = t, i, f, m
        self.schreibe(EP_INFO, 0, bytes(typ) + bytes(iv) + bytes(ifc) + struct.pack('<32H', *mps) + bytes(32 * 4))
        dd = g.dd
        self.schreibe(DEV_CONNECT, 0, struct.pack('<BBBBHHH', g.speed, dd[4], dd[5], dd[6],
                      *struct.unpack('<HHH', dd[8:14])))
        log('Geraet gemeldet: %04x:%04x' % struct.unpack('<HH', dd[8:12]))

    def iso_senden(self, ep):
        """Im Millisekundentakt (Wanduhr) ein Paket -- QEMU puffert."""
        t = time.monotonic()
        while self.iso_an:
            t += 0.001
            d = self.g.iso_paket()
            self.eigene_id += 1
            try:
                self.schreibe(ISO, self.eigene_id, struct.pack('<BBH', ep, OK, len(d)) + d)
            except OSError:
                return
            w = t - time.monotonic()
            if w > 0:
                time.sleep(w)
            elif w < -0.1:
                t = time.monotonic()            # weit zurueck: nicht nachholen

    def int_senden(self, ep, daten):
        self.eigene_id += 1
        self.schreibe(INTERRUPT, self.eigene_id, struct.pack('<BBH', ep, OK, len(daten)) + daten)

    def lauf(self):
        self.hello()
        gemeldet = False
        while True:
            if self.id64:
                typ, laenge, kennung = struct.unpack('<IIQ', self.lies_genau(16))
            else:
                typ, laenge, kennung = struct.unpack('<III', self.lies_genau(12))
            nutz = self.lies_genau(laenge) if laenge else b''
            if typ == HELLO:
                log('QEMU:', nutz[:64].rstrip(b'\0').decode(errors='replace'))
                pcaps = struct.unpack('<I', nutz[64:68])[0] if len(nutz) >= 68 else 0
                self.id64 = bool(pcaps & (1 << CAP_64BITS_IDS))
                log('  Faehigkeiten des Gastes %08x, 64-Bit-Kennungen: %s' % (pcaps, self.id64))
                if not gemeldet:
                    self.melde_geraet(); gemeldet = True
            elif typ == RESET:
                log('Gast: Reset')
                self.g.konf = 0
            elif typ == SET_CONF:
                self.g.konf = nutz[0]
                log('Gast: set_configuration %d' % nutz[0])
                self.schreibe(CONF_STATUS, kennung, bytes([OK, nutz[0]]))
            elif typ == GET_CONF:
                self.schreibe(CONF_STATUS, kennung, bytes([OK, self.g.konf]))
            elif typ == SET_ALT:
                self.g.alts[nutz[0]] = nutz[1]
                log('Gast: set_alt_setting Schnittstelle %d -> %d' % (nutz[0], nutz[1]))
                self.schreibe(ALT_STATUS, kennung, bytes([OK, nutz[0], nutz[1]]))
                self.melde_geraet_eps()
            elif typ == GET_ALT:
                self.schreibe(ALT_STATUS, kennung, bytes([OK, nutz[0], self.g.alts.get(nutz[0], 0)]))
            elif typ == START_INT:
                ep = nutz[0]
                self.g.empfang.add(ep)
                self.g.stumm_bis = time.time() + STUMM
                log('Gast liest Endpunkt %02x' % ep)
                self.schreibe(INT_STATUS, kennung, bytes([OK, ep]))
                if isinstance(self.g, XboxOne) and ep == 0x82:
                    threading.Timer(0.3, self.g.ankuendigen).start()
                else:
                    threading.Timer(0.2, self.g.bericht).start()
            elif typ == START_ISO:
                ep, ppu, nurb = struct.unpack('<BBB', nutz[:3])
                log('Gast startet ISO-Strom %02x (%d Pakete/URB, %d URBs)' % (ep, ppu, nurb))
                self.schreibe(ISO_STATUS, kennung, bytes([OK, ep]))
                if ep & 0x80 and hasattr(self.g, 'iso_paket'):
                    self.iso_an = True
                    threading.Thread(target=self.iso_senden, args=(ep,), daemon=True).start()
            elif typ == STOP_ISO:
                self.iso_an = False
                log('Gast stoppt ISO-Strom %02x (%d Pakete gesendet)' % (nutz[0], getattr(self.g, 'gesendet', 0)))
                self.schreibe(ISO_STATUS, kennung, bytes([OK, nutz[0]]))
            elif typ == STOP_INT:
                self.g.empfang.discard(nutz[0])
                self.schreibe(INT_STATUS, kennung, bytes([OK, nutz[0]]))
            elif typ == CONTROL:
                ep, req, rt, _st, wert, index, ln = struct.unpack('<BBBBHHH', nutz[:10])
                daten = nutz[10:]
                st, antwort = self.g.steuer(rt, req, wert, index, ln, daten)
                if st != OK:
                    log('  control rt=%02x req=%02x wert=%04x index=%04x -> STALL' % (rt, req, wert, index))
                kopf = struct.pack('<BBBBHHH', ep, req, rt, st, wert, index,
                                   len(antwort) if rt & 0x80 else (len(daten) if st == OK else 0))
                self.schreibe(CONTROL, kennung, kopf + (antwort if rt & 0x80 else b''))
            elif typ == INTERRUPT:
                ep, _st, ln = struct.unpack('<BBH', nutz[:4])
                self.g.eingang_out(ep, nutz[4:])
                self.schreibe(INTERRUPT, kennung, struct.pack('<BBH', ep, OK, ln))
            elif typ == ISO:
                ep, _st, ln = struct.unpack('<BBH', nutz[:4])
                if hasattr(self.g, 'iso_aus'):
                    self.g.iso_aus(ep, nutz[4:])
            elif typ == BULK:
                ep, _st, ln, sid, lnh = struct.unpack('<BBHIH', nutz[:10])
                self.schreibe(BULK, kennung, struct.pack('<BBHIH', ep, STALL, 0, sid, 0))
            elif typ == CANCEL:
                pass
            elif typ == FILTER_FILTER:
                pass
            else:
                log('unbehandelt: %s (%d) %d Byte' % (NAMEN.get(typ, '?'), typ, laenge))

    def melde_geraet_eps(self):
        g = self.g
        typ, iv, ifc, mps = [EP_INVALID] * 32, [0] * 32, [0] * 32, [0] * 32
        typ[0] = typ[16] = EP_CONTROL
        mps[0] = mps[16] = g.dd[7]
        for adr, (t, i, f, m) in g.endpunkte().items():
            k = ep_index(adr)
            typ[k], iv[k], ifc[k], mps[k] = t, i, f, m
        self.schreibe(EP_INFO, 0, bytes(typ) + bytes(iv) + bytes(ifc) + struct.pack('<32H', *mps) + bytes(32 * 4))

# ---------------------------------------------------------------- Steuerung
def steuerung(pfad, g):
    if os.path.exists(pfad):
        os.unlink(pfad)
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(pfad); srv.listen(4)
    knoepfe = set(g.zust)
    while True:
        c, _ = srv.accept()
        f = c.makefile('rw')
        for zeile in f:
            w = zeile.split()
            if not w:
                continue
            n = w[0].upper()
            if n == 'RAD' and len(w) == 2 and hasattr(g, 'fu'):
                #  Lautstaerkerad am Headset: aendert den Wert IM GERAET,
                #  ohne dass der Rechner etwas davon weiss.  So laesst sich
                #  pruefen, ob der Mixer die Aenderung bemerkt.
                db = int(w[1])
                g.fu[2]['laut'] = max(-12288, min(0, db))
                log('  Rad am Headset: FU 2 Lautstaerke %d' % g.fu[2]['laut'])
                f.write('ok %d\n' % g.fu[2]['laut'])
            elif n == 'LOS':
                for k in g.zust: g.zust[k] = 0
                g.bericht(); f.write('ok\n')
            elif n == 'ZUSTAND':
                f.write(' '.join('%s=%d' % kv for kv in g.zust.items() if kv[1]) + '\n')
            elif n in knoepfe and len(w) == 2:
                g.gesetzt(n, int(w[1])); f.write('ok\n')
            else:
                f.write('? %s\n' % zeile.strip())
            f.flush()
        c.close()

def main():
    a = argparse.ArgumentParser()
    a.add_argument('--art', default='360', choices=['360', 'one', 'hid', 'mikro', 'headset'])
    a.add_argument('--sock', default='/tmp/pad.sock')
    a.add_argument('--steuer', default='/tmp/pad.ctl')
    a.add_argument('--stumm', type=float, default=0,
                   help='erst so viele Sekunden nach dem Einstecken Eingaben senden')
    a.add_argument('--pid', help='Produktkennung (hex) fuer --art one, z. B. 0b12 = Series X|S')
    o = a.parse_args()
    if o.pid:
        XboxOne.pid = int(o.pid, 16)
    global STUMM
    STUMM = o.stumm
    g = {'360': Xbox360, 'one': XboxOne, 'hid': HidPad, 'mikro': Mikrofon, 'headset': Headset}[o.art]()
    threading.Thread(target=steuerung, args=(o.steuer, g), daemon=True).start()
    # Halte-Bericht: echte Controller schicken bei Aenderung; zusaetzlich
    # alle 0,5 s, damit ein spaet startender Leser den Zustand sieht.
    def puls():
        while True:
            time.sleep(0.5); g.bericht()
    threading.Thread(target=puls, daemon=True).start()
    while True:
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(o.sock)
        except OSError:
            time.sleep(0.5); continue
        log('verbunden mit', o.sock, '-- Art', o.art)
        try:
            Redir(s, g).lauf()
        except (EOFError, ConnectionError) as e:
            log('Verbindung beendet', e)
            g.empfang.clear(); time.sleep(1)

if __name__ == '__main__':
    main()
