#!/usr/bin/python3.12
"""
usb_einbinden.py -- den FreeBSD-USB-Treiber in eine NT-4.0-Installations-CD
einbinden, so dass das installierte System USB von Anfang an kann
(Controller UHCI/OHCI/EHCI/xHCI, Tastatur, Maus, Massenspeicher, CD/DVD).

    usb_einbinden.py <cd-verz> <usbnt.sys> <werkzeug-verz>

  1. I386\\USBNT.SYS und Eintrag in TXTSETUP.SIF [SourceDisksFiles]
     (Textsetup kopiert ihn nach system32\\drivers, Verzeichnis 4).
  2. Registry-Vorlage I386\\SYSTEM (wird von Textsetup nach
     system32\\config kopiert) -- genau das, was "USBINST ein" eintraegt:
       Services\\usbnt   Type 1, Start 1, ErrorControl 1,
                        Group "Keyboard Port", ImagePath, Parameters\\Ausgabe 0
       Services\\Kbdclass\\Parameters\\ConnectMultiplePorts = 1
       Services\\Mouclass\\Parameters\\ConnectMultiplePorts = 1
     Danach lh -> lf (hivex legt lh an; NT 4.0 kennt nur lf).
  3. Die Werkzeuge (USBINST, USBGERAET, Geraetemanager ...) nach \\USBNT.
  4. Textmodus-Setup (Start vom USB-CD/DVD-Laufwerk, USB-Tastatur):
       TXTSETUP.SIF [Extenders.Load]  usbnt = usbnt.sys,4
         -> SETUPLDR laedt usbnt als Starttreiber; usbnt wartet im Setup
            (Dienst setupdd vorhanden) bis zu 15 s auf das USB-Laufwerk,
            damit NT den ARC-Namen multi(0)disk(0)cdrom(N) anlegen kann.
       SETUPREG.HIV  Kbdclass/Mouclass\\Parameters MaximumPortsServiced 2,
                     ConnectMultiplePorts 1 (USB- und PS/2-Tastatur),
                     usbnt\\Parameters Ausgabe 0
"""
import sys, os, shutil, struct, hivex

cd, sysdatei, werkzeug = sys.argv[1:4]
i386 = os.path.join(cd, 'I386')

# 1. Datei + TXTSETUP.SIF
shutil.copyfile(sysdatei, os.path.join(i386, 'USBNT.SYS'))
sif = os.path.join(i386, 'TXTSETUP.SIF')
d = open(sif, 'rb').read()
if b'\nusbnt.sys' not in d.lower():
    marke = b'\r\nuniata.sys'
    i = d.find(marke)
    assert i > 0, 'uniata-Zeile nicht gefunden'
    d = d[:i] + b'\r\nusbnt.sys    = 1,,,,,,,4,0,0' + d[i:]
    open(sif, 'wb').write(d)

# 2. Registry-Vorlage
hiv = os.path.join(i386, 'SYSTEM')
h = hivex.Hivex(hiv, write=True)
def knoten(pfad):
    n = h.root()
    for teil in pfad.split('\\'):
        k = h.node_get_child(n, teil)
        n = k if k else h.node_add_child(n, teil)
    return n
def dw(n, name, wert):
    h.node_set_value(n, {'key': name, 't': 4, 'value': struct.pack('<I', wert)})
def sz(n, name, text):
    h.node_set_value(n, {'key': name, 't': 1, 'value': (text + '\0').encode('utf-16le')})
for cs in ('ControlSet001',):
    s = knoten(cs + '\\Services\\usbnt')
    dw(s, 'Type', 1); dw(s, 'Start', 1); dw(s, 'ErrorControl', 1)
    sz(s, 'Group', 'Keyboard Port')
    sz(s, 'ImagePath', 'System32\\drivers\\usbnt.sys')
    sz(s, 'DisplayName', 'FreeBSD-USB fuer NT 4.0 (usbnt)')
    dw(knoten(cs + '\\Services\\usbnt\\Parameters'), 'Ausgabe', int(os.environ.get('AUSGABE', '0')))
    dw(knoten(cs + '\\Services\\Kbdclass\\Parameters'), 'ConnectMultiplePorts', 1)
    dw(knoten(cs + '\\Services\\Mouclass\\Parameters'), 'ConnectMultiplePorts', 1)
h.commit(None); del h

def lh_nach_lf(datei):
    d = bytearray(open(datei, 'rb').read()); n = 0; pos = 0x1000
    while pos < len(d) and d[pos:pos + 4] == b'hbin':
        groesse = struct.unpack_from('<I', d, pos + 8)[0]; c = pos + 0x20
        while c < pos + groesse:
            cg = struct.unpack_from('<i', d, c)[0]
            if cg == 0: break
            if cg < 0 and d[c + 4:c + 6] == b'lh':
                anz = struct.unpack_from('<H', d, c + 6)[0]; d[c + 4:c + 6] = b'lf'
                for i in range(anz):
                    nk = 0x1000 + struct.unpack_from('<I', d, c + 8 + i * 8)[0] + 4
                    nl = struct.unpack_from('<H', d, nk + 0x48)[0]
                    fl = struct.unpack_from('<H', d, nk + 2)[0]
                    name = bytes(d[nk + 0x4C:nk + 0x4C + nl])
                    if not (fl & 0x20): name = name.decode('utf-16le').encode('latin-1', 'replace')
                    d[c + 12 + i * 8:c + 16 + i * 8] = (name[:4] + b'\0\0\0\0')[:4]
                n += 1
            c += abs(cg)
        pos += groesse
    # Kopfpruefsumme (XOR der ersten 508 Byte) neu
    s = 0
    for i in range(0, 508, 4): s ^= struct.unpack_from('<I', d, i)[0]
    struct.pack_into('<I', d, 508, s)
    open(datei, 'wb').write(d); return n
print('lh->lf:', lh_nach_lf(hiv))

# 3. Werkzeuge
ziel = os.path.join(cd, 'USBNT')
if os.path.exists(ziel): shutil.rmtree(ziel)
shutil.copytree(werkzeug, ziel)
shutil.copyfile(sysdatei, os.path.join(ziel, 'USBNT.SYS'))
# 4. Textmodus-Setup
d = open(sif, 'rb').read()
marke = b'[Extenders.Load]\r\npcmcia = pcmcia.sys,4\r\n'
assert marke in d, '[Extenders.Load] nicht gefunden'
if b'usbnt  = usbnt.sys' not in d:
    d = d.replace(marke, marke + b'usbnt  = usbnt.sys,4\r\n')
    open(sif, 'wb').write(d)
sreg = os.path.join(i386, 'SETUPREG.HIV')
tmp = sreg + '.neu'; shutil.copyfile(sreg, tmp); os.replace(tmp, sreg)   # Hardlink loesen
h = hivex.Hivex(sreg, write=True)
for k in ('Kbdclass', 'Mouclass'):
    p = knoten('ControlSet001\\Services\\%s\\Parameters' % k)
    dw(p, 'MaximumPortsServiced', 2); dw(p, 'ConnectMultiplePorts', 1)
dw(knoten('ControlSet001\\Services\\usbnt\\Parameters'), 'Ausgabe', int(os.environ.get('AUSGABE', '0')))
h.commit(None); del h
print('SETUPREG lh->lf:', lh_nach_lf(sreg))
print('fertig')
