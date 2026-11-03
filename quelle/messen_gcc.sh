#!/bin/sh
#
#   messen_gcc.sh -- wie scripts/messen.sh aus dem Skill, aber mit gcc,
#   weil in diesem Container kein clang liegt.  Verfahren identisch:
#   alle nicht gefundenen Kopfdateien werden zu leeren Rumpfen, die
#   Kopfdateien des Untersystems bleiben echt, und der Uebersetzer sagt,
#   was ihm dann noch fehlt.
#
#       ./messen_gcc.sh <unterlage> <untersystem-inc> <quellverz> datei...

set -e
UNTERLAGE="$1"; shift
UNTERSYSTEM="$1"; shift
QUELLVERZ="$1"; shift

T=$(mktemp -d)
mkdir -p "$T/rumpf"

FLAGS="-std=gnu11 -ffreestanding -nostdinc -fsyntax-only -m32 -w"
INC="-I$UNTERLAGE -I$T/rumpf -I$UNTERSYSTEM -I$QUELLVERZ"

runde=0
while [ $runde -lt 400 ]; do
    fehlt=$(for f in "$@"; do
        gcc $FLAGS $INC "$QUELLVERZ/$f" 2>&1 || true
    done | sed -n "s/.*fatal error: \([^:]*\): No such file or directory.*/\1/p" | sort -u)

    [ -z "$fehlt" ] && break
    for h in $fehlt; do
        mkdir -p "$T/rumpf/$(dirname "$h")"
        : > "$T/rumpf/$h"
    done
    runde=$((runde + 1))
done

echo "Runden bis alle Includes aufgeloest: $runde"
echo "Als leerer Rumpf noetig: $(cd "$T/rumpf" && find . -type f | wc -l) Kopfdateien"
(cd "$T/rumpf" && find . -type f | sed 's|^\./|   |' | sort > "$T/rumpfe.txt")

for f in "$@"; do
    gcc $FLAGS $INC "$QUELLVERZ/$f" 2>&1 || true
done > "$T/roh.txt"

sed -n "s/.*unknown type name .\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p;
        s/.*error: .\([A-Za-z_][A-Za-z0-9_]*\). undeclared.*/\1/p;
        s/.*implicit declaration of function .\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p;
        s/.*has no member named .\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p" "$T/roh.txt" \
    | sort -u > "$T/offen.txt"

echo "Fehlermeldungen:        $(grep -c 'error:' "$T/roh.txt" || true)"
echo "Offene Bezeichner:      $(wc -l < "$T/offen.txt")"
echo
echo "Nach Sachgebieten:"
zaehl() { n=$(grep -cE "$2" "$T/offen.txt" || true); printf '   %-26s %4d\n' "$1" "$n"; }
zaehl "Speicher"           '^(k[mzcv]alloc|kfree|vmalloc|vfree|devm_)'
zaehl "Sperren und Warten" '^(spin_|raw_spin|mutex_|msleep|udelay|mdelay|wait_|complete|schedule)'
zaehl "PCI"                '^(pci_|PCI_|pcim_)'
zaehl "DMA und Abbildung"  '^(dma_|DMA_|io[a-z]*map|iounmap)'
zaehl "Register"           '^(read[bwlq]|write[bwlq]|ioread|iowrite|bus_space)'
zaehl "Interrupt"          '^(request_irq|free_irq|irq|IRQF|synchronize_irq)'
zaehl "Geraetemodell"      '^(dev_|device_|pm_|put_device|get_device|driver_)'
zaehl "Fehler"             '^(E[A-Z]+$|IS_ERR|PTR_ERR|ERR_PTR)'
zaehl "Makros und CONFIG"  '^(CONFIG_|ARRAY_SIZE|BIT$|BIT_ULL|GENMASK|likely|unlikely)'
zaehl "USB-Untersystem"    '^(usb_|USB_|hcd|urb)'
echo
cp "$T/offen.txt" "./offen-$(basename $QUELLVERZ)-$$.txt"
echo "Liste: ./offen-$(basename $QUELLVERZ)-$$.txt"
cp "$T/rumpfe.txt" "./rumpfe-$$.txt"
rm -rf "$T"
