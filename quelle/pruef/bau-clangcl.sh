#!/bin/sh
# echter Bau: clang-cl + lld-link gegen das NT-4.0-DDK
DDK=/home/claude/ddk/x
UNT=/home/claude/usb/unterlage
SRC=/home/claude/usb/bau/src
OBJ=${OBJ:-obj}
NUR=${NUR:-}
mkdir -p $OBJ; rm -f $OBJ/*.obj

CF="--target=i386-pc-windows-msvc -c -Gz -GS- -Gy -O1 -Oi- -fno-builtin"
CF="$CF -D_X86_ -Di386=1 -D_KERNEL -DNTDDI_VERSION=0"
CF="$CF -Wno-missing-prototype-for-cc -Wno-ignored-attributes -w"
CI="-I$UNT -I$UNT/dev/usb -I$SRC -I$DDK/inc-patched -I$DDK/sdk"
[ "$NUR" = xhci ] && CF="$CF -DNTU_NUR_XHCI"

LISTE=$(ls $SRC/dev/usb/*.c | grep -vE '/(usb_dev|usb_generic|usb_pf)\.c$')
if [ "$NUR" = xhci ]; then
  HCD="$SRC/dev/usb/controller/usb_controller.c $SRC/dev/usb/controller/xhci.c $SRC/dev/usb/controller/xhci_pci.c"
  ZIEL=xhcint.sys
else
  HCD=$(ls $SRC/dev/usb/controller/*.c); ZIEL=usbnt.sys
fi

ok=0; bad=0
for f in $LISTE $HCD $UNT/nt/*.c $UNT/ober/*.c; do
  o=$OBJ/$(basename $f .c).obj
  if clang-cl $CF $CI "$f" -Fo$o 2>/dev/null; then ok=$((ok+1)); else
    bad=$((bad+1)); echo "### $(basename $f)"
    clang-cl $CF $CI "$f" -Fo$o 2>&1 | grep -E "error" | head -4
  fi
done
echo "uebersetzt: $ok   gescheitert: $bad"
[ $bad != 0 ] && exit 1

# Einfuhrbibliotheken.  Die des DDK lassen sich mit lld-link nicht
# binden -- der Grund steht in einfuhrlib.py.
python3 einfuhrlib.py $DDK $OBJ

# Dazu KiSerialPutString: der 1017. Export des selbstgebauten Kernels
# steht in keiner NTOSKRNL.LIB, die Microsoft ausgeliefert hat.
# -k (--kill-at) ist Pflicht: der Kern exportiert KiSerialPutString
# UNDEKORIERT, wie alle NT-Exporte.  Ohne -k sucht der Binder
# KiSerialPutString@4 im Exportverzeichnis und findet nichts.
llvm-dlltool-18 -m i386 -k -d $UNT/pruef/kiserial.def -D ntoskrnl.exe \
    -l $OBJ/kiserial.lib 2>/dev/null

lld-link /out:$ZIEL /machine:x86 /subsystem:native,4.00 /driver \
  /entry:DriverEntry@8 /nodefaultlib /safeseh:no \
  /merge:NTUDRV=.rdata /ignore:4078 \
  /base:0x10000 /release \
  $OBJ/ntoskrnl.lib $OBJ/hal.lib $OBJ/kiserial.lib $OBJ/*.obj 2>&1 | grep -v "unrecognized magic" | head -20
# /release setzt die Pruefsumme im Kopf.  Ohne sie meldet NT beim
# Starten 0xC0000221 (STATUS_IMAGE_CHECKSUM_MISMATCH) und sagt nichts
# weiter -- der Treiber ist dabei in Ordnung, er wird nur nicht
# angesehen.  Nachgerechnet wird trotzdem, weil sich auf einen Schalter
# zu verlassen billiger ist als ihn zu pruefen, aber nicht sicherer.
python3 pruefsumme.py $ZIEL --pruefen || python3 pruefsumme.py $ZIEL

ls -la $ZIEL 2>/dev/null
