#!/bin/sh
# bau-nsis.sh -- NSIS-Installer bauen.  Braucht makensis (NSIS 3).
#   FASSUNG=2026.11.17.66 sh installer-nsis/bau-nsis.sh
# Erwartet die fuenf Treiber in bin/$FASSUNG und die Werkzeuge in
# installer-mit-werkzeugen.
cd "$(dirname "$0")" || exit 1
FASSUNG=${FASSUNG:-2026.11.17.66}
makensis -V2 -INPUTCHARSET CP1252 -DFASSUNG=$FASSUNG usbnt.nsi
