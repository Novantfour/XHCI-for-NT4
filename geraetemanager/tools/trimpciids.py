#!/usr/bin/env python3
"""
    trimpciids.py -- PCI.IDS verkleinern

    Die Datei von pci-ids.ucw.cz ist rund 1,3 MB.  Das meiste davon
    sind Untersystemzeilen, die der Geraetemanager nur fuer die
    Eigenschaftenseite braucht.  Wer sie nicht will:

        python3 trimpciids.py pci.ids PCI.IDS --no-subsys

    Kommentare und Leerzeilen fallen immer weg.  Ergebnis rund 400 KB.

        --no-subsys   Untersystemzeilen weglassen
        --no-class    Klassenteil weglassen
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
nosub = '--no-subsys' in sys.argv
nocls = '--no-class'  in sys.argv

out, inclass = [], False
for line in open(src, encoding='utf-8', errors='replace'):
    line = line.rstrip('\r\n')
    if not line or line.lstrip().startswith('#'):
        continue
    if not line.startswith('\t'):
        inclass = line.startswith('C ')
    if inclass and nocls:
        continue
    if nosub and line.startswith('\t\t'):
        continue
    out.append(line)

open(dst, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print('%s: %d Zeilen, %d Bytes' % (dst, len(out), sum(len(x)+1 for x in out)))
