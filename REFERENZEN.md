# Referenzen

Alles, worauf dieses Repo aufbaut: übernommener Code, gelesene Vorlagen,
Spezifikationen, Unterlagen, Rückmeldungen und Werkzeuge. Stand 2026.11.18.67.

Gliederung nach Art der Nutzung, weil davon abhängt, was für die Lizenz zählt:

1. **Übernommen** – liegt im Repo, mit Herkunft
2. **Nachgebildet** – Verhalten oder Registerfolgen aus fremdem Quelltext gelesen und
   neu geschrieben
3. **Spezifikationen und Datenblätter**
4. **Microsoft-Unterlagen** – nicht im Repo, nicht weitergebbar
5. **Rückmeldungen und Recherchen**
6. **Werkzeuge und Prüfstand**

---

## 1. Übernommen (im Repo)

| Was | Herkunft | Lizenz | Wo |
|---|---|---|---|
| FreeBSD-USB-Stapel (`sys/dev/usb`, Controller UHCI/OHCI/EHCI/xHCI, `quirk/`, `dev/hid/hid.c`) | https://github.com/freebsd/freebsd-src, Commit `139fa2c12ddc028048efe3946f2663dcfba9b122` (2026-10-07) | BSD-2-Clause | `freebsd/` – Ausschnitt und eigene Änderungen in `freebsd/HERKUNFT.txt`, Unterschied in `freebsd/NT4-AENDERUNGEN.diff` |
| `usbdevs.h` | erzeugt aus `sys/dev/usb/usbdevs` mit `sys/tools/usbdevs2h.awk` (FreeBSD, gleicher Commit) | BSD-2-Clause | `freebsd/dev/usb/` |
| PCI-Namensliste `PCI.IDS` | The PCI ID Repository, https://pci-ids.ucw.cz/v2.2/pci.ids (Stand 2026-09-25) | GPL-2.0-or-later oder BSD-3-Clause | `geraetemanager/`, Pakete |
| Zertifikatsliste `CACERT.PEM` | Mozilla-Liste über certifi, https://raw.githubusercontent.com/certifi/python-certifi/master/certifi/cacert.pem | MPL-2.0 | `pcihole/`, Pakete |
| wolfSSL 5.9.1 (TLS 1.2 für PCIHOLE) | https://github.com/wolfSSL/wolfssl, Tag `v5.9.1-stable`; als `wolfssl_nt4.a` für NT 4.0 gebaut | GPL-3.0 (oder kommerziell) | `pcihole/wolfssl_nt4.a`, `pcihole/user_settings.h` |
| DDK-Ersatzköpfe | eigene Kurzfassungen (`ctype.h`, `excpt.h`, `pshpack*.h`) | wie das Repo | `ddk-ersatz/` |

Eigener Code steht unter der GPL-3.0 (`LICENSE`).

## 2. Nachgebildet (gelesen, neu geschrieben)

### Linux (Kernel, GPL-2.0)

| Datei | Wofür | Wo hier |
|---|---|---|
| `drivers/usb/host/pci-quirks.c` | BIOS-Übergabe EHCI/xHCI, EHCI-SMI immer abschalten (`EHCI_USBLEGCTLSTS = 0`), xHCI-SMI (`XHCI_LEGACY_DISABLE_SMI`, `XHCI_LEGACY_SMI_EVENTS`) | `freebsd/dev/usb/controller/ehci_pci.c`, `xhci_pci.c` |
| `drivers/usb/host/pci-quirks.c`: `amd_chipset_sb_type_init`, `usb_amd_find_chipset_info`, `usb_amd_quirk_pll` | AMD-PLL-Abhilfe für isochrone Ströme (SB700–Bolton) | `quelle/nt/ntamdpll.c` – **nach eigener Angabe „Zeile für Zeile nach Linux“** |
| `drivers/usb/host/xhci-pci.c` | Bausteinbesonderheiten (`XHCI_TRB_OVERFETCH` VIA VL805, `NO_64BIT_SUPPORT`, Renesas-Firmware, `AMD_PLL_FIX`) | `quelle/werkzeug/xhcitest.c` (Anzeige), `quelle/nt/ntunterlage.c` |
| `drivers/usb/host/ehci-hcd.c` / `xhci.c`: `ehci_shutdown`, `xhci_shutdown` | Begründung, Controller beim Herunterfahren anzuhalten | `quelle/nt/ntusbnt.c` (Herunterfahren) |
| `drivers/input/joystick/xpad.c` | Xbox-360/One-Berichte, GIP-Startpakete (`xboxone_init_packets`), Vibration | `quelle/ober/ntujoy.c`, `quelle/xbox/ntxbox.c` |
| `sound/usb/` (`format.c`, `clock.c`, `endpoint.c`, `mixer.c`, `quirks.c`) | UAC-Versionserkennung, Rate/Lautstärke, Gerätefehler | `quelle/werkzeug/uac.c`, `usbaudio.c` |

**Hinweis zur Lizenz:** Linux steht unter GPL-2.0 *only*; dieses Repo unter GPL-3.0.
Registeradressen und Abläufe als Tatsachen sind unkritisch. Eine Datei, die ausdrücklich
„Zeile für Zeile“ nachgeschrieben ist (`ntamdpll.c`), kann als abgeleitetes Werk gelten;
GPL-2.0-only und GPL-3.0 sind nicht miteinander verträglich. Vor einer Weitergabe klären
oder `ntamdpll.c` aus der Spezifikation neu schreiben (Bauschalter `-DNTU_OHNE_AMDPLL`
lässt sie weg).

### Weitere Vorlagen

| Quelle | Wofür |
|---|---|
| FreeBSD `sys/kern/subr_bus.c` (`device_delete_child`) | Reihenfolge erst abhängen, dann Kinder löschen (Fehler bis .66) – `quelle/nt/ntnewbus.c` |
| FreeBSD `sys/dev/sound/usb/uaudio.c` | UAC1-Rate per `SET_CUR` an den Endpunkt nur bei `UA_SED_FREQ_CONTROL` |
| QEMU `hw/usb/dev-audio.c` | Prüfgerät UAC1: nur 48 kHz, Pakete genau 192 Byte |
| NT-4.0-DDK-Beispiel i8042prt (`I8042DEP.C`, Zeile 1223) | Tastatur-Klassenanbindung – `quelle/ober/ntkbdport.c` |
| ReactOS (`/fallback`-Muster) | Ausweg bei clang-cl-Abweichungen – `TREIBER-BAU-NT4.md` |
| EMU10K1-Treiber (`pawave.dll`) | Mischen, Anlaufschwelle, Lautstärke unter NT 4.0 – `TREIBER-SOUNDKARTE.md` |

## 3. Spezifikationen und Datenblätter

Nicht im Repo (Rechte bei den Herausgebern); hier nur genannt.

| Dokument | Herausgeber | Wofür |
|---|---|---|
| Universal Serial Bus Specification 2.0 (+ ECN/Errata) | USB-IF | Grundlage |
| Universal Host Controller Interface (UHCI) Design Guide 1.1 | Intel, 1996 | UHCI; USBSTS-Bits (HSE/HCPE nicht sperrbar, HCHalted kein Ereignis) |
| Open Host Controller Interface 1.0a | Compaq/Microsoft/National | OHCI |
| Enhanced Host Controller Interface 1.0 (+ Addendum) | Intel | EHCI, USBLEGSUP/USBLEGCTLSTS, CONFIGFLAG |
| eXtensible Host Controller Interface 1.2 | Intel | xHCI; HSE meldet sich über SERR, nicht über den Interrupter |
| USB Mass Storage Class – Bulk-Only Transport 1.0 | USB-IF | umass |
| SCSI SPC / MMC | INCITS T10 | CD/DVD (umass, Setup vom USB-DVD) |
| Device Class Definition for HID 1.11, HID Usage Tables | USB-IF | HID, Sondertasten (Consumer Page 0x0C) |
| USB Audio Class 1.0 und 2.0 (+ Terminal Types, Formats) | USB-IF | usbaudio |
| IPP over USB 1.0; RFC 8011 (IPP/1.1) | USB-IF; IETF | Drucker (`usbdruck`, `ipptest`) |
| PCI Local Bus 3.0 (Befehlsregister Bit 10 „Interrupt Disable“ ab 2.3) | PCI-SIG | INTx-Sperre im Abfragebetrieb und beim Anschließen |
| PCI-to-PCI Bridge Architecture 1.2 | PCI-SIG | Kopftyp 1, Busnummern 0x18–0x1A (PCISCAN) |
| PCI BIOS 2.1 | PCI-SIG | Busanzahl, wie NTDETECT/HAL sie übernehmen |
| C-Media CM108/CM108AH, TI PCM2900/2902 | Hersteller | typische UAC1-Geräte |

## 4. Microsoft-Unterlagen (nicht im Repo)

| Was | Nutzung | Im Repo |
|---|---|---|
| Windows NT 4.0 DDK | Köpfe (`ntddk-patch.py` erzeugt daraus `inc-patched`), Einfuhrbibliotheken (`einfuhrlib.py` aus `NTOSKRNL.LIB`/`HAL.LIB`), Prüfung, ob Exporte unter NT 4.0 existieren | nein – Pfad per `DDK=`; `bau-frei.sh` baut ohne DDK mit mingw-w64-Köpfen |
| Symbolpaket NT 4.0 SP6 (`german-sp6symi.exe` / `sp6symi.exe`) | HAL-Befund zum PCI-Zugriff (HALAPIC, `HalGetBusDataByOffset` → ohne Handler 0; `HalpInitializePciBus`), siehe `AENDERUNGEN.md` 2026.11.18.67 | nein; Werkzeuge `pruefstand/dbgsym.py`, `pruefstand/dis.py` |
| NT-4.0-Quellbaum (`private/ntos`, `public/sdk`) | **nur** `geraetemanager/build.sh` (Kopfdateien) und zwei Kommentarverweise: `geraetemanager/src/pciscan.c` (Verfahren aus `pcidump.c`), `geraetemanager/build.sh`/`README.md` (`mm/creasect.c`) | nein. `geraetemanager/bau-hier.sh` baut ohne ihn (DDK + `kompat/`) |

Der NT-4.0-Quellbaum ist nicht öffentlich freigegeben. Für ein öffentliches Repo:
`bau-hier.sh` ist der gültige Bauweg; die Verweise in `build.sh`, `README.md` und
`pciscan.c` beschreiben Verhalten, enthalten aber keinen Microsoft-Code.

## 5. Rückmeldungen und Recherchen

- **Ray Hinchliffe** (Autor von SIV, `SIVNT4.SYS`): zwei Rückmeldungen zu Abschnitten
  (`.text/.rdata/.data` resident, `DriverEntry` in INIT, `-MERGE:.rdata=.text`) und zu
  `PAGED_CODE()` – umgesetzt nach `TREIBER-BAU-NT4.md`.
- **Recherche USB Audio Class 1.0/2.0/3.0** (17.11.2026), Hauptquellen:
  - QEMU `hw/usb/dev-audio.c` – https://github.com/qemu/qemu/blob/master/hw/usb/dev-audio.c
  - FreeBSD `uaudio.c` – https://github.com/freebsd/freebsd-src/blob/master/sys/dev/sound/usb/uaudio.c
  - Linux `include/uapi/linux/usb/audio.h`, `sound/usb/endpoint.c`, `clock.c`
  - Microsoft: USB Audio 2.0 Drivers – https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers ;
    USB Audio Class System Driver (Usbaudio.sys) – https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-audio-class-system-driver--usbaudio-sys-
  - TI PCM2902 – https://www.ti.com/product/PCM2902 ; C-Media CM108AH-Datenblatt
  - Linux-Kernel-Mailingliste (UAC2/3-Reihenfolge Alt-Setting/Rate, Quirk-Flags),
    QEMU-Puffer-Diskussion (qemu-devel 2019-09)
- **Projektnotizen** (Projekt „kernel nt4“): `NT4-AUF-NEUER-HARDWARE.md` (USB-DVD-Setup,
  Abfragebetrieb, Busse hinter Lücken, BIOS-Übergabe), `NT4-SP6-SYMBOLE.md`,
  `TREIBER-BAU-NT4.md`.
- **Erprobte Geräte:** HP EliteDesk 705 G5 Mini (Ryzen 5 PRO 3400GE, xHCI 1022:15e0/15e1
  auf Bus 0a), HyperX Cloud Stinger Core Wireless 7.1 (0951:170b, UAC1), Xbox-Controller
  (360 und One/GIP).

## 6. Werkzeuge und Prüfstand

| Werkzeug | Fassung | Wofür |
|---|---|---|
| LLVM clang-cl, lld-link, llvm-dlltool, llvm-rc | 18.1.3 | Kerntreiber, PCISCAN |
| mingw-w64 GCC (i686) | 13 | Werkzeuge, DLLs, CPLs |
| NSIS | 3.09 | `installer-nsis/` |
| QEMU (+ SeaBIOS) | 8.2.2 | Prüfstand (`pruefstand/`), Buslücke per `pxb` |
| Wine | 9.0 | Werkzeug- und PCIHOLE-Prüfung |
| Python 3, pefile 2024.8.26, capstone 5.0.7, hivex, ntfs-3g 2022.10.3 | – | PE-Nachbearbeitung (`abschnitte.py`, `pruefsumme.py`, `pruefe-bau.py`), Symbole, Registry und Abbild im Prüfstand |
| 7-Zip | – | DDK-Abbild, Symbolpaket auspacken |
