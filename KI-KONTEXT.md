# KI-KONTEXT — FreeBSD-USB auf Windows NT 4.0

Arbeitsgrundlage für KI-Assistenten, die an diesem Projekt weiterarbeiten.
Knapp und vollständig; für Menschen gibt es `LIESMICH.md`. Maßgeblich sind
bei Widerspruch immer die Kopfdateien in `quelle/ober/*.h` und die
Kopfkommentare der Quelldateien. Sprache im Projekt: Deutsch, auch in
Bezeichnern und Kommentaren (ASCII, Umlaute als ae/oe/ue/ss).

## 1. Was es ist

Kernmodustreiber `usbnt.sys` für Windows NT 4.0 SP6a (x86). Er enthält
FreeBSDs USB-Stapel (`sys/dev/usb`, Controller UHCI/OHCI/EHCI/xHCI, HID-Parser)
**unverändert** aus `freebsd-src` main, eine Unterlage, die FreeBSD-Kern-APIs
auf NT abbildet (Vorbild: FreeBSDs LinuxKPI), und obere Treiber, die
USB-Schnittstellen als NT-Geräte bereitstellen. Dazu Win32-Programme ohne CRT
(Installer, Systemsteuerung, Werkzeuge). Schema: `ARCHITEKTUR.svg`.

## 2. Harte Regeln

1. **`freebsd/` nie ändern.** Jede Anpassung gehört in `quelle/` – als Makro,
   Kopfdatei in `quelle/sys/`, Unterlagenfunktion in `quelle/nt/` oder
   Nachbildung (z. B. `SYSCTL_INT` → Linker-Sektion `NTUSYS`,
   `DRIVER_MODULE` → `NTUDRV`). Prüfsummen: `freebsd/PRUEFSUMMEN.md5`.
2. **Nur Exporte von NT 4.0 SP6a benutzen.** Nach jedem Bau die Einfuhren
   jedes `.sys` gegen die Exporte von `ntoskrnl.exe` **und** `ntkrnlmp.exe`,
   `hal.dll` **und** `halmps.dll` prüfen; bei Programmen gegen `kernel32`,
   `user32`, `gdi32`, `advapi32`, `winmm` von NT 4.0. Fehlt ein Export,
   lädt der Treiber nicht (bzw. das Programm startet nicht). Beispiele:
   vieles aus Windows 2000 fehlt (`ExAllocatePoolWithTagPriority` kam erst mit
   SP4); `IoCopyCurrentIrpStackLocationToNext` ist ein Makro späterer DDKs und
   fehlt im NT4-DDK; `CREATE_NO_WINDOW` wurde bewusst gemieden
   (`DETACHED_PROCESS` genommen), weil die NT4-Unterstützung nicht gesichert ist.
3. **IRQL.** FreeBSD-Code läuft bei PASSIVE_LEVEL: `mtx` ist ein `KMUTEX`
   (war früher ein Spinlock → IRQL-Leck, behoben). Nie ein `mtx` bei
   DISPATCH_LEVEL oder höher nehmen; von dort mit `ExQueueWorkItem` auf
   PASSIVE wechseln (so die Tastatur-LEDs). Daten, die ein DPC oder ISR
   anfasst, schützt ein `KSPIN_LOCK` (z. B. `sc_wd_sperre` der
   Tastenwiederholung, `ntu_maus_sperre` der Maustastenbelegung).
4. **Unterbrechungen:** ISR (DIRQL) prüft nur, stellt die Quelle am Baustein
   ab und reiht einen DPC ein; der DPC setzt ein Ereignis; ein Systemfaden
   ruft bei PASSIVE den FreeBSD-Handler und gibt die Quelle frei
   (`nt/ntintr.c`). INTx, pegelgesteuert – ohne Abstellen im ISR entsteht ein
   Unterbrechungssturm, der wie ein Hänger aussieht.
5. **Mehrprozessor:** geteilte Zähler nur mit `Interlocked*` (war `+= 1`,
   behoben). Einfuhrprüfung gegen MP-Kern und -HAL (Regel 2).
6. **Win32-Programme ohne CRT**, gebaut mit mingw-w64 (`-nostdlib`, eigener
   Einstiegspunkt, eigenes `memset`). Fensterprogramme und DLLs mit
   Ressourcen **ohne `-s` binden, dann `strip`** (sonst ld: „(.rsrc) is too
   large"). Befehle: `quelle/werkzeug/bauen.md`.
7. **Nur ein Dienst der Familie** (`usbnt`, `uhcint`, `ohcint`, `ehcint`,
   `xhcint`) darf aktiv sein; zwei Treiber an einem Controller erzeugen
   Resets, `USB_ERR_TIMEOUT` und „host controller error". `USBINST ein/probe`
   entfernt die anderen.
8. **Befunde belegen, Prüfaufbau-Fehler abgrenzen.** Mehrere scheinbare
   Treiberfehler waren Fehler des Prüfaufbaus (Abschnitt 10). Vor einer
   Treiberänderung die Ursache mit Protokoll, Zählern oder
   QEMU-Ablaufprotokoll (`-trace usb_xhci_*`) nachweisen.
9. **Ausgelieferte Binärdateien = geprüfte Binärdateien.** Nach Änderungen neu
   bauen, im Prüfstand prüfen, erst dann packen; im Paket per `cmp` gegen die
   geprüfte Datei vergleichen. Ein Neubau ohne Codeänderung unterscheidet
   sich nur in PE-Zeitstempel und -Prüfsumme (bei `usbnt.sys` Offset `0x88`
   und `0xD8`).

## 3. Schichten

```
Programme (Win32, kein CRT)      USBSETUP/USBINST, usbmaus.cpl, usbjoy.cpl+dll,
                                 usbgeraet, usbdruck, joytest, usbtest, xhcitest
        | CreateFile / DeviceIoControl / ReadFile / WriteFile
NT 4.0                           kbdclass, mouclass, Dateisysteme, Spooler, winmm
        | IRP  /  ClassService-Rückruf (Tastatur, Maus)
obere Treiber  quelle/ober/      ntukbd+ntkbdport, ntums+ntmaustasten, ntumass,
                                 ntuprn, ntujoy, ntugen, ntsteuer, ntxhci*
        | usbd_transfer_*  ·  newbus probe/attach
FreeBSD-USB    freebsd/          usb_*.c, uhci/ohci/ehci/xhci(.c,_pci.c), hid.c
        | mtx, callout, bus_dma, bus_space, malloc, kthread, newbus, sysctl
Unterlage      quelle/nt, sys/   KMUTEX, ISR->DPC->Faden, KTIMER+KDPC,
                                 PsCreateSystemThread, HalAllocateCommonBuffer,
                                 HalGetBusDataByOffset+MmMapIoSpace,
                                 ExAllocatePoolWithTag+Poolwache, Linker-Sektionen
NT-Kern + HAL                    ntoskrnl/ntkrnlmp, hal/halapic/halmps
Hardware                         PCI 0C/03/{00,10,20,30}; INTx, kein MSI
```

Zuordnung Schnittstelle → oberer Treiber (newbus-Güte; `ntugen` hat
`BUS_PROBE_HOOVER`, nimmt also nur den Rest):

| Klassen-Tripel | Treiber | NT-Gerät |
|---|---|---|
| 03 HID, Tastatur (Boot-Protokoll erzwungen) | `ntukbd` | `\Device\KeyboardPortN` → kbdclass |
| 03 HID, Maus (Berichtsdeskriptor, sonst Boot) | `ntums` | `\Device\PointerPortN` → mouclass |
| 03 HID Gamepad; FF/5D/01 Xbox 360; FF/47/D0 Xbox One (GIP) | `ntujoy` | `\Device\UsbJoyN`, `\DosDevices\UsbJoyN` |
| 08 / 06, 05, 02 / 50 (Bulk-Only) | `ntumass` | `\Device\UsbPlatteN` + Laufwerksbuchstabe |
| 07 / 01 / 02 vor 01 (Dot4-only 03 → ntugen) | `ntuprn` | `\Device\UsbDruckN` = `\DosDevices\USBPRN(N+1)` |
| alles andere, je Schnittstelle | `ntugen` | `\Device\UsbDevN`, `\DosDevices\UsbDevN`, DEVICEMAP\USB |
| – (eins je Treiber) | `ntsteuer` | `\Device\UsbNt` (bzw. `UhciNt`, `OhciNt`, `EhciNt`, `XhciNt`) |

## 4. Dateikarte

`quelle/nt/` – Unterlage (NT-Seite)

| Datei | Aufgabe |
|---|---|
| `ntusbnt.c` | DriverEntry: Parameter lesen, PCI-Suchlauf (0C/03), Controller anhängen, Herzschlag (`-DNTU_HERZSCHLAG`) |
| `ntunterlage.c/.h` | malloc→Pool mit Poolwache (Kopf/Endzeichen, Zähler vergeben/offen/gemeckert), mtx→KMUTEX, callout, kthread, Hilfen |
| `ntintr.c` | ithread-Nachbau: ISR → DPC → Faden; Zähler Unterbrechungen/Fadenläufe/ArbeitVerloren |
| `ntgeraet.c` | PCI-Konfiguration, BARs (`MmMapIoSpace`, E/A-Ports), `IoConnectInterrupt`, Uhr, Fäden |
| `ntnewbus.c` | newbus-Baum; `DRIVER_MODULE`-Sammlung über Sektion `NTUDRV$a..$z` |
| `ntsysctl.c` | `SYSCTL_INT` über Sektion `NTUSYS`; setzt Werte aus `Parameters\Sysctl` |
| `ntamdpll.c` | AMD-PLL-Abhilfe (SB700 bis Rev 3B, SB800) für isochron; nie auf echtem Silizium gelaufen |

`quelle/sys/` – FreeBSD-Kopfdateien als Nachbildung (`mutex.h`, `bus.h`,
`sysctl.h`, `module.h`, `callout.h`, …).

`quelle/ober/` – obere Treiber und Schnittstellen

| Datei | Aufgabe |
|---|---|
| `ntukbd.c` | USB-Tastatur: HID→Satz-1-Codes, Wiederholung per KTIMER/DPC (Vorgabe 30/s nach 500 ms), LEDs per SET_REPORT (Arbeitsauftrag), Feststelltasten selbst umschalten |
| `ntkbdport.c` | `KeyboardPortN`/`PointerPortN`, IOCTL-Beantwortung für kbdclass/mouclass, DEVICEMAP-Einträge |
| `ntums.c` | USB-Maus: Berichtsdeskriptor (bis 8 Tasten, Rad) oder Boot; Tasten 1–8 über Belegung, Zähler für links/rechts/Mitte, Belegung beim Drücken festgehalten |
| `ntmaustasten.c` | Belegungen `Taste1..8` parsen (Tastennamen deutsch, `code:xx`, `klick-links/-rechts`, `mitte`, `doppelklick`, `aus`), lesen, zur Laufzeit setzen und speichern |
| `ntumass.c` | Massenspeicher (BOT, SCSI), Partition/Dateisystem, Laufwerksbuchstabe, Auswerfen |
| `ntuprn.c/.h` | Drucker: Einstellungswahl wie FreeBSD `ulpt`, ein Schreiber, Bulk OUT in 64-kB-Stücken, ReadFile-Rückkanal (nur Protokoll 02) |
| `ntujoy.c/.h` | Gamepads: HID, Xbox 360 (nachgebauter Deskriptor, LED), Xbox One (GIP) |
| `ntugen.c`, `ntusbgen.h` | rohe Geräte `UsbDevN`; **`ntusbgen.h` ist der Vertrag** für neue Gerätetreiber (Kern oder Benutzermodus) |
| `ntsteuer.c`, `ntusbioctl.h` | Steuergerät: Controller, Geräte, Register, Zähler, Spur, Maustasten |
| `ntxhcipcd.c` | xHCI: verpasste Anschlussänderungen nachholen |
| `ntxhcisonder.c` | xHCI: Intel-Sonderbehandlung (Schrittmodus `sc_ctlstep`, Anschlussumschaltung) melden; `Parameters\XhciSchritt` |
| `ntkbdmou.h` | gemeinsame Tastatur-/Maus-Deklarationen (NT-Strukturen ohne DDK-Kopf) |

`quelle/werkzeug/` – Win32 ohne CRT (`ntbasis.c` ersetzt die CRT; `printf` →
`WriteFile` auf die Standardausgabe, daher umleitbar)

| Datei | Programm |
|---|---|
| `usbinst.c` | `USBINST.EXE`: `probe`/`ein`/`aus` [Bauform] `-seriell 0/1/2` `-neustart`; `taste [1..8 belegung]` (wirkt sofort, wenn der Treiber läuft); ohne Befehl: Zustand. Kopiert Treiber, usbjoy, usbmaus.cpl, legt `\\.\USBPRN1..4` als Anschlüsse an |
| `usbsetup.c` | `USBSETUP.EXE`: Fenster vor USBINST (Pipe, Arbeitsfaden, `DETACHED_PROCESS`), setzt `XhciSchritt` |
| `usbmaus.c` | `usbmaus.cpl`: Tasten 1–8 anzeigen (Abfrage 30 ms) und belegen |
| `usbjoy.c`, `usbjoycpl.c` | winmm-Joystick-Treiber `usbjoy.dll`, Systemsteuerung `usbjoy.cpl` |
| `usbgeraet.c` | alle `UsbDevN`: INFO, DESKRIPTOR, LESEN, SCHREIBEN, ISO, ALT, STALL, `auswerfen X:`, `lesetest` |
| `usbdruck.c` | Drucker: Kennung, Status, Info, `schreibe`, `storno`, `pjl`, `lies`, `lesestorno` |
| `joytest.c`, `eingabetest.c`, `usbtest.c`, `xhcitest.c` | Prüfwerkzeuge |

## 5. Steuercodes

Alle `METHOD_BUFFERED`, Gerätetyp `0x22`. Code = `(0x22 << 16) | (Funktion << 2)`.

| Name | Funktion | Code |
|---|---|---|
| `IOCTL_NTU_HOSTBAUSTEINE` | `0x800` | `0x00222000` |
| `IOCTL_NTU_GERAETE` | `0x801` | `0x00222004` |
| `IOCTL_NTU_REGISTER` | `0x802` | `0x00222008` |
| `IOCTL_NTU_ZAEHLER` | `0x803` | `0x0022200C` |
| `IOCTL_NTU_VERSION` | `0x804` | `0x00222010` |
| `IOCTL_NTU_SPUR` | `0x805` | `0x00222014` |
| `IOCTL_NTU_MAUS_ZUSTAND` | `0x806` | `0x00222018` |
| `IOCTL_NTU_MAUS_HOLEN` | `0x807` | `0x0022201C` |
| `IOCTL_NTU_MAUS_SETZEN` | `0x808` | `0x00222020` |
| `IOCTL_NTUG_INFO` | `0x900` | `0x00222400` |
| `IOCTL_NTUG_DESKRIPTOR` | `0x901` | `0x00222404` |
| `IOCTL_NTUG_STEUER` | `0x902` | `0x00222408` |
| `IOCTL_NTUG_LESEN` | `0x903` | `0x0022240C` |
| `IOCTL_NTUG_SCHREIBEN` | `0x904` | `0x00222410` |
| `IOCTL_NTUG_ALTERNATIVE` | `0x905` | `0x00222414` |
| `IOCTL_NTUG_STALL_WEG` | `0x906` | `0x00222418` |
| `IOCTL_NTUG_ISO` | `0x907` | `0x0022241C` |
| `IOCTL_NTUJ_INFO` | `0x8B0` | `0x002222C0` |
| `IOCTL_NTUJ_ZUSTAND` | `0x8B1` | `0x002222C4` |
| `IOCTL_NTUP_KENNUNG` | `0x8C0` | `0x00222300` |
| `IOCTL_NTUP_STATUS` | `0x8C1` | `0x00222304` |
| `IOCTL_NTUP_RESET` | `0x8C2` | `0x00222308` |
| `IOCTL_NTUP_INFO` | `0x8C3` | `0x0022230C` |

Wohin: `IOCTL_NTU_*` an das Steuergerät (`\\.\UsbNt` usw., erstes, das sich
öffnen lässt); `IOCTL_NTUG_*` an `\\.\UsbDevN`; `IOCTL_NTUJ_*` an
`\\.\UsbJoyN`; `IOCTL_NTUP_*` an `\\.\USBPRNn`.

Strukturen: `ntusbioctl.h` (`NTU_ZAEHLER`, `NTU_MAUS_ZUSTAND`,
`NTU_MAUS_BELEGUNG`), `ntusbgen.h` (`NTUG_*`; jede Antwort beginnt mit
`NTUG_ERGEBNIS`, ein USB-Fehler steht in `.Fehler`, der Aufruf selbst gelingt;
höchstens `NTUG_MAX_DATEN` = 65536 je Aufruf; Abziehen → `NTUG_FEHLER_WEG`,
Griff bleibt gültig), `ntujoy.h`, `ntuprn.h` (`NTUP_INFO`, 120 Byte,
`NTUP_FASSUNG` 3).

Drucker, Win32-Rückgaben (gemessen): Zeitüberschreitung beim Schreiben 121
(angenommene Byte in `lpNumberOfBytesWritten`), `CancelIo` 995, Abziehen 21,
kein Drucker bei `CreateFile` 2, zweiter Schreiber 32, `ReadFile` ohne Daten
→ 0 Byte ohne Fehler nach `LeseWartezeit`. Nur ein Griff mit `GENERIC_WRITE`;
Griffe ohne Schreibzugriff (`0`, `GENERIC_READ`) immer.

## 6. Registry

`HKLM\SYSTEM\CurrentControlSet\Services\<dienst>` – Start 1, Gruppe
**„Keyboard Port"** (lädt vor kbdclass; ohne Gruppe bleibt die USB-Tastatur
tot). kbdclass/mouclass brauchen `Parameters\ConnectMultiplePorts = 1`
(sonst landet die USB-Tastatur auf `KeyboardClass1`, die USER nie liest).

`…\<dienst>\Parameters`:

| Wert | Typ | Bedeutung |
|---|---|---|
| `Ausgabe` | DWORD | Diagnoseausgabe 0 aus, 1 COM1, 2 COM2 |
| `Taste1` … `Taste8` | SZ | Maustastenbelegung (Vorgabe: klick-links, klick-rechts, mitte, alt+links, alt+rechts, aus, aus, aus) |
| `SchreibWartezeit` | DWORD | Drucker, Sekunden je 64-kB-Stück, 0 = unbegrenzt, Vorgabe 30 |
| `LeseWartezeit` | DWORD | Drucker-Rückkanal, ms, 0 = unbegrenzt, Vorgabe 2000 |
| `XhciSchritt` | DWORD | 0 aus / 1 ein: Intel-Schrittmodus; fehlt = FreeBSD-Vorgabe |
| `Sysctl` | SZ | FreeBSD-Schalter, `hw.usb.xhci.polling=1;…` (jeder wird auf COM1 gemeldet) |

`HKLM\HARDWARE\DEVICEMAP\USB`: `\Device\UsbDevN = "VID_xxxx&PID_xxxx&MI_xx KK/UU/PP …"`.

## 7. Datenflüsse (Kurzform)

- **Tastatur:** Interrupt-IN-Bericht (8 Byte Boot) → Differenz zum vorigen →
  Satz-1-Codes → `ClassService` von kbdclass bei DISPATCH (unter
  `sc_wd_sperre`). Wiederholung: `KeSetTimerEx`-DPC. LEDs:
  `IOCTL_KEYBOARD_SET_INDICATORS` → Arbeitsauftrag → SET_REPORT (Rückruf prüft
  nach **jeder** fertigen Übertragung, ob ein neuer Stand ansteht).
- **Maus:** Bericht → Tasten 1–8 → je Wechsel die beim Drücken
  festgehaltene Belegung (Tasten einspeisen / Zähler links-rechts-Mitte /
  Doppelklick) → `MOUSE_INPUT_DATA` an mouclass; Rohzustand in `ntu_maus_roh`.
- **Speicher:** SCSI über BOT → Blockgerät → Laufwerksbuchstabe.
- **Drucker:** Spooler → `\\.\USBPRNn` → `IRP_MJ_WRITE` → Bulk OUT;
  `IRP_MJ_READ` → eigener Faden, Bulk IN.
- **Roh:** Programm → `\\.\UsbDevN` → `IOCTL_NTUG_*` → `usbd_transfer`.
- **Unterbrechung:** siehe Regel 4.

## 8. Bauen

```
DDK=/pfad/ddk ./bau-clangcl.sh        # clang-cl + lld-link, /subsystem:native,4.00 /driver
NUR=uhci|ohci|ehci|xhci               # Einzelbauform (uhcint.sys …)
EXTRA=-DNTU_EINGABE_PROTOKOLL         # Diagnosebau: jeder Tastatur-/Mausbericht auf COM1
HERZ=1                                # Herzschlag: alle 5 s Zähler, PORTSC, hängende Fäden
```

Das NT-4.0-DDK ist nicht im Paket. Vorbereitung einmal:
`ddk-ersatz/ntddk-patch.py <ddk>/inc <ddk>/inc-patched` – kopiert `inc` und
ändert genau eine Stelle (Inline-Assembler in `KeGetCurrentProcessorNumber`,
den clang-cl nicht nimmt); `ddk-ersatz/` liefert die wenigen fehlenden
Kopfdateien. `einfuhrlib.py` erzeugt die Einfuhrbibliotheken (die des DDK kann
lld-link nicht lesen). `pruefsumme.py` **setzt** die PE-Prüfsumme: lld-link
lässt sie ohne `/release` auf Null, und NT 4.0 lehnt einen Kerntreiber dann
wortlos mit `0xC0000221` (STATUS_IMAGE_CHECKSUM_MISMATCH) ab. Am Ende des Baus
muss „im Kopf … gerechnet … stimmt" erscheinen.
Programme: `quelle/werkzeug/bauen.md`.

## 9. Prüfstand (`pruefstand/`, QEMU)

Nicht im Paket: das NT-4.0-Abbild und Microsofts `ntkrnlmp.exe`/`halmps.dll`
(aus `sp6i386.exe` des Anwenders, nach `mp/` legen).

- `start.sh` startet QEMU (`-M pc -cpu pentium3 -m 128`, COM1 → `com1.log`,
  COM2 → Agent, HMP-Monitor `/tmp/mon.sock`, QMP `/tmp/qmp.sock`).
  `geraete_*.sh` hängen Controller und Geräte an (`geraete_direkt.sh`: NEC-xHCI
  mit freien Anschlüssen; `geraete_ss.sh`: Stick auf der USB-3-Seite;
  `geraete_mp.sh`: `-smp 4,sockets=4,cores=1,threads=1`; `geraete_ps2.sh`: ohne
  USB-Tastatur, zum Bedienen ohne Treiber).
- `einbau.py <treiber.sys|keiner> 1` baut die Arbeitskopie. Umgebung:
  `NT4_BASIS=neu` (Abbild des Anwenders), `MP=1` (ntkrnlmp/halmps +
  unveränderte NTDLL/KERNEL32/WIN32K + boot.ini-Eintrag), `DATEIEN="wirt=gast;…"`
  (Dateien direkt ins Abbild), `SYSCTL=…`, `XHCISCHRITT=0|1`. Der Dienst heißt
  wie die Datei; andere Dienste der Familie werden abgeschaltet.
- `agent.py` spricht mit `ntagent` im Gast über COM2: `PING`, `put`, `get`,
  `BG` (Hintergrund), `IA` (auf dem sichtbaren Desktop). **Eine Verbindung zur
  Zeit**; ein laufendes `IA` blockiert den Agenten bis zum Programmende.
- usbredir-Nachbauten (64-Bit-Kennungen, warten auf QEMUs HELLO):
  `udruck.py` (HP LaserJet 1200; `--dot4`, `--nurdot4`, `--hs`), `xpad.py`
  (Xbox 360; `--one`), `lsgeraet.py` (Low-Speed-Maus mit 8 Tasten / `--tastatur`
  mit LEDs). Steuerung jeweils über `/tmp/*_ctl.sock`.
- `flut.py <s> [maus]`: Eingabeflut über QMP. `taste.py`, `bild.sh`: Tasten,
  Bildschirmfoto.

## 10. Fallen (mit Ursache)

| Beobachtung | Ursache | Nicht verwechseln mit |
|---|---|---|
| Resets, `USB_ERR_TIMEOUT`, „host controller error" | zwei Dienste der Familie aktiv | Controllerfehler |
| xHCI-Einrichtung hängt mit `ctlstep` in QEMU | QEMU bündelt SETUP..STATUS (`hw/usb/hcd-xhci.c`, „hack to bundle") | Fehler im Schrittmodus |
| MP-Kern sieht 1 Prozessor | `-smp 4` = ein Sockel; SeaBIOS: ein MPS-Eintrag je Sockel | HAL-Fehler |
| STOP c0000249 mit MP-Kern | UP-Installation hat LOCK-Präfixe in NTDLL/KERNEL32/WIN32K entfernt | Treiberfehler |
| Agent stumm unter MP | große COM2-Übertragungen hängen unter halmps in QEMU (64 kB ja, 1 MB nie) | USB-Hänger |
| Wiederholrate unter MP kleiner | Gastuhr läuft unter QEMU-MP ~⅓ langsamer | verlorene Tasten (Zähler geliefert = angekommen) |
| LEDs / Wiederholrate kommen nicht an | kbdclass leitet an `PortDeviceObjectList[UnitId]`, UnitId 0 = i8042prt | fehlendes IOCTL |
| „kein Dateisystem" auf `stick.img` | Abbild durch Rohschreibtest überschrieben | SuperSpeed-Fehler |
| Wiederanstecken scheitert | `device_del` entfernt auch das Laufwerk; vorher `drive_add` | Treiber |
| QMP-Anfrage hängt | QMP nimmt nur einen Teilnehmer | – |
| keine Autoanmeldung nach Neustart aus dem Gast | Prüfaufbau | – |

## 11. Nachgewiesen (QEMU, NT 4.0 SP6a)

UHCI/OHCI/EHCI/xHCI; Low/Full/High/SuperSpeed (Stick 5 Gbit/s, Datei im Abbild
byte-gleich); Tastatur (Wiederholung, LEDs), Maus (Tasten 1–8, Belegung
sofort, Umbelegen unter gedrückter Taste), Speicher, Drucker (Rückkanal, Dot4,
High Speed, Codes oben), Gamepads; isochron OUT bitgenau an UHCI/OHCI/xHCI;
Mehrprozessor (2 CPUs): Last, Abziehen unter Last, Drucker+Gamepad, kein Leck
(offene Vergaben vorher = nachher), `unt = faden`, `verl 0`; grafischer
Installer von der Einrichtung bis zum Neustart.

## 12. Offen

Echte Hardware (echte Drucker, NEC-Karte, SB7x0 mit AMD-PLL-Abhilfe, echte
Xbox-One-Controller, HID-Gamepads, Mäuse mit Berichtsnummer oder 16-Bit-Weg);
VirtualBox „3.0"-Hänger (Hypothese Schrittmodus; Prüfung mit `XhciSchritt=0`
beim Anwender); mehr als 2 Prozessoren; DirectInput; USB-Audio (Wave-Treiber
für winmm fehlt); Scanner (herstellereigen, Weg über `UsbDevN`); Renesas
µPD720201/202 ohne EEPROM (Firmware-Laden fehlt); Anzeigefehler: nach
`USBINST taste N vorgabe` steht der Wert ohne „(Vorgabe)".
Vollständig: `PRUEFLISTE.md`; Verlauf und Begründungen: `AENDERUNGEN.md`.

## Nachtrag: bekannte Falle

| USB-Maus: Klicks ~3 s verzoegert, gebuendelt; Bewegung, USB-Tastatur und PS/2-Maus normal | Logitech-Maussoftware (MouseWare + Filtertreiber) aktiv -- abschalten. Treiber lieferte nachweislich sofort (MAUSDATEN); fuenf Uebergabewege (MausWeg 0-4) aenderten nichts |

## Nachtrag 2026-09-27

- Werkzeuge IMMER mit werkzeug/bau-werkzeuge.sh bauen (alle 19), Treiber
  mit bau-clangcl.sh (FASSUNG=..., NUR=uhci|ohci|ehci|xhci).
- Kernvariablen (KeTickCount, KeNumberProcessors) nur ueber __imp_-Zeiger;
  das DDK-Makro KeQueryTickCount bindet eine Funktionsbruecke -> Bluescreen.
- Benanntes Ereignis nicht in DriverEntry anlegen: \BaseNamedObjects gibt
  es dort noch nicht -> beim ersten IOCTL.
- Ohne CRT: Stapelvariablen unter 4 KB (sonst __chkstk_ms).
- Pruefstand: MPS im Nachbau muss zum Deskriptor passen, sonst verwirft
  QEMU Berichte.  pkill -f nie mit einem Muster, das in der eigenen
  Befehlszeile steht.


## Gamepads: eigener Treiber USBXBOX.SYS (ab 2026.10.8.1)

ober/ntujoy.c ist nicht mehr Teil von usbnt.sys, sondern steckt mit
xbox/ntxbox.c in USBXBOX.SYS.  Der importiert Stapelfunktionen aus
usbnt.sys (Exportliste baut bau-clangcl.sh automatisch) und meldet sich
mit ntu_treiber_anmelden (nt/ntzusatz.c) an; der Stapel zaehlt dann
Geraete neu auf, an denen ntugen haengt.  Weitere Zusatztreiber koennen
genauso gebaut werden.  Eigenes Paket: installer-xbox (XBOXINST).

## USB-Audio: USBAUDIO.SYS (ab 2026.10.9.1)

Zweiter Zusatztreiber nach dem Muster von USBXBOX: ober/ntuaudio.c +
audio/ntaudio.c.  Uebernimmt AudioStreaming-Schnittstellen, Ring im Kern,
zwei ISO-Uebertragungen im Wechsel.  Schnittstelle: ober/ntuaudio.h
(\\.\UsbAudioN).  Steuerung (Lautstaerke) weiter ueber ntugen an der
AudioControl-Schnittstelle.  Werkzeug: werkzeug/usbton.c.  Warum nicht
ntugen fuer den Strom: dort ist jeder ISO-Aufruf synchron (2-3 ms Luecke).

Benutzermodus-Teil: werkzeug/uac.c (Geraeteauswertung), usbaudio.c
(winmm-Wave-Treiber usbaudio.dll), audioinst.c (Installer; prueft
Importe von usbaudio.sys gegen Exporte von usbnt.sys).  Paket:
installer-audio.
