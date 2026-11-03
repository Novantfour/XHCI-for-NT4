#!/bin/sh
# echter Bau: clang-cl + lld-link gegen das NT-4.0-DDK
DDK=${DDK:-/home/claude/ddk/x}
UNT=${UNT:-/home/claude/usb/unterlage}
SRC=${SRC:-/home/claude/usb/bau/src}
OBJ=${OBJ:-obj}
NUR=${NUR:-}
mkdir -p $OBJ; rm -f $OBJ/*.obj

CF="--target=i386-pc-windows-msvc -c -Gz -GS- -Gy -O1 -Oi- -fno-builtin"
CF="$CF -D_X86_ -Di386=1 -D_KERNEL -DNTDDI_VERSION=0"
CF="$CF -Wno-missing-prototype-for-cc -Wno-ignored-attributes -w"
# KISERIAL=1 fuer den selbstgebauten Kern: Ausgabe ueber dessen
# KiSerialPutString.  Ohne das schreibt der Treiber selbst auf COM1 und
# laeuft auf jedem NT-4.0-Kern.
[ "$KISERIAL" = 1 ] && CF="$CF -DNTU_KISERIAL"
# HERZ=1: alle fuenf Sekunden Zaehler und PORTSC auf COM1 (Pruefstand)
[ "$HERZ" = 1 ] && CF="$CF -DNTU_HERZSCHLAG"
# EXTRA: weitere Schalter, z. B. -DNTU_AMDPLL_TROCKEN
CF="$CF $EXTRA"
# Dateifassung (auch Versionsressource, s. u.): aus Datum und Baunummer.
FASSUNG=${FASSUNG:-$(date +%Y).$(date +%-m).$(date +%-d).${BAUNR:-1}}
CF="$CF -DNTU_DATEIFASSUNG=\"$FASSUNG\""
# UDEBUG=n: Gespraechigkeit des FreeBSD-Stapels (usb_debug)
[ -n "$UDEBUG" ] && CF="$CF -DNTU_USB_DEBUG=$UDEBUG"
CI="-I$UNT -I$UNT/dev/usb -I$SRC -I$DDK/inc-patched -I$DDK/sdk"
# Der USB-Kern ist in allen Fassungen derselbe.  Nicht gebaut werden
# usb_dev.c, usb_generic.c und usb_pf.c -- /dev/ugen ist ueber
# USB_HAVE_UGEN = 0 abgeschaltet.
LISTE=$(ls $SRC/dev/usb/*.c | grep -vE '/(usb_dev|usb_generic|usb_pf)\.c$')

# NUR=uhci|ohci|ehci|xhci baut genau eine Bauart, mit eigenem
# Geraetenamen und eigenem Dienst.  Damit laesst sie sich einzeln laden
# und entladen, ohne dass die anderen Bausteine derselben Suedbruecke
# mitgehen -- und ohne dass ein Fehler dort wie ein Fehler hier aussieht.
case "$NUR" in
uhci|ohci|ehci|xhci)
  HCD="$SRC/dev/usb/controller/usb_controller.c"
  HCD="$HCD $SRC/dev/usb/controller/$NUR.c"
  HCD="$HCD $SRC/dev/usb/controller/${NUR}_pci.c"
  CF="$CF -DNTU_NUR_$(echo $NUR | tr a-z A-Z)"
  ZIEL="${NUR}nt.sys"
  echo "Fassung: nur $NUR"
  ;;
*)
  HCD=$(ls $SRC/dev/usb/controller/*.c)
  ZIEL=usbnt.sys
  echo "Fassung: alle vier Bauarten"
  ;;
esac

ok=0; bad=0
# dev/hid/hid.c: FreeBSDs HID-Deskriptor-Parser (Maus, Joystick).
# Zusatztreiber: eigene .sys, die sich bei usbnt.sys anmelden (unten).
# Ihre Schnittstellentreiber gehoeren NICHT in usbnt.sys.
#   Name      Verzeichnis  Dateien                              Beschreibung
ZUSATZ="usbxbox:xbox:ober/ntujoy.c,xbox/ntxbox.c:USB-Gamepads, Xbox 360/One/Series und HID
usbaudio:audio:ober/ntuaudio.c,audio/ntaudio.c:USB-Audio (UAC1), Wiedergabe"
OBER=$(ls $UNT/ober/*.c | grep -v '/ntujoy\.c$' | grep -v '/ntuaudio\.c$')
for f in $LISTE $SRC/dev/hid/*.c $HCD $UNT/nt/*.c $OBER; do
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

# Versionsressource: FASSUNG von oben (FASSUNG=2026.9.26.2 ./bau.sh setzt
# sie von Hand, BAUNR=2 nur die letzte Stelle).
F4=$(echo $FASSUNG | tr . ,)
case "$EXTRA $HERZ" in
  *EINGABE_PROTOKOLL*) FORM="Diagnose" ;;
  *1*)                 FORM="Herzschlag" ;;
  *)                   FORM="Freigabe" ;;
esac
NAME=$(basename $ZIEL .sys)
cat > $OBJ/version.rc <<RC
1 VERSIONINFO
FILEVERSION $F4
PRODUCTVERSION $F4
FILEFLAGSMASK 0x3F
FILEFLAGS 0
FILEOS 0x40004
FILETYPE 3
FILESUBTYPE 7
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040704E4"
    BEGIN
      VALUE "CompanyName", "FreeBSD-USB fuer Windows NT 4.0 (freie Software)"
      VALUE "LegalCopyright", "BSD-Lizenz; Quelltext liegt bei"
      VALUE "FileDescription", "USB-Treiber ($NAME, $FORM)"
      VALUE "FileVersion", "$FASSUNG"
      VALUE "InternalName", "$NAME"
      VALUE "OriginalFilename", "$NAME.sys"
      VALUE "ProductName", "FreeBSD-USB fuer Windows NT 4.0"
      VALUE "ProductVersion", "$FASSUNG"
      VALUE "Comments", "Druckerschnittstelle Fassung 4; FreeBSD-USB-Stapel unveraendert"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x0407, 1252
  END
END
RC
i686-w64-mingw32-windres $OBJ/version.rc -O coff -o $OBJ/zz_version.obj || exit 1

# ---- USBXBOX.SYS uebersetzen und die Exportliste von usbnt.sys bilden ----
# Exportiert wird genau, was die Objekte von USBXBOX offen lassen und die
# von usbnt.sys definieren.  Die Liste pflegt sich damit selbst.
ZOBJ=$OBJ/zusatz; rm -rf $ZOBJ; mkdir -p $ZOBJ
echo "$ZUSATZ" | while IFS=: read zn zv zd zb; do
  mkdir -p $ZOBJ/$zv
  for f in $(echo $zd | tr , ' '); do
    clang-cl $CF $CI "$UNT/$f" -Fo$ZOBJ/$zv/$(basename $f .c).obj 2>&1 | grep -E "error" | head -4
  done
done
python3 - "$OBJ" "$ZOBJ" <<'PY'
import subprocess, sys, glob
obj, xobj = sys.argv[1], sys.argv[2]
def nm(f, art):
    out = subprocess.run(['llvm-nm', f], capture_output=True, text=True).stdout
    return {z.split()[-1] for z in out.splitlines() if z.split() and z.split()[-2:-1] == [art]}
offen = set()
for zv in glob.glob(xobj + '/*/'):                  # je Zusatztreiber getrennt
    zf = glob.glob(zv + '*.obj')
    u = set().union(*(nm(f, 'U') for f in zf))
    u -= set().union(*(nm(f, 'T') | nm(f, 'D') | nm(f, 'B') for f in zf))
    offen |= u
da = set().union(*(nm(f, 'T') for f in glob.glob(obj + '/*.obj')))
exp = sorted(s for s in offen if s in da)
# usbnt.def: undekoriert (lld sucht die @N-Fassung selbst);
# Einfuhr-.def: dekoriert, dlltool -k streift beim Import wieder ab.
undek = lambda s: s[1:].split('@')[0] if s.startswith('_') else s
open(obj + '/usbnt.def', 'w').write('LIBRARY usbnt.sys\nEXPORTS\n' +
    ''.join('  %s\n' % undek(s) for s in exp))
open(xobj + '/usbnt-imp.def', 'w').write('LIBRARY usbnt.sys\nEXPORTS\n' +
    ''.join('  %s\n' % s[1:] for s in exp))
print('Stapel exportiert %d Namen fuer die Zusatztreiber' % len(exp))
PY
llvm-dlltool-18 -m i386 -k -d $ZOBJ/usbnt-imp.def -l $ZOBJ/usbnt.lib || exit 1

lld-link /out:$ZIEL /machine:x86 /subsystem:native,4.00 /driver \
  /def:$OBJ/usbnt.def /implib:$OBJ/usbnt-lld.lib \
  /entry:DriverEntry@8 /nodefaultlib /safeseh:no \
  /merge:NTUDRV=.rdata /merge:NTUSYS=.rdata /ignore:4078 \
  /base:0x10000 /release \
  /section:INIT,ERWD /section:PAGE,ERP \
  $OBJ/ntoskrnl.lib $OBJ/hal.lib $OBJ/kiserial.lib $OBJ/*.obj 2>&1 | grep -v "unrecognized magic" | head -20
# /release setzt die Pruefsumme im Kopf.  Ohne sie meldet NT beim
# Starten 0xC0000221 (STATUS_IMAGE_CHECKSUM_MISMATCH) und sagt nichts
# weiter -- der Treiber ist dabei in Ordnung, er wird nur nicht
# angesehen.  Nachgerechnet wird trotzdem, weil sich auf einen Schalter
# zu verlassen billiger ist als ihn zu pruefen, aber nicht sicherer.
# Abschnittsattribute richtigstellen: NT laegt Treibercode sonst aus (paged),
# was bei erhoehtem IRQL einen Bugcheck ausloest (vom SIV-Autor gemeldet).
# lld-link setzt das NOT_PAGED-Bit nicht; deshalb hier direkt im PE-Kopf.
#   .text  -> CODE EXEC READ        + NOT_PAGED   (nicht beschreibbar!)
#   .rdata -> IDATA READ            + NOT_PAGED
#   .data  -> IDATA READ WRITE      + NOT_PAGED
# PAGE und INIT sind in .text gemergt; der gesamte Code ist damit
# resident.  (Ein eigener auslagerbarer PAGE-Abschnitt lohnt bei diesem
# Treiber nicht -- fast alles laeuft im IRP-/DPC-/ISR-Pfad.)
python3 abschnitte.py $ZIEL
# Pruefsumme NACH der Attributaenderung neu setzen (nicht erst pruefen).
python3 pruefsumme.py $ZIEL

# ---- Zusatztreiber binden (nur zur vollen Fassung: sie importieren aus usbnt.sys) ----
if [ -z "$NUR" ]; then
  echo "$ZUSATZ" | while IFS=: read zn zv zd zb; do
    sed -e "s/USB-Treiber ($NAME, $FORM)/$(echo "$zb" | sed 's,/,\\/,g') ($zn, $FORM)/" \
        -e "s/\"InternalName\", \"[^\"]*\"/\"InternalName\", \"$zn\"/" \
        -e "s/\"OriginalFilename\", \"[^\"]*\"/\"OriginalFilename\", \"$zn.sys\"/" \
        $OBJ/version.rc > $ZOBJ/$zv/version.rc
    i686-w64-mingw32-windres $ZOBJ/$zv/version.rc -O coff -o $ZOBJ/$zv/zz_version.obj || exit 1
    lld-link /out:$zn.sys /machine:x86 /subsystem:native,4.00 /driver \
      /entry:DriverEntry@8 /nodefaultlib /safeseh:no \
      /merge:NTUDRV=.rdata /ignore:4078 /base:0x10000 /release \
      /section:INIT,ERWD /section:PAGE,ERP \
      $OBJ/ntoskrnl.lib $OBJ/hal.lib $ZOBJ/usbnt.lib $ZOBJ/$zv/*.obj 2>&1 | grep -v "unrecognized magic" | head -20
    python3 abschnitte.py $zn.sys | tail -1
    python3 pruefsumme.py $zn.sys
  done
fi

ls -la $ZIEL 2>/dev/null
