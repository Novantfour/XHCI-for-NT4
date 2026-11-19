#!/usr/bin/env python3
# dis2.py <pe> <dbg> <symbol> [bytes] -- wie dis.py, aber mit OMAP (FastFat, NDIS, ...)
import sys, subprocess, pefile, capstone, bisect, struct
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
d = open(sys.argv[2], 'rb').read()
nsec, expsz, dbgsz = struct.unpack_from('<III', d, 24)
off = 48 + nsec * 40 + expsz; tab = []
for i in range(dbgsz // 28):
    ch, ts, ma, mi, typ, size, rva, ptr = struct.unpack_from('<IIHHIIII', d, off + i * 28)
    if typ == 8: tab = [struct.unpack_from('<II', d, ptr + j * 8) for j in range(size // 8)]
src = [a for a, b in tab]
def abb(x):
    i = bisect.bisect_right(src, x) - 1
    a, b = tab[i]; return 0 if b == 0 else b + (x - a)
syms = [l.split(' ', 1) for l in subprocess.check_output([sys.executable, '-I', '/home/claude/symwerk/dbgsym.py', sys.argv[2]]).decode().splitlines()]
syms = sorted((abb(int(a, 16)), b) for a, b in syms)
rvas = [a for a, b in syms]
def name(rva):
    i = bisect.bisect_right(rvas, rva) - 1
    return '%s+%x' % (syms[i][1], rva - syms[i][0]) if i >= 0 else hex(rva)
imp = {}
for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for i in e.imports: imp[i.address - base] = (i.name or b'?').decode()
rva = [a for a, b in syms if b == sys.argv[3]][0]
n = int(sys.argv[4]) if len(sys.argv) > 4 else 400
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for ins in md.disasm(pe.get_data(rva, n), base + rva):
    t = ''
    for op in ins.op_str.replace('[', ' ').replace(']', ' ').replace('+', ' ').split():
        try:
            v = int(op.strip(','), 16) - base
            if 0 <= v < pe.OPTIONAL_HEADER.SizeOfImage: t = imp.get(v) or name(v)
        except ValueError: pass
    print('%06x %-7s %-36s %s' % (ins.address - base, ins.mnemonic, ins.op_str, t))
