#!/usr/bin/env python3
"""
ntddk-patch.py -- die eine Zeile im NT-4.0-DDK, die clang-cl nicht nimmt.

    ./ntddk-patch.py <ddk>/inc <ziel>

Kopiert <ddk>/inc nach <ziel> und aendert dort genau eine Stelle.  Das
Original bleibt unberuehrt -- eine DDK-Kopfdatei an Ort und Stelle zu
aendern ist der sichere Weg, spaeter nicht mehr zu wissen, was der
eigene Stand ist und was Microsofts.

Die Stelle:

    __inline ULONG KeGetCurrentProcessorNumber(VOID)
    {
        __asm {  movzx eax, _PCR KPCR.Number  }
    }

_PCR ist fs:[0].  clang kann dieser Form die Operandenbreite nicht
ansehen und meldet 'ambiguous operand size'.  Ersetzt durch
__readfsbyte mit FIELD_OFFSET auf dieselbe Struktur -- die Lage rechnet
also weiterhin der Uebersetzer aus KPCR und nicht der Mensch.  Genau
dort waere eine eingetippte Zahl still falsch.
"""
import sys, os, shutil

ALT = ("__inline ULONG KeGetCurrentProcessorNumber(VOID)\r\n{\r\n"
       "    __asm {  movzx eax, _PCR KPCR.Number  }\r\n}")

NEU = ("/*  GEAENDERT gegenueber dem Original -- siehe ntddk-patch.py.  */\r\n"
       "unsigned char __readfsbyte(unsigned long);\r\n"
       "__inline ULONG KeGetCurrentProcessorNumber(VOID)\r\n{\r\n"
       "    return (ULONG)__readfsbyte(FIELD_OFFSET(KPCR, Number));\r\n}")

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(1)

quelle, ziel = sys.argv[1], sys.argv[2]
os.makedirs(ziel, exist_ok=True)

for n in os.listdir(quelle):
    p = os.path.join(quelle, n)
    if os.path.isfile(p):
        shutil.copy2(p, os.path.join(ziel, n.lower()))

p = os.path.join(ziel, 'ntddk.h')
s = open(p, 'rb').read().decode('latin-1')

if ALT not in s:
    if '__readfsbyte' in s:
        print('schon geaendert.')
        sys.exit(0)
    print('Die Stelle steht nicht so drin.  Von Hand nachsehen:')
    print('  grep -n KeGetCurrentProcessorNumber', p)
    sys.exit(1)

open(p, 'wb').write(s.replace(ALT, NEU).encode('latin-1'))
print('ntddk.h geaendert:', p)
