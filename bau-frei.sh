#!/bin/sh
# bau-frei.sh -- usbnt.sys OHNE NT-4.0-DDK bauen, mit den freien
# DDK-Koepfen von mingw-w64 (z. B. aus dem Python-Paket ziglang:
#   pip install ziglang  ->  .../ziglang/lib/libc/include/any-windows-any).
#
#   MINGW=<any-windows-any> KERN=<NTOSKRNL.EXE> HAL=<HAL*.DLL> ./bau-frei.sh
#
# KERN/HAL: ein echter NT-4.0-Kern und HAL (z. B. von der CD, I386).  Gegen
# deren Exportlisten werden die Einfuhren geprueft; was NT 4.0 nicht
# exportiert, bricht den Bau ab (Treiber wuerde sonst nicht laden).
#
# Unterschiede zum DDK-Bau (gemessen, Fassung 2026.11.15):
#   - Interlocked*, KeInitializeSpinLock kommen als eingebaute Befehle statt
#     als Kernaufrufe (5 Einfuhren weniger), sonst gleiche Einfuhren;
#   - Abschnitte und Groessen praktisch gleich.
# Ausgleich der Kopfunterschiede: quelle/nt/ntfreiekoepfe.h, Schalter
# NTU_FREIE_KOEPFE; quelle/sys/types.h prueft _UINTPTR_T_DEFINED usw.
set -e
MINGW=${MINGW:?MINGW=<pfad zu any-windows-any>}
KERN=${KERN:?KERN=<NTOSKRNL.EXE von NT 4.0>}
HAL=${HAL:?HAL=<HAL.DLL von NT 4.0>}
H=$(cd "$(dirname "$0")" && pwd)
D=${TMPDIR:-/tmp}/usbnt-freiddk; mkdir -p $D/stub
ln -sfn $MINGW/ddk $D/inc-patched; ln -sfn $MINGW $D/sdk
# clang-cl gilt fuer die mingw-Koepfe nicht als GNU-C -> sie streichen
# __attribute__ weg (packed!).  Deshalb -fgnuc-version.  NTDDI_WINVISTA kennt
# sdkddkver.h von mingw nicht (heisst dort NTDDI_VISTA).
FREI="/clang:-fgnuc-version=4.2.1 -mno-mmx -mno-sse -mno-sse2 -DNTDDI_VERSION=0x05000000 -D_WIN32_WINNT=0x0500 -DNTDDI_WINVISTA=0x06000000 -D_ULONG_PTR_DEFINED_ -DNTU_FREIE_KOEPFE -FIsdkddkver.h"
cd $H
DDK=$D UNT=$H/quelle SRC=$H/freebsd OBJ=${OBJ:-obj} EXTRA="$FREI $EXTRA" \
  CLANGCL=${CLANGCL:-clang-cl-18} WINDRES="${WINDRES:-llvm-windres-18 --target=i686-w64-mingw32}" \
  FREI_KERN=$KERN FREI_HAL=$HAL sh bau-clangcl.sh
