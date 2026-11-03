#!/bin/sh
#
#   bau.sh -- uebersetzt und "bindet" den FreeBSD-USB-Stapel gegen die
#   Unterlage.
#
#   Der Syntaxlauf allein sagt nichts ueber fehlende Symbole.  Deshalb
#   entstehen hier echte Objektdateien, und nm sagt, was aussen
#   nachgefragt wird.  Genau das hat elf Namen gefunden, die deklariert
#   und nie hingeschrieben waren.
#
#       ./bau.sh <quellbaum> [<ntddk-verz>]
#
#   Fuer den echten Bau clang-cl und lld-link einsetzen; die Zeilen
#   stehen unten.

SRC="${1:-../bau/src}"
UNT="$(cd "$(dirname "$0")" && pwd)"
NTDDK="${2:-/tmp/ntstub}"
OBJ="${OBJ:-./obj}"

# --- der echte Bau ------------------------------------------------
#
# clang-cl --target=i386-pc-windows-msvc -fms-compatibility \
#   -fms-extensions -c -Os -Zp8 -Gz -GS- -Zl -fno-builtin -w \
#   -D_KERNEL -D__FreeBSD__=14 \
#   -I$UNT -I$SRC -I<ntddk> -Fo<ziel> <quelle>
#
# lld-link /out:usbnt.sys /machine:x86 /subsystem:native,4.00 \
#   /driver /entry:DriverEntry /nodefaultlib /safeseh:no \
#   /merge:NTUDRV=.rdata \
#   ntoskrnl.lib hal.lib obj\*.obj
#
# /merge:NTUDRV=.rdata ist Pflicht -- sonst kostet die
# Treiberanmeldeliste eine eigene Seite.
#
# -Zp8: die Unterlage setzt die Packung ueber __packed selbst; ein
# anderer Vorgabewert hebt das nicht auf, verschiebt aber alles andere.
#
# Nicht gebaut werden usb_dev.c, usb_generic.c und usb_pf.c -- das
# Zeichengeraet /dev/ugen ist ueber USB_HAVE_UGEN = 0 abgeschaltet.
# ------------------------------------------------------------------

mkdir -p "$OBJ"
rm -f "$OBJ"/*.o

CFLAGS="-c -m32 -nostdinc -w -fno-builtin -fno-stack-protector -fno-pic"
CDEF="-D_KERNEL -include $UNT/pruef/gccvorspann.h"
CINC="-I$UNT -I$SRC -I$NTDDK"

# Der USB-Kern ist in beiden Fassungen derselbe.  Nicht gebaut werden
# usb_dev.c, usb_generic.c und usb_pf.c -- /dev/ugen ist ueber
# USB_HAVE_UGEN = 0 abgeschaltet.
LISTE=$(ls "$SRC"/dev/usb/*.c | grep -vE '/(usb_dev|usb_generic|usb_pf)\.c$')

# NUR=xhci baut die getrennte Fassung: nur der xHCI-Treiber, eigener
# Geraetename, eigener Dienst.  Damit laesst er sich im Benutzermodus
# laden und entladen, ohne dass die OHCI und EHCI derselben Suedbruecke
# mitgehen -- und ohne dass ein Fehler dort einen Fehler hier vortaeuscht.
if [ "$NUR" = "xhci" ]; then
    HCD="$SRC/dev/usb/controller/usb_controller.c
         $SRC/dev/usb/controller/xhci.c
         $SRC/dev/usb/controller/xhci_pci.c"
    CDEF="$CDEF -DNTU_NUR_XHCI"
    echo "Fassung: nur xHCI"
else
    HCD=$(ls "$SRC"/dev/usb/controller/*.c)
    echo "Fassung: alle vier Bauarten"
fi

ok=0; bad=0

for f in $LISTE $HCD "$UNT"/nt/*.c "$UNT"/ober/*.c; do
    if gcc $CFLAGS $CDEF $CINC -o "$OBJ/$(basename "$f" .c).o" "$f" 2>/dev/null; then
        ok=$((ok + 1))
    else
        bad=$((bad + 1))
        echo "  FEHLER $(basename "$f")"
        gcc $CFLAGS $CDEF $CINC -o /dev/null "$f" 2>&1 | head -4
    fi
done

echo "Objekte: $ok   gescheitert: $bad"
[ "$bad" != 0 ] && exit 1

nm --defined-only "$OBJ"/*.o | awk 'NF>=3 {print $3}' | sort -u > "$OBJ/def.txt"
nm -u "$OBJ"/*.o | awk '{ if ($1 == "U") print $2; else if ($2 == "U") print $3 }' \
    | sort -u > "$OBJ/und.txt"
comm -23 "$OBJ/und.txt" "$OBJ/def.txt" > "$OBJ/aussen.txt"

echo "Symbole definiert  : $(wc -l < "$OBJ/def.txt")"
echo "aussen nachgefragt : $(wc -l < "$OBJ/aussen.txt")"
echo
echo "gegen pruef/einfuhr.txt abgleichen -- was dort nicht steht, ist neu."
grep -v '^#' "$UNT/pruef/einfuhr.txt" | grep -v '^$' | sort > "$OBJ/soll.txt"
comm -23 "$OBJ/aussen.txt" "$OBJ/soll.txt" | sed 's/^/   NEU  /' 
