# Jedes USB-Geraet bereitstellen

Tastatur und Maus gehen weiter ueber ihren festen Weg an kbdclass und
mouclass -- NT braucht sie im Kern, damit USER sie liest.  **Alles
andere** bekommt jetzt ein eigenes NT-Geraet mit einer festen
Schnittstelle:

```
\Device\UsbDevN        \\.\UsbDevN aus Win32
HKLM\HARDWARE\DEVICEMAP\USB\UsbDevN = "VID_0403&PID_6001&MI_00 FF/FF/FF"
```

Der Treiber dafuer steht in `quelle/ober/ntugen.c`, der Vertrag in
`quelle/ober/ntusbgen.h`.  Ein Joystick-, Drucker- oder Scannertreiber
-- im Kern oder im Benutzermodus -- wird gegen diese Kopfdatei
geschrieben.  Der USB-Treiber weiss nichts ueber Joysticks, und er soll
es auch nicht.

## Was bereitgestellt wird

Jede SCHNITTSTELLE, fuer die kein anderer Treiber zustaendig ist.  Ein
zusammengesetztes Geraet -- Tastatur mit Medientasten, Drucker mit
Scanner, Audio mit Steuer- und Datenteil -- erscheint einmal je Teil, und
der Tastaturteil geht trotzdem an ukbd.  Die Gueteziffer im probe ist
BUS_PROBE_HOOVER, die niedrigste: jeder andere Treiber gewinnt, hier
landet nur, was sonst keiner nimmt.

## Die Schnittstelle

```
IOCTL_NTUG_INFO          Kennung, Klassen, Geschwindigkeit, Endpunkte,
                         Hersteller/Produkt/Seriennummer
IOCTL_NTUG_DESKRIPTOR    Geraet, Konfiguration, Zeichenketten --
                         und HID/HID-Bericht, automatisch an die
                         Schnittstelle gerichtet
IOCTL_NTUG_STEUER        rohe Steueranfrage, IN oder OUT
IOCTL_NTUG_LESEN         Bulk oder Interrupt IN
IOCTL_NTUG_SCHREIBEN     Bulk oder Interrupt OUT
IOCTL_NTUG_ALTERNATIVE   alternative Einstellung waehlen
IOCTL_NTUG_STALL_WEG     Endpunktsperre aufheben
IOCTL_NTUG_ISO           isochron, IN oder OUT, eine Laenge je Rahmen
```

Jede Uebertragung wird je Endpunkt EINMAL eingerichtet und danach
wiederverwendet (gehaltene Uebertragung).  Abgebaut wird nur beim
Wechsel der Alternative und beim Abziehen.

Ein USB-Fehler ist kein Fehler des Aufrufs: DeviceIoControl gelingt,
und der USB-Fehler steht in NTUG_ERGEBNIS.Fehler.  So bekommt man auch
bei Kurzpaket oder Zeitueberschreitung, was angekommen ist.

## Im QEMU-Lauf nachgewiesen

Am NEC uPD720200, mit sieben verschiedenen Geraeten gleichzeitig:

```
UsbDev0  46F4:0001  08/06/50  Massenspeicher      5 GBit
UsbDev1  0627:0001  03/00/00  Grafiktablett (HID) 480 MBit
UsbDev2  0403:6001  FF/FF/FF  FTDI-Wandler        12 MBit
UsbDev3  08E6:4433  0B        Chipkartenleser     12 MBit
UsbDev4  46F4:0002  MI_00     Audio Steuerteil
UsbDev5  46F4:0002  MI_01     Audio Datenstrom
Tastatur und Maus -> kbdclass / mouclass
```

**HID -- der Weg eines Joysticktreibers.**  Berichtsdeskriptor gelesen
(74 Byte: drei Knoepfe, X/Y 16 Bit absolut, Rad).  Dann Berichte vom
Interrupt-Endpunkt, waehrend ueber QMP bewegt wurde -- jeder Wert
stimmt exakt:

```
geschickt           gelesen
X=1000  Y=2000      00 e8 03 d0 07 00
X=30000 Y=25000     00 30 75 a8 61 00
linke Taste         01 30 75 a8 61 00
Ruhe                Zeitlimit (usb 20)
```

**Herstellereigen -- der Weg eines Druckertreibers.**  FTDI-Anfrage
`C0 05` (Modemstatus) liefert `b1 60`.  Text auf den Bulk-OUT-Endpunkt
kommt auf dem Wirt Zeichen fuer Zeichen an.

**Massenspeicher, roh.**  SCSI-INQUIRY in einem Bulk-Only-Befehlsblock:
36 Byte Antwort ("QEMU HARDDISK"), danach die Quittung `USBS`, Status 0.

## Grenzen, ehrlich

- **Isochron IN** ist gebaut, aber ungeprueft: QEMU hat kein Geraet mit
  isochronem Eingang (keine Kamera, kein Mikrofon).  Isochron OUT ist an
  UHCI, OHCI und xHCI bitgenau nachgewiesen.
- **AMD-PLL-Abhilfe** (`nt/ntamdpll.c`) ist eingebaut, Zeile fuer Zeile
  nach Linux, aber **auf echter AMD-Hardware ungeprueft**.  Siehe
  AENDERUNGEN.md.
- **Isochron am OHCI, Richtung OUT:** die Laengen je Rahmen sind
  GESCHAETZT (NTUG_ISO_ANTWORT.Geschaetzt = 1) -- die OHCI-Norm meldet
  dort 0.  Siehe ntusbgen.h.
- Beim rohen Bulk-Only lief der erste Leseversuch nach der Datenphase
  ins Zeitlimit; der naechste brachte die Quittung.  Vermutlich eine
  Eigenheit von QEMUs Speichernachbildung -- **nicht belegt**.  Die
  Bulk-Only-Norm sieht fuer die Statusphase ohnehin einen zweiten
  Versuch vor.
- Ein Drucker war nicht zur Hand -- QEMU bildet keinen nach.  Der Weg
  (Klassenanfrage GET_DEVICE_ID, dann Bulk OUT) ist derselbe wie beim
  FTDI-Test und damit gedeckt, aber an keinem echten Drucker geprueft.

## Werkzeug

```
usbgeraet                       alle bereitgestellten Geraete
usbgeraet N                     Geraet N ausfuehrlich
usbgeraet N hid                 HID-Berichtsdeskriptor
usbgeraet N lesen EP [n]        n Berichte lesen
usbgeraet N schreiben EP text   Text auf Bulk OUT
usbgeraet N schreiben EP -x ..  beliebige Bytes
usbgeraet N steuer RT RQ WV WI WL
usbgeraet N stall EP
```
