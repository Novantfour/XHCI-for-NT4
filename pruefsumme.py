#!/usr/bin/env python3
"""
pruefsumme.py -- die PE-Pruefsumme setzen und nachrechnen.

    pruefsumme.py <datei.sys> [--pruefen]

NT 4.0 prueft bei Kerntreibern die Pruefsumme im Kopf.  Steht dort eine
Null -- und lld-link laesst sie ohne /release auf Null --, meldet der
Dienstverwalter beim Starten

    0xC0000221   STATUS_IMAGE_CHECKSUM_MISMATCH

und sagt dazu nichts weiter.  Der Treiber ist dabei vollkommen in
Ordnung; er wird nur nicht angesehen.  Fuer Programme im
Benutzermodus prueft NT nicht, deshalb laufen die .exe ohne das hier.

Das Verfahren (seit NT 3.1 unveraendert):

    1  Das Pruefsummenfeld selbst als Null lesen.
    2  Alle 16-Bit-Woerter der Datei mit Uebertragsrueckfuehrung
       addieren -- Einerkomplement.
    3  Die Dateigroesse dazu.

Ein ungerades letztes Byte wird mit Null aufgefuellt.
"""
import sys, struct

def summe(d, pos):
    s = 0

    for i in range(0, len(d) - 1, 2):

        if i == pos or i == pos + 2:
            continue                 # das Feld ist VIER Byte, also zwei
                                     # Woerter -- eines zu ueberspringen
                                     # ergibt eine Summe, die um den
                                     # oberen Halbteil danebenliegt

        s += d[i] | (d[i + 1] << 8)
        s = (s & 0xFFFF) + (s >> 16)

    if len(d) & 1:
        s += d[-1]
        s = (s & 0xFFFF) + (s >> 16)

    return ((s & 0xFFFF) + (s >> 16) + len(d)) & 0xFFFFFFFF

datei = sys.argv[1]
nur_pruefen = '--pruefen' in sys.argv

d = bytearray(open(datei, 'rb').read())
pe = struct.unpack_from('<I', d, 0x3c)[0]
pos = pe + 24 + 64                   # OptionalHeader.CheckSum

alt  = struct.unpack_from('<I', d, pos)[0]
soll = summe(d, pos)

if nur_pruefen:
    print('%s  im Kopf %08x  gerechnet %08x  %s'
          % (datei, alt, soll, 'stimmt' if alt == soll else 'FALSCH'))
    sys.exit(0 if alt == soll else 1)

struct.pack_into('<I', d, pos, soll)
open(datei, 'wb').write(d)
print('%s  Pruefsumme %08x gesetzt (vorher %08x)' % (datei, soll, alt))
