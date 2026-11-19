; usbnt.nsi -- NSIS-Installer fuer FreeBSD-USB unter Windows NT 4.0
;
; Bauen:   sh installer-nsis/bau-nsis.sh  (Fassung aus FASSUNG, Vorgabe unten)
;
; Der Installer richtet NICHTS selbst in der Registry ein.  Er ruft
; USBINST.EXE -- dieselbe gepruefte Logik wie USBSETUP.EXE:
;   usbinst pruefen               fremde Maus-/Tastatur-/USB-Treiber finden
;   usbinst abschalten <fund>     angehakte Funde abschalten (alter Wert gesichert)
;   usbinst ein <art> -seriell n  Treiber, Dienst, Klassenwerte, Werkzeuge
;   usbinst taste 4|5 <belegung>  Maustasten 4 und 5
;   usbinst aus                   (Deinstallation) alles zurueck, auch Abgeschaltetes
; Aendert sich dort etwas, gilt es hier automatisch mit.

Unicode false                        ; NT 4.0: ANSI-Installer
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!ifndef FASSUNG
  !define FASSUNG "2026.11.19.71"
!endif
!ifndef TREIBER                      ; Ordner mit usbnt/uhcint/ohcint/ehcint/xhcint.sys
  !define TREIBER "..\bin\${FASSUNG}"
!endif
!ifndef PAKET                        ; Ordner mit USBINST.EXE und den Werkzeugen
  !define PAKET "..\installer-mit-werkzeugen"
!endif

!define NAME      "FreeBSD-USB für NT 4.0"
!define KURZ      "UsbNt"
!define UNINST    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${KURZ}"

Name    "${NAME}"
OutFile "usbnt-setup-${FASSUNG}.exe"
InstallDir "$PROGRAMFILES\FreeBSD-USB"
InstallDirRegKey HKLM "${UNINST}" "InstallLocation"
ShowInstDetails show
ShowUninstDetails show
BrandingText "FreeBSD-USB ${FASSUNG}"

VIProductVersion "${FASSUNG}"
VIAddVersionKey /LANG=1031 "ProductName"     "${NAME}"
VIAddVersionKey /LANG=1031 "FileDescription" "Installer FreeBSD-USB für NT 4.0"
VIAddVersionKey /LANG=1031 "FileVersion"     "${FASSUNG}"
VIAddVersionKey /LANG=1031 "ProductVersion"  "${FASSUNG}"
VIAddVersionKey /LANG=1031 "LegalCopyright"  "BSD-Lizenz (FreeBSD-USB-Stapel)"

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "TextFunc.nsh"
!include "WordFunc.nsh"
!include "WinVer.nsh"

!define MUI_ICON   "..\quelle\werkzeug\res\usbsetup.ico"
!define MUI_UNICON "..\quelle\werkzeug\res\usbsetup.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "FreeBSD-USB ${FASSUNG}"
!define MUI_WELCOMEPAGE_TEXT "Dieser Assistent richtet den USB-Treiber für Windows NT 4.0 ein: UHCI, OHCI, EHCI und xHCI, dazu Tastatur, Maus, Massenspeicher, CD/DVD und Drucker.$\r$\n$\r$\nVorher wird geprüft, ob fremde Maus-, Tastatur- oder USB-Treiber eingerichtet sind, die sich mit diesem Treiber nicht vertragen.$\r$\n$\r$\nDanach ist ein Neustart nötig.$\r$\n$\r$\nGeht beim Start etwas schief: beim Booten LEERTASTE drücken und die letzte als funktionierend bekannte Konfiguration wählen."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
Page custom EinstellungenZeigen EinstellungenVerlassen
Page custom FundeZeigen FundeVerlassen
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "Der Treiber ist eingerichtet und wird beim nächsten Start geladen."
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "German"

; --------------------------------------------------------------------
;  Auswahl aus der Einstellungsseite
; --------------------------------------------------------------------
Var Art            ; usbnt|uhci|ohci|ehci|xhci (Parameter fuer usbinst)
Var Dienst         ; usbnt|uhcint|...         (Dienstname, fuer XhciSchritt)
Var Seriell        ; 0|1|2
Var Taste4
Var Taste5
Var VBox           ; 1 = XhciSchritt 0 (VirtualBox USB 3.0)

Var hArt0
Var hArt1
Var hArt2
Var hArt3
Var hArt4
Var hSeriell
Var hTaste4
Var hTaste5
Var hVBox

Var FundAnzahl

; --------------------------------------------------------------------
;  Abschnitte
; --------------------------------------------------------------------
Section "USB-Treiber" SecTreiber
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "/oname=USBNT.SYS"  "${TREIBER}\usbnt.sys"
  File "/oname=UHCINT.SYS" "${TREIBER}\uhcint.sys"
  File "/oname=OHCINT.SYS" "${TREIBER}\ohcint.sys"
  File "/oname=EHCINT.SYS" "${TREIBER}\ehcint.sys"
  File "/oname=XHCINT.SYS" "${TREIBER}\xhcint.sys"
  File "${PAKET}\USBINST.EXE"
  File "${PAKET}\USBSETUP.EXE"
  File "${PAKET}\USBNT.INF"
  File "${PAKET}\LIESMICH.TXT"
  File "${PAKET}\USBMAUS.CPL"
  File "${PAKET}\USBMAUSX.DLL"
  File "${PAKET}\USBTASTX.DLL"
SectionEnd

; PCI-Scan: PCISCAN.SYS liest den PCI-Bus fuer den Geraetemanager.  Liegt
; die Datei neben USBINST, richtet "usbinst ein" den Dienst PciScan ein
; (Start automatisch, seit 2026.11.18).  Ohne diesen Bestandteil zeigt der
; Geraetemanager nur die USB-Geraete.
Section "PCI-Scan und Gerätemanager" SecPciScan
  SetOutPath "$INSTDIR"
  File "/oname=PCISCAN.SYS" "${TREIBER}\pciscan.sys"
  File "${PAKET}\DEVMGR.EXE"
  File "${PAKET}\DEVMGR.CPL"
  File "${PAKET}\PCI.IDS"
  File "${PAKET}\PCIHOLE.EXE"
  File "${PAKET}\CACERT.PEM"
SectionEnd

Section "Zusatzprogramme" SecWerkzeuge
  SetOutPath "$INSTDIR"
  File "${PAKET}\USBTASTEN.EXE"
  File "${PAKET}\USBTEST.EXE"
  File "${PAKET}\USBGERAET.DLL"
  File "${PAKET}\USBDRUCK.EXE"
SectionEnd

; Laeuft zuletzt: erst wenn alle Dateien neben USBINST liegen, richtet
; "usbinst ein" sie mit ein.
Section "-Einrichten"
  SetOutPath "$INSTDIR"

  ; 1. Angehakte Funde abschalten (alter Zustand wird von usbinst gesichert)
  StrCpy $R9 0
  ${While} $R9 < $FundAnzahl
    ReadINIStr $R0 "$PLUGINSDIR\funde.ini" "f$R9" "kenn"
    ReadINIStr $R1 "$PLUGINSDIR\funde.ini" "f$R9" "an"
    ${If} $R1 == "1"
      DetailPrint "Abschalten: $R0"
      nsExec::ExecToLog '"$INSTDIR\USBINST.EXE" abschalten $R0'
      Pop $R2
    ${EndIf}
    IntOp $R9 $R9 + 1
  ${EndWhile}

  ; 2. Einrichten
  DetailPrint "usbinst ein $Art -seriell $Seriell"
  nsExec::ExecToLog '"$INSTDIR\USBINST.EXE" ein $Art -seriell $Seriell'
  Pop $R0
  ${If} $R0 != "0"
    DetailPrint "USBINST meldet Fehler ($R0)."
    MessageBox MB_ICONSTOP|MB_OK "Das Einrichten ist gescheitert (USBINST: $R0).$\r$\nEinzelheiten stehen in der Liste oben.$\r$\nAn der Registry wurde nichts weiter verändert."
    Abort
  ${EndIf}

  ; 3. Maustasten 4/5
  nsExec::ExecToLog '"$INSTDIR\USBINST.EXE" taste 4 $Taste4'
  Pop $R0
  nsExec::ExecToLog '"$INSTDIR\USBINST.EXE" taste 5 $Taste5'
  Pop $R0

  ; 4. VirtualBox-xHCI (wie USBSETUP: XhciSchritt=0 setzen oder entfernen)
  ${If} $VBox == "1"
    WriteRegDWORD HKLM "SYSTEM\CurrentControlSet\Services\$Dienst\Parameters" "XhciSchritt" 0
    DetailPrint "$Dienst: XhciSchritt = 0 (VirtualBox USB 3.0)"
  ${Else}
    DeleteRegValue HKLM "SYSTEM\CurrentControlSet\Services\$Dienst\Parameters" "XhciSchritt"
  ${EndIf}

  ; 5. Deinstallation eintragen
  WriteUninstaller "$INSTDIR\Deinstallieren.exe"
  WriteRegStr   HKLM "${UNINST}" "DisplayName"     "${NAME}"
  WriteRegStr   HKLM "${UNINST}" "DisplayVersion"  "${FASSUNG}"
  WriteRegStr   HKLM "${UNINST}" "UninstallString" '"$INSTDIR\Deinstallieren.exe"'
  WriteRegStr   HKLM "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${UNINST}" "Art"             "$Art"
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\FreeBSD-USB"
  CreateShortCut  "$SMPROGRAMS\FreeBSD-USB\USB einrichten (USBSETUP).lnk" "$INSTDIR\USBSETUP.EXE"
  ${If} ${FileExists} "$INSTDIR\DEVMGR.EXE"
    CreateShortCut "$SMPROGRAMS\FreeBSD-USB\Gerätemanager.lnk" "$INSTDIR\DEVMGR.EXE"
  ${EndIf}
  CreateShortCut  "$SMPROGRAMS\FreeBSD-USB\Liesmich.lnk" "$INSTDIR\LIESMICH.TXT"
  CreateShortCut  "$SMPROGRAMS\FreeBSD-USB\Deinstallieren.lnk" "$INSTDIR\Deinstallieren.exe"

  SetRebootFlag true
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecTreiber}   "Der Treiber in allen fünf Fassungen (die gewählte wird eingerichtet), USBINST/USBSETUP, Reiter für Maus- und Sondertasten."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPciScan}   "PCISCAN.SYS (Dienst PciScan, startet automatisch) und der Gerätemanager mit PCI-Namen. Ohne ihn zeigt der Gerätemanager keine PCI-Geräte."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecWerkzeuge} "USBTASTEN (Medien- und Sondertasten), USBTEST, USBGERAET, USBDRUCK. Der Treiber braucht sie nicht."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; --------------------------------------------------------------------
;  Start: nur Windows NT, nur Administratoren
; --------------------------------------------------------------------
Function .onInit
  ${IfNot} ${IsNT}
    MessageBox MB_ICONSTOP|MB_OK "Dieser Treiber ist für Windows NT 4.0."
    Abort
  ${EndIf}
  UserInfo::GetAccountType
  Pop $0
  ${If} $0 != "Admin"
    MessageBox MB_ICONSTOP|MB_OK "Bitte als Administrator anmelden."
    Abort
  ${EndIf}

  StrCpy $Art "usbnt"
  StrCpy $Dienst "usbnt"
  StrCpy $Seriell "0"
  StrCpy $Taste4 "alt+links"
  StrCpy $Taste5 "alt+rechts"
  ; Erneute Installation: die vorige Controllerart vorwaehlen
  ReadRegStr $0 HKLM "${UNINST}" "Art"
  ${Select} $0
    ${Case} "uhci"
      StrCpy $Art "uhci"
      StrCpy $Dienst "uhcint"
    ${Case} "ohci"
      StrCpy $Art "ohci"
      StrCpy $Dienst "ohcint"
    ${Case} "ehci"
      StrCpy $Art "ehci"
      StrCpy $Dienst "ehcint"
    ${Case} "xhci"
      StrCpy $Art "xhci"
      StrCpy $Dienst "xhcint"
  ${EndSelect}
  StrCpy $VBox "0"
  StrCpy $FundAnzahl 0

  InitPluginsDir
  File "/oname=$PLUGINSDIR\USBINST.EXE" "${PAKET}\USBINST.EXE"
FunctionEnd

; --------------------------------------------------------------------
;  Seite "Einstellungen": Controllerart, Bericht, Maustasten, VirtualBox
; --------------------------------------------------------------------
!macro TASTEN_LISTE hwnd
  ${NSD_CB_AddString} ${hwnd} "Zurück (Alt+Links)"
  ${NSD_CB_AddString} ${hwnd} "Vorwärts (Alt+Rechts)"
  ${NSD_CB_AddString} ${hwnd} "Mittlere Maustaste"
  ${NSD_CB_AddString} ${hwnd} "Doppelklick"
  ${NSD_CB_AddString} ${hwnd} "Kopieren (Strg+C)"
  ${NSD_CB_AddString} ${hwnd} "Einfügen (Strg+V)"
  ${NSD_CB_AddString} ${hwnd} "Aus"
!macroend

Function TasteWert            ; Eingang: Anzeigetext auf dem Stapel, Ausgang: Wert
  Exch $0
  ${Select} $0
    ${Case} "Zurück (Alt+Links)"
      StrCpy $0 "alt+links"
    ${Case} "Vorwärts (Alt+Rechts)"
      StrCpy $0 "alt+rechts"
    ${Case} "Mittlere Maustaste"
      StrCpy $0 "mitte"
    ${Case} "Doppelklick"
      StrCpy $0 "doppelklick"
    ${Case} "Kopieren (Strg+C)"
      StrCpy $0 "strg+c"
    ${Case} "Einfügen (Strg+V)"
      StrCpy $0 "strg+v"
    ${Default}
      StrCpy $0 "aus"
  ${EndSelect}
  Exch $0
FunctionEnd

Function EinstellungenZeigen
  !insertmacro MUI_HEADER_TEXT "Einstellungen" "Welche Controller, Diagnose und Maustasten"
  nsDialogs::Create 1018
  Pop $0

  ${NSD_CreateGroupBox} 0 0 100% 72u "Treiber"
  Pop $0
  ${NSD_CreateRadioButton} 8u 10u 95% 11u "USBNT -- alle Controller (empfohlen)"
  Pop $hArt0
  ${NSD_AddStyle} $hArt0 ${WS_GROUP}
  ${NSD_CreateRadioButton} 8u 22u 95% 11u "UHCINT -- nur UHCI (USB 1.1, Intel/VIA)"
  Pop $hArt1
  ${NSD_CreateRadioButton} 8u 34u 95% 11u "OHCINT -- nur OHCI (USB 1.1, AMD/SiS/ALi/NEC)"
  Pop $hArt2
  ${NSD_CreateRadioButton} 8u 46u 95% 11u "EHCINT -- nur EHCI (USB 2.0)"
  Pop $hArt3
  ${NSD_CreateRadioButton} 8u 58u 95% 11u "XHCINT -- nur xHCI (USB 3.x)"
  Pop $hArt4
  ${Select} $Art
    ${Case} "uhci"
      ${NSD_Check} $hArt1
    ${Case} "ohci"
      ${NSD_Check} $hArt2
    ${Case} "ehci"
      ${NSD_Check} $hArt3
    ${Case} "xhci"
      ${NSD_Check} $hArt4
    ${Default}
      ${NSD_Check} $hArt0
  ${EndSelect}

  ${NSD_CreateLabel} 0 80u 40% 10u "Diagnoseausgabe:"
  Pop $0
  ${NSD_CreateDropList} 42% 78u 58% 60u ""
  Pop $hSeriell
  ${NSD_CB_AddString} $hSeriell "Aus (Vorgabe)"
  ${NSD_CB_AddString} $hSeriell "COM1 (nur zur Fehlersuche)"
  ${NSD_CB_AddString} $hSeriell "COM2 (nur zur Fehlersuche)"
  SendMessage $hSeriell ${CB_SETCURSEL} $Seriell 0

  ${NSD_CreateLabel} 0 95u 40% 10u "Maustaste 4:"
  Pop $0
  ${NSD_CreateDropList} 42% 93u 58% 80u ""
  Pop $hTaste4
  !insertmacro TASTEN_LISTE $hTaste4
  ${NSD_CB_SelectString} $hTaste4 "Zurück (Alt+Links)"

  ${NSD_CreateLabel} 0 110u 40% 10u "Maustaste 5:"
  Pop $0
  ${NSD_CreateDropList} 42% 108u 58% 80u ""
  Pop $hTaste5
  !insertmacro TASTEN_LISTE $hTaste5
  ${NSD_CB_SelectString} $hTaste5 "Vorwärts (Alt+Rechts)"

  ${NSD_CreateCheckBox} 0 124u 100% 11u "VirtualBox mit USB 3.0 (xHCI-Schrittmodus aus)"
  Pop $hVBox
  ${If} $VBox == "1"
    ${NSD_Check} $hVBox
  ${EndIf}

  nsDialogs::Show
FunctionEnd

Function EinstellungenVerlassen
  ${NSD_GetState} $hArt1 $0
  ${NSD_GetState} $hArt2 $1
  ${NSD_GetState} $hArt3 $2
  ${NSD_GetState} $hArt4 $3
  StrCpy $Art "usbnt"
  StrCpy $Dienst "usbnt"
  ${If} $0 == ${BST_CHECKED}
    StrCpy $Art "uhci"
    StrCpy $Dienst "uhcint"
  ${ElseIf} $1 == ${BST_CHECKED}
    StrCpy $Art "ohci"
    StrCpy $Dienst "ohcint"
  ${ElseIf} $2 == ${BST_CHECKED}
    StrCpy $Art "ehci"
    StrCpy $Dienst "ehcint"
  ${ElseIf} $3 == ${BST_CHECKED}
    StrCpy $Art "xhci"
    StrCpy $Dienst "xhcint"
  ${EndIf}

  SendMessage $hSeriell ${CB_GETCURSEL} 0 0 $Seriell
  ${If} $Seriell < 0
    StrCpy $Seriell 0
  ${EndIf}

  ${NSD_GetText} $hTaste4 $0
  Push $0
  Call TasteWert
  Pop $Taste4
  ${NSD_GetText} $hTaste5 $0
  Push $0
  Call TasteWert
  Pop $Taste5

  ${NSD_GetState} $hVBox $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $VBox "1"
  ${Else}
    StrCpy $VBox "0"
  ${EndIf}
FunctionEnd

; --------------------------------------------------------------------
;  Seite "Unvertraegliche Treiber": Ausgabe von "usbinst pruefen"
;  Zeilen  FUND|<kennung>|<art>|<text>  -- Arten maus, autostart, usb,
;  ps2.  Vorgewaehlt wie in USBSETUP: alles ausser ps2.
; --------------------------------------------------------------------
Function FundeLesen
  StrCpy $FundAnzahl 0
  Delete "$PLUGINSDIR\funde.ini"
  ; Ausgabe in eine Datei (nsExec::ExecToStack schneidet bei 1 KB ab)
  nsExec::Exec '"$SYSDIR\cmd.exe" /c ""$PLUGINSDIR\USBINST.EXE" pruefen > "$PLUGINSDIR\pruefen.txt""'
  Pop $0
  FileOpen $4 "$PLUGINSDIR\pruefen.txt" r
  ${If} $4 == ""
    Return
  ${EndIf}
  ${Do}
    FileRead $4 $1
    ${If} ${Errors}
      ${ExitDo}
    ${EndIf}
    ${TrimNewLines} "$1" $1
    StrCpy $2 $1 5
    ${If} $2 == "FUND|"
    ${AndIf} $FundAnzahl < 12
      ${WordFind} "$1" "|" "+2" $5
      ${WordFind} "$1" "|" "+3" $6
      ${WordFind} "$1" "|" "+4" $7
      WriteINIStr "$PLUGINSDIR\funde.ini" "f$FundAnzahl" "kenn" "$5"
      WriteINIStr "$PLUGINSDIR\funde.ini" "f$FundAnzahl" "art"  "$6"
      WriteINIStr "$PLUGINSDIR\funde.ini" "f$FundAnzahl" "text" "$7"
      ${If} $6 == "ps2"
        WriteINIStr "$PLUGINSDIR\funde.ini" "f$FundAnzahl" "an" "0"
      ${Else}
        WriteINIStr "$PLUGINSDIR\funde.ini" "f$FundAnzahl" "an" "1"
      ${EndIf}
      IntOp $FundAnzahl $FundAnzahl + 1
    ${EndIf}
  ${Loop}
  FileClose $4
FunctionEnd

Function FundeZeigen
  Call FundeLesen
  ${If} $FundAnzahl == 0
    Abort                              ; nichts gefunden: Seite ueberspringen
  ${EndIf}

  !insertmacro MUI_HEADER_TEXT "Unverträgliche Treiber" "Angehakte Einträge werden abgeschaltet; der alte Zustand wird gesichert"
  nsDialogs::Create 1018
  Pop $0

  ${NSD_CreateLabel} 0 0 100% 28u "Diese Treiber und Programme vertragen sich nicht mit dem USB-Treiber. Beim Deinstallieren wird alles Abgeschaltete wiederhergestellt. PS/2 nur abschalten, wenn weder PS/2-Tastatur noch PS/2-Maus gebraucht wird."
  Pop $0

  StrCpy $R9 0
  StrCpy $R8 30                        ; y in Dialogeinheiten
  ${While} $R9 < $FundAnzahl
    ReadINIStr $1 "$PLUGINSDIR\funde.ini" "f$R9" "art"
    ReadINIStr $2 "$PLUGINSDIR\funde.ini" "f$R9" "text"
    ReadINIStr $3 "$PLUGINSDIR\funde.ini" "f$R9" "an"
    ${Select} $1
      ${Case} "maus"
        StrCpy $1 "Maustreiber"
      ${Case} "autostart"
        StrCpy $1 "Autostart"
      ${Case} "usb"
        StrCpy $1 "USB-Treiber"
      ${Case} "ps2"
        StrCpy $1 "PS/2"
    ${EndSelect}
    ${NSD_CreateCheckBox} 0 $R8u 100% 11u "$1: $2"
    Pop $0
    WriteINIStr "$PLUGINSDIR\funde.ini" "f$R9" "hwnd" "$0"
    ${If} $3 == "1"
      ${NSD_Check} $0
    ${EndIf}
    IntOp $R8 $R8 + 12
    IntOp $R9 $R9 + 1
  ${EndWhile}

  nsDialogs::Show
FunctionEnd

Function FundeVerlassen
  StrCpy $R9 0
  ${While} $R9 < $FundAnzahl
    ReadINIStr $0 "$PLUGINSDIR\funde.ini" "f$R9" "hwnd"
    ${NSD_GetState} $0 $1
    ${If} $1 == ${BST_CHECKED}
      WriteINIStr "$PLUGINSDIR\funde.ini" "f$R9" "an" "1"
    ${Else}
      WriteINIStr "$PLUGINSDIR\funde.ini" "f$R9" "an" "0"
    ${EndIf}
    IntOp $R9 $R9 + 1
  ${EndWhile}
FunctionEnd

; --------------------------------------------------------------------
;  Deinstallation: usbinst aus nimmt alles zurueck (Dienst, Klassenwerte,
;  Werkzeuge, Tasten, und mit "wieder" auch das Abgeschaltete).
; --------------------------------------------------------------------
Function un.onInit
  UserInfo::GetAccountType
  Pop $0
  ${If} $0 != "Admin"
    MessageBox MB_ICONSTOP|MB_OK "Bitte als Administrator anmelden."
    Abort
  ${EndIf}
FunctionEnd

Section "Uninstall"
  ${If} ${FileExists} "$INSTDIR\USBINST.EXE"
    nsExec::ExecToLog '"$INSTDIR\USBINST.EXE" aus'
    Pop $0
    ${If} $0 != "0"
      MessageBox MB_ICONEXCLAMATION|MB_OK "USBINST aus meldet einen Fehler ($0). Einzelheiten in der Liste."
    ${EndIf}
  ${EndIf}

  Delete "$INSTDIR\*.SYS"
  Delete "$INSTDIR\*.EXE"
  Delete "$INSTDIR\*.DLL"
  Delete "$INSTDIR\*.CPL"
  Delete "$INSTDIR\USBNT.INF"
  Delete "$INSTDIR\LIESMICH.TXT"
  Delete "$INSTDIR\PCI.IDS"
  Delete "$INSTDIR\CACERT.PEM"
  Delete "$INSTDIR\PCISCAN.SYS"
  RMDir  "$INSTDIR"
  Delete "$SMPROGRAMS\FreeBSD-USB\*.lnk"
  RMDir  "$SMPROGRAMS\FreeBSD-USB"
  DeleteRegKey HKLM "${UNINST}"
  SetRebootFlag true
SectionEnd
