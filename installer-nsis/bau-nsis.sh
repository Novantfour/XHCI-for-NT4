#!/bin/sh
# bau-nsis.sh -- NSIS-Installer bauen.  Braucht makensis (NSIS 3).
#   FASSUNG=2026.11.19.73 sh installer-nsis/bau-nsis.sh
# Erwartet die fuenf Treiber in bin/$FASSUNG und die Werkzeuge in
# installer-mit-werkzeugen.
cd "$(dirname "$0")" || exit 1
FASSUNG=${FASSUNG:-2026.11.19.73}
# Treiber, die seit der letzten Treiberfassung unveraendert sind, bleiben
# in ihrem Ordner (.72/.73 aendern nur USBTASTEN.EXE).
TREIBERFASSUNG=${TREIBERFASSUNG:-2026.11.19.71}
makensis -V2 -INPUTCHARSET CP1252 -DFASSUNG=$FASSUNG \
  "-DTREIBER=..\\bin\\$TREIBERFASSUNG" usbnt.nsi
