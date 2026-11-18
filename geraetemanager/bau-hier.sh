#!/bin/sh
#  Geraetemanager bauen -- in DIESER Umgebung (ohne den NT-Quellbaum):
#  PCISCAN.SYS mit clang-cl gegen das NT4-DDK (kompat/ntos.h, pci.h
#  verweisen auf ntddk.h), die Oberflaeche mit mingw, Ressourcen mit
#  windres.  Symbole: tools/mkicons.py.  USB: src/ntusbioctl.h aus dem
#  USB-Treiber (IOCTL_NTU_USBGERAETE).
set -e
cd "$(dirname "$0")"
DDK=${DDK:-/home/claude/ddk/x}
mkdir -p obj
# 1. Hilfstreiber
clang-cl --target=i386-pc-windows-msvc -fms-compatibility -fms-extensions -c -Os -Zp8 -Gz -GS- -Zl -fno-builtin -w \
  -D_X86_=1 -Di386=1 -DSTD_CALL -DCONDITION_HANDLING=1 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0400 \
  -DWIN32_LEAN_AND_MEAN=1 -DDEVL=1 -DFPO=0 -DDBG=0 -Ikompat -I$DDK/inc-patched -I$DDK/sdk -Isrc -Foobj/pciscan.obj src/pciscan.c
llvm-dlltool-18 -m i386 --input-def def/ntoskrnl.def --output-lib obj/ntoskrnl.lib --kill-at
llvm-dlltool-18 -m i386 --input-def def/hal.def --output-lib obj/hal.lib --kill-at
# Versionsangaben (pciscan.rc) -- ohne sie werten Virenscanner den
# Treiber als verdaechtig, und pruefe-bau.py meldet einen Mangel.
llvm-rc-18 /c 1252 /fo obj/pciscan.res pciscan.rc
lld-link /nologo /out:obj/PCISCAN.SYS /machine:x86 /subsystem:native,4.00 /version:4.00 /osversion:4.00 /driver \
  /entry:DriverEntry /base:0x10000 /nodefaultlib /safeseh:no /release obj/pciscan.obj obj/pciscan.res obj/ntoskrnl.lib obj/hal.lib
# Abschnitte wie beim USB-Treiber: .text/.rdata/.data resident, INIT
# verwerfbar, PAGE auslagerbar; danach die Pruefsumme neu.
python3 ../abschnitte.py obj/PCISCAN.SYS
python3 ../pruefsumme.py obj/PCISCAN.SYS
# PCISCAN.SYS wird NICHT mehr eingebettet -- liegt als eigene Datei bei.
# (frueher: mkpayload.py -> src/drvpayload.h)
# 2. Symbole und Ressourcen
python3 tools/mkicons.py res
(cd res && i686-w64-mingw32-windres -c 1252 devmgr.rc -O coff -o ../obj/devmgr_res.o)
# 3. Oberflaeche: eine Quelle, zwei Ergebnisse
CC="i686-w64-mingw32-gcc -Os -w -fno-builtin -nostdlib -fno-asynchronous-unwind-tables -D_WIN32_WINNT=0x0400 -DWINVER=0x0400 -Isrc"
NT4="-Wl,--major-subsystem-version=4 -Wl,--minor-subsystem-version=0 -Wl,--major-os-version=4 -Wl,--minor-os-version=0"
LIBS="-lkernel32 -ladvapi32 -luser32 -lgdi32 -lcomctl32 -lcomdlg32"
$CC -o obj/DEVMGR.EXE src/devmgr.c obj/devmgr_res.o -Wl,-e,_WinMainCRTStartup@0 -Wl,--subsystem,windows $NT4 $LIBS
$CC -DNUR_ANSEHEN -shared -o obj/DEVMGR.CPL src/devmgr.c src/devmgr.def obj/devmgr_res.o -Wl,-e,_DllMain@12 -Wl,--enable-stdcall-fixup -Wl,--subsystem,windows $NT4 $LIBS
i686-w64-mingw32-objcopy --remove-section=.eh_frame obj/DEVMGR.EXE 2>/dev/null
i686-w64-mingw32-objcopy --remove-section=.eh_frame obj/DEVMGR.CPL 2>/dev/null
i686-w64-mingw32-strip obj/DEVMGR.EXE obj/DEVMGR.CPL
ls -la obj/DEVMGR.EXE obj/DEVMGR.CPL obj/PCISCAN.SYS | awk '{print $9, $5}'
