#!/usr/bin/env python3
"""
sende_datei.py -- eine grosse Datei zur NT-4.0-Maschine uebertragen.

Der Agent liest Auftragszeilen mit hoechstens 1400 Zeichen, und jedes
PUT ueberschreibt die Zieldatei (fopen "wb").  Grosse Dateien gehen
deshalb nur in Stuecken:  jedes Stueck als eigene kleine Datei, danach
auf der Maschine mit  copy /b  zusammensetzen.

    sende_datei.py <lokale_datei> <ziel_auf_nt> [praefix]

Grosse Dateien werden auf mehrere Auftraege verteilt (je hoechstens 90
Stuecke); der erste legt die Zieldatei an, die weiteren haengen an.

Beispiel:
    sende_datei.py /tmp/wz/usbaudio.dll "C:\\crc\\usbaudio.dll" /tmp/a.txt
    crc_claude.py lauf /tmp/a.txt
"""
import base64
import os
import sys

#  1400 ist die Grenze im Agenten (char zeile[1400]).  Platz lassen fuer
#  "PUT <pfad> " und die Null -- mit 1200 base64-Zeichen (900 Byte) bleibt
#  reichlich Luft, auch fuer laengere Pfade.
B64_JE_ZEILE = 1200
TEILORDNER = r"C:\crc\teile"


JE_AUFTRAG = 90                                 # Stuecke je Auftrag


def auftrag(lokal, ziel, stuecke, erster, letzter, nr):
    zeilen = ["CMD   if not exist %s mkdir %s" % (TEILORDNER, TEILORDNER)]
    namen = []
    for n, s in enumerate(stuecke):
        name = "%s\\t%02d.bin" % (TEILORDNER, n)
        namen.append(name)
        zeilen.append("PUT %s %s" % (name, base64.b64encode(s).decode()))

    #  copy /b  a+b+c  ziel  -- die Befehlszeile darf nicht zu lang werden,
    #  deshalb in Gruppen von acht anhaengen.  Nur der erste Auftrag legt
    #  die Zieldatei neu an, die weiteren haengen an.
    if erster:
        zeilen.append("CMD   if exist %s del %s" % (ziel, ziel))
    erste = erster
    for i in range(0, len(namen), 8):
        gruppe = "+".join(namen[i:i + 8])
        if erste:
            zeilen.append("CMD   copy /b %s %s" % (gruppe, ziel))
            erste = False
        else:
            zeilen.append("CMD   copy /b %s+%s %s" % (ziel, gruppe, ziel))
    zeilen.append("CMD   dir %s" % ziel)
    zeilen.append("CMD   rmdir /s /q %s" % TEILORDNER)
    zeilen.append("MELDE Teil %d%s" % (nr, " -- fertig" if letzter else ""))
    return "\n".join(zeilen) + "\n"


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    lokal, ziel = sys.argv[1], sys.argv[2]
    praefix = sys.argv[3] if len(sys.argv) > 3 else "/tmp/auftrag"
    daten = open(lokal, "rb").read()
    roh = B64_JE_ZEILE // 4 * 3
    alle = [daten[i:i + roh] for i in range(0, len(daten), roh)]
    gruppen = [alle[i:i + JE_AUFTRAG] for i in range(0, len(alle), JE_AUFTRAG)]
    print("%s -> %s  (%d Byte, %d Stuecke, %d Auftraege)"
          % (lokal, ziel, len(daten), len(alle), len(gruppen)))
    for k, g in enumerate(gruppen):
        text = auftrag(lokal, ziel, g, k == 0, k == len(gruppen) - 1, k + 1)
        name = "%s%d.txt" % (praefix, k + 1)
        open(name, "w").write(text)
        lang = max(len(z) for z in text.splitlines())
        print("  %s  %d Zeilen, laengste %d%s"
              % (name, len(text.splitlines()), lang,
                 "  ZU LANG!" if lang >= 1400 else ""))


if __name__ == "__main__":
    main()
