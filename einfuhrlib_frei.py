#!/usr/bin/env python3
"""
einfuhrlib_frei.py -- Einfuhrbibliotheken ohne NT-4.0-DDK.

    einfuhrlib_frei.py <ziel> <ntoskrnl.exe> <hal.dll> <obj-verz>...

Statt die NTOSKRNL.LIB/HAL.LIB des DDK zu lesen, werden die Namen genommen,
die die Objektdateien tatsaechlich einfuehren (__imp__Name@N bzw.
__imp_@Name@N), und gegen die Exportlisten eines echten NT-4.0-Kerns und
HAL geprueft.  Was dort fehlt, ist ein Fehler: der Treiber wuerde unter
NT 4.0 nicht laden.  So faellt auf, wenn die freien Koepfe (mingw-w64)
eine Funktion erst neuerer Windows-Fassungen hereinziehen.
"""
import subprocess, sys, glob, os, pefile

ziel, kern, hal = sys.argv[1], sys.argv[2], sys.argv[3]
verz = sys.argv[4:]

def exporte(f):
    p = pefile.PE(f, fast_load=True)
    p.parse_data_directories(directories=[0])
    return {e.name.decode() for e in p.DIRECTORY_ENTRY_EXPORT.symbols if e.name}

ek, eh = exporte(kern), exporte(hal)
imp, offen, da = set(), set(), set()
for v in verz:
    for o in glob.glob(v + '/**/*.obj', recursive=True):
        out = subprocess.run(['llvm-nm-18', o], capture_output=True, text=True).stdout
        for z in out.splitlines():
            t = z.split()
            if len(t) >= 2 and t[-2] == 'U':
                if t[-1].startswith('__imp_'): imp.add(t[-1][6:])
                else: offen.add(t[-1])
            elif len(t) >= 2 and t[-2] in 'TDBRC':
                da.add(t[-1])
# Direkt (ohne __imp_) gerufene Kernexporte: memset, memcpy, sprintf, _alldiv ...
for n in offen - da:
    if n.startswith('__imp_'): continue
    r = n[1:] if n.startswith('_') else n
    if r.split('@')[0] in ek or r.split('@')[0] in eh or r in ek:
        imp.add(n)

def roh(n):                    # _Name@8 -> Name ; @Name@8 -> Name ; __alldiv -> _alldiv
    if n.startswith('@'): return n[1:].split('@')[0]
    if n.startswith('_'): n = n[1:]
    return n.split('@')[0] if '@' in n else n

def defname(n):                # Schreibweise fuer dlltool -k
    return n[1:] if n.startswith('_') else n

lk, lh, fehlt = [], [], []
for n in sorted(imp):
    r = roh(n)
    if r in ek: lk.append(defname(n))
    elif r in eh: lh.append(defname(n))
    else: fehlt.append(r)
if fehlt:
    print('FEHLT in NT 4.0:', ' '.join(fehlt)); sys.exit(1)
for kurz, dll, liste in (('ntoskrnl', 'ntoskrnl.exe', lk), ('hal', 'hal.dll', lh)):
    d = '%s/%s.def' % (ziel, kurz)
    open(d, 'w').write('LIBRARY %s\nEXPORTS\n' % dll + ''.join('    %s\n' % n for n in liste))
    subprocess.run(['llvm-dlltool-18', '-m', 'i386', '-k', '-d', d, '-D', dll,
                    '-l', '%s/%s.lib' % (ziel, kurz)], check=True)
    print('  %-9s %4d Namen (gegen %s geprueft)' % (kurz, len(liste), os.path.basename(kern if kurz == 'ntoskrnl' else hal)))
