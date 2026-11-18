#!/usr/bin/env python3
# dis.py <pe> <dbg> <symbol> [bytes] -- Funktion zerlegen, Aufrufziele benennen
import sys, subprocess, pefile, capstone, bisect
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
syms = [l.split(' ', 1) for l in subprocess.check_output([sys.executable, '-I', '/home/claude/symwerk/dbgsym.py', sys.argv[2]]).decode().splitlines()]
syms = [(int(a, 16), b) for a, b in syms]; rvas = [a for a, b in syms]
def name(rva):
    i = bisect.bisect_right(rvas, rva) - 1
    return '%s+%x' % (syms[i][1], rva - syms[i][0]) if i >= 0 else hex(rva)
rva = [a for a, b in syms if b == sys.argv[3]][0]
n = int(sys.argv[4]) if len(sys.argv) > 4 else 200
code = pe.get_data(rva, n)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
imp = {}
for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for i in e.imports: imp[i.address - base] = (i.name or b'?').decode()
for ins in md.disasm(code, base + rva):
    t = ''
    for op in ins.op_str.replace('[', ' ').replace(']', ' ').split():
        try:
            v = int(op.strip(','), 16) - base
            if 0 <= v < pe.OPTIONAL_HEADER.SizeOfImage:
                t = imp.get(v) or name(v)
        except ValueError: pass
    print('%08x %-8s %-34s %s' % (ins.address - base, ins.mnemonic, ins.op_str, t))
    if ins.mnemonic == 'ret' and len(sys.argv) <= 5: break
