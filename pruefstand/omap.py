#!/usr/bin/env python3
# omap.py <dbg> <rva-hex> : Quell-RVA (Symbol) -> RVA im Abbild (OMAP_FROM_SRC, Typ 8)
import struct, sys, bisect
d = open(sys.argv[1], 'rb').read()
nsec, expsz, dbgsz = struct.unpack_from('<III', d, 24)
off = 48 + nsec * 40 + expsz
tab = []
for i in range(dbgsz // 28):
    ch, ts, ma, mi, typ, size, rva, ptr = struct.unpack_from('<IIHHIIII', d, off + i * 28)
    if typ == 8:
        tab = [struct.unpack_from('<II', d, ptr + j * 8) for j in range(size // 8)]
src = [a for a, b in tab]
def abb(x):
    i = bisect.bisect_right(src, x) - 1
    a, b = tab[i]
    return 0 if b == 0 else b + (x - a)
for a in sys.argv[2:]:
    print(hex(abb(int(a, 16))))
