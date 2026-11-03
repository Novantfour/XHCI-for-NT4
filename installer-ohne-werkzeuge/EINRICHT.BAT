@echo off
rem  Fest einrichten.  Der Treiber laedt ab dem naechsten Start.
rem  Beispiel mit serieller Maus an COM1:   EINRICHT -seriell 0
"%~dp0USBINST.EXE" ein %1 %2 %3 %4
pause
