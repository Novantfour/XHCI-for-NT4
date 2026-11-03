# Unterlage FreeBSD-USB auf NT 4.0 -- alle vier Bauarten, mit Werkzeug

Schritt 1 bis 6 des Skills `linux-treiber-unterlage` sind abgearbeitet.
Offen ist nur noch Schritt 7: an die Hardware.

```
Objekte       31 Dateien uebersetzen zu Objekten, keine Ausnahme
Symbole       1 031 definiert, 63 aussen nachgefragt
              44 in tools/ntkrnlmp.def belegt
              18 in HAL.DLL erwartet
               1 Makro im echten DDK, kein Import
              ungeklaert: keiner

Untersystem   39 273 Zeilen, unveraendert -- md5 gleich dem FreeBSD-Baum
Unterlage     60 Kopfdateien, 5 Umsetzungsdateien
Oberkante      3 Dateien -- Tastatur, Maus, NT-Anschlussgeraete
Messung       vorher 127 Kernnamen offen, nachher 0
```

## Der Binderlauf

Der Syntaxlauf sagt nichts ueber fehlende Symbole. Deshalb entstehen
jetzt echte Objektdateien, und `nm` sagt, was aussen nachgefragt wird.
Die Liste steht in `pruef/einfuhr.txt`, erzeugt und nicht von Hand
gepflegt; `bau.sh` meldet jeden Namen, der neu dazukommt.

Der Lauf hat **elf Namen gefunden, die deklariert und nie hingeschrieben
waren** -- `ntu_strdup`, `sysctl_handle_int`, `usbd_dummy_timeout`,
`ntu_usb_handle_request`, `rebooting`, `Giant`, `usb_ref_lock` und vier
Wahlschalter-Makros. Keiner davon faellt beim Uebersetzen auf. Genau
dafuer steht die Zeile *jeder Import gegen die echten Binaerdateien des
Zielsystems geprueft* in der Pruefliste des Skills.

Nebenbei hat er einen Fehler in meinem eigenen Bauskript gefunden:
`grep -vE "usb_dev.c|..."` schliesst auch `usb_device.c` aus -- der
Punkt ist im regulaeren Ausdruck ein Platzhalter. Die Datei fehlte im
Bau, und vierzig Namen sahen aus wie Luecken der Unterlage.

## Der Aufbau, wie der Skill ihn zeichnet

```
  NT-Anwendung
      |
  kbdclass / mouclass        NT bringt sie mit
      |
  \Device\KeyboardPort1      ober/ntkbdport.c            NEU
  \Device\PointerPort1
      |
  ntukbd / ntums             ober/ntukbd.c, ober/ntums.c NEU
      |
  USB-Kern                   unveraendert uebersetzt
  UHCI OHCI EHCI xHCI        unveraendert uebersetzt
      |
  Unterlage                  nt/*.c                      NEU
      |
  HAL, Kern
```

Neu geschrieben ist nur oben und unten. Die Mitte -- 39 273 Zeilen --
ist Byte fuer Byte der FreeBSD-Baum.

## Die Oberkante

Nach oben sind wir **Anschlusstreiber**, nicht Filter. kbdclass und
mouclass suchen ein Geraeteobjekt, melden sich mit
`IOCTL_INTERNAL_KEYBOARD_CONNECT` an und hinterlegen dabei ihren
Rueckruf. Von da an liefern wir `KEYBOARD_INPUT_DATA`.

Drei Dinge daran sind leicht falsch zu machen:

**Der Rueckruf will DISPATCH_LEVEL.** Unser Faden laeuft auf
PASSIVE_LEVEL. Also `KeRaiseIrql` davor, `KeLowerIrql` danach -- die
einzige Stelle im ganzen Bau, an der wir anheben, und sie ist kurz.

**Die Anmeldung ist METHOD_NEITHER.** Der Puffer steht in
`Type3InputBuffer`, nicht in `AssociatedIrp.SystemBuffer`. Wer dort
liest, traegt einen Rueckruf ins Nichts ein und merkt es erst beim
ersten Tastendruck.

**E0 ist ein Flag, kein Ereignis.** Auf dem Draht schickt eine Tastatur
`E0 4D` fuer Pfeil rechts. kbdclass will `MakeCode = 0x4D` mit
`Flags = KEY_E0`. Das ist die haeufigste Verwechslung beim Nachbau.

Und der Bericht des Startprotokolls ist eine **Menge**, keine Folge: er
sagt, was gerade gedrueckt ist, nicht was sich geaendert hat. Die
Aenderung muss aus dem Vergleich mit dem vorigen Bericht kommen. Wer das
uebersieht, bekommt bei jeder Wiederholung neue Anschlaege.

Warum nicht FreeBSDs `ukbd.c` und `ums.c`: sie haengen an kbdreg und
evdev, zwei weiteren Untersystemen, deren Ertrag unter NT null ist --
NT hat seine eigene Tastaturkette. Das Startprotokoll deckt jede Tastatur
ab, die auch im BIOS funktioniert, und kostet eine Umsetzungstabelle.
Genau die Abwaegung, die der Skill fuer die Oberkante vorsieht.

## Am DDK geprueft

Das NT-4.0-DDK liegt jetzt vor. Alle Lagen, Flagwerte und IOCTL-Nummern
aus `ober/ntkbdmou.h` stimmen mit `ddk/src/input/inc/NTDDKBD.H`,
`NTDDMOU.H` und `KBDMOU.H` ueberein. Mit `-DNTU_ECHTES_DDK` werden statt
des Nachbaus die Originale benutzt (Kopien unter `ober/ddk/`).

**Zwei Annahmen waren aber falsch**, und beide haetten dazu gefuehrt,
dass die Tastatur nichts liefert, ohne dass irgendwo ein Fehler steht.

### kbdclass sucht keine Geraetenamen ab

Es zaehlt die Werte unter `HKLM\HARDWARE\DEVICEMAP\KeyboardPort`. Der
Rueckruf in `KbdDeviceMapQueryCallback` tut nichts weiter als
`*(PULONG)Context += 1` -- der Name des Wertes wird nicht einmal
angesehen. Wer sich dort nicht eintraegt, wird nie geoeffnet, ganz
gleich wie das Geraet heisst.

Der Anschlusstreiber traegt sich jetzt selbst ein, mit
`RtlWriteRegistryValue` und genau der Form, die i8042prt benutzt
(`I8042DEP.C` Zeile 1223): Wertname ist der volle Geraetename, Inhalt
der Registrierungspfad des eigenen Dienstes. Beim Abhaengen wird der
Eintrag wieder geloescht -- bleibt er stehen, zaehlt kbdclass beim
naechsten Start einen Anschluss mit, den es nicht gibt.

### Die Nummern muessen ab 0 luckenlos sein

Woertlich aus dem DDK-Quelltext: *"The port device objects are created
with suffixes in strictly ascending order, starting with suffix 0. There
are no gaps in the list."* kbdclass baut die Namen aus der **Anzahl**
zusammen, nicht aus den eingetragenen Namen.

Mein erster Entwurf begann bei 1, um i8042prt auszuweichen. Das ist
genau in dem Fall falsch, auf den es ankommt: ohne PS/2-Tastatur traegt
i8042prt nichts ein, kbdclass zaehlt eins und oeffnet `KeyboardPort0`,
den es dann nicht gibt. Jetzt wird die kleinste freie Nummer genommen,
auch die 0.

### Und die Einstellung, die man trotzdem braucht

`MaximumPortsServiced` hat den Vorgabewert 1 und wird auf die Anzahl im
DEVICEMAP heruntergeklemmt. `ConnectMultiplePorts` hat den Vorgabewert 0,
woraus `ConnectOneClassToOnePort = 1` folgt -- je Anschluss ein eigenes
Klassengeraet. Im selben Quelltext steht:

> *"Note that for Product 1, the Raw Input Thread (Windows USER) will
> only deign to open and read from one keyboard device."*

Steckt also zusaetzlich eine PS/2-Tastatur, landet die USB-Tastatur auf
`KeyboardClass1` und USER liest sie nie. Die Vorlage liegt bei:

```
pruef/kbdmou.reg     MaximumPortsServiced = 4
                     ConnectMultiplePorts = 1
                     fuer Kbdclass und Mouclass
```

Hinzugekommene Importe: `RtlWriteRegistryValue` und
`RtlDeleteRegistryValue`, beide in `tools/ntkrnlmp.def` belegt.

## Die Reihenfolge beim Anhaengen ist nicht beliebig

Erst alle EHCI, dann der Rest. Auf jedem Brett mit EHCI teilen sich
EHCI und die begleitenden UHCI/OHCI dieselben Buchsen: der EHCI besitzt
sie und gibt ab, was er nicht bedienen kann. Haengt der Begleiter
zuerst, nimmt er Buchsen an sich, die der EHCI gleich darauf
beansprucht -- schnelle Geraete laufen dann langsam oder gar nicht, und
es sieht nach einem Kabelproblem aus.

**Alle vier Bauarten werden jetzt angehaengt**, xHCI eingeschlossen.
Dafuer hat er die genaue Maskierung bekommen: `IMAN.IE` im Laufzeitblock
statt `USBCMD.INTE`. Der Laufzeitblock steht in `RTSOFF` (cap 0x18,
untere fuenf Bit gehoeren nicht zur Adresse), Unterbrecher 0 liegt bei
+0x20.

`IMAN.IP` ist write-1-to-clear. Beim Sperren wird deshalb `0x3`
abgezogen und beim Freigeben Bit 0 ausdruecklich **nicht** gesetzt --
sonst quittiert die Unterlage eine Unterbrechung, die der Treiber noch
gar nicht gesehen hat. `USBCMD.INTE` waere einfacher gewesen, schaltet
aber den ganzen Baustein ab und damit auch Unterbrecher, die uns nicht
gehoeren.

Draussen bleiben nur Bausteine mit herstellereigener Schnittstelle
(progif 0x80): fuer die gibt es keinen Treiber, und ein probe waere ein
Ratespiel am fremden Registerblock.

## Das Werkzeug

`werkzeug/usbtest.c` -- ein Win32-Programm, nur kernel32 und advapi32,
laeuft auf NT 4.0.

```
usbtest umgebung     Registrierung, Dienst, DEVICEMAP  -- OHNE Treiber
usbtest bausteine    PCI-Tabelle, jetzt gelesen, nicht vom Start
usbtest geraete      der Geraetebaum, den der USB-Kern fuehrt
usbtest register N   Registerabzug von Baustein N
usbtest zaehler      Betriebszaehler mit Deutung
usbtest beobachten   im Sekundentakt -- tippen und zusehen
usbtest dienst ein   Dienst anlegen und starten
```

Der Treiber legt dafuer `\Device\UsbNt` mit Verweis
`\DosDevices\UsbNt` an; aus Win32 also `\\.\UsbNt`.

**Warum der Treiber die Arbeit tut und nicht das Programm:** ein
Benutzerprozess unter NT 4.0 kommt weder an CF8/CFC noch an den
Registerblock des Bausteins. Es gibt dafuer keinen Weg ohne
Kernbeteiligung; wer einen kennt, kennt eine Sicherheitsluecke.

`usbtest umgebung` braucht den Treiber nicht und beantwortet die Fragen,
an denen es in der Praxis haengt, **bevor** man ihn startet: steht der
Dienst, was steht im DEVICEMAP, und ist `ConnectMultiplePorts` gesetzt.

Die Zaehler sind so gewaehlt, dass sie ohne Kerndebugger etwas sagen:

```
Unterbrechungen == 0        Leitung falsch, Busmeister aus, oder der
                            Baustein wurde nie gestartet
Fadenlaeufe < Unterbrech.   waechst der Abstand, laeuft die Maskierung
                            im ISR nicht -- Sturm
ArbeitVerloren != 0         callout-Ring uebergelaufen; die Aufzaehlung
                            kann dadurch stehenbleiben
AnschlaegeVerworf != 0      kbdclass' Ring war voll
```

Der Treiber meldet seine ABI-Fassung; passt sie nicht zum Werkzeug,
bricht es ab, statt Strukturen zu lesen, die anders liegen.

## Massenspeicher: bewusst noch nicht

FreeBSDs `umass.c` haengt an CAM -- ein zweites Untersystem von der
Groessenordnung des USB-Kerns, und unter NT 4.0 ohne Gegenwert, weil NT
mit `scsiport` seine eigene Kette hat.

Der Weg ist ein eigener Klassentreiber gegen `usbdi`, der Bulk-Only
Transport spricht, und darueber ein `scsiport`-Miniport. Dabei ist zu
loesen, dass ein Miniport auf DIRQL gerufen wird und unser Stapel
PASSIVE_LEVEL braucht -- also Warteschlange hin, Abschluss per
`ScsiPortNotification` aus einem Zeitgeber. Das ist ein eigener Schritt,
kein Anhaengsel.

Und unabhaengig davon: **von USB booten geht nicht.** NTLDR und NTDETECT
haben keine USB-Unterstuetzung.

## Bauen

```
./bau.sh ../bau/src
```

Die clang-cl- und lld-link-Zeilen stehen als Kommentar in `bau.sh`.
`/merge:NTUDRV=.rdata` ist Pflicht -- ohne das kostet die
Treiberanmeldeliste eine eigene Seite.

## Was die Werkzeuglaeufe bisher gefunden haben

```
__packed und __aligned leer    USB-Deskriptoren waeren mit Fuellbytes
                               gelesen worden -- ohne ein Fehlerbit.
                               Gefunden an FreeBSDs eigener Zusicherung
                               sizeof(struct usb_device_id) == 32.
-D_KERNEL fehlte               usbdi.h faellt ab Zeile 90 weg
Funktionszeiger in void *      kein konstanter Ausdruck; die
                               Methodentabellen sind statisch
hz und ticks als Makro         in FreeBSD sind es Variablen
hz war 1000                    NT taktet bei rund 10 ms -> 100
driver_t mit eigenen Feldern   die Treiber fuellen .name/.methods/.size
```

Sechs Fehler, alle vom Uebersetzer, alle vor der Maschine. Das ist der
eigentliche Ertrag der Regel aus dem Skill, jede angenommene Lage als
Zusicherung zu hinterlegen.

## Naechster Schritt

Der PCI-Scan-Patch aus dem ersten Schritt. Er sagt, welche Bausteine
stecken, ob der Busmeister an ist und ob das BIOS noch emuliert. Danach
der erste Bau gegen echte `ntoskrnl.lib` und `hal.lib` -- der
Syntaxlauf sagt nichts ueber fehlende Symbole.

**Waehrend der ganzen Inbetriebnahme eine PS/2-Tastatur angesteckt
lassen.** In dem Augenblick, in dem der Hostbaustein uebernommen wird,
hoert die BIOS-Nachbildung auf.
