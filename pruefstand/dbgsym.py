#!/usr/bin/env python3
# dbgsym.py <datei.dbg> [muster] -- COFF-Symbole (RVA) einer NT-.dbg-Datei
import struct, sys, re
d = open(sys.argv[1], 'rb').read()
nsec, expsz, dbgsz = struct.unpack_from('<III', d, 24)
off = 48 + nsec * 40 + expsz
syms = []
for i in range(dbgsz // 28):
    ch, ts, ma, mi, typ, size, rva, ptr = struct.unpack_from('<IIHHIIII', d, off + i * 28)
    if typ == 1:
        n, lva = struct.unpack_from('<II', d, ptr)
        base = ptr + lva
        strt = base + n * 18
        j = 0
        while j < n:
            raw = d[base + j * 18: base + j * 18 + 18]
            name8, val, sec, typ2, cls, aux = struct.unpack('<8sIhHBB', raw)
            if name8[:4] == b'\0\0\0\0':
                o = struct.unpack_from('<I', name8, 4)[0]
                e = d.index(b'\0', strt + o); name = d[strt + o:e].decode('latin-1')
            else:
                name = name8.rstrip(b'\0').decode('latin-1')
            if cls in (2, 3) and sec > 0:
                syms.append((val, name))
            j += 1 + aux
syms.sort()
pat = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
for v, n in syms:
    if pat is None or pat.search(n):
        print('%08x %s' % (v, n))
