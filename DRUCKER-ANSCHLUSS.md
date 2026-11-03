# USB-Drucker unter NT 4.0 — der Anschluss (Fassung 3)

Übergabe an das Drucksystem (CUPS4NT). Fassung 2 beantwortete
`USB-DRUCKEN-RUECKMELDUNG.md` Punkt für Punkt (Abschnitt „Antwort auf die
Rückmeldung“ am Ende). **Fassung 3** bringt den Rückkanal (`ReadFile`, 4.7)
und prüft Dot4 und High Speed nach. Beschrieben ist nur die **Anschluss-Seite**: was
`usbnt.sys` (und die Einzelbauformen) bereitstellt und wie es sich verhält.

Alles aus Fassung 1 und 2 gilt weiter: **Namen, Steuercodes 0–3,
`NTUP_STATUS` und das Layout von `NTUP_INFO` sind unverändert.** Neu ist nur
Hinzugefügtes (in `NTUP_INFO` wurde ein freies Byte zu `Rueckkanal`).

## Das Gerät

Jeder USB-Drucker der Klasse 07, Unterklasse 01, wird beim Anstecken zu

```
\Device\UsbDruckN        FILE_DEVICE_PRINTER
\DosDevices\USBPRNn      n = N + 1  (USBPRN1 … USBPRN8)
```

Beim Abziehen verschwinden Name und Gerät; beim Wiederanstecken ist es die
niedrigste freie Nummer.

**Einstellung:** Wie FreeBSDs `ulpt` wird unter den alternativen
Einstellungen der Schnittstelle die mit **Protokoll 02** (beide Richtungen)
gewählt, sonst die mit **01**. Geräte, die nur **03** („Dot4“, IEEE 1284.4)
anbieten, werden nicht genommen und bleiben roh als `\\.\UsbDevN`. Haben sie
zusätzlich eine 02-Einstellung, werden sie über diese zu `USBPRNn` (neu in
Fassung 2). `COM1` meldet dann zusätzlich `uprn:   Einstellung k, Protokoll 02`.

## Öffnen

| Zugriff | erlaubt |
|---|---|
| **mit Schreibzugriff** (`GENERIC_WRITE`, `FILE_WRITE_DATA`) | **genau ein** Griff zur Zeit. Ein zweiter scheitert mit **32** `ERROR_SHARING_VIOLATION`. Frei wird der Platz, wenn der Schreiber seinen Griff schließt |
| ohne Schreibzugriff (`0`, `GENERIC_READ`) | immer, beliebig viele — auch während eines Drucks |

Alle Steuercodes gehen mit einem Griff **ohne** Schreibzugriff (sie sind
`FILE_ANY_ACCESS`). Für Kennung, Status und Info bitte so öffnen, damit der
Schreiberplatz dem Druck bleibt.

Fehlt der Drucker, scheitert `CreateFile` mit **2** `ERROR_FILE_NOT_FOUND`
(der Name existiert dann nicht).

## Schreiben = drucken

`WriteFile` geht unverändert per Bulk an den Drucker, in Stücken bis 64 kB,
in Ankunftsfolge. Die Daten werden nicht umgesetzt.

**Gemessene Win32-Codes** (`GetLastError` nach `WriteFile`), QEMU 8.2.2,
NT 4.0 SP6a, Nachbau direkt am NEC-xHCI:

| Lage | `GetLastError` | `lpNumberOfBytesWritten` |
|---|---|---|
| normal | — | alles |
| Drucker nimmt nichts an (Papier leer, beschäftigt) | — wartet; geht es weiter, läuft es vollständig durch | alles |
| länger als die Wartezeit je 64-kB-Stück nichts angenommen | **121** `ERROR_SEM_TIMEOUT` | die **tatsächlich angenommenen** Byte dieses Aufrufs (gemessen: beim stehenden Stück 0 von 65536; alles davor war angekommen) |
| `CancelIo` | **995** `ERROR_OPERATION_ABORTED` | die bis dahin angenommenen (gemessen: 0 von 1 MB) |
| Drucker abgezogen, während `WriteFile` wartet | **21** `ERROR_NOT_READY`, sofort | die bis dahin angenommenen |
| `CloseHandle` des Schreibers, während eine Anfrage wartet | 995 — *nicht eigens gemessen*; derselbe NT-Status (`STATUS_CANCELLED`) wie bei `CancelIo` | — |

Hinweis: `copy` meldet bei der Zeitüberschreitung „Das System kann nicht auf
das angegebene Gerät schreiben“ — das ist `copy`s eigene Übersetzung; der
Code selbst ist 121.

**Schließen eines Griffs** bricht nur die Anfragen **dieses** Griffs ab
(`IRP_MJ_CLEANUP` je Dateiobjekt). Ein Statusprogramm, das kurz nachsieht und
schließt, stört einen laufenden Druck nicht.

## Lesen = Rückkanal (neu in Fassung 3)

Bei Druckern mit **Protokoll 02** liest `ReadFile` vom Bulk-IN-Endpunkt —
PJL-Antworten (`@PJL INFO STATUS`, `@PJL USTATUS …`), Füllstände, was der
Drucker eben sagt. Ob es geht, sagt `NTUP_INFO.Rueckkanal` (ab Fassung 3).

- **Ein `ReadFile` kehrt zurück, sobald etwas da ist** (bis 4 kB je Aufruf);
  es füllt den Puffer nicht auf. Für eine vollständige Antwort also lesen,
  bis ein Aufruf 0 Byte liefert.
- **Kommt innerhalb der Lesewartezeit nichts, kehrt es mit 0 Byte und
  Erfolg zurück** — kein Fehler, es hängt nie. Vorgabe **2 s**:

  ```
  HKLM\SYSTEM\CurrentControlSet\Services\usbnt\Parameters
      LeseWartezeit   REG_DWORD   Millisekunden, 0 = unbegrenzt
  ```
- **Eigener Faden:** Lesen und Schreiben laufen unabhängig. Ein Statusabfrager,
  der auf Antwort wartet, hält keinen Druck auf; und während ein Druck steht,
  wird gelesen.
- **Zugriff:** Lesen geht mit einem Griff **ohne** Schreibzugriff
  (`GENERIC_READ`) — neben dem Druck. Wer eine PJL-Anfrage schicken und die
  Antwort lesen will, braucht den Schreiberplatz (`GENERIC_READ |
  GENERIC_WRITE`), wenn gerade niemand druckt; während eines Drucks kommen
  unaufgeforderte Meldungen (`@PJL USTATUS`) trotzdem über einen reinen
  Lesegriff an.
- Mehrere gleichzeitige Leser teilen sich die Antworten (wer zuerst liest,
  bekommt sie) — ein Leser je Drucker empfohlen.
- Bei **Protokoll 01** (kein Endpunkt herein) scheitert `ReadFile` mit **1**
  `ERROR_INVALID_FUNCTION`.

| Lage (gemessen) | Ergebnis |
|---|---|
| Drucker hat nichts zu sagen | nach ~2 s **0 Byte, Erfolg** |
| `@PJL INFO STATUS` geschickt | Antwort vollständig (64 Byte), dann ein Aufruf mit 0 Byte |
| `CancelIo` auf ein wartendes `ReadFile` | **995** `ERROR_OPERATION_ABORTED` |
| Drucker abgezogen, während `ReadFile` wartet | **21** `ERROR_NOT_READY`, sofort |
| `ReadFile`, während ein Druck steht | sofort bedient; der Druck läuft danach vollständig durch |

## Wartezeit

```
HKLM\SYSTEM\CurrentControlSet\Services\usbnt\Parameters
    SchreibWartezeit   REG_DWORD   Sekunden je 64-kB-Stück, 0 = unbegrenzt
```

Vorgabe ohne Eintrag: **30**. Gelesen beim Laden des Treibers (Neustart).
`COM1` meldet `uprn: SchreibWartezeit = 0 s (unbegrenzt)`, wenn gesetzt;
`IOCTL_NTUP_INFO` liefert den geltenden Wert. Bei einer Einzelbauform steht
statt `usbnt` deren Dienstname (`uhcint`, `ohcint`, `ehcint`, `xhcint`).

Bei „unbegrenzt“ wartet ein Druck, solange der Drucker angesteckt ist.
Herauskommen kann man mit `CancelIo`, `CloseHandle` oder durch Abziehen.

## Steuercodes (`ober/ntuprn.h`)

```c
#define NTUP_IOCTL(n)  (((0x00000022) << 16) | ((0x8C0 + (n)) << 2))
#define IOCTL_NTUP_KENNUNG  NTUP_IOCTL(0)   /* char[]: IEEE-1284-Kennung, nullterminiert */
#define IOCTL_NTUP_STATUS   NTUP_IOCTL(1)   /* NTUP_STATUS (unveraendert) */
#define IOCTL_NTUP_RESET    NTUP_IOCTL(2)   /* SOFT_RESET */
#define IOCTL_NTUP_INFO     NTUP_IOCTL(3)   /* NTUP_INFO -- neu in Fassung 2 */
#define NTUP_FASSUNG        3   /* 3: ReadFile-Rueckkanal */

typedef struct _NTUP_INFO {
    unsigned long  Fassung;          /* NTUP_FASSUNG */
    unsigned short Vid, Pid;
    unsigned short BcdDevice;
    unsigned char  Protokoll;        /* genutzte Einstellung: 1 oder 2 */
    unsigned char  Schnittstelle;    /* bInterfaceNumber */
    unsigned char  Alternative;      /* bAlternateSetting */
    unsigned char  Rueckkanal;       /* 1 = ReadFile geht (ab Fassung 3) */
    unsigned char  Frei[2];
    unsigned long  Aenderungen;      /* zaehlt jedes An-/Abstecken eines Druckers */
    unsigned long  Wartezeit;        /* Sekunden je 64-kB-Stueck, 0 = unbegrenzt */
    char           Seriennummer[64]; /* iSerialNumber, "" wenn keine */
    char           Pfad[32];         /* "Controller-Anschluss.Hub-Anschluss..." */
} NTUP_INFO;                         /* 120 Byte */
```

- **Versionsprüfung:** Liefert `IOCTL_NTUP_INFO` Erfolg, gibt es mindestens
  Fassung 2; `Fassung >= 3` heißt: `Rueckkanal` ist gültig. Bei einem älteren Treiber scheitert es mit **1**
  `ERROR_INVALID_FUNCTION`.
- **Seriennummer** kommt aus dem USB-String-Deskriptor, nicht aus der
  IEEE-1284-Kennung — damit sind zwei gleiche Modelle unterscheidbar, sofern
  der Drucker eine hat.
- **Pfad** ist `Bus-Anschluss[.Hub-Anschluss…]`, z. B. `0-6` oder `0-1.4`. Er
  bleibt gleich, solange der Drucker an derselben Buchse steckt — Ersatz, wenn
  es keine Seriennummer gibt. Beim NEC-xHCI zählen erst die USB-3-, dann die
  USB-2-Anschlüsse; QEMUs „Port 2“ ist dort Anschluss 6.
- **Änderungszähler** zählt über alle Drucker; ändert er sich zwischen zwei
  Abfragen, lohnt es, USBPRN1–9 neu durchzusehen.

## Lokale Anschlüsse für den Spooler

`USBINST ein` legt unter

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Ports
    "\\.\USBPRN1" … "\\.\USBPRN4" = ""   (REG_SZ)
```

die an, die **fehlen**, und startet danach den Spooler neu (nur wenn er lief
und nur wenn etwas neu angelegt wurde).

**Zuständigkeit:** USBINST merkt sich, welche es selbst angelegt hat
(`HKLM\SOFTWARE\UsbNt\Sicherung\Anschluesse`, z. B. `1234`). `USBINST aus`
entfernt **nur diese**, und nur, wenn kein Drucker sie benutzt (geprüft über
`…\Control\Print\Printers\*\Port`, auch Listen wie `LPT1:,\\.\USBPRN2`, ohne
Rücksicht auf Groß-/Kleinschreibung). Benutzte bleiben stehen und bleiben
vorgemerkt. Legt CUPS4NT sie zuerst an, fasst USBINST sie nie an. Wir
schlagen vor, dass das CUPS4NT-Setup nach derselben Regel verfährt.

## Werkzeug

```
usbdruck                     alle USBPRNn: Kennung, Info, Status  (ohne Schreibzugriff)
usbdruck reset n             SOFT_RESET
usbdruck schreibe n datei    schreiben; bei Fehler GetLastError und Byte
usbdruck storno n datei ms   ueberlappt schreiben, nach ms CancelIo
usbdruck lies n              Rueckkanal lesen, bis ein Aufruf 0 Byte liefert
usbdruck pjl n               @PJL INFO STATUS schicken, Antwort lesen
usbdruck lesestorno n ms     ueberlappt lesen, nach ms CancelIo
copy /b datei \\.\USBPRN1    roh drucken, ohne Spooler
```

## Prüfen ohne echten Drucker

`agent/udruck.py` bildet einen USB-Drucker nach (HP LaserJet 1200, 03F0:0517,
07/01/02, Seriennummer `00CNBB012345`), `agent/xpad.py` einen Xbox-Controller.
QEMU sieht beide über usbredir als echte Geräte. Beide bieten jetzt
`ep_info_max_packet_size`, `32bits_bulk_length` und **`64bits_ids`** an und
laufen damit **direkt am xHCI**, wo QEMU 8.2.2 das verlangt.

```
QEMU:    -device nec-usb-xhci,id=xhci,msi=off,msix=off,p2=8,p3=4
         (msi=off,msix=off ist fuer NT 4.0 noetig)
python3 udruck.py
Monitor: chardev-add socket,id=druck,path=/tmp/udruck.sock
         device_add usb-redir,chardev=druck,id=rdruck,bus=xhci.0
```

Fertig als `agent/geraete_direkt.sh` (xHCI mit freien Anschlüssen). Alternativ
UHCI oder EHCI (`-device piix3-usb-uhci,id=uhci`, dann `bus=uhci.0`).

Steuerung über `/tmp/udruck_ctl.sock`: `papier 0|1`, `halt 1|0`,
`zaehler`, `leeren`, `antwort <text>` (für den Rückkanal; `\r \n \f`
gehen). Empfangenes landet in `/tmp/udruck.bin`. Auf `@PJL INFO STATUS`
antwortet der Nachbau selbst wie ein HP-Drucker (`CODE=10001`, ohne Papier
`41000`).

Betriebsarten: `--dot4` (Einstellung 0 = Protokoll 03, 1 = 02),
`--nurdot4` (nur 03), `--hs` (High Speed, 512-Byte-Endpunkte).

## Nachgewiesen (Fassung 2, QEMU 8.2.2, NT 4.0 SP6a, direkt am NEC-xHCI)

- Nachbau direkt am xHCI angenommen (vorher: „lacks capabilities“),
  64-Bit-Kennungen ausgehandelt; Drucker und Xbox-Controller im Gast erkannt.
- `IOCTL_NTUP_INFO`: 03F0:0517, bcdDevice 0100, Seriennummer, Pfad `0-6`,
  Protokoll 02, Wartezeit, Fassung 2.
- Zweiter Schreiber → 32; Statusabfrage während des Drucks geht; erster Druck
  byte-gleich; danach ist der Platz frei.
- Win32-Codes 121, 995, 21 und 2 wie in der Tabelle.
- `SchreibWartezeit = 0`: 45 s Stillstand überstanden, byte-gleich; `CancelIo`
  nach 40 s → 995; danach normal.
- `USBINST ein`: 4 Anschlüsse angelegt, ein zweites `ein` legt nichts doppelt
  an; `aus` mit einem Drucker auf `LPT1:,\\.\usbprn2`: USBPRN2 bleibt, 1/3/4
  weg; ohne ihn ist auch USBPRN2 weg. Nach jedem Spooler-Neustart läuft der
  Spooler.
- Freigabebau: Info, einzelner Schreiber, 1 MB byte-gleich.

## Nachgewiesen (Fassung 3)

- **Dot4 mit 02-Einstellung** (`--dot4`): der Treiber schaltet per
  SET_INTERFACE auf Einstellung 1 um; `COM1` und `NTUP_INFO` zeigen es;
  1 MB byte-gleich; Rückkanal und PJL gehen (auch im Freigabebau).
- **Nur Dot4** (`--nurdot4`): kein `USBPRNn`, das Gerät bleibt roh
  `UsbDev0` (07/01/03).
- **High Speed** (`--hs`, 480 Mbit/s, 512-Byte-Endpunkte): 1 MB byte-gleich.
- **Rückkanal:** Tabelle oben.

## Nicht nachgewiesen

- Echte Drucker (auch echte Dot4-Geräte).
- `LeseWartezeit = 0` (unbegrenzt): gebaut, nicht eigens geprüft; der Pfad
  ist derselbe wie bei 2 s, nur ohne Zeitgrenze — `CancelIo`, Schließen und
  Abziehen wirken auch dort.
- Ein Drucker mit Protokoll 01 (kein Nachbau): `ReadFile` → 1.

---

## Antwort auf die Rückmeldung

| Nr. | Punkt | Stand |
|---|---|---|
| 4.1 | Nachbau am xHCI | **Behoben.** Ihr hattet recht: direkt am xHCI weist QEMU 8.2.2 ihn ab. Bei uns lief es nur, weil QEMU bei fast vollen Anschlüssen selbst einen USB-1.1-Hub einschiebt — dahinter prüft es nicht. Unsere Angabe „nachgewiesen am NEC-xHCI“ galt also nur über diesen Umweg. Korrektur zu eurer Beschreibung: `64bits_ids` ist **Bit 5** (libusbredirparser 0.13, `usbredirproto.h`), nicht 7. Euer Versuch scheiterte am Zeitpunkt: beide HELLO gehen noch mit 12-Byte-Kopf; der Nachbau muss **QEMUs HELLO abwarten**, bevor er Schnittstellen und Geräteanschluss meldet — erst dann gilt der 16-Byte-Kopf. `msi=off,msix=off` steht jetzt oben. |
| 4.2 | Lokale Anschlüsse | **Gebaut**, mit der Zuständigkeitsregel oben. Beim Prüfen gefunden: der sofort nach „angehalten“ neu gestartete Spooler endete mit Fehler 12, obwohl `StartService` Erfolg meldete. Jetzt: 2 s warten, prüfen, dass er läuft und laufen bleibt, bis zu zwei weitere Versuche, Ergebnis melden. `USB001`-Namen: nicht gebaut — `USB001:` wäre für Win32 kein Gerätename, der Anschluss müsste ohnehin `\\.\USB001` heißen. |
| 4.3 | Datensalat bei zwei Schreibern | **Gebaut**: ein schreibender Griff, Status über Griffe ohne Schreibzugriff. |
| 4.4 | Zwei gleiche Drucker | **Gebaut**: `IOCTL_NTUP_INFO` mit Seriennummer, Pfad, VID/PID, Fassung, Änderungszähler. |
| 4.5 | Win32-Codes | **Gemessen**, Tabelle oben. Für eure „abgezogen“-Liste: bei `WriteFile` kommt 21, bei `CreateFile` 2. |
| 4.6 | Wartezeit | **Gebaut**: `SchreibWartezeit`, 0 = unbegrenzt. Dafür wirkt jetzt auch **`CancelIo`** — das tat es vorher nicht (keine Abbruch-Routine); ohne sie wäre „unbegrenzt“ nur über Abziehen oder Schließen zu verlassen gewesen. |
| 4.7 | Rückkanal | **Gebaut in Fassung 3**: `ReadFile` mit Lesewartezeit (0 Byte statt Hängen), eigener Faden, `NTUP_INFO.Rueckkanal`. |
| 4.8 | Dot4 über 02 | **Gebaut** wie `ulpt`, **in Fassung 3 geprüft** (Nachbau `--dot4`/`--nurdot4`). High Speed geprüft (`--hs`). Änderungszähler: in `NTUP_INFO`. |

Außerdem beim Umbau gefunden und behoben: Wurde der Drucker abgezogen, während
der Arbeitsfaden eine Schreibanfrage gerade aus der Warteschlange genommen
hatte, wurde diese nie abgeschlossen — das schreibende Programm hätte ewig
gewartet.
