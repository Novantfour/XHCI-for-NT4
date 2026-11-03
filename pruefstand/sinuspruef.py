#!/usr/bin/env python3
"""sinuspruef.py DATEI.WAV [AB] [LAENGE] -- Kontinuitaet eines 1-kHz-Sinus.

AB/LAENGE in Byte ab Beginn der Daten (fuer Ausschnitte eines Mitschnitts);
ohne: die ganze Datei.  Rate und Kanaele aus dem WAV-Kopf, gewertet wird
Kanal 1.  Amplitude nur in der mittleren Haelfte (QEMU schreibt seinen
Mitschnitt verzoegert, an den Raendern steht oft der vorige Ton).
Eine Unstetigkeit ist ein Schritt, der steiler ist, als ein 1-kHz-Sinus
dieser Amplitude sein kann (mit Reserve 1,6)."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
i = 12; kan = 2; fs = 44100; bits = 16
while i < len(d) - 8:
    cid = d[i:i+4]; n = struct.unpack_from('<I', d, i+4)[0]
    if cid == b'fmt ':
        kan, fs = struct.unpack_from('<HI', d, i+10)
        bits = struct.unpack_from('<H', d, i+22)[0]
    if cid == b'data':
        break
    i += 8 + n
daten = d[i+8:]
if len(sys.argv) > 2:
    ab = int(sys.argv[2]); daten = daten[ab:ab+int(sys.argv[3])]
if bits == 8:                       # 8 Bit: vorzeichenlos, Mitte 128
    daten = daten[:len(daten)//kan*kan]
    L = [(b - 128) << 8 for b in daten[0::kan]]
else:
    daten = daten[:len(daten)//(2*kan)*(2*kan)]
    L = struct.unpack('<%dh' % (len(daten)//2), daten)[0::kan]
laut = [k for k, v in enumerate(L) if abs(v) > 300]
if not laut:
    print('  kein Ton'); sys.exit()
a, b = laut[0] + 200, laut[-1] - 200
m1, m2 = a + (b-a)//4, b - (b-a)//4
A = max(abs(v) for v in L[m1:m2])
grenze = 2*3.14159*1000/fs*A*1.6
st = 0; last = -10
for k in range(a+1, b):
    if abs(L[k]-L[k-1]) > grenze:
        if k - last > 3: st += 1
        last = k
print('  Ton %.3f s (%d Hz, %d Bit, %d Kanal/Kanaele), Amplitude %d: %d Unstetigkeiten (%.1f je Sekunde)'
      % ((b-a)/fs, fs, bits, kan, A, st, st/((b-a)/fs)))
