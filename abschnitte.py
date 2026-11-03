#!/usr/bin/env python3
"""abschnitte.py <treiber.sys>

Setzt die Abschnittsattribute eines fertig gebundenen NT-Treibers.
lld-link laesst das NOT_PAGED-Bit sonst weg; dann lagert NT auch Code
aus, der im ISR-/DPC-Pfad laeuft -> Bugcheck bei erhoehtem IRQL.
.text/.rdata/.data werden resident (NOT_PAGED) gesetzt, INIT verwerfbar,
PAGE bleibt auslagerbar (dort liegt nur nachweislich passiver Code).

NOT_PAGED = 0x08000000.  .text bleibt NUR ausfuehrbar+lesbar (nicht
schreibbar); .data lesbar+schreibbar; .rdata nur lesbar.  Die
Pruefsumme rechnet danach pruefsumme.py neu.
"""
import struct, sys

CODE, IDATA, UDATA = 0x20, 0x40, 0x80
DISCARD, NOTPAGED, SHARED = 0x02000000, 0x08000000, 0x10000000
EXEC, READ, WRITE = 0x20000000, 0x40000000, 0x80000000

# Zielattribute je Abschnitt.  Nicht genannte bleiben unveraendert,
# bekommen aber ebenfalls NOT_PAGED (ausser .reloc: wird verworfen).
ZIEL = {
    '.text':  CODE  | EXEC | READ            | NOTPAGED,
    '.rdata': IDATA | READ                    | NOTPAGED,
    '.data':  IDATA | READ | WRITE            | NOTPAGED,
    '.rsrc':  IDATA | READ                    | NOTPAGED,
    '.reloc': IDATA | READ | DISCARD,          # verworfen: paged egal
    # INIT wie in SIVNT4.SYS: Code, verwerfbar, nach dem Start freigegeben.
    'INIT':   CODE  | EXEC | READ | WRITE | DISCARD,
    # PAGE: auslagerbarer Code (Hinweis Ray Hinchliffe) -- KEIN NOT_PAGED,
    # nur passive Routinen liegen hier (nt/ntpaged.h, NTU_PAGED).
    'PAGE':   CODE  | EXEC | READ,
}

def main(pfad):
    d = bytearray(open(pfad, 'rb').read())
    p = struct.unpack_from('<I', d, 0x3c)[0]
    if d[p:p+4] != b'PE\0\0':
        print('kein PE'); return 1
    nsec = struct.unpack_from('<H', d, p+6)[0]
    sh = p + 24 + struct.unpack_from('<H', d, p+20)[0]
    print('Abschnitt   vorher    nachher')
    for i in range(nsec):
        off = sh + i*40
        name = d[off:off+8].rstrip(b'\0').decode('latin-1')
        alt = struct.unpack_from('<I', d, off+36)[0]
        if name in ZIEL:
            neu = ZIEL[name]
        else:
            neu = alt | NOTPAGED
        struct.pack_into('<I', d, off+36, neu)
        print('%-10s  %08x  %08x%s' % (name, alt, neu,
              '' if (neu & (NOTPAGED|DISCARD)) else '  (noch paged!)'))
    open(pfad, 'wb').write(d)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
