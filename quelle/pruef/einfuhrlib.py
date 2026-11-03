#!/usr/bin/env python3
"""
einfuhrlib.py -- Einfuhrbibliotheken aus den DDK-Bibliotheken neu bauen.

Warum ueberhaupt:

Die NTOSKRNL.LIB und HAL.LIB des NT-4.0-DDK lassen sich mit lld-link
nicht binden.  Sie legen .idata$4 und .idata$5 als COMMON-Symbole mit
der Groesse c0000040 an -- das ist in Wahrheit der Abschnittskennwert
(IMAGE_SCN_MEM_READ | ...), den MSVC 4 an dieser Stelle missbraucht hat.
lld nimmt die Zahl woertlich und meldet

    lld-link: error: section larger than 4 GiB: .data

Die Namen selbst sind einwandfrei.  Also werden sie ausgelesen und mit
llvm-dlltool neu gebunden.  Das ist keine Notloesung: die Namen kommen
aus derselben Quelle wie vorher, nur das Verpackungsformat ist von 1996
auf heute gehoben.

Fastcall zaehlt mit.  NT 4.0 hat @KfAcquireSpinLock@4 und Verwandte
eingefuehrt; sie tragen ein fuehrendes @ und wuerden von einem Ausdruck,
der nur auf __imp__ passt, uebersehen -- und dann fehlen beim Binden
genau die Sperren.
"""
import subprocess, re, sys, os

DDK = sys.argv[1] if len(sys.argv) > 1 else '/home/claude/ddk/x'
ZIEL = sys.argv[2] if len(sys.argv) > 2 else 'obj'

os.makedirs(ZIEL, exist_ok=True)

for lib, kurz, dll in (('NTOSKRNL', 'ntoskrnl', 'ntoskrnl.exe'),
                       ('HAL',      'hal',      'hal.dll')):

    pfad = '%s/lib/i386/free/%s.LIB' % (DDK, lib)
    out  = subprocess.run(['llvm-nm-18', pfad], capture_output=True,
                          text=True).stdout

    namen = set()

    for n in re.findall(r'__imp_(\S+)', out):
        namen.add(n[1:] if n.startswith('_') else n)

    namen = sorted(namen)
    fast  = [n for n in namen if n.startswith('@')]

    d = '%s/%s.def' % (ZIEL, kurz)

    with open(d, 'w') as f:
        f.write(';  Erzeugt von einfuhrlib.py aus %s.LIB des NT-4.0-DDK.\n'
                ';  %d Namen, davon %d fastcall.\n'
                'LIBRARY %s\nEXPORTS\n' % (lib, len(namen), len(fast), dll))
        for n in namen:
            f.write('    %s\n' % n)

    subprocess.run(['llvm-dlltool-18', '-m', 'i386', '-k',
                    '-d', d, '-D', dll,
                    '-l', '%s/%s.lib' % (ZIEL, kurz)], check=True)

    print('  %-9s %4d Namen, davon %2d fastcall' % (kurz, len(namen), len(fast)))
