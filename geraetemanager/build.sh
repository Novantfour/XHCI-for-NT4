#!/bin/sh
#
#   Geraetemanager fuer Windows NT 4.0 bauen.
#
#   Eine Uebersetzungseinheit, zwei Ergebnisse: als
#   Systemsteuerungselement (.cpl, eine DLL) und als Programm (.exe).
#   Der Unterschied ist nur der Einstiegspunkt.
#
#   Voraussetzung: clang-18, lld-18, llvm-18 und der NT-4.0-Quellbaum
#   fuer die Kopfdateien.
#
set -e

NT=${NT:-/home/claude/nt}
mkdir -p obj

# ---------------------------------------------------------------
#  1. Der Hilfstreiber
# ---------------------------------------------------------------
#
#  PCISCAN.SYS liest den PCI-Bus ueber HalGetBusData.  Er wird in die
#  EXE beziehungsweise CPL eingebettet, beim Aufruf nach
#  system32\drivers geschrieben, geladen, befragt und wieder entfernt.
#
#  Wichtig: /align und /filealign NICHT setzen.  Treiber brauchen die
#  Vorgaben 0x1000 / 0x200 -- mit 0x40 lehnt der Speichermanager das
#  Abbild ab (mm/creasect.c, Zeile 1615).

clang-cl --target=i386-pc-windows-msvc -fms-compatibility -fms-extensions \
  -c -Os -Zp8 -Gz -GS- -Zl -fno-builtin -w -ftrivial-auto-var-init=zero \
  -FI$NT/tools/ntcompat.h \
  -D_X86_=1 -Di386=1 -DSTD_CALL -DCONDITION_HANDLING=1 -DNT_INST=0 \
  -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0400 \
  -DWIN32_LEAN_AND_MEAN=1 -DDEVL=1 -DFPO=0 -DDBG=0 \
  -I$NT/private/ntos/inc -I$NT/public/oak/inc -I$NT/public/sdk/inc \
  -I$NT/public/sdk/inc/crt -Isrc \
  -Foobj/pciscan.obj src/pciscan.c

#  HalGetBusData liegt in hal.dll, nicht im Kernel -- der Treiber
#  braucht deshalb zwei Importbibliotheken, nicht eine.

llvm-dlltool -m i386 --input-def def/ntoskrnl.def \
  --output-lib obj/ntoskrnl.lib --kill-at
llvm-dlltool -m i386 --input-def def/hal.def \
  --output-lib obj/hal.lib --kill-at

lld-link /nologo /out:obj/PCISCAN.SYS /machine:x86 \
  /subsystem:native,4.00 /version:4.00 /osversion:4.00 /driver \
  /entry:DriverEntry /base:0x10000 /nodefaultlib /safeseh:no \
  obj/pciscan.obj obj/ntoskrnl.lib obj/hal.lib

python3 $NT/tools/pesum.py obj/PCISCAN.SYS

# ---------------------------------------------------------------
#  2. Den Treiber einbetten
# ---------------------------------------------------------------

rm -f src/drvpayload.h
python3 tools/mkpayload.py src/drvpayload.h obj/PCISCAN.SYS

# ---------------------------------------------------------------
#  3. Importbibliotheken
# ---------------------------------------------------------------

for m in kernel32c advapi32c user32 gdi32 comctl32 comdlg32; do
    llvm-dlltool -m i386 --input-def def/$m.def \
      --output-lib obj/$m.lib --kill-at
done

# ---------------------------------------------------------------
#  4. Uebersetzen
# ---------------------------------------------------------------

clang-cl --target=i386-pc-windows-msvc -fms-compatibility -fms-extensions \
  -c -Os -GS- -Zl -fno-builtin -w \
  -D_X86_=1 -Di386=1 -DWIN32=100 -D_WIN32_WINNT=0x0400 -DWINVER=0x0400 \
  -I$NT/public/sdk/inc -I$NT/public/oak/inc -I$NT/public/sdk/inc/crt \
  -Isrc -Foobj/devmgr.obj src/devmgr.c

# ---------------------------------------------------------------
#  5a. Als Systemsteuerungselement
# ---------------------------------------------------------------
#
#  /export:CPlApplet@16 wird abgewiesen -- clang legt das Symbol als
#  _CPlApplet@16 ab.  Mit der Zuweisungsform steht der Name unverziert
#  in der Exporttabelle, und genau so sucht ihn die Systemsteuerung.

lld-link /nologo /dll /out:obj/DEVMGR.CPL /machine:x86 \
  /subsystem:windows,4.00 /version:4.00 /osversion:4.00 \
  /entry:DllMain /nodefaultlib /safeseh:no \
  /export:CPlApplet=_CPlApplet@16 \
  obj/devmgr.obj obj/kernel32c.lib obj/advapi32c.lib \
  obj/user32.lib obj/gdi32.lib obj/comctl32.lib obj/comdlg32.lib

# ---------------------------------------------------------------
#  5b. Als Programm
# ---------------------------------------------------------------

lld-link /nologo /out:obj/DEVMGR.EXE /machine:x86 \
  /subsystem:windows,4.00 /version:4.00 /osversion:4.00 \
  /entry:WinMainCRTStartup /nodefaultlib /safeseh:no \
  obj/devmgr.obj obj/kernel32c.lib obj/advapi32c.lib \
  obj/user32.lib obj/gdi32.lib obj/comctl32.lib obj/comdlg32.lib

ls -la obj/DEVMGR.CPL obj/DEVMGR.EXE obj/PCISCAN.SYS
