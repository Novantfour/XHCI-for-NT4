#!/bin/sh
# Alle Werkzeuge aus dem aktuellen Stand bauen -- damit Installer und
# Gesamtpaket immer zusammenpassen (vorher einzeln gebaut; eine alte
# Fassung blieb so leicht liegen).
#   ./bau-werkzeuge.sh <zielordner>
# Konsolenprogramme: ntbasis.c, Einstieg _ntb_start.  Fensterprogramme:
# Einstieg@0.  DLLs/CPLs: DllEntry@12 und .def.  Ressourcen OHNE -s binden,
# dann strip (mit -s wurde .rsrc zu gross).  Kein CRT.
set -e
cd "$(dirname "$0")"
Z=${1:-/tmp/werkzeuge}; mkdir -p "$Z"
CC=i686-w64-mingw32-gcc
F="-Os -Wall -Wno-format -Wno-unused-function -Wno-unused-variable -fno-builtin -nostdlib -fno-asynchronous-unwind-tables -fno-unwind-tables"
FASSUNG=${FASSUNG:-2026.9.27.2}
F="$F -DFASSUNG_TEXT=\"$FASSUNG\""
NT4="-Wl,--major-subsystem-version=4 -Wl,--minor-subsystem-version=0 -Wl,--major-os-version=4 -Wl,--minor-os-version=0"
# Versionsangaben fuer JEDE Datei (Hersteller, Beschreibung, Produkt,
# Lizenz): fehlende Versionsangaben werten Virenscanner als verdaechtig.
# ver <name> <endung> <beschreibung> -> Objekt; eigene .rc (Symbol, Texte)
# wird mit eingebunden.
ver () {
  F4=$(echo $FASSUNG | tr . ,)
  {
    if [ -f res/$1.rc ] && grep -q VERSIONINFO res/$1.rc; then
      cat res/$1.rc                   # hat schon Versionsangaben (usbsetup)
    else
    [ -f res/$1.rc ] && cat res/$1.rc
    cat <<RC
1 VERSIONINFO
FILEVERSION $F4
PRODUCTVERSION $F4
FILEOS 0x40004
FILETYPE $( [ "$2" = exe ] && echo 1 || echo 2 )
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040704E4"
    BEGIN
      VALUE "CompanyName", "FreeBSD-USB fuer Windows NT 4.0 (freie Software)"
      VALUE "FileDescription", "$3"
      VALUE "FileVersion", "$FASSUNG"
      VALUE "InternalName", "$1"
      VALUE "OriginalFilename", "$4"
      VALUE "ProductName", "FreeBSD-USB fuer Windows NT 4.0"
      VALUE "ProductVersion", "$FASSUNG"
      VALUE "LegalCopyright", "BSD-Lizenz; Quelltext liegt bei"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x0407, 1252
  END
END
RC
    fi
  } > "$Z/$1_ver.rc"
  #  Konsolenwerkzeuge bekommen zusaetzlich ein Icon und ein Manifest:
  #  Ressourcen und ein Manifest lassen die winzigen Dateien weniger nach
  #  nacktem Stub aussehen (has_resources; asInvoker).  Hat die Datei schon
  #  eine eigene .rc (Fenster mit eigenem Symbol), NICHT das Standard-Icon
  #  aufdraengen.
  if [ "$2" = exe ] && ! { [ -f res/$1.rc ] && grep -q ICON res/$1.rc; }; then
    {
      echo '1 ICON "werkzeug.ico"'
      echo '1 24 "manifest.xml"'
    } >> "$Z/$1_ver.rc"
  fi
  (cd res && i686-w64-mingw32-windres -c 1252 "$Z/$1_ver.rc" -O coff -o "$Z/$1_res.o")
  echo "$Z/$1_res.o"
}
konsole () { $CC $F -o "$Z/$1.exe" $1.c ntbasis.c $3 -Wl,-e,_ntb_start $NT4 $2; i686-w64-mingw32-strip "$Z/$1.exe"; }
fenster () { $CC $F -o "$Z/$1.exe" $1.c $3 -Wl,-e,_Einstieg@0 -Wl,--subsystem,windows $NT4 $2; i686-w64-mingw32-strip "$Z/$1.exe"; }
# ACHTUNG, gemessen (2026-09-28): mit dem MinGW-w64-Startcode gebaute
# Werkzeuge HAENGEN unter echtem NT 4.0 SP6a (Wine: einwandfrei; alle
# Importe vorhanden).  Deshalb derzeit NICHT benutzt -- die Werkzeuge
# bleiben bei ntbasis.c / eigenem Einstieg.  Nur nach Test unter NT 4.0.
# Mit normaler C-Laufzeit (crt2.o, main/WinMain, printf aus msvcrt.dll):
# eigener Einstiegspunkt + eigene Laufzeit (ntbasis.c) sahen fuer Virenscanner
# wie ein Packer-Stub aus -- auch Werkzeuge ohne eine einzige heikle Funktion
# wurden gemeldet (Gruppentest).  Jeden Import gegen NT 4.0 SP6a pruefen:
#   pe_pruefen.py --gegen <nt4-dlls> *.exe   (Skill av-fehlalarm)
FC=$(echo "$F" | sed 's/ -nostdlib//')
# Der Startcode von mingw bringt .eh_frame mit (in C nie benutzt); nach
# strip hiesse der Abschnitt "/4" -- also vorher entfernen.
glatt () { i686-w64-mingw32-objcopy --remove-section=.eh_frame "$1"; i686-w64-mingw32-strip "$1"; }
konsole_crt () { $CC $FC -o "$Z/$1.exe" $1.c $3 $NT4 $2; glatt "$Z/$1.exe"; }
fenster_crt () { $CC $FC -mwindows -o "$Z/$1.exe" $1.c $3 $NT4 $2; glatt "$Z/$1.exe"; }
dll () { $CC $F -shared -o "$Z/$1.$2" $1.c $1.def $4 -Wl,-e,_DllEntry@12 -Wl,--enable-stdcall-fixup $NT4 $3; i686-w64-mingw32-strip "$Z/$1.$2"; }
# dll_mehr: DLL aus mehreren Quellen ($5).  Fuer USBGERAET.DLL, das die
# gesamte Logik von usbgeraet.c samt ntbasis.c mitbringt (rundll32-Einstieg).
dll_mehr () { $CC $F -DNTB_DLL -shared -o "$Z/$1.dll" $1dll.c $5 $1.def $4 -Wl,-e,_DllEntry@12 -Wl,--enable-stdcall-fixup $NT4 $3; i686-w64-mingw32-objcopy --remove-section=.eh_frame "$Z/$1.dll" 2>/dev/null; i686-w64-mingw32-strip "$Z/$1.dll"; }

konsole usbinst    "-lkernel32 -ladvapi32 -luser32" "$(ver usbinst exe 'Einrichtung des USB-Treibers (Befehlszeile)' USBINST.EXE)"
konsole xboxinst   "-lkernel32 -ladvapi32 -luser32" "$(ver xboxinst exe 'Einrichtung der USB-Gamepads (Befehlszeile)' XBOXINST.EXE)"
konsole audioinst  "-lkernel32 -ladvapi32 -luser32" "$(ver audioinst exe 'Einrichtung von USB-Audio (Befehlszeile)' AUDIOINST.EXE)"
konsole usbton     "uac.c -lkernel32 -ladvapi32 -luser32 -lwinmm -lversion" "$(ver usbton exe 'USB-Audio pruefen und Ton ausgeben (UAC1)' USBTON.EXE)"
konsole hidtest    "-lkernel32 -luser32" "$(ver hidtest exe 'HID-Geraete anzeigen (ueber HID.DLL)' HIDTEST.EXE)"
konsole usbtest    "-lkernel32 -ladvapi32" "$(ver usbtest exe 'USB-Treiber: Zustand und Geraetebaum' USBTEST.EXE)"
konsole xhcitest   "-lkernel32 -ladvapi32" "$(ver xhcitest exe 'USB-Treiber: xHCI-Pruefung' XHCITEST.EXE)"
konsole diprobe    "-lkernel32 -luser32 -lwinmm -ldinput" "$(ver diprobe exe 'USB-Treiber: DirectInput-Probe' DIPROBE.EXE)"
konsole joytest    "-lkernel32 -lwinmm" "$(ver joytest exe 'USB-Treiber: Gamepad-Pruefung' JOYTEST.EXE)"
konsole usbdruck   "-lkernel32" "$(ver usbdruck exe 'USB-Treiber: Drucker-Pruefung' USBDRUCK.EXE)"
konsole ipptest    "-lkernel32" "$(ver ipptest exe 'USB-Treiber: Drucker-Schnittstelle pruefen' IPPTEST.EXE)"
konsole eingabetest "-lkernel32 -luser32" "$(ver eingabetest exe 'USB-Treiber: Eingabe-Pruefung' EINGABETEST.EXE)"
fenster usbsetup   "-lkernel32 -luser32 -lgdi32 -ladvapi32 -lversion" "$(ver usbsetup exe 'Einrichtung des USB-Treibers' USBSETUP.EXE)"
fenster usbtasten  "auswurf.c -lkernel32 -luser32 -lwinmm -lshell32" "$(ver usbtasten exe 'USB-Treiber: Lautstaerke- und Programmtasten' USBTASTEN.EXE)"
fenster mauslatenz "-lkernel32 -luser32 -lgdi32" "$(ver mauslatenz exe 'USB-Treiber: Maus-Messung' MAUSLATENZ.EXE)"
fenster klicktest  "-lkernel32 -luser32 -lgdi32" "$(ver klicktest exe 'USB-Treiber: Klick-Pruefung' KLICKTEST.EXE)"
fenster mausdaten  "-lkernel32 -luser32 -lgdi32" "$(ver mausdaten exe 'USB-Treiber: Maus-Protokoll' MAUSDATEN.EXE)"
dll usbmausx  dll "-lkernel32 -luser32 -lgdi32 -ladvapi32 -lcomctl32" "$(ver usbmausx dll 'Systemsteuerung Maus: Reiter USB-Maustasten' USBMAUSX.DLL)"
dll usbtastx  dll "-lkernel32 -luser32 -lgdi32 -ladvapi32 -lcomctl32 -lcomdlg32" "$(ver usbtastx dll 'Systemsteuerung Tastatur: Reiter Sondertasten' USBTASTX.DLL)"
dll_mehr usbgeraet "" "-lkernel32 -luser32" "$(ver usbgeraet dll 'USB-Geraete ansehen und roh ansprechen (rundll32)' USBGERAET.DLL)" "usbgeraet.c ntbasis.c auswurf.c"
dll usbjoy    dll "-lkernel32 -luser32 -ladvapi32 -lwinmm" "$(ver usbjoy dll 'Joystick-Treiber fuer USB-Gamepads (winmm)' USBJOY.DLL)"
dll usbjoycpl cpl "-lkernel32 -luser32 -lgdi32 -lwinmm -lcomctl32" "$(ver usbjoycpl dll 'Systemsteuerung Joystick' USBJOY.CPL)"
mv "$Z/usbjoycpl.cpl" "$Z/usbjoy.cpl"
# usbaudio.dll: winmm-Wave-Treiber, mit uac.c und ntbasis.c (ohne Startcode).
# HID.DLL -- die originalen HID-Schnittstellen fuer NT 4.0 (aufgesetzt auf
# \\\\.\\UsbGenN).  Quelle hiddll.c, damit der Name nicht mit einer
# moeglichen hid.c kollidiert; -I.. fuer ober/ntusbgen.h.
$CC $F -I.. -shared -o "$Z/hid.dll" hiddll.c hid.def "$(ver hid dll 'HID-Schnittstelle fuer Programme (NT 4.0)' HID.DLL)" \
    -Wl,-e,_DllEntry@12 -Wl,--enable-stdcall-fixup $NT4 -lkernel32 -luser32
i686-w64-mingw32-strip "$Z/hid.dll"
$CC $F -DNTB_DLL -shared -o "$Z/usbaudio.dll" usbaudio.c uac.c ntbasis.c usbaudio.def \
    "$(ver usbaudio dll 'USB-Audio: Wave-Treiber fuer winmm (UAC1)' USBAUDIO.DLL)" \
    -Wl,-e,_DllEntry@12 -Wl,--enable-stdcall-fixup $NT4 -lkernel32 -luser32 -ladvapi32 -lwinmm
i686-w64-mingw32-strip "$Z/usbaudio.dll"
dll usbmaus   cpl "-lkernel32 -luser32 -lgdi32" "$(ver usbmaus dll 'Systemsteuerung USB-Maus' USBMAUS.CPL)"
rm -f "$Z"/*_res.o "$Z"/*_ver.rc
ls "$Z"
