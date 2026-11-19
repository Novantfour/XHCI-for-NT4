#!/usr/bin/env python3
"""
mk-usbtasten-ico.py -- erzeugt usbtasten.ico (Symbol im Infobereich).

USBTASTEN ist seit 2026.11.19.71 vor allem "USB-Laufwerk sicher entfernen"
(dazu die Sondertasten).  Das alte Lautsprecher-Symbol passte nicht mehr.
Neu: ein USB-Stick mit gruenem Haken.

Wie die anderen Symbole hier: 16x16 und 32x32, je 4 Bit (16 Farben) und
8 Bit (256 Farben), Durchsichtigkeit ueber die UND-Maske -- NT 4.0 kennt
keine 32-Bit-Symbole mit Alphakanal und keine PNG-Symbole.

Aufruf:  python3 mk-usbtasten-ico.py [ausgabe.ico]   (Vorgabe: usbtasten.ico)
"""

import struct
import sys

# Die 16 Standardfarben (VGA-/Windows-Grundpalette)
PAL = [
    (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
    (0, 0, 128), (128, 0, 128), (0, 128, 128), (192, 192, 192),
    (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]
T = None                       # durchsichtig
K, DG, NV, TE, SI, GR, LG, WE, BL = 0, 2, 4, 6, 7, 8, 10, 15, 12



def leer(n):
    return [[T] * n for _ in range(n)]


def rechteck(px, x0, y0, x1, y1, rand, innen):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px[y][x] = rand if (x in (x0, x1) or y in (y0, y1)) else innen


def punkt(px, x, y, c):
    if 0 <= y < len(px) and 0 <= x < len(px):
        px[y][x] = c


def linie(px, a, b, c, dick=1):
    (x0, y0), (x1, y1) = a, b
    n = max(abs(x1 - x0), abs(y1 - y0)) * 4 or 1
    for k in range(n + 1):
        x = x0 + (x1 - x0) * k / n
        y = y0 + (y1 - y0) * k / n
        for dx in range(dick):
            for dy in range(dick):
                punkt(px, int(round(x)) + dx, int(round(y)) + dy, c)


def plakette(px, cx, cy, r):
    """Gruener Kreis mit dunklem Rand: 'erledigt, sicher'."""
    n = len(px)
    for y in range(n):
        for x in range(n):
            d = ((x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2) ** 0.5
            if d <= r - 1.0:
                px[y][x] = LG
            elif d <= r:
                px[y][x] = DG


def stick(n):
    px = leer(n)
    if n == 32:
        rechteck(px, 10, 1, 20, 8, K, SI)               # Stecker
        for x, y in ((12, 3), (13, 3), (12, 4), (13, 4),
                     (17, 3), (18, 3), (17, 4), (18, 4)):
            px[y][x] = K                                 # Kontaktfenster
        rechteck(px, 7, 8, 23, 28, NV, TE)               # Gehaeuse
        for y in range(9, 28):
            px[y][8] = BL                                # Lichtkante links
        for x in range(8, 23):
            px[9][x] = BL                                # und oben
        # USB-Dreizack (weiss), klein in der Mitte
        linie(px, (15, 11), (15, 22), WE)
        for x, y in ((14, 12), (16, 12), (13, 13), (17, 13)):
            px[y][x] = WE                                # Pfeilspitze
        linie(px, (15, 19), (11, 16), WE)
        linie(px, (11, 16), (11, 14), WE)
        for x, y in ((10, 13), (11, 13), (12, 13)):
            px[y][x] = WE                                # Kreis links
        linie(px, (15, 18), (19, 15), WE)
        for x, y in ((19, 14), (20, 14), (19, 13), (20, 13)):
            px[y][x] = WE                                # Quadrat rechts
        for x, y in ((14, 22), (16, 22), (14, 23), (15, 23), (16, 23)):
            px[y][x] = WE                                # Fuss
        plakette(px, 24.5, 24.5, 7.5)
        linie(px, (20, 24), (23, 27), WE, 2)             # Haken
        linie(px, (23, 27), (28, 21), WE, 2)
    else:
        rechteck(px, 4, 0, 9, 4, K, SI)                  # Stecker
        px[2][5] = K; px[2][8] = K                       # Kontaktfenster
        rechteck(px, 2, 4, 11, 14, NV, TE)               # Gehaeuse
        for y in range(5, 14):
            px[y][3] = BL
        for x in range(3, 11):
            px[5][x] = BL
        linie(px, (6, 7), (6, 11), WE)                   # angedeuteter Dreizack
        px[8][5] = WE; px[8][7] = WE
        plakette(px, 11.5, 11.5, 4.6)
        linie(px, (9, 11), (10, 12), WE)                 # Haken
        linie(px, (10, 12), (13, 9), WE)
    return px


def dib(px, n, bpp):
    """BITMAPINFOHEADER + Palette + XOR-Bild + UND-Maske (von unten nach oben)."""
    farben = 16 if bpp == 4 else 256
    kopf = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, bpp, 0, 0, 0, 0,
                       16 if bpp == 4 else 256, 0)
    pal = b''.join(struct.pack('<BBBB', b, g, r, 0) for (r, g, b) in PAL)
    pal += b'\0\0\0\0' * (farben - 16)

    xor = b''
    und = b''
    zeile_xor = ((n * bpp + 31) // 32) * 4
    zeile_und = ((n + 31) // 32) * 4
    for y in range(n - 1, -1, -1):
        z = px[y]
        if bpp == 4:
            b = bytearray()
            for x in range(0, n, 2):
                a = z[x] if z[x] is not None else 0
                c = z[x + 1] if z[x + 1] is not None else 0
                b.append((a << 4) | c)
        else:
            b = bytearray((c if c is not None else 0) for c in z)
        xor += bytes(b) + b'\0' * (zeile_xor - len(b))

        m = bytearray(zeile_und)
        for x in range(n):
            if z[x] is None:
                m[x // 8] |= 0x80 >> (x % 8)
        und += bytes(m)
    return kopf + pal + xor + und


def main():
    ziel = sys.argv[1] if len(sys.argv) > 1 else 'usbtasten.ico'
    bilder = []
    for n in (16, 32):
        px = stick(n)
        for bpp in (4, 8):
            bilder.append((n, bpp, dib(px, n, bpp)))

    kopf = struct.pack('<HHH', 0, 1, len(bilder))
    off = 6 + 16 * len(bilder)
    eintraege = b''
    daten = b''
    for n, bpp, d in bilder:
        eintraege += struct.pack('<BBBBHHII', n, n, 16 if bpp == 4 else 0, 0,
                                 1, bpp, len(d), off + len(daten))
        daten += d
    with open(ziel, 'wb') as f:
        f.write(kopf + eintraege + daten)


if __name__ == '__main__':
    main()
