# Übergabe: USB-Treiber für Windows NT 4.0

Stand: 7. Oktober 2026. Diese Datei ist für ein neues Gespräch gedacht.
Anhängen: `usb-nt4-komplett.zip` (Quellen, Bauskripte, beide Installer,
Prüfstand mit Controller-Emulator) und `nt_small_vdi.xz` (Testabbild).

## Worum es geht

Der FreeBSD-USB-Stapel wurde auf Windows NT 4.0 portiert. NT 4.0 hat
kein natives USB. Der Treiber bedient UHCI, OHCI, EHCI und xHCI und
bindet Tastatur, Maus, Massenspeicher, Drucker und Gamepads an.
Dazu kommen ein Gerätemanager, ein Installer und Werkzeuge.

Alles ist auf Deutsch benannt und kommentiert; bitte so weiterführen.

## Davor abgeschlossen (6. Oktober)

### Abschnittsattribute (Meldung von Ray Hinchliffe, SIV)

Gemeldet: ".text + .data are marked as paged, they should not be,
further paged code should be in PAGE, and DriverEntry() should be in
INIT." Der Befund war richtig und ist behoben. Maßstab war sein eigener
`SIVNT4.SYS`; die Werte stimmen jetzt bitgenau überein:

| Abschnitt | SIVNT4.SYS | vorher | jetzt |
|---|---|---|---|
| `.text`  | `68000020` | `60000020` | `68000020` |
| `.data`  | `c8000040` | `c0000040` | `c8000040` |
| `INIT`   | `e2000020` | fehlte     | `e2000020` |

Umsetzung:
- `#pragma alloc_text(INIT, DriverEntry)` in `nt/ntusbnt.c`
- `/section:INIT,ERWD` und `/merge:PAGE=.text` beim Binden
- `abschnitte.py` setzt NOT_PAGED direkt im PE-Kopf, weil lld-link
  dieses Bit im Gegensatz zu Microsofts Binder nicht selbst setzt;
  danach rechnet `pruefsumme.py` die PE-Prüfsumme neu
- `PAGE` wird bewusst in `.text` gemergt: der Treiber arbeitet im ISR-
  und DPC-Pfad, da darf nichts auslagerbar sein (strenger als SIV)
- betrifft auch `PCISCAN.SYS` (Gerätemanager), dort gleich behoben

Verifiziert in NT 4.0 SP6a (QEMU): xHCI erkannt und angehängt (5 Gbps),
Root-Hub 12 Anschlüsse, USB-Tastatur an kbdclass, USB-Maus (5 Knöpfe,
Rad) an mouclass, USB-Stick als Laufwerk E:, Xbox-One-Controller mit
6 Achsen und 11 Knöpfen.

**Aktuelle Treiberfassung: 2026.09.27.8.** Der Anwender hatte zuletzt
noch `.5` installiert (an `.text = 60000020` und fehlendem `INIT` im
SIV-Bildschirmfoto erkennbar).

### Joystick: der Registry-Alias

Ursache gefunden: Der Wert unter
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32`
muss **`joystick`** heißen — ohne Punkt, ohne Nummer. Vorher stand dort
`joystick.1`; das ist das Muster der Codec-Aliase (`msacm.xxx`,
`vidc.xxxx`). winmm fragt diesen Namen nie ab, lud die DLL also nie.

Nach der Korrektur in NT 4.0 gemessen:
```
winmm: joyGetNumDevs = 2
Joystick 1: "Controller"  6 Achsen, 11 Knoepfe Z R U V POV
X 32768  Y 32767   Knoepfe 00000000
nach "A und B drücken, Stick bewegen":
X 52768  Y 47767   Knoepfe 00000003
```

Zusätzlich drei echte Fehler in `usbjoy.c` behoben:
- 16-Bit-`JOYCAPS` ist 54 Byte groß, nicht 404 — wurde mit
  `JOYERR_PARMS` abgewiesen
- 16-Bit-`JOYINFO` ist 8 Byte groß, nicht 16 — es wurde über das
  Pufferende hinaus geschrieben
- nur die ersten vier Knöpfe wurden durchgereicht (`& 0x0F`)

### Mehrsprachigkeit

`sprache.h` (liegt in `geraetemanager/src/` und `quelle/werkzeug/`)
wählt über `GetUserDefaultLangID()` zwischen Deutsch (0x07),
Französisch (0x0C) und sonst Englisch. Nur die Hauptsprache zählt,
damit auch Schweizer und kanadisches Französisch passen. Fehlende
Einträge fallen auf Englisch zurück. Im Quelltext: `S(T_SCHLIESSEN)`.

Umgesetzt: Gerätemanager (Knöpfe, Klassen, Eigenschaftenseiten,
USB-Texte, Meldungen), Installer (Grundtexte), Reiter für Maus und
Tastatur. Kirgisisch wurde bewusst weggelassen: NT 4.0 kennt die
Sprache nicht und hat auf westlichen Systemen keine kyrillische
Codepage.

## In den Sitzungen vom 7. Oktober abgeschlossen

### Testabbild `nt_small.vdi` (UUID 61307e18-…)

VirtualBox-Dienste aus (i8042prt wieder auf i8042prt.sys statt
VBoxMouseNT.sys), ntagent als Dienst auf COM2, Cirrus 5446 (cl54xx 1.41)
mit vbemp als Rückfall, vga.sys aus, USB-Treiber .8 eingerichtet
(`C:\USBNT` = neuer Installer).  Netzwerk: PCnet an PCI-Steckplatz 3
(QEMU: `-device pcnet,netdev=n0,addr=3`).

**Autologon:** NT-4.0-MSGINA setzt `AutoAdminLogon` bei leerem
`DefaultPassword` nach jeder Anmeldung auf 0 zurück (msgina.dll,
GetProfileStringW → Länge 0 → WriteProfileStringW "0").  Abhilfe ohne
Kennwort: Run-Eintrag `AutoAnmeldung` = `regedit /s C:\WINNT\AUTOLOGON.REG`.
Über zwei Neustarts nachgewiesen.

### Gamepads: winmm, DirectInput, Win16 — alles belegt

Siehe AENDERUNGEN.md, Abschnitt 2026.10.7.  Kurz:
- `szRegKey` = "usbjoy.dll"; `Joystick<n>Configuration` = binäres
  JOYREGHWCONFIG (112 Byte, JOY_US_PRESENT, dwType 12) — schreibt USBINST.
  **USBJOY.REG ist veraltet und falsch** (Textwert).
- Win16: Fehler in winmm!joy32Message (2-Byte-Versatz der 16-Bit-JOYCAPS)
  — das war SSPROs „at least 2 buttons“.  usbjoy.dll gleicht ihn aus.
- Beide Installer enthalten jetzt USBJOY.DLL/CPL.

Geprüft mit `pruefstand/pademu.py` (der ältere Nachbau `pruefstand/xpad.py`
kann 360 und One; pademu dazu HID, Series, `--stumm`, zwei gleichzeitig) (360, One, Series, HID; zwei
gleichzeitig; Vibration; Abziehen/Anstecken; usbjoy.cpl).

**Win16 vollständig:** `JOY16` (Abfrage) und `JOY16 fang <s>`
(joySetCapture mit MM_JOYn-Nachrichten, wie viele Win16-Spiele) laufen
unter WOW richtig, auch über Abziehen/Anstecken hinweg.  Behoben dabei:
Xbox-Trigger standen bis zum ersten Bericht auf „halb gedrückt“.

**Belegte DLL tauschen:** Ein WOW-Prozess (ntvdm) hält usbjoy.dll nach
jedem Win16-Programm geladen — PUT schlägt dann mit „nicht anlegbar“ fehl.
Über `USBINST ein` (ersetzt beim Neustart) und neu starten.

## 8. Oktober: Gamepads getrennt

USB-Paket und Gamepads sind getrennt: eigener Kerntreiber USBXBOX.SYS,
eigener Installer (installer-xbox, XBOXINST).  XBOXINST schaltet das Alte
vorher ab (alte usbnt.sys mit eingebautem Joystick-Teil -> Abbruch mit
Anleitung; winmm-Joystick-Treiber, Gameport-Dienst, joy.cpl gesichert).
Alles geprueft, auch das Laden im laufenden Betrieb.  Einzelheiten:
AENDERUNGEN.md, Abschnitt 2026.10.8.1; Aufbau: KI-KONTEXT.md.

## 9. Oktober: USB-Audio angefangen

USBAUDIO.SYS (eigener Zusatztreiber) + USBTON.EXE: UAC1-Wiedergabe laeuft
lueckenlos an xHCI, OHCI, UHCI (QEMU usb-audio, Mitschnitt geprueft).
Einzelheiten: AENDERUNGEN.md 2026.10.9.1.  Von Hand einrichten (noch kein
Installer): usbaudio.sys nach system32\drivers, Dienst usbaudio Typ 1,
Start 1, Gruppe "Extended Base", ErrorControl 0, ImagePath als REG_SZ
"System32\drivers\usbaudio.sys" -- NICHT hex(2) ueber REGEDIT4 (wird
doppelt verbreitert, Fehler 123).  Braucht usbnt.sys ab 2026.10.9.1
(52 Exporte).  Pruefstand: pruefstand/boot-audio.sh <uhci|ohci|xhci>,
Ausgabe in /tmp/ton.wav, pruefstand/sinuspruef.py.

## 10. Oktober (3): USB-Audio Lautstaerke -- endgueltig

2026.10.10.3: Oeffnen stellt nur hoerbar, wenn das Geraet am Minimum steht
(sonst bleibt die Einstellung des Anwenders); WODM_SETVOLUME meldet dem
Mixer; Waechterfaden meldet Aenderungen am Geraet (Rad am Headset).
Alles am Emulat geprueft.  Falle: bau-werkzeuge.sh liegt in quelle/werkzeug/.

## 10. Oktober (2): USB-Audio Lautstaerke

Anwender: Lautstaerke springt beim Start-Klick hoch, Regler nicht synchron.
Behoben (2026.10.10.2): Oeffnen hebt nur Stumm auf (nicht die Lautstaerke);
WODM_GETVOLUME liest frisch aus dem Geraet.  Geraet = einzige Wahrheit.
Am Emulat geprueft: winmm, Mixer und Geraet zeigen denselben Wert.

## 10. Oktober: Treiberqualitaet (Ray Hinchliffe, SIV)

Zweiter Hinweis umgesetzt: auslagerbarer Code in PAGE, PAGED_CODE() als
erste Anweisung passiver Routinen (DriverEntry, Unload, probe/attach/
detach).  PAGE nicht mehr in .text gemergt -> eigener auslagerbarer
Abschnitt; .text/.rdata/.data resident, INIT verwerfbar.  Alle .sys neu
(2026.10.10.1), im Gast geprueft.  Leitfaden: TREIBER-BAU-NT4.md.
Einzelheiten: AENDERUNGEN.md 2026.10.10.1.

## Offen, in dieser Reihenfolge

0. **USB-Audio weiter:** usbaudio.dll (winmm) und AUDIOINST sind fertig
   und geprueft (2026.10.9.2-.5, Paket installer-audio), auch Win16 ueber
   WOW (WAVE16.EXE), Pause/Restart, Aufnahme (winmm waveIn).  ANWENDER
   TESTET: HyperX Cloud Stinger Core Wireless (0951:170b) -- erster Test
   "kein Ton" (falsche Feature Unit, Zweig stumm), 2026.10.9.5 behoben,
   Anwender bestaetigt: Ton geht.  2026.10.9.6: Mixer (sndvol32) dazu --
   Rueckmeldung am echten Headset abwarten.  Offen danach: echte Hardware
   weiter (CM108, PCM2902), 
   (widMessage, IN-Strom im Kern), Rueckmelde-Endpunkt fuer asynchrone
   Geraete, spaeter UAC2.  DirectInput (Joystick) ist zurueckgestellt.


1. ~~Einzelbauformen prüfen~~ — **erledigt** (2026.10.8.2): alle vier laufen;
   dabei zwei Fehler in USBINST behoben (Wechsel der Bauart: Fehler 1078
   und Hänger beim Anhalten des laufenden Treibers).  bin/diagnose und
   bin/herzschlag neu gebaut.
   USBNT.SYS ist schon 2026.10.7.1 (Spieler-LED, geprüft an allen vier
   Controllerarten).  Bauen: DDK-ISO entpacken, `ddk-ersatz/ntddk-patch.py`,
   `freebsd/` als SRC (siehe freebsd/HERKUNFT.txt).
2. ~~SSPRO prüfen~~ — **erledigt**: SinkSub Pro erkennt den Controller und
   ist damit spielbar (Lenken, Bomben links/rechts).  SSPRO ist NICHT im Testabbild
   (im Gast und in den Rohdaten gesucht).  Ersatz bis dahin:
   `JOY16.EXE` (werkzeug16/) macht dieselben Win16-Aufrufe.
3. ~~**Echter Controller** am echten Rechner, Vibration~~ -- vom Anwender bestätigt (geht).  DirectInput am echten Rechner: zurückgestellt.
4. **Audio im Controller** — erst, wenn 1–3 erledigt sind.  360: Headset
   über Schnittstelle 1 (FF/5D/03, Interrupt); One ab ~2015: GIP-Audio,
   Schnittstelle 1 Alt 1 (isochron).  Kein USB-Audio nach Norm.  Braucht
   Kernteil + winmm-Wave-Treiber (wodMessage/widMessage).  Formate erst
   aus xpad/GIP belegen.  pademu bildet die Schnittstellen schon nach,
   spielt aber kein Audio.
5. Aus der alten Liste unverändert offen: ~~Gerätemanager ohne PCISCAN~~ (erledigt: startet mit Warnknoten; CPL hat „Treiber einrichten“ wieder, startet DEVMGR.EXE /einrichten -- VirusTotal der neuen CPL steht aus)
   (gPciFehlt), USBSETUP im InstallShield-Layout, restliche Übersetzungen.

## Fallen im Prüfstand

**Neu (7. Oktober):**

- **Abbild als VirtualBox-Snapshot** (Differenz-VDI, „non-NULL link UUID“):
  qemu-img liest es nicht.  `vdi_zusammen.py basis.vdi snapshot.vdi ziel.raw`
  (nt-einrichtung.zip) führt es auf das Original zusammen.  War das Volume
  als „zu prüfen“ markiert: einmal unverändert starten, im Anmeldedialog
  per Tab (4×) auf „Herunterfahren…“ und sauber ausschalten — erst dann
  `einbau.sh`.  Nicht einfach `ntfsfix -d`, das verwirft das Journal.
- **Partitionsgröße immer aus dem MBR lesen.**  Das zweite Abbild hatte
  749 MB und NTFS 3.1 statt 511 MB/NTFS 1.2; mit der alten Größe meldete
  ntfsfix Scheinfehler (abgeschnittenes Volume).

- **Sitzungsende = harter Abbruch.**  Die VM stirbt mit der Sitzung; was
  NTFS noch nicht geschrieben hatte, ist weg — einmal sogar eine halb
  geschriebene usbnt.sys (STOP c000026c / c0000221).  Nach jeder
  Einrichtung SOFORT sauber neu starten (`ntagent: SHUTDOWN` = Neustart),
  zum Ausliefern `pruefstand/ausschalten.c` (ohne Neustart) benutzen.
- **`pkill -f <muster>` beendet die eigene Shell** des Werkzeugs.  Nur über
  PID-Dateien beenden (`halt.sh`).
- **hivex legt neue Unterschlüssellisten als `lh` an** (erst ab XP) →
  NTLDR: „SYSTEM fehlt oder ist beschädigt“.  `lh2lf.py` danach immer.
- **SYSTEM.ALT** kann eine höhere Folgenummer haben als `system`;
  beide mit max+1000 schreiben.
- **Volle Platte:** `dd` zurück ins Abbild schreibt dann ein halbes
  Abbild.  Vor dem Einspielen `df` prüfen; die Arbeitsdateien (raw, img)
  sind je 512 MB.  Nach jedem `ntfscp` mit `ntfscat | md5sum` gegenprüfen.
- Lange Schritte (Start + Prüfung + VDI + xz) einzeln ausführen — ein
  Befehl über 300 s wird abgebrochen.
- `copy` über den Agenten schlug einmal still fehl — Dateien direkt mit
  PUT ans Ziel legen und mit GET + md5 gegenprüfen.

- **qcow2-Testkopien nur mit `cache=writethrough`.**  Ohne verliert QEMU
  beim harten Sitzungsende Verwaltungsdaten: eine längst fertig
  geschriebene usbnt.sys war danach keine Programmdatei mehr
  (STOP c000026c / 0xC000012F), obwohl der Gast sauber neu gestartet hatte.
- **LIESMICH.TXT der Installer hat gemischte Zeilenenden** (CRLF und LF).
  Nicht an `\r\n` aufteilen, sondern mit `\r?\n` ersetzen.

- **Laufenden USB-Treiber nie anhalten** (ControlService STOP): NT 4.0
  versucht dann, ihn zu entladen, obwohl Tastatur und Maus daran hängen —
  das Werkzeug hängt, mit ihm Agent und Dienstverwaltung.
- **Testskripte:** `boot7.sh <uhci|ohci|ehci|xhci>` (ein Controllertyp),
  `aus.sh` (über `ausschalten.exe` sauber aus, dann QEMU beenden) — für
  Wechsel der Controller-Hardware.  QEMUs EHCI allein nimmt nur High-Speed-
  Geräte (Stick), keine Tastatur/Maus.

- **QEMU-Zeitmessung ohne KVM** laeuft nicht in Echtzeit (5 s Ton in
  3,9 s Bus-Zeit).  Fuer Audio zaehlen Unterlaeufe des Treibers und die
  Kontinuitaet im Mitschnitt, nicht die Laufzeit.
- **QEMUs WAV-Mitschnitt** schreibt nur, was ankommt -- Unterlaeufe
  erscheinen dort NICHT als Stille.  Darum Sinus-Kontinuitaet pruefen.

- **Mitschnitt-Pruefung nur in der Tonmitte messen** (sinuspruef.py tut
  das jetzt): QEMU schreibt verzoegert, am Rand steht der vorige Ton.

- **usb-redir im Gast verzerrt die Gastzeit** (ohne KVM): Laufzeiten mit
  angestecktem pademu-Geraet sind wertlos (3 s Ton "dauern" 24..39 s).
  Ton, Unter-/Ueberlaeufe und Kontinuitaet zaehlen.
- **Geraete kommen oft stumm/leise hoch** -- der Audiotreiber muss beim
  Oeffnen selbst freischalten (vor dem Strom).

**Weiter gültig:**
 (QEMU, NT 4.0 SP6a)

Diese drei haben in der letzten Sitzung viele Stunden gekostet:

1. **Sauber herunterfahren.** NT übernimmt von außen in die Registry
   geschriebene Dienste erst dauerhaft, wenn einmal regulär
   heruntergefahren wurde (`shutdown /L /R /Y /C`). Hartes Beenden der
   VM lässt die Änderungen beim nächsten Start verschwinden — das sieht
   aus wie ein Treiberfehler, ist aber keiner.
2. **Hive-Folgenummer.** Die bearbeitete Registry muss im Kopf eine
   höhere Folgenummer tragen als die vorhandene, sonst hält NT sie für
   veraltet. `einbau.py` hebt sie um 1000 an und rechnet die
   Kopfprüfsumme (XOR der ersten 127 DWORDs, bei 0x1FC) neu.
3. **NTFS-Dirty-Bit.** Nach hartem Beenden verweigern `ntfscat` und
   `ntfscp` den Zugriff; der ganze Einbau läuft dann still ins Leere.
   `einbau.py` macht jetzt immer erst `ntfsfix -d`.

Weiteres: Ein neu eingetragener Dienst braucht `DisplayName`, sonst
meldet `net start` nur „Name ist ungültig", und der Dienstmanager kennt
ihn erst nach einem Neustart. Die Tastatursteuerung über QMP braucht
die deutsche Belegung (Y und Z vertauscht, Backslash auf AltGr+ß,
eckige Klammern auf AltGr+8/9) — dafür liegt `tipp2.py` im Prüfstand.

**Nicht anfassen:** die Anzeige-Einstellungen unter
`Services\vga\Device0` im Testabbild. Ein Versuch, dort eine gültige
Auflösung zu setzen, führte zu `STOP c0000143` („DISPLAY_DRIVER.DLL
fehlt") und musste zurückgenommen werden.

## Bauen

```
cd quelle && ./bau-clangcl.sh          # Treiber, alle fünf Bauarten
cd werkzeug && ./bau-werkzeuge.sh ZIEL # 19 Werkzeuge
cd geraetemanager && ./bau-hier.sh     # DEVMGR.EXE/CPL + PCISCAN.SYS
```

Werkzeugkette: clang-cl und lld-link gegen das NT-4.0-DDK für die
Treiber, mingw für alles im Benutzermodus. Nach jedem Treiberbau laufen
`abschnitte.py` und `pruefsumme.py`.

**Immer prüfen:** Importe gegen die echten NT-4.0-DLLs (aus dem
Testabbild extrahieren), sonst startet das Programm unter NT 4.0 nicht.
So wurde entdeckt, dass die mitgelieferte wolfSSL-Bibliothek
`strcpy_s`/`strtok_s` braucht, die es in NT 4.0 nicht gibt.

## Virenscanner

Heuristische Treffer (BitDefender-Familie, Elastic, Bkav) wurden auf 9
von 13 gesenkt: Versionsangaben in alle Dateien, keine Abwicklungs-
tabellen, eigener NTUSYS-Abschnitt in `.rdata` gemergt, Messwerkzeuge in
ein eigenes Paket. Gruppenweise geprüft: Treiber, Einrichtung und
Systemsteuerung sind sauber; gemeldet wurden `USBTASTEN` und die
Befehlszeilenwerkzeuge.

**Daraus folgt:** kein Programm darf einen Treiber aus sich selbst
auspacken und laden (siehe Punkt 1). Das ist eine bewusste
Entscheidung, keine Nachlässigkeit.
