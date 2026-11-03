# usbnt – Antwort auf die Wünsche von CUPS4NT (0.9.0)

Stand: 26.09.2026, Schnittstellenfassung **4** (`NTUP_FASSUNG`).
Die bestehende Schnittstelle bleibt **unverändert**: `\\.\USBPRNn`, Codes
`0x8C0`–`0x8C3`, `NTUP_STATUS`, `NTUP_INFO` (120 Byte), ein schreibender Griff,
gemessene Fehlercodes (21, 121, 995). Maßgeblich ist `ntuprn.h` in diesem Paket.

## Übersicht

| Wunsch | Stand |
|---|---|
| W1 IPP over USB | **neu**: `\\.\USBIPPn` |
| W2 Rückkanal | **gab es schon** (seit Fassung 3) |
| W3 Zeitgrenze je Griff | **neu**: `IOCTL_NTUP_ZEIT` |
| W4 Einstellungswahl | **gab es schon**, wie gewünscht |
| W5 usbprint.sys-Codes | **neu**, zusätzlich |
| W6 reines 1284.4 | bleibt roh (`\\.\UsbDevN`) |
| W7 weitere Schnittstellen roh | **gibt es schon**, heißt `\\.\UsbDevN` |

## W1 – IPP over USB

- Jede Schnittstelle mit einer Einstellung **07/01/04** wird ein eigenes Gerät:
  `\Device\UsbIppN`, erreichbar als **`\\.\USBIPP1`**, `\\.\USBIPP2`, … (eigene
  Nummerierung, unabhängig von `USBPRNn`). Die Einstellung 04 wird gewählt.
- **Aufteilung**, wenn eine Schnittstelle 02 **und** 04 anbietet: Die **erste**
  solche Schnittstelle des Geräts bleibt `USBPRNn` (Einstellung 02), damit jedes
  Gerät einen klassischen Druckzugang behält; alle weiteren 04-fähigen werden
  `USBIPPn`. IPP-USB-Geräte haben mindestens zwei 04-Schnittstellen, es bleibt
  also mindestens eine, meist mehrere.
- `WriteFile` → Bulk-Out, `ReadFile` → Bulk-In, reiner Bytestrom.
- `ReadFile` kehrt zurück, sobald Daten da sind; nach der Lesezeitgrenze mit
  **0 Byte, ohne Fehler**. Grenze einstellbar je Griff (W3).
- Ein schreibender Griff **je Schnittstelle** (zweiter: `ERROR_SHARING_VIOLATION`);
  Griffe ohne Schreibrecht jederzeit. Verschiedene Schnittstellen gleichzeitig: ja.
- `IOCTL_NTUP_INFO`: `Protokoll = 4`, `Schnittstelle` = bInterfaceNumber,
  `Alternative`, `Rueckkanal = 1`, Seriennummer, VID/PID, Pfad – zum Zuordnen der
  Schnittstellen eines Geräts.
- Die 1284-Kennung wird für IPP-Schnittstellen nicht abgefragt (wie gewünscht
  nicht nötig).
- Abziehen während `WriteFile`/`ReadFile`: 21, sofort, wie bei `USBPRNn`.

## W2 – Rückkanal (bestätigt)

`ReadFile` auf `\\.\USBPRNn` liest Bulk-In, wenn `NTUP_INFO.Rueckkanal = 1`
(Protokoll 02). Liefert der Drucker nichts: **0 Byte, kein Fehler**, nach der
Lesezeitgrenze (Vorgabe 2000 ms, `Parameters\LeseWartezeit`, oder je Griff über
W3). Lesen und Schreiben laufen unabhängig (eigener Faden je Richtung); `CancelIo`
wirkt (995). Geprüft mit `@PJL INFO STATUS`.

## W3 – Zeitgrenze je Griff

```c
#define IOCTL_NTUP_ZEIT   NTUP_IOCTL(4)    /* 0x00222310 */
#define NTUP_ZEIT_BLEIBT  0xFFFFFFFFul
typedef struct { unsigned long LesenMs, SchreibenMs; } NTUP_ZEIT;
```

- Gilt für **diesen Griff** (Dateiobjekt), andere Griffe unberührt.
- Eingabe `NTUP_ZEIT` setzt; `NTUP_ZEIT_BLEIBT` lässt ein Feld unverändert;
  `0` = unbegrenzt. Ohne Eingabe: nur abfragen. Die Antwort enthält immer die
  gültigen Werte.
- `SchreibenMs` gilt je 64-kB-Stück (wie bisher `SchreibWartezeit`).
- Ohne Einstellung gelten die Registry-Vorgaben wie bisher.
- Geht auf `\\.\USBPRNn` und `\\.\USBIPPn`.

## W4 – Einstellungswahl (bestätigt)

02 vor 01, nie 03 für `\\.\USBPRNn` (wie FreeBSDs `ulpt`); reine 03-Geräte
bleiben roh. `Protokoll` und `Alternative` in `NTUP_INFO` sind gefüllt (die
tatsächlich gesetzte Einstellung).

## W5 – usbprint.sys-Codes (zusätzlich)

| Code | Antwort |
|---|---|
| `IOCTL_USBPRINT_GET_LPT_STATUS` `0x00220030` | 1 Byte wie Parallelport (0x20 Papier leer, 0x10 ausgewählt, 0x08 kein Fehler) |
| `IOCTL_USBPRINT_GET_1284_ID` `0x00220034` | 2 Byte Länge (big endian, **einschließlich** dieser 2 Byte), dann die Kennung ohne Null |
| `IOCTL_USBPRINT_SOFT_RESET` `0x00220040` | Soft-Reset |

## W6 / W7

- **W6:** Geräte, die **nur** 03 anbieten, bleiben roh als `\\.\UsbDevN`. Ein
  echtes solches Gerät ist mir nicht bekannt; geprüft ist nur ein Nachbau.
- **W7:** Alle Schnittstellen, die kein eigener Treiber übernimmt (Scanner 06,
  herstellereigen FF, …), stehen schon heute als **`\\.\UsbDevN`** bereit – mit
  Steuer-, Bulk-, Interrupt- und isochronem Zugriff, Deskriptoren, Einstellungen.
  Schnittstelle und Beispiele: `ntusbgen.h`, `GERAETE.md`, `usbgeraet.c` im usbnt-Paket.
  Zuordnung über `HKLM\HARDWARE\DEVICEMAP\USB`
  (`\Device\UsbDevN = "VID_xxxx&PID_xxxx&MI_xx KK/UU/PP"`).

## Geprüft (QEMU, NT 4.0 SP6a, nachgebautes IPP-USB-Gerät)

Nachbau: Schnittstelle 0 mit 02 **und** 04, Schnittstellen 1 und 2 nur 04.

- Aufteilung: 0 → `USBPRN1`, 1 → `USBIPP1`, 2 → `USBIPP2`.
- `USBIPP1` und `USBIPP2` gleichzeitig offen: je 64 Byte HTTP-Anfrage
  geschrieben, je 103 Byte HTTP-Antwort gelesen; `NTUP_INFO`: Fassung 4,
  Protokoll 4, Schnittstelle 1 bzw. 2, Rückkanal 1.
- W3: Lesegrenze 300 ms gesetzt → 0 Byte nach 313 ms; der andere Griff behielt
  2000 ms.
- W5: `GET_1284_ID` 109 Byte, Längenfeld 109; `GET_LPT_STATUS` 0x18;
  `SOFT_RESET` ok.
- Kein Rückschritt: klassischer Drucker → `USBPRN1`, 300 kB byte-gleich,
  PJL-Abfrage über den Rückkanal beantwortet.

Nicht geprüft: echte IPP-USB-Geräte. `IPPTEST.EXE` in diesem Paket prüft W1, W3
und W5 auf einem echten Gerät (erwartet `USBIPP1`, `USBIPP2`, `USBPRN1`).
