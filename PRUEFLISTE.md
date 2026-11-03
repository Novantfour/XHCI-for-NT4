# Pruefliste -- offene Punkte

Was gebaut, aber nicht nachgewiesen ist, oder was ein Befund offen
laesst.  Jeder Punkt mit dem, was zur Klaerung gebraucht wird.

## Erledigt durch Tests auf echter Eingabe (VirtualBox, USB 2.0, 1 Prozessor)

- Tastenwiederholung: lange druecken wiederholt.
- Maus: Fenster ziehen geht -- der alte Befund "kein Dauerklicken" ist
  erledigt.
- USB-Stick: geht mit NTFS.  Mit FAT32/exFAT nicht -- das kann NT 4.0
  nicht; der Treiber sagt es jetzt auf COM1.

## Wartet auf Daten von echter Hardware

**AMD-PLL-Abhilfe (SB700 bis Rev 3B, SB800).**  Zeile fuer Zeile nach
Linux, im Trockenlauf gegen Linux nachgerechnet, auf echtem Silizium
nie gelaufen.  Gebraucht: die `amdpll:`-Zeile beim Start auf dem
SB7x0-Brett, und ob isochrone Geraete (USB-Audio) ohne Aussetzer laufen.

**VirtualBox, USB-Controller "3.0": Haenger beim Anschliessen von
Tastatur und Maus.**  Gemeldet vom Einsatz (8086:1e31 an 00:0c.0); mit
"2.0" geht es.  Stand: Low Speed am xHCI ist geprueft und nicht die
Ursache.  Starker Verdacht: FreeBSDs Schrittmodus fuer Steuertransfers,
den es fuer 8086:1e31 einschaltet und den QEMUs xHCI nachweislich nicht
kann.  **Gebraucht:** die COM1-Zeile "usb: xHCI ..." unter VirtualBox,
dann Parameters\XhciSchritt = 0 (REG_DWORD), neu starten, Tastatur und
Maus anstecken -- geht es dann?  Ausserdem: liefen damals zwei Dienste
der Familie gleichzeitig (usbnt UND eine Einzelbauform)?  Das erzeugt
genau solche Stoerungen; USBINST entfernt die anderen, eine Handinstal-
lation nicht.

**Mehrprozessor.**  Unter QEMU mit ntkrnlmp + halmps (SP6a) auf 2
Prozessoren nachgewiesen: Massenspeicher, Eingabe, Drucker, Gamepad, Ab-
und Anstecken unter Last, kein Leck.  Tastenwiederholung und Leuchten
auf 2 Prozessoren nachgeprueft (Leuchten-Fehler dabei gefunden und
behoben).  Offen: mehr als 2 Prozessoren (Workstation-
Lizenz); echte Mehrkern-Hardware mit halapic/halmps.  Hinweis: eine NT 4.0
Workstation nutzt auch auf 4 Kernen nur 2.

**Maus mit Berichtsnummer oder 16-Bit-Bewegung.**  Der Deskriptor-Weg ist
an QEMUs Maus nachgewiesen (keine Berichtsnummer, 8 Bit).  Echte
Spiele- und Funkmaeuse haben oft beides.  Gebraucht: mit dem
Diagnosetreiber die Zeile "ums: Berichtsdeskriptor: ..." und einige
ums:-Zeilen beim Bewegen und Drehen am Rad.

**Maustasten-Belegung "doppelklick".**  Der Treiber schickt nachweislich
zwei vollstaendige Linksklicks; ob USER sie als Doppelklick erkennt, ist
am Bildschirm nicht geprueft.  Gebraucht: Taste mit "doppelklick"
belegen, auf ein Symbol im Arbeitsplatz zeigen, druecken.

**Xbox-360-Controller (echt).**  Kern und usbjoy.dll am Nachbau
nachgewiesen, bis in die Systemsteuerung.  Gebraucht: die Zeile
"ujoy: UsbJoy0 = ..." auf COM1, ob LED 1 leuchtet (statt zu blinken),
Systemsteuerung "Joystick" -> Testen, und ein Spiel.

**Xbox-One-Controller (echt).**  Am Nachbau nachgewiesen.  Echte
Controller unterscheiden sich nach Modell und Firmware; Linux' xpad
schickt manchen weitere Init-Pakete.  Gebraucht: COM1-Zeile "ujoy: ...
Xbox-One-Protokoll", ob er reagiert (ohne Einschalten schweigt er), und
JOYTEST.

**DirectInput** (DirectX 3 unter NT 4.0 SP3+) liest Joysticks ueber
winmm; ungeprueft.  Funk-Controller mit Empfaenger (FF/5D/81) sprechen ein
anderes Protokoll und werden noch nicht erkannt.

**HID-Gamepads und -Joysticks.**  Der Weg ueber den Deskriptor ist gebaut,
aber ohne Nachbau ungeprueft.

**Tastatur-Leuchten und Wiederholrate mit PS/2 im Rechner.**  kbdclass
schickt beides nur an Anschluss 0 (i8042prt).  Die USB-Leuchten folgen nur
Tastendruecken auf USB-Tastaturen.  Der saubere Weg waere ein Filter ueber
kbdclass (wie ctrl2cap) -- bewusst nicht gebaut: ein Fehler dort legt
alle Tastaturen lahm.

**USB-Drucker (echt).**  Anschluss am Nachbau nachgewiesen, auch direkt am
xHCI.  Offen: Dot4-Geraet mit zusaetzlicher 02-Einstellung, High-Speed-
Endpunkte (512 Byte).  Gebraucht:
COM1-Zeile "uprn: USBPRN1 = ...", `usbdruck`, und `copy /b` einer
Druckdatei in der Sprache des Druckers (z. B. PCL) an \\.\USBPRN1.

## Unter QEMU nicht pruefbar

**Isochron IN** (Kamera, Mikrofon): gebaut, QEMU hat kein solches
Geraet.  Isochron OUT ist an UHCI, OHCI, xHCI bitgenau nachgewiesen.

**Stick ziehen mitten in einer USB-Uebertragung.**  QEMU ist zu schnell;
der Cache war beim Ziehen schon ausgeschrieben.

**SB800-Pfad der PLL-Abhilfe** (Registerbasis ueber 0xCD6/0xCD7): nicht
einmal im Trockenlauf.

## Nicht wiederholbar -- beobachten

**Nach 20 Abzieh-Zyklen unter Last blieb ein Geraet bei Adresse 0**
(EHCI mit OHCI-Begleitern, Tablett).  Wiederholt mit Herzschlag-Bau und
Warteliste: 20 von 20 Zyklen unter Last (15 Dateikopien auf den Stick,
200 FTDI-Schreibvorgaenge), Tablett danach adressiert, kein haengender
Faden, Pool unveraendert, Tastatur geht.  Ohne Last ist ein
angestecktes Geraet nach einer Sekunde adressiert -- "zu frueh
nachgesehen" erklaert den alten Befund also NICHT.

Unterschiede zum ersten Lauf: dort Freigabebau statt Herzschlag-Bau;
Last war der rohe Lesetest ueber UsbDevN statt Dateikopien ueber
ntumass; und der QEMU-Monitor wurde gleichzeitig vom Stoepsel-Skript
und von Stichproben benutzt -- er nimmt nur einen Teilnehmer an, einzelne
Steckbefehle koennten abgewiesen worden sein.  Ursache unbekannt.

## Bewusste Grenzen

- Massenspeicher: nur erste primaere Partition, nur 512-Byte-Sektoren,
  hoechstens 8 Schaechte je Leser.
- Massenspeicher mit CBI (alte USB-Diskettenlaufwerke) werden nur
  allgemein bereitgestellt, nicht als Laufwerk.
