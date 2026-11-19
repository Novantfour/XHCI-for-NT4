#!/usr/bin/env python3
"""
mk-usbtasten-ico.py -- erzeugt die Symbole von USBTASTEN.EXE (Infobereich).

Vier Zustaende, angelehnt an einen Entwurf des Nutzers (USB-Dreizack,
hell mit dunklem Umriss, Plakette unten rechts), aber Pixel fuer Pixel
fuer 16x16 und 32x32 gesetzt -- verkleinerte Grossbilder mit weichem
Schatten sind im Infobereich nicht lesbar, und NT 4.0 kennt weder
Alphakanal noch PNG-Symbole:

  usbtasten.ico         kein USB-Laufwerk (nur der Dreizack); auch EXE-Symbol
  usb-haken.ico         ein Laufwerk angeschlossen (gruener Haken)
  usb-auswurf.ico       Auswurf laeuft / kann herausgezogen werden
  usb-anzahl-2..9.ico   mehrere Laufwerke, Zahl auf dunkler Plakette
  usb-anzahl-mehr.ico   zehn und mehr ("+")

Je 16 und 32 Pixel, 4 Bit (VGA-Grundfarben) und 8 Bit (eigene Palette),
Durchsichtigkeit ueber die UND-Maske.

Aufruf:  python3 mk-usbtasten-ico.py [zielordner]   (Vorgabe: Ordner des Skripts)
"""

import math
import os
import struct
import sys

# Die 16 Standardfarben (VGA-/Windows-Grundpalette)
PAL = [
    (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
    (0, 0, 128), (128, 0, 128), (0, 128, 128), (192, 192, 192),
    (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]
FARBEN = {
 'K': (40, 42, 48),     # Umriss
 'W': (250, 250, 250),  # Dreizack hell
 'S': (205, 208, 214),  # Dreizack Schatten
 'g': (24, 110, 40),    # Gruen Rand
 'G': (52, 170, 74),    # Gruen
 'H': (110, 210, 120),  # Gruen Glanz
 'B': (58, 62, 70),     # Zaehler dunkel
 'R': (235, 235, 235),  # Zaehler-Rand hell
 'X': (255, 255, 255),  # Zeichen weiss
}
# 4-Bit: feste Zuordnung zur VGA-Palette
VGA4 = {'K':0,'W':15,'S':7,'g':2,'G':10,'H':10,'B':0,'R':15,'X':15}

def neu(n): return [[None]*n for _ in range(n)]

def seg_d(px, py, a, b):
    (ax, ay), (bx, by) = a, b
    dx, dy = bx-ax, by-ay
    t = ((px-ax)*dx + (py-ay)*dy) / (dx*dx+dy*dy or 1)
    t = max(0, min(1, t))
    return math.hypot(px-(ax+t*dx), py-(ay+t*dy))

def form(n, teile):
    """teile: Liste von Funktionen f(x,y)->bool (Pixelmitte)."""
    m = [[False]*n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            cx, cy = x+0.5, y+0.5
            m[y][x] = any(f(cx, cy) for f in teile)
    return m

def linie(a, b, r): return lambda x, y: seg_d(x, y, a, b) <= r
def kreis(c, r): return lambda x, y: math.hypot(x-c[0], y-c[1]) <= r
def kasten(x0, y0, x1, y1): return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1
def dreieck(p):
    def f(x, y):
        s = []
        for i in range(3):
            (ax, ay), (bx, by) = p[i], p[(i+1)%3]
            s.append((bx-ax)*(y-ay)-(by-ay)*(x-ax))
        return all(v >= -0.01 for v in s) or all(v <= 0.01 for v in s)
    return f

def umriss(m, acht=True):
    n = len(m); o = [[False]*n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            if m[y][x]: continue
            for dx in (-1,0,1):
                for dy in (-1,0,1):
                    if not acht and dx and dy: continue
                    xx, yy = x+dx, y+dy
                    if 0 <= xx < n and 0 <= yy < n and m[yy][xx]:
                        o[y][x] = True
    return o

def malen(px, m, farbe):
    for y in range(len(m)):
        for x in range(len(m)):
            if m[y][x]: px[y][x] = farbe


def dreizack(n):
    if n == 16:
        f = [[False]*16 for _ in range(16)]
        pts = [(6,1),(5,2),(6,2),(7,2),(4,3),(5,3),(6,3),(7,3),(8,3)]
        pts += [(6,y) for y in range(4,12)]
        pts += [(x,y) for x in (1,2,3) for y in (5,6,7)]
        pts += [(2,8),(3,9),(4,10),(5,11)]
        pts += [(x,y) for x in (10,11,12) for y in (6,7,8)]
        pts += [(11,9),(10,10),(9,10),(8,11),(7,11)]
        pts += [(x,y) for x in (5,6,7) for y in (12,13,14)]
        for x, y in pts: f[y][x] = True
        return f
    r = 1.1
    teile = [
      dreieck([(12,1.5), (7,8.5), (17,8.5)]),
      linie((12,8), (12,26), r),
      kreis((12,27.6), 3.3),
      kreis((5,14), 3.0),
      linie((5,16), (5,19), r), linie((5,19), (12,25), r),
      kasten(17.5,11.5,23.5,17.5),
      linie((20.5,17), (20.5,19), r), linie((20.5,19), (12,24), r),
    ]
    return form(n, teile)

def grund(n):
    px = neu(n)
    f = dreizack(n)
    malen(px, umriss(f, True), 'K')
    malen(px, f, 'W')
    if n == 32:                       # Schatten unten rechts innen
        for y in range(n):
            for x in range(n):
                if f[y][x] and (y+1 < n and not f[y+1][x] or x+1 < n and not f[y][x+1]):
                    px[y][x] = 'S'
    return px

def ausschneiden(px, m, rand=1):
    """Luecke um die Plakette, damit sie sich vom Dreizack loest."""
    n = len(px); g = m
    for _ in range(rand):
        o = umriss(g, True)
        g = [[g[y][x] or o[y][x] for x in range(n)] for y in range(n)]
    for y in range(n):
        for x in range(n):
            if g[y][x] and not m[y][x]: px[y][x] = None

HAKEN16 = ["..gggg..",
           ".gGGGGg.",
           "gGGGGGXg",
           "gGGGGXXg",
           "gXXGXXGg",
           "gGXXXGGg",
           ".gGXGGg.",
           "..gggg.."]
AUSWURF16 = ["...KK...",
             "..KWWK..",
             ".KWWWWK.",
             "KWWWWWWK",
             "KKKKKKKK",
             "        ",
             "KKKKKKKK",
             "KWWWWWWK",
             "KKKKKKKK"]

def handbild(px, x0, y0, bild):
    """'.' laesst den Dreizack stehen, ' ' macht durchsichtig."""
    n = len(px)
    m = [[False]*n for _ in range(n)]
    for yy, z in enumerate(bild):
        for xx, c in enumerate(z):
            if c != '.': m[y0+yy][x0+xx] = True
    ausschneiden(px, m, 1)
    for yy, z in enumerate(bild):
        for xx, c in enumerate(z):
            if c == ' ': px[y0+yy][x0+xx] = None
            elif c != '.': px[y0+yy][x0+xx] = c
    return px

def haken(n):
    px = grund(n)
    if n == 16:
        return handbild(px, 8, 8, HAKEN16)
    if n == 32:
        c, r = (24, 24), 7.6
    else:
        c, r = (12.0, 12.0), 4.0
    k = form(n, [kreis(c, r)])
    ausschneiden(px, k, 1)
    innen = form(n, [kreis(c, r-1.0)])
    for y in range(n):
        for x in range(n):
            if k[y][x]: px[y][x] = 'g'
            if innen[y][x]: px[y][x] = 'G'
    if n == 32:
        h = form(n, [lambda x,y: seg_d(x,y,(20,24.5),(23,27.5))<=1.2,
                     lambda x,y: seg_d(x,y,(23,27.5),(28,21.5))<=1.2])
        malen(px, h, 'X')
        return px
    return handbild(px, 8, 8, HAKEN16)

def auswurf(n):
    px = grund(n)
    if n == 16:
        return handbild(px, 8, 7, AUSWURF16)
    if n == 32:
        tri = form(n, [dreieck([(23.5,15.5),(16.5,23.5),(30.5,23.5)])])
        bal = form(n, [kasten(16.5,25.5,30.5,29.5)])
    else:
        tri = form(n, [dreieck([(12,7.5),(8.2,11.6),(15.8,11.6)])])
        bal = form(n, [kasten(8.4,12.5,15.6,14.5)])
    alles = [[tri[y][x] or bal[y][x] for x in range(n)] for y in range(n)]
    o = umriss(alles, True)
    ausschneiden(px, [[alles[y][x] or o[y][x] for x in range(n)] for y in range(n)], 1)
    malen(px, o, 'K'); malen(px, alles, 'W')
    if n == 32:
        for y in range(n):
            for x in range(n):
                if alles[y][x] and (y+1 < n and not alles[y+1][x]): px[y][x] = 'S'
    return px

# Ziffern 3x5 (16 px) und 5x7 (32 px)
Z3 = {'2':['XXX','..X','XXX','X..','XXX'], '3':['XXX','..X','XXX','..X','XXX'],
      '4':['X.X','X.X','XXX','..X','..X'], '5':['XXX','X..','XXX','..X','XXX'],
      '6':['XXX','X..','XXX','X.X','XXX'], '7':['XXX','..X','..X','..X','..X'],
      '8':['XXX','X.X','XXX','X.X','XXX'], '9':['XXX','X.X','XXX','..X','XXX'],
      '+':['...','.X.','XXX','.X.','...']}
Z5 = {'2':['.XXX.','X...X','....X','..XX.','.X...','X....','XXXXX'],
      '3':['XXXX.','....X','....X','.XXX.','....X','....X','XXXX.'],
      '4':['...X.','..XX.','.X.X.','X..X.','XXXXX','...X.','...X.'],
      '5':['XXXXX','X....','XXXX.','....X','....X','X...X','.XXX.'],
      '6':['..XX.','.X...','X....','XXXX.','X...X','X...X','.XXX.'],
      '7':['XXXXX','....X','...X.','..X..','.X...','.X...','.X...'],
      '8':['.XXX.','X...X','X...X','.XXX.','X...X','X...X','.XXX.'],
      '9':['.XXX.','X...X','X...X','.XXXX','....X','...X.','.XX..'],
      '+':['.....','..X..','..X..','XXXXX','..X..','..X..','.....']}

def anzahl(n, z):
    px = grund(n)
    if n == 32:
        x0, y0, x1, y1 = 18, 17, 31, 31
    else:
        x0, y0, x1, y1 = 7, 6, 15, 15      # gross genug fuer 5x7-Ziffern
    m = [[x0 <= x <= x1 and y0 <= y <= y1 and not
          ((x in (x0, x1)) and (y in (y0, y1))) for x in range(n)] for y in range(n)]
    ausschneiden(px, m, 1)
    for y in range(n):
        for x in range(n):
            if m[y][x]:
                rand = x in (x0, x1) or y in (y0, y1)
                px[y][x] = 'K' if rand else 'B'
    g = Z5[z]
    h, b = len(g), len(g[0])
    gx = x0 + (x1 - x0 + 1 - b) // 2 + (1 if n == 32 else 0)
    gy = y0 + (y1 - y0 + 1 - h + 1) // 2
    for yy, zeile in enumerate(g):
        for xx, c in enumerate(zeile):
            if c == 'X': px[gy+yy][gx+xx] = 'X'
    return px


# ---------------- Schreiben ----------------

def dib(px, n, bpp):
    """BITMAPINFOHEADER + Palette + XOR-Bild + UND-Maske (von unten nach oben)."""
    if bpp == 4:
        pal = list(PAL)
        index = dict(VGA4)
    else:
        pal = list(PAL) + [FARBEN[k] for k in sorted(FARBEN)]
        index = {k: 16 + i for i, k in enumerate(sorted(FARBEN))}
        pal += [(0, 0, 0)] * (256 - len(pal))
    kopf = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, bpp, 0, 0, 0, 0,
                       len(pal), 0)
    palb = b''.join(struct.pack('<BBBB', b, g, r, 0) for (r, g, b) in pal)
    xor = b''
    und = b''
    zeile_xor = ((n * bpp + 31) // 32) * 4
    zeile_und = ((n + 31) // 32) * 4
    for y in range(n - 1, -1, -1):
        z = [index[c] if c is not None else 0 for c in px[y]]
        if bpp == 4:
            b = bytearray((z[x] << 4) | z[x + 1] for x in range(0, n, 2))
        else:
            b = bytearray(z)
        xor += bytes(b) + b'\0' * (zeile_xor - len(b))
        m = bytearray(zeile_und)
        for x in range(n):
            if px[y][x] is None:
                m[x // 8] |= 0x80 >> (x % 8)
        und += bytes(m)
    return kopf + palb + xor + und


def ico(datei, f):
    bilder = []
    for n in (16, 32):
        px = f(n)
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
    with open(datei, 'wb') as h:
        h.write(kopf + eintraege + daten)


def main():
    ziel = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    ico(os.path.join(ziel, 'usbtasten.ico'), grund)
    ico(os.path.join(ziel, 'usb-haken.ico'), haken)
    ico(os.path.join(ziel, 'usb-auswurf.ico'), auswurf)
    for z in '23456789':
        ico(os.path.join(ziel, 'usb-anzahl-%s.ico' % z), lambda n, z=z: anzahl(n, z))
    ico(os.path.join(ziel, 'usb-anzahl-mehr.ico'), lambda n: anzahl(n, '+'))


if __name__ == '__main__':
    main()
