@echo off
rem  Probelauf: Treiber einmal von Hand starten, nichts fest eintragen.
rem  Geht dabei etwas schief, genuegt ein Neustart.
"%~dp0USBINST.EXE" probe %1 %2 %3
pause
