# Fassung 2026.11.17.66 -- offene Punkte behoben, Doppelfreigabe beim Hub-Abbau

- **Hub mit Geraeten dahinter abbauen: Detach auf freiem Speicher und
  doppelte Freigabe** (ntnewbus.c, device_delete_child).  Die Unterlage
  loeschte erst die Kinder, dann das Kind; FreeBSD tut es umgekehrt, und
  uhub_detach verlaesst sich darauf (es loescht seine Kinder selbst ueber
  usb_free_device -> usb_detach_device -> device_delete_child).  Folge:
  Detach-Routine und RemoveEntryList auf schon freigegebenem Speicher,
  "POOL doppelte Freigabe" fuer softc und Knoten.  Betroffen: Abziehen
  eines Hubs mit angeschlossenen Geraeten (auch Tastaturen mit
  eingebautem Hub) und Abkonfigurieren des Wurzelhubs.  Jetzt wie
  FreeBSD: erst abhaengen, dann Uebriggebliebenes loeschen.  Am Pruefstand
  beim Herunterfahren nachgewiesen (Hub + Tastatur am EHCI): vorher vier
  Doppelfreigaben und zwei fremde Zeiger, jetzt keine.  Das Abziehen
  eines Hubs selbst laesst sich in QEMU nicht pruefen (QEMU bricht mit
  einer Assertion ab).
- **Herunterfahren**: IoRegisterShutdownNotification an einem eigenen,
  unbenannten Geraet.  Je Baustein ohne Massenspeicher der FreeBSD-Weg
  (DEVICE_SHUTDOWN -> usb_shutdown -> usb_bus_shutdown: Kinder abhaengen,
  Wurzelhub abkonfigurieren, set_hw_power_sleep(SHUTDOWN)), danach Faden
  und Sicherungstakt beenden und die Register anhalten (EHCI: CONFIGFLAG
  0).  Bausteine mit Massenspeicher (auch hinter Hubs) bleiben an: NT 4.0
  leert die Dateisysteme erst in Phase 1, nach dem Rueckruf.
  bus_generic_shutdown war ein leerer Stumpf und reicht DEVICE_SHUTDOWN
  jetzt an die Kinder weiter.  Erster Versuch, die Bausteine nur von aussen
  anzuhalten, war falsch (Zeitueberschreitungen, Neuaufzaehlung, "host
  controller halted" alle 50 ms) -- deshalb der FreeBSD-Weg.
- **UHCI-Filter 0x3F -> 0x1F**: HCHalted ist ein Zustand, kein Ereignis.
  Ein angehaltener UHCI nahm jede Unterbrechung eines Nachbarn an
  derselben Leitung an.
- **UHCI HSE/HCPE**: lassen sich ueber UHCI_INTR nicht sperren.  Der ISR
  quittiert sie im gesperrten Zustand selbst, der Faden meldet sie.
- **Grafisches Setup ohne USB-Quelle**: nach 45 s ohne jeden
  USB-Massenspeicher nicht mehr bis 3 min weiter warten.
- xHCI-HSE (vorher als offen gefuehrt): kein Fehler.  HSE meldet sich laut
  xHCI-Spezifikation ausserhalb der Unterbrechungsleitung (SERR), nicht
  ueber den Interrupter; den Filter darum zu erweitern, nahm nur einem
  Nachbarn Unterbrechungen weg.

Pruefstand: Start bis zur Anmeldung, Maeuse ab- und wieder angesteckt,
Herunterfahren und Neustart mit Stick am xHCI ("bleibt an") und Hub mit
Tastatur am EHCI.  pruefstand/ausschalten-tasten.sh meldet an und faehrt
ueber die Tastatur herunter.

## Seit wann die Fehler drin waren

| Fehler | eingefuehrt |
|---|---|
| Hub-Abbau: Kinder zuerst geloescht | schon im Quellstand vom 27.09.2026 (usb-nt4-komplett), Git: e2c08e0 |
| bus_generic_shutdown leer, kein Herunterfahren | ebenso |
| UHCI-Filter mit HCHalted, HSE/HCPE nicht sperrbar | ebenso |
| EHCI: Handler oeffnet USBINTR im gesperrten Lauf (.65 behoben) | ebenso (ISR-Sperrlogik) |
| Handler ungesperrt neben dem ISR (.65 behoben) | b4f60d9, 2026.11.15.56 (Sicherungstakt) |
| Abfragebetrieb ohne INTx-Abschaltung (.65 behoben) | b4f60d9, 2026.11.15.56 |
| HalDisplayString nach DriverEntry (.65 behoben) | b4f60d9, 2026.11.15.56 |
| ZwLoadDriver ohne Deklaration, DDK-Bau bricht (.65 behoben) | fc08ee9, 2026.11.15.54 |
| 3 min Warten ohne USB-Quelle | fcfd280, 2026.11.16.61 |
| EHCI-SMI nur nach BIOS-Uebernahme aus (.65 behoben) | 40f6e02, 2026.11.16.64 |

# Fassung 2026.11.17.65 -- Durchsicht der Fassungen .52 bis .64 (Host-Controller)

Durchgesehen: a55e942 bis 40f6e02 (Abfragebetrieb, Sicherungstakt, PCI-Suche,
BIOS-Uebergabe/SMI, Setup-Warten).  Gefunden und behoben:

- **Sicherungstakt lief den FreeBSD-Handler ungesperrt neben dem ISR.**
  Seit .62 weckt der 50-ms-Takt den Faden auch ohne Unterbrechung.  Der
  Handler lief dann mit offener Freigabe; sperrt der ISR mitten im Lauf,
  oeffnet der Handler gleich wieder (xhci_interrupt schreibt IMAN mit dem
  vorher gelesenen IE zurueck).  Folge: Leitung oben, ISR meldet
  "gesperrt, nicht meins" -- auf einem Prozessor Haenger.  Jetzt sperrt der
  Faden vor jedem Handlerlauf selbst (KeSynchronizeExecution, nur wenn der
  ISR nicht schon gesperrt hat).  ntintr.c.
- **EHCI/xHCI: Handler oeffnet die Freigabe waehrend des gesperrten Laufs.**
  Auch ohne Takt: ehci_interrupt (PCD) und ehci_pcd_enable schreiben
  USBINTR = sc_eintrs, waehrend wir gesperrt haben.  Der ISR prueft jetzt
  im gesperrten Zustand, ob Freigabe UND Ereignis anliegen, und sperrt dann
  nach (Saved uebernimmt den vom Handler gewollten Wert), statt FALSE zu
  melden.  Nur EHCI und xHCI; UHCI/OHCI-Handler fassen den Hauptschalter
  nicht an.  Zaehler Nachsperre je Baustein.
- **Abfragebetrieb: INTx abschalten** (Befehlsregister Bit 10).  Sonst
  zieht der Baustein seine Leitung weiter -- war sie nur deshalb nicht
  anschliessbar, weil ein anderer Treiber sie exklusiv haelt, haengt dessen
  Leitung dauerhaft.  ntgeraet.c.
- **EHCI: SMI auch abschalten, wenn das BIOS den Baustein nicht haelt**
  (der Kommentar sagte "immer", der Code tat es nur nach Uebernahme; Linux
  tut es immer).  ehci_pci.c.
- **Bau mit dem NT-4.0-DDK ging nicht mehr:** ZwLoadDriver ist dort nicht
  deklariert (nur in den freien Koepfen).  Vorausdeklaration in ntumass.c;
  der Export ist in NTOSKRNL.EXE 4.0 vorhanden.
- Diagnosebau NTU_SETUP_BILDSCHIRM: HalDisplayString nach DriverEntry
  abgeschaltet -- spaeter holt es sich den Bildschirm vom Setup zurueck.
  ntu_setup_laeuft liegt in INIT.

Pruefstand (QEMU, NEC-xHCI + ICH9-EHCI mit drei UHCI + OHCI, Tastatur,
Tablet, Maus, Stick): bis zur Anmeldung, alle sechs Bausteine angehaengt,
verl 0.  Ebenso im erzwungenen Abfragebetrieb (-DNTU_ABFRAGE_ERZWINGEN=1).
Echte Hardware: noch nicht.

Hinweis Pruefstand: einbau.py schreibt den SYSTEM-Hive mit hivex; mit dem
aktuellen hivex lehnt NTLDR ihn ab und nimmt SYSTEM.ALT -- es lief dann
still der ALTE Treiber.  pruefstand/tausch.sh ersetzt nur usbnt.sys und
laesst die Registry unberuehrt.

Offen (unveraendert): kein Herunterfahren-Rueckruf; xHCI-HSE vom Filter
nicht erkannt; im grafischen Setup wird auch ohne USB-Quelle bis 3 min
gewartet.

# Fassung 2026.11.16.64 -- Setup schaltet nicht mehr alle SCSI-Miniports ein

EliteDesk 705 G5, Ereignis 7026 nach der Installation: rund 30 alte
SCSI-Treiber (Aha154x, Sparrow, ...) scheitern bei jedem Start.  In QEMU
nachgestellt: nach dem Textmodus-Setup stehen alle Miniports aus
[SCSI.Load] auf Start 0.

Ursache (gdb am QEMU-Kern): ntum_cdfs_laden rief ZwLoadDriver(Cdfs) schon
waehrend der Starttreiber-Phase des Textmodus.  ZwLoadDriver ruft am Ende
die Nachinitialisierungsroutinen aller Treiber auf -- auch die von
setupdd, die anhand von LDRP_FAILED_BUILTIN_LOAD die gescheiterten
Miniports aus ihrer Liste streicht.  Zu dem Zeitpunkt war noch keiner
gekennzeichnet, also blieben alle stehen.  Jetzt im Textmodus kein
ZwLoadDriver (setupldr laedt Cdfs dort selbst).  Nachher: 29 gestrichen,
1 behalten (uniata am AHCI von q35).

# Fassung 2026.11.16.63 -- BIOS-Uebergabe: SMI abschalten

xhci_pci.c / ehci_pci.c: gibt das BIOS den Baustein nicht frei (705 G5 im
CSM), wird das BIOS-Semaphor zwangsweise geloescht und in USBLEGCTLSTS
jede SMI-Freigabe abgeschaltet (xHCI: Statusbits quittiert).  Sonst kann
das BIOS weiter per SMI in den Baustein greifen, waehrend wir ihn fahren.
In QEMU ohne Wirkung pruefbar (Installation laeuft durch); auf dem 705 G5
noch nicht erprobt.

# Fassung 2026.11.15.58 -- Lesefehler am USB-DVD wiederholen

EliteDesk 705 G5, Textmodus-Setup vom USB-DVD-Laufwerk: mehrere Dateien
"konnten nicht kopiert werden" (z.B. qv.sys), beim erneuten Versuch ging
es.  ntum_lesen machte genau einen Versuch.  Jetzt bis zu 5 Versuche mit
250 ms Pause; nicht bei Sense 06 (Medium gewechselt) oder abgezogenem
Geraet.  Jeder Fehlversuch wird gemeldet (im Setup auf dem Bildschirm).

# Fassung 2026.11.15.57 -- Ryzen-USB hinter einer Bus-Luecke

Zweiter Lauf auf dem EliteDesk 705 G5 (Abfragebetrieb aktiv): der
Chipsatz-xHCI 1022:43d2 lief, Tastatur, Maus und Bluetooth wurden
gefunden -- das USB-DVD-Laufwerk an der vorderen Buchse nicht.  Die
Buchse haengt an einem xHCI der Ryzen-APU; den fand die PCI-Suche nie,
weil sie hinter Bus 8 beim ersten leeren Bus aufhoerte.  Jetzt alle 256
Busse.  QEMU: qemu-xhci hinter pxb-pcie auf Bus 13 -- gefunden, Setup
startet.

BIOS-Uebergabe (xhci_pci.c, ehci_pci.c): 100 statt 500 Runden.  Unter
NT runden die 10-ms-Pausen auf den Systemtakt, 500 Runden waren ~10 s je
Baustein; das BIOS des 705 G5 gibt im CSM nie frei.

Setup-Diagnose: Wartezeit 30 s mit Zwischenmeldung alle 5 s; ohne
Laufwerk eine gefilterte Zusammenfassung und 90 s Pause zum Abfotografieren.

# Fassung 2026.11.15.56 -- Abfragebetrieb ohne IRQ-Leitung

HP EliteDesk 705 G5 (Ryzen 5 PRO 3400GE, CSM): Setup blieb nach dem
Startbanner stehen.  Bildschirmdiagnose (neu, siehe unten) zeigte:

    usb: 01:00.0  1022:43d2  xhci  irq 255
    usb: 05:00.4  10ec:816d  ehci  irq 31
    usb: IoConnectInterrupt auf Leitung 31: c000000d

Das BIOS traegt im CSM-Betrieb nur noch APIC-Leitungen ein (31) oder
gar keine (255).  Unter der PIC-HAL ist beides nicht anschliessbar; der
Treiber brach das Einrichten ab und blieb im Abbau haengen.

**Abfragebetrieb.**  Ist die Leitung 0/255, liefert HalGetInterruptVector
nichts oder schlaegt IoConnectInterrupt fehl, weckt ein periodischer
Zeitgeber (KeSetTimerEx, Systemtakt 10-15 ms; ExSetTimerResolution gibt
es in NT 4.0 nicht) den Unterbrechungsfaden.  Der FreeBSD-Handler prueft
den Baustein ohnehin selbst; Sperren/Freigeben entfaellt.  Die
ResourceMap meldet dann keinen Interrupt.  Pruefstand: mit
`-DNTU_ABFRAGE_ERZWINGEN=1` in QEMU erzwungen -- Textmodus-Setup,
Kopieren und GUI-Setup vom USB-DVD-Laufwerk laufen.

**Bildschirmdiagnose.**  Mit `-DNTU_SETUP_BILDSCHIRM` schreibt der
Treiber im Textmodus-Setup jede Zeile mit Zeitstempel per
HalDisplayString unter das Startbanner -- fuer Rechner ohne COM.

**"noname" in der Statuszeile.**  `[Extenders]` bekommt
`usbnt = "USB-Unterstuetzung"` (usb_einbinden.py).

# Fassung 2026.11.15.54 -- Installation vom USB-DVD-Laufwerk

Gefunden auf einem HP EliteDesk 705 G5 (Ryzen, kein eingebautes
Laufwerk): Textmodus-Setup vom USB-DVD-Laufwerk endete mit Stop 0x7B
(0xC0000034).  In QEMU nachgestellt (xHCI, usb-bot + scsi-cd, kein
SATA-Laufwerk): Setup sucht `\ArcName\multi(0)disk(0)cdrom(224)`.

**Im Textmodus laden.**  `TXTSETUP.SIF [Extenders.Load]` bekommt
`usbnt = usbnt.sys,4`; SETUPLDR laedt den Treiber als Starttreiber.

**Auf das Laufwerk warten -- nur im Setup.**  NT legt die ARC-Namen
gleich nach den Starttreibern an (IopCreateArcNames); unsere Aufzaehlung
laeuft im Hintergrund.  `DriverEntry` wartet deshalb, wenn der Dienst
`setupdd` existiert (gibt es nur in der Textmodus-Registry), bis ein
USB-CD-Laufwerk angelegt ist, hoechstens 15 s (`NTU_SETUP_WARTEN`).
In QEMU nach 1,25 s da.  Im installierten System wird nie gewartet.

**Tastatur im Setup.**  `SETUPREG.HIV` kennt kbdclass ohne Parameter
(nur KeyboardPort0).  usbnt startet vor i8042prt und bekommt die 0 --
eine PS/2-Tastatur waere verloren.  Daher dort `MaximumPortsServiced 2`,
`ConnectMultiplePorts 1` (auch Mouclass).

**CD-Dateisystem nachladen.**  Setup schaltet `Cdfs` ab (Start 4), wenn
cdrom.sys kein Laufwerk findet -- das grafische Setup bei jedem Start
erneut.  Ohne Cdfs wird die Scheibe RAW: "Der Parameter stimmt nicht".
ntumass laedt jetzt beim ersten CD-Laufwerk `Cdfs` per `ZwLoadDriver`
nach, falls `\Cdfs` fehlt (Arbeitsauftrag).  Hilft auch im
installierten System ohne SATA-/IDE-Laufwerk.

Getestet in QEMU (q35, xHCI, nur USB-DVD, USB-Tastatur an die Konsole
gebunden, USB-Maus aktiv): Textmodus, Kopieren, GUI-Setup, Anmeldung,
Explorer auf D:\I386 -- vollstaendig ohne PS/2-Eingaben und ohne
SATA-Laufwerk.  `cd-einbindung/usb_einbinden.py` macht alle Schritte.

# Was der QEMU-Lauf gefunden und geaendert hat

Getestet gegen ein echtes NT 4.0 SP6 (das hochgeladene Abbild) mit
einem emulierten NEC uPD720200 -- derselben xHCI-Bauart wie die
Steckkarte im Projekt.

## Drei echte Fehler, alle erst auf der Maschine sichtbar

**Eigene serielle Ausgabe.**  Der Standardkern hat kein
KiSerialPutString; der Treiber haette gar nicht geladen.  Die Unterlage
schreibt jetzt selbst auf COM1 (eigener 16550-Treiber).  Fuer den
selbstgebauten Kern weiterhin KISERIAL=1 beim Bauen.

**Dienstgruppe "Keyboard Port".**  Ohne Gruppe laedt der Treiber nach
kbdclass -- und kbdclass zaehlt HARDWARE\DEVICEMAP\KeyboardPort nur
einmal, in seinem eigenen DriverEntry.  Die USB-Tastatur waere auf jedem
System tot gewesen, obwohl "Anschluss angelegt" im Bericht stand.  Die
Anschluesse entstehen jetzt fest in DriverEntry, nicht beim Aufzaehlen.

**Unterbrechungs-Livelock (der schwerste).**  Der Faden loeste erst die
Quelle am Baustein und dann die eigene Sperre.  Faellt zwischen beiden
Zeilen ein Ereignis an, sieht der ISR die Sperre noch stehen, meldet
"nicht meins", die pegelgesteuerte Leitung bleibt stehen, NT ruft sofort
wieder -- und auf einem Prozessor kommt der Faden nie mehr dran, die
Sperre zu loeschen.  Das Startbild stand minutenlang, der Herzschlag kam
nie.  Jetzt: erst Sperre loesen, dann Quelle freigeben.

## Ergebnis

xhcitest im Gast, ueber den Agenten gelesen:

```
NEC uPD720200 rev 03, 1033:0194
Anschluss 1  usb3  CCS PED PP   Stick, SuperSpeed
Anschluss 6  usb2  CCS PED PP   Tastatur
Anschluss 7  usb2  CCS PED PP   Maus
78 Unterbrechungen, 78 Fadenlaeufe, 0 fremd, 0 verloren
```

## Zweite Runde: alle Geraete bereitstellen

Neu: `quelle/ober/ntugen.c` stellt jede nicht beanspruchte Schnittstelle
als `\Device\UsbDevN` bereit, Vertrag in `ntusbgen.h`, Werkzeug
`usbgeraet.exe`.  Siehe GERAETE.md.

Zwei Fehler, die erst der Lauf gezeigt hat -- beide derselben Art:
etwas wurde **still verfaelscht statt gemeldet**.

**Falsche Fehlerwerte in der oeffentlichen Kopfdatei.**  Aus dem
Gedaechtnis eingetragen: STALL 4, ZEIT 18, KURZ 20.  Wirklich sind es
22, 20, 21 -- ZEIT und KURZ waren vertauscht.  Ein Joysticktreiber haette
jede Leerlauf-Zeitueberschreitung fuer ein harmloses Kurzpaket gehalten;
das alte Werkzeug hat es im Lauf genau so vorgefuehrt.  Jetzt aus dem
Stapel uebernommen und mit CTASSERT gegen usb_error_t gesichert.

**Abgeschnittene Befehlszeile.**  `ntbasis.c` nahm hoechstens 23
Argumente und liess den Rest weg.  Von einem 31-Byte-Befehlsblock kamen
18 an, das Speichergeraet sperrte den Endpunkt.  Jetzt 256, und darueber
eine Meldung statt eines verstuemmelten Aufrufs.  Derselbe Fehler stand
im Agenten bei 600 Zeichen; auch dort jetzt Meldung statt Kuerzung.

Der Agent kann jetzt `PUT` und `GET` -- Dateien roh ueber die Leitung,
byte-genau geprueft.  Neue Werkzeuge kommen damit ohne Neustart in den
Gast.

## Dritte Runde: der SB7x0-Aufbau, Abziehen im Betrieb

Nachgestellt wie auf dem echten Brett: ein EHCI mit sechs Buchsen, zwei
OHCI als Begleiter (QEMU `ich9-usb-ehci1` + `pci-ohci`, masterbus).
Treiber `usbnt.sys`.

**Die Begleiteruebergabe funktioniert.**  EHCI haengt zuerst an, dann
die OHCI.  Der Stick laeuft mit 480 MBit am EHCI; Tastatur und Maus
(reines Full Speed) gibt der EHCI an den OHCI-Begleiter ab, beide mit
kbdclass und mouclass verbunden.

**Die geteilte Leitung funktioniert.**  Beide OHCI auf IRQ 10:
`hc1 fremd 20` gegen `hc2 unt 20` -- jede Unterbrechung des zweiten hat
der erste gesehen und sauber abgewiesen, auf die Zahl genau.

### Ein Unterbrechungssturm am EHCI

17 Millionen abgewiesene Unterbrechungen in 40 Sekunden, bei 77 echten.
Der Herzschlag mit Zaehlern je Baustein hat es eingegrenzt: alle am
EHCI, alle waehrend er gesperrt war.

Ursache, im QEMU-Quelltext nachgesehen: `ehci_opreg_write` rechnet den
Leitungspegel beim Schreiben auf USBSTS neu, beim Schreiben auf USBINTR
**nicht**.  Die Sperre stellt USBINTR auf null -- und die Leitung blieb
oben, bis der Faden quittierte.  UHCI und OHCI haben die Eigenheit
nicht; dort wird beim Schreiben der Freigabe neu gerechnet.

Abhilfe: nach dem Sperren und nach dem Freigeben USBSTS mit NULL
beschreiben.  Die Statusbits dort sind write-1-to-clear; eine Null
aendert nach der EHCI-Norm nichts.  Auf echtem Silizium ist die Leitung
ohnehin die Verknuepfung aus Status und Freigabe.  Die Zeile kostet
einen Buszugriff und macht die Sperre unabhaengig davon, wie genau eine
Umgebung die Norm nimmt.  Danach: `fremd 0`, und die Aufzaehlung ist
in Sekunden statt Minuten durch.

### Abziehen im Betrieb

```
Tablett abgezogen, waehrend gelesen wird   -> "Geraet abgezogen"
                                              (NTUG_FEHLER_WEG), kein
                                              Absturz, Eintrag weg
Tablett viermal an und ab                  -> nach jeder Runde wieder
                                              genau 831 offene Bloecke,
                                              kein Leck
Tastatur ab und wieder an                  -> 10 Anschlaege vorher,
                                              +8 fuer "welt" danach
Maus ab und wieder an                      -> 5 Meldungen: drei
                                              Bewegungen, Druecken,
                                              Loslassen
```

### Zwei Fehler im Pruefaufbau, nicht im Treiber

Die Tastatur schien vorher nie Anschlaege zu liefern.  Grund:
`input-send-event` erwartet bei `device` das ANZEIGEgeraet, an dessen
Konsole die Tastatur gebunden ist -- nicht die Tastatur selbst.  Mit
`vga0` statt `ukbd` kamen alle Anschlaege an.  Und `usb-mouse` hat gar
keine `display`-Eigenschaft; Maeuse schaltet QEMU ueber `mouse_set`.

Beides ist hier vermerkt, weil es ein Jahr spaeter wieder genauso
aussehen wird wie ein Treiberfehler.

## Vierte Runde: UHCI, Hubs, isochron, und zwei Fehler im xHCI-Pfad

### Nachgewiesen

Intel-Aufbau (ICH9-EHCI, drei UHCI-Begleiter, externer Hub): alle drei
UHCI haengen an, der Hub wird an den UHCI abgegeben, dahinter Tastatur,
Tablett, FTDI.  Daten fliessen: Anschlaege, Mausmeldungen, Tablett exakt.
Leitungsteilung zwischen EHCI und UHCI auf einer Leitung: `fremd 19`
gegen `unt 19`.  Damit laufen alle vier Bauarten im Betrieb.

Isochron (`IOCTL_NTUG_ISO`): QEMUs USB-Audiogeraet, Ausgabe auf dem Wirt
mitgeschnitten, 100 Rahmen zu 192 Byte -- **bitgenau** an UHCI, OHCI und
xHCI.  (Erst sah es nach Verfaelschung aus; QEMUs Lautstaerkestufe rechnet
mit 240/255.  Mit dem Regler auf Maximum ueber eine Klassenanfrage: null
Abweichungen.)

### OHCI: gesendete isochrone Rahmen galten als leer

Die OHCI-Norm setzt bei einem erfolgreich GESENDETEN isochronen Rahmen
das Groessenfeld auf 0.  FreeBSDs `ohci_isoc_done` nimmt es als Laenge.
Jeder Ton galt als "nicht gesendet".  Trifft die echte SB7x0 genauso.
Die Schnittstelle meldet dort jetzt die angeforderten Laengen und
kennzeichnet sie als geschaetzt.

### xHCI: der Baustein zeigte in freigegebenen Speicher

Isochron am xHCI: Erfolg und Zeitlimit im Wechsel.  QEMUs
Ereignisprotokoll: beim zweiten Aufruf ein `ep_kick`, aber kein einziges
`fetch_trb`.  Ursache: nach einer Uebertragung zeigt der Ringzeiger des
Bausteins auf das letzte LINK-TRB -- im Speicher dieser Uebertragung.
Die erste Fassung baute jede Uebertragung nach dem Aufruf ab.  Die
naechste bekam denselben Speicher, und das LINK dort zeigte an der neuen
TD vorbei.  **Echte Hardware folgt demselben Zeiger.**  Abhilfe:
gehaltene Uebertragungen, eine je Endpunkt.

Dabei: die Rahmenobergrenze war ungeprueft 128; FreeBSD erlaubt bei
Full Speed 120.  Jetzt 120, per CTASSERT gegen den Stapel gesichert.

### xHCI: Abziehen wurde nie bemerkt

FreeBSDs xHCI-Treiber stoesst die Aufzaehlung NUR ueber `USBSTS.PCD`
an; das Ereignis `PORT_STATUS_CHANGE` wird abgeholt, aber nicht
ausgewertet.  QEMU 8.2 setzt PCD nirgends (im Quelltext nachgesehen) --
entgegen der xHCI-Norm 5.4.2.  Unter QEMU wurde am xHCI nach der ersten
Aufzaehlung keine Anschlussaenderung mehr bemerkt.

Die NEC-Karte setzt PCD normgerecht.  Aber ein nicht bemerktes Abziehen
ist der schlimmste Fehler, den ein USB-Treiber haben kann, und Linux
verlaesst sich aus gutem Grund nicht auf das Bit.  `ober/ntxhcipcd.c`
liest nach jedem Handlerlauf die Aenderungsbits aller Anschluesse und
stoesst bei Bedarf `usb_needs_explore` an.  Danach am xHCI: abziehen in
Ruhe, abziehen beim Lesen, wieder anstecken -- Pool jedes Mal zurueck
auf genau denselben Stand.

**Korrektur einer frueheren Aussage:** in der Runde davor stand, der
xHCI "verstumme" und die gehaltenen Uebertragungen seien vermutlich
schuld.  Beides war falsch.  Die Zeitlimits waren richtige Zeitlimits
eines ruhenden Tabletts, und das Abziehen am xHCI hatte ich vorher
schlicht nie geprueft.

### Neue Diagnose: wer wartet worauf

Jede Wartestelle der Unterlage traegt den wartenden Faden in eine Liste
ein, mit Objekt, Zeitpunkt und Halter.  Der Herzschlag (HERZ=1) meldet
jeden Faden, der laenger als 3 s wartet -- ohne den normalen Leerlauf
der Prozessfaeden.

## Fuenfte Runde: AMD-PLL-Abhilfe

Auf SB700 (bis Revision 0x3B), SB800, Hudson2 und Bolton faehrt die
A-Link-Bruecke im Leerlauf eine PLL herunter; waehrend eines isochronen
Stroms -- Ton, Kamera -- gibt das Aussetzer.  Linux schaltet die
Stromsparfunktion ab, solange ein isochroner Strom laeuft.

`nt/ntamdpll.c` ist Zeile fuer Zeile nach Linux geschrieben
(`drivers/usb/host/pci-quirks.c`): Erkennung ueber den SMBus-Baustein
1002:4385 und seine Revision, Northbridge 1022:9601 / 1022:1510 /
1022:9600, dieselbe Registerfolge, derselbe Referenzzaehler.  Gilt nur
fuer Hostbausteine der AMD-Bruecke selbst (Hersteller 1002/1022) -- die
NEC-Steckkarte ist nicht betroffen.

Eingeschaltet wird, sobald eine gehaltene isochrone Uebertragung auf so
einem Baustein entsteht; ausgeschaltet beim Wechsel auf Alternative 0
(so beendet ein Audiotreiber seinen Strom) oder beim Abziehen.

**Wichtig fuer das echte Brett:** eine SB700 mit Revision bis 0x3B
braucht die Abhilfe, eine SB710 oder SB750 nach Linux nicht.  Der
Treiber meldet beim Start auf COM1, was er gefunden hat:

```
amdpll: SB700 Rev 3a -- Abhilfe AKTIV, Northbridge RS780
amdpll: SB700 Rev 3c -- nicht noetig
amdpll: keine AMD-Suedbruecke -- nicht noetig
```

### Wie das ohne AMD-Hardware geprueft ist

- **Trockenlauf** (`EXTRA="-DNTU_AMDPLL_TROCKEN -DNTU_AMDPLL_ERZWINGEN=700"`):
  eine SB700 Rev 0x3A mit RS780 vorgetaeuscht, jeder Port- und
  Konfigraumzugriff protokolliert statt ausgefuehrt.  Die Folge und jede
  Bitrechnung von Hand gegen Linux nachgerechnet: stimmt, fuer Beginn
  und Ende des Stroms.  Zwei isochrone Aufrufe -- ein Abschalten, ein
  Einschalten.
- **Freigabebau auf QEMU:** "keine AMD-Suedbruecke", null
  Registerzugriffe, isochron an xHCI und OHCI weiter bitgenau.

**Nicht geprueft:** der SB800-Pfad (Registerbasis ueber 0xCD6/0xCD7) im
Trockenlauf, und beide Pfade auf echtem Silizium.  Abschalten beim Bauen
mit `-DNTU_OHNE_AMDPLL`.

## Sechste Runde: Installer

`installer/` ist der fertige Installationsordner -- auf Diskette, CD oder
in eine Freigabe legen, als Administrator PROBE.BAT, dann EINRICHT.BAT.
Anleitung in `installer/LIESMICH.TXT`.

    USBINST                  Zustand
    USBINST probe [art]      von Hand starten, nichts fest eintragen
    USBINST ein   [art]      fest einrichten (Start 1, Gruppe "Keyboard Port")
    USBINST aus              entfernen, geaenderte Werte zurueck
       -seriell 0|1|2        Bericht aus / COM1 / COM2
       -neustart

Neu im Treiber: `Services\<dienst>\Parameters\Ausgabe` legt fest, wohin
der serielle Bericht geht (0 aus, 1 COM1, 2 COM2).  Gelesen, bevor das
erste Zeichen geschrieben wird -- an COM1 haengt auf NT-4.0-Rechnern oft
eine serielle Maus, und 115200 Baud haetten sie beim Booten abgewuergt.
Fehlt der Wert, bleibt es bei COM1 wie bisher.

### Auf einer frischen NT-4.0-Maschine durchgeprueft

- Zustand: erkennt Kbdclass ConnectMultiplePorts = 0 ("USB-Tastatur
  wird so nicht gelesen") und die eingerichtete serielle Maus.
- probe: laedt ohne Neustart, alle Geraete bereitgestellt.
- ein + Neustart: laedt beim Booten, kbdclass/mouclass verbunden.
  **Anmeldung ausschliesslich ueber die USB-Tastatur** -- Strg+Alt+Entf
  oeffnete das Anmeldefenster, Enter meldete an.
- -seriell 0: null Byte auf COM1, Treiber laeuft, Tastatur geht.
- aus + Neustart: Treiber laedt nicht mehr, Kbdclass zurueck auf 0,
  Mouclass auf 1, Sicherung aufgeraeumt.
- Die ausgelieferten Batchdateien selbst, in der Reihenfolge der
  Anleitung: ENTFERNE, PROBE, EINRICHT -- alles wie erwartet.
- Alle 33 Einfuhren des Installers gegen die echten NT4-DLLs geprueft.

### Ein Fehler, den der Test fand

Nach `aus` lud der Treiber beim naechsten Start noch EINMAL.  Ein
laufender Starttreiber wird von DeleteService nur vorgemerkt; geloescht
wird von der Dienstverwaltung -- und die laeuft nach dem Kern, der
Systemtreiber vorher laedt.  Jetzt setzt `aus` den Dienst zuerst auf
"Deaktiviert" (Start 4); den Wert liest der Kern.  Nachgeprueft: danach
keine einzige Treibermeldung mehr.

### Pruefaufbau

NT setzt AutoAdminLogon bei leerem Kennwort nach dem ersten Gebrauch
zurueck.  `agent/autologon.py` traegt es vor jedem Start neu ein.
`einbau.py keiner` baut ein Abbild ohne vorinstallierten Treiber.

## Siebte Runde: Tastenwiederholung (das "USB-Shim")

Rueckmeldung vom echten Einsatz: Tastatur und Maus gehen, aber gehaltene
Tasten kommen nur einmal.

**Ursache.**  Eine PS/2-Tastatur wiederholt selbst (Typematic); i8042prt
reicht das nur durch, und NT erzeugt keine eigenen Wiederholungen.  Eine
USB-Tastatur wiederholt NIE.  Die erste Fassung verwarf SET_TYPEMATIC
und setzte SET_IDLE(0) mit der Begruendung "NT wiederholt selbst" --
genau diese falsche Annahme war der Fehler.  Die Wiederholung
nachzubilden ist die Aufgabe eines USB-zu-PS/2-Shims; unter Windows 2000
tut es kbdhid.sys.

**Abhilfe** (ober/ntukbd.c): ein Zeitgeber je Tastatur, wie eine
PS/2-Tastatur: nur die zuletzt gedrueckte Taste, nach 500 ms im Takt von
30/s (Windows-Vorgabe); neue Taste -> diese, mit voller Verzoegerung;
losgelassen -> Schluss.  Umschalter und Feststelltasten wiederholen
nicht.  SET_TYPEMATIC wird gespeichert, QUERY_TYPEMATIC beantwortet.

Mehrkern-Sicherheit: Wiederholung (DPC) und Loslassen (USB-Faden) melden
unter DERSELBEN Sperre an kbdclass -- sonst koennte eine letzte
Wiederholung nach dem Loslassen ankommen, und die Taste bliebe fuer
Windows gedrueckt.  Beim Abziehen: Zeitgeber anhalten, eingereihten DPC
entfernen, Sperre einmal nehmen und freigeben.

**Nachweis** hinter USER, mit dem neuen EINGABE.EXE (Konsolenereignisse):
Taste A 2,5 s gehalten -> 1x Druecken, 51 Wiederholungen, 1x Loslassen.
(Rechnerisch ~60; der NT-Zeitgeber rundet 33 ms auf seine Taktung.)

### Maus: noch offen

Der Treiber meldet nachweislich richtig: Bericht 01 -> LEFT_BUTTON_DOWN,
Bericht 00 -> LEFT_BUTTON_UP, beide von mouclass angenommen.  Meine
Messung dahinter (GetAsyncKeyState) zeigte den Knopf danach dauerhaft
gedrueckt -- aber genauso bei der PS/2-Maus.  Die Messung taugt also
nicht; ob hinter mouclass etwas klemmt, ist damit weder bestaetigt noch
ausgeschlossen.

## Achte Runde: USB-Sticks als Laufwerk

Neu: `quelle/ober/ntumass.c`.  Massenspeicher mit Bulk-Only-Transport
(Klasse 08, Unterklasse 06/05/02, Protokoll 50) bekommen ein
NT-Plattengeraet `\Device\UsbPlatteN` und einen Laufwerksbuchstaben (der
erste freie ab D:); das Dateisystem bindet sich beim ersten Zugriff
selbst an.

**Warum kein SCSI-Miniport:** NT 4.0 hat kein Plug and Play; disk.sys
sucht Platten nur beim Booten.  Ein eigenes Plattengeraet (Muster: das
RAM-Disk-Beispiel des DDK) kann beim Einstecken entstehen und beim
Abziehen verschwinden.  **Warum nicht FreeBSDs umass:** der haengt an
CAM.  Bulk-Only und die paar SCSI-Befehle eines Sticks sind neu
geschrieben; der Stapel darunter ist unveraendert.

Aufbau: IRP_MJ_READ/WRITE -> Warteschlange -> Arbeitsfaden (PASSIVE) ->
SCSI READ(10)/WRITE(10) in Stuecken zu 64 kB -> Bulk-Only (Befehl, Daten,
Quittung) -> zwei gehaltene Bulk-Uebertragungen.  Partition aus dem MBR;
Sticks ohne Partitionstabelle am FAT-Bootsektor erkannt.  Schreibschutz
aus MODE SENSE.  Beim Abziehen: Bruecke kappen, DO_VERIFY_VOLUME setzen
(das Dateisystem gibt den Datentraeger frei), Buchstabe weg.

Nebenbei gefunden: `ntu_port_pass` beantwortete JEDE Anfrage mit
"Erfolg, 0 Byte" -- auch IRP_MJ_READ.  Fuer ein Plattengeraet waere das
verheerend gewesen (Dateisystem liest Nullen, haelt Schreiben fuer
gelungen).  Lesen/Schreiben/Leeren haben jetzt eine eigene Verteilung;
fuer alle anderen Geraete des Treibers lehnt sie Lesen/Schreiben ab.

`usbgeraet auswerfen X:` -- NT 4.0 kennt kein "sicher entfernen":
ausschreiben, sperren (mit Nachfassen, der Cache-Manager haelt nach dem
Schliessen noch Sekunden einen Verweis), Dateisystem abloesen.

### Nachgewiesen (NEC-xHCI, NT 4.0 SP6, FastFat)

- FAT16-Stick mit Partitionstabelle -> E:, Bezeichnung und
  Datentraegernummer wie auf dem Wirt formatiert, freie Bytes auf das
  Byte gleich.
- Schreiben: Binaerdatei, Ordner, Textdatei, NTOSKRNL.EXE (931.712 Byte,
  15 Stuecke) -- auf dem Wirt im Abbild byte-gleich, fsck.fat ohne
  Fehler.
- Superfloppy ohne Partitionstabelle: erkannt (Partition ab 0), lesbar.
- Abziehen, wieder anstecken: neuer Buchstabe, Dateien frisch vom Stick.
- Ohne Auswerfen gezogen: kein Haenger, kein Absturz; auf dem Stick nur
  das Dirty-Bit.  Mit Auswerfen: sauber, ohne Dirty-Bit.
- Pool vor dem Anstecken und nach dem Ziehen: exakt gleich.
- Freigabebau: lesen, schreiben, sofort auswerfen -- byte-gleich.

**Nicht nachgewiesen:** Ziehen MITTEN in einer USB-Uebertragung -- QEMU
war so schnell, dass der Cache vor dem Ziehen schon ausgeschrieben war.

## Neunte Runde: Kartenleser

**Mehrere Schaechte.**  Ein Kombi-Kartenleser ist EIN USB-Geraet mit
mehreren LUNs.  ntumass.c fragt GET MAX LUN (Klassenanfrage FE; Geraete
mit einer LUN duerfen sperren) und legt je LUN eine Einheit an: eigenes
Medium, eigenes NT-Geraet, eigener Buchstabe -- auch ohne Karte.  Alle
Einheiten teilen den einen Bulk-Only-Kanal; der Faden bedient sie
reihum.

**Kartenwechsel ohne Abziehen.**  Im Leerlauf alle zwei Sekunden TEST
UNIT READY je Einheit.  Entnommen -> Medium 0 und DO_VERIFY_VOLUME;
eingelegt oder gewechselt (UNIT ATTENTION) -> neu einlesen und
DO_VERIFY_VOLUME.  IOCTL_DISK_CHECK_VERIFY antwortet jetzt wie disk.sys
mit STATUS_VERIFY_REQUIRED, solange die Pruefung aussteht -- nur daran
merkt das Dateisystem einen Wechsel.

**Befund dabei:** QEMUs SCSI-Platte meldet einen leeren Schacht mit
05/25 (LOGICAL UNIT NOT SUPPORTED), nicht mit dem 02/3A echter Leser.
Die erste Fassung kannte nur 02/3A und bemerkte die entnommene Karte
nicht.  Jetzt gilt jedes NOT READY und 05/25 als leer.

### Nachgewiesen (QEMU usb-bot, zwei removable scsi-hd als LUN 0/1)

- beide Schaechte: D: (mit Partitionstabelle) und E: (ohne), lesbar;
- Karte aus Schacht 1 -> "E: LUN 1 entnommen", dir E: -> "Das Geraet
  ist nicht bereit.";
- andere Karte -> "eingelegt", E: zeigt die neue Karte (anderer Name,
  andere Datei, Inhalt richtig); D: unberuehrt;
- direkter Wechsel ohne Pause -> erkannt, E: zeigt die richtige Karte.

### Nicht nachgewiesen

- **Schreibschutz einer Karte:** der Versuch scheiterte an meinem
  QEMU-Befehl (die Karte wurde nur ausgeworfen, nicht schreibgeschuetzt
  eingelegt).  Der Pfad (MODE SENSE WP-Bit -> STATUS_MEDIA_WRITE_PROTECTED)
  ist ungeprueft.
- Ein Zugriff auf den leeren Schacht blockierte danach das aufrufende
  Programm -- vermutlich die NT-Meldung "Kein Datentraeger", die hinter
  einem offenen Systemdialog wartete.  Der Treiber lief weiter
  (Herzschlag, Leerlaufpruefung, keine haengenden Faeden).  Belegt ist
  die Erklaerung nicht.
- Der Freigabebau dieser Runde ist nur uebersetzt und gegen beide Kerne
  geprueft; getestet wurde mit dem Herzschlag-Bau (Unterschied: nur
  Protokollzeilen).

### Nachtrag: Schreibschutz, leerer Schacht -- jetzt nachgewiesen

Mit dem Freigabebau (damit ist auch der fehlende Rauchtest nachgeholt):

- **Schreibgeschuetzte Karte:** erkannt ("8 MB schreibgeschuetzt"),
  lesbar.  Schreiben -> NT zeigt "cmd.exe - Schreibschutzfehler: Der
  Datentraeger ist schreibgeschuetzt ..." mit Abbrechen/Wiederholen/
  Ignorieren -- dieselbe Meldung wie bei einer Diskette.  Abbrechen ->
  "0 Datei(en) kopiert", auf der Karte nichts angekommen.
- **Leerer Schacht:** "cmd.exe - Kein Datentraeger: Es befindet sich
  kein Datentraeger im Laufwerk ...".  Karte einlegen, Wiederholen ->
  das Programm bekommt seine Datei.

**Die "haengenden" Programme der Runde davor** waren genau diese
Meldungen: NT haelt das Programm an, bis jemand antwortet -- gewolltes
Verhalten.  Sie standen verdeckt hinter dem Dialog
"Dienstkontroll-Manager", der bei jedem Start der VM erschien.

**Pruefaufbau:** Der Dialog kam von zwei Starttreibern ohne Hardware in
der VM, laut Ereignisprotokoll (dumpel aus dem Resource Kit): `ati` und
`Sermouse`.  einbau.py schaltet sie im Pruefabbild ab.  Derselbe Dialog
hatte frueher schon die Mausmessung verfaelscht (Fokus).

Kleinigkeit: nach UNIT ATTENTION meldet der Bericht jetzt, was es
wirklich war -- entnommen, eingelegt oder gewechselt.

## Zehnte Runde: Maus in der sauberen VM

Ohne den Systemdialog neu gemessen, diesmal mit Windows' eigener
Fensterverwaltung statt eines eigenen Pruefprogramms: Zeiger ueber das
Tablett auf eine Titelleiste gesetzt, mit der USB-Maus gedrueckt,
gezogen, losgelassen.  Das Fenster folgte und blieb nach dem Loslassen
stehen.  "Dauerklick" funktioniert mit der USB-Maus.

Zurueckgezogen: `EINGABE maus` (GetAsyncKeyState -- klemmte auch bei
PS/2) und `EINGABE fenster` (das Pruefenster verschwand beim ersten
Klick, gestartet direkt wie ueber BG).  Beide waren im Diagnosepaket;
auf echter Hardware haetten sie falsche Befunde geliefert.  Die Anleitung
nennt stattdessen den Test, der traegt: ein Fenster an der Titelleiste
ziehen.

Relative Mausbewegungen sind unter NT nicht genau steuerbar
(Beschleunigung ab 6 Punkten, QEMU-Umrechnung).  Pruefaufbau:
`agent/absolut.py` setzt den Zeiger ueber das Tablett,
`agent/zeiger.py` schickt Maus-Monitorbefehle ueber eine Verbindung.

## Elfte Runde: NTAGENT als Dienst

Anregung: den Agenten im Hintergrund starten, als Dienst.  Das Fenster
der Konsolenfassung stand bei den Tests im Vordergrund und schluckte
Tastendruecke, die fuer Dialoge gedacht waren; und sie startete erst
nach der Anmeldung, die NT bei leerem Kennwort nach dem ersten Mal
wieder abschaltet.

Jetzt: `ntagent -einrichten` legt den Dienst an (automatisch,
LocalSystem), entfernt einen alten Run-Eintrag und startet ihn.  Ohne
Dienstverwaltung gestartet, laeuft er wie bisher als Programm.
Befehle laufen auf dem unsichtbaren Dienst-Desktop; `IA <befehl>`
startet gezielt auf dem sichtbaren Desktop (Tastatur-, Fenstertests).
"Beenden" setzt ein Flag, das die Leseschleife beim naechsten
Zeitablauf (50 ms) sieht.

Nachgewiesen: Agent antwortet am Anmeldebildschirm ohne Anmeldung; kein
Fenster auf dem Bildschirm, auch waehrend Befehlen; PUT/GET, BG; nach der
Anmeldung per USB-Tastatur IA-Tastentest wie vorher (51
Wiederholungen); net stop/net start ueber den Agenten selbst; einrichten
bei laufendem Dienst.  Alle Einfuhren gegen die NT4-DLLs geprueft.

Nicht geprueft: EINRICHT.BAT und ENTFERNE.BAT des Agentenpakets selbst im
Gast -- der Dienst wurde im Pruefaufbau offline eingetragen, -einrichten
nur bei schon laufendem Dienst aufgerufen.

Pruefaufbau: einbau.py traegt den Agenten als Dienst ein;
OHNE_ANMELDUNG=1 schaltet die automatische Anmeldung ab.  Mit
abgeschaltetem ati-Treiber zeigt NT nach der Anmeldung "Ungueltige
Anzeigeoptionen" (Rueckfall auf Standardaufloesung) -- fuer den Agenten
jetzt belanglos, er braucht keinen Fokus mehr.

## Zwoelfte Runde: Gesamtpruefung vor dem Test auf echter Hardware

Alle vier Pakete ausgepackt und so geprueft, wie sie ankommen.

**In Ordnung:** Die Treiber im Installer entsprechen bitgenau (bis auf
Zeitstempel und Pruefsumme) einem Neubau aus dem aktuellen Quelltext; der
Bau ist reproduzierbar.  Der ganze Installer-Ablauf aus dem ausgepackten
ZIP auf frischer Maschine (Aufbau wie SB7x0): PROBE, EINRICHT mit
Neustart, Anmeldung allein ueber die USB-Tastatur, Tastenwiederholung an
der OHCI-Tastatur, Stick schreiben und auswerfen (byte-gleich), ENTFERNE
mit Neustart (Treiber weg, Werte zurueck).

**Gefunden und behoben:**
1. Diagnosepaket: USBNT.SYS war veraltet (vor dem Speichertreiber).
   Neu gebaut aus dem aktuellen Quelltext, mit -DNTU_EINGABE_PROTOKOLL.
2. NTAGENT: ENTFERNE.BAT + EINRICHT.BAT verlor den Dienst -- die alte
   Datei war noch belegt.  -entfernen wartet jetzt aufs Anhalten;
   -einrichten haelt eine laufende Fassung an, kopiert sich selbst mit
   Wiederholung, startet.  Geprueft: Entfernen+Einrichten und
   Aktualisieren im Betrieb.
3. bau-clangcl.sh suchte die Ersatzkoepfe in einem privaten Verzeichnis
   ($DDK/sdk) statt in ./ddk-ersatz.  Mit einem frischen DDK waere der
   Bau gescheitert.  Behoben; Selbstbau mit einem DDK ohne dieses
   Verzeichnis: 36 Dateien, 0 gescheitert.  Unterschied zum
   ausgelieferten Treiber: nur drei eingebettete Dateipfade (__FILE__).

### Nachtrag: Einzelbauformen und Diagnosetreiber gestartet

Jede Bauform aus dem ausgelieferten ZIP auf frischem Abbild:

| Bauform | Aufbau | Ergebnis |
|---|---|---|
| OHCINT | ICH9-EHCI + 2 OHCI (SB7x0-artig) | Tastatur, Maus, FTDI; Stick und Tablett (USB 2.0) unsichtbar |
| EHCINT | derselbe | Stick (E:), Tablett; Tastatur, Maus, FTDI (USB 1.1) nicht |
| UHCINT | ICH9-EHCI + 3 UHCI + Hub | Tastatur, Maus, Tablett, FTDI, Audio hinter dem Hub; Stick (USB 2.0) unsichtbar |
| XHCINT | NEC-xHCI | Tastatur, Maus, Stick (E:) |
| Diagnose-USBNT | NEC-xHCI | wie USBNT, dazu ums:/ukbd:-Zeilen, kein Herzschlag |

Keine Panik, kein Haenger.  Das Fehlen der USB-2.0-Geraete bei OHCINT/
UHCINT allein ist kein Treiberfehler: das BIOS leitet schnelle Geraete
an den EHCI, und ohne EHCI-Treiber gibt sie niemand zurueck.  Die
Anleitung sagt das jetzt; fuer Bretter mit USB 2.0 gilt: USBNT nehmen.

## Dreizehnte Runde: "EINRICHT: Fehler 5" auf echter Hardware

Gemeldet: EINRICHT bricht mit Fehler 5 ab, obwohl als Administrator
angemeldet.

**Ursache (nachgestellt):** CopyFile uebertraegt den Schreibschutz der
Quelldatei.  Liegt der Installer-Ordner auf CD oder kommt er
schreibgeschuetzt aus einem Archiv, legt PROBE usbnt.sys schreibgeschuetzt
nach drivers; das naechste EINRICHT scheitert beim Ueberschreiben:
"CopyFile: Fehler 5 Zugriff verweigert -- als Administrator anmelden" --
eine irrefuehrende Meldung.  In meinen Tests kamen die Dateien ueber den
Agenten, also ohne Schreibschutz; deshalb nie gesehen.  COM1 war nicht
beteiligt -- `ein` oeffnet es nicht.

**Behoben:** usbinst hebt den Schreibschutz am Ziel vor jedem Kopieren auf
und an der Kopie danach auch.  Das Kopieren der Werkzeuge meldet Fehler
jetzt, statt still eine alte Fassung liegen zu lassen.  Die Meldung zu
Fehler 5 nennt auch Schreibschutz und offene Dateien.  Nachgewiesen:
aus genau dem Fehlerzustand heraus laeuft EINRICHT durch; Treiber im Gast
byte-gleich, keine Datei mehr schreibgeschuetzt.

NTAGENT: dieselbe Falle bei PUT auf eine schreibgeschuetzte Datei --
ebenso behoben, geprueft.  Nebenbei die zweite erfolgreiche Aktualisierung
des laufenden Dienstes ueber EINRICHT.BAT.

Abhilfe fuer eine alte Installer-Fassung:
    attrib -r %SystemRoot%\system32\drivers\*.sys
    attrib -r %SystemRoot%\system32\usbtest.exe   (usbgeraet, xhcitest)

## Vierzehnte Runde: Rueckmeldungen aus VirtualBox

Getestet vom Anwender in VirtualBox, USB-Controller "2.0", ein Prozessor:
Tastenwiederholung geht, Maus ziehen geht, Stick geht mit NTFS.  Mit
"USB 3.0" (VirtualBox-xHCI) haengt es beim Anschliessen von Tastatur und
Maus -- auf der Pruefliste.

**Stick "geht nicht" = FAT32.**  NT 4.0 kennt FAT32 und exFAT nicht; fast
jeder Stick ab etwa 4 GB kommt ab Werk so.  Das Laufwerk entstand, aber
ohne Hinweis.  Jetzt erkennt ntumass.c FAT32/exFAT am Bootsektor der
Partition (nicht allein am Partitionstyp) und meldet auf COM1:
"F: ist FAT32 formatiert -- das kann NT 4.0 nicht lesen.  Mit FAT16
(bis 2 GB) oder NTFS formatieren."  Nachgewiesen mit FAT16- und
FAT32-Stick, Herzschlag- und Freigabebau.  Anleitung: Absatz zu
Dateisystemen und dazu, dass der NT4-Explorer ein neues Laufwerk erst
nach F5 zeigt.

Entfernt: die Pruefzeile "pruefe LUN" aus der Kartenleser-Runde -- sie
verrauschte im Herzschlag-Bau das Protokoll.

## Fuenfzehnte Runde: Maus ueber den Berichtsdeskriptor

Bisher zwang ntums jede Maus ins Startprotokoll (3 Byte, 3 Tasten); das
Rad kam nur, wenn die Maus freiwillig ein viertes Byte schickte, und
Maeuse ohne Boot-Kennzeichen landeten roh bei UsbDevN.

Jetzt: ntums holt den Berichtsdeskriptor (FreeBSDs usbd_req_get_hid_desc)
und findet darin mit FreeBSDs Parser (hid_locate) X, Y, Rad, Tasten 1-3
und die Berichtsnummer; schaltet dann ausdruecklich ins
Berichtsprotokoll (1), weil ein BIOS die Maus fuers Booten ins
Startprotokoll gesetzt haben kann.  Bewegungen in 8, 12 oder 16 Bit.
Scheitert der Deskriptor: Startprotokoll wie bisher.  probe nimmt auch
Maeuse ohne Boot-Kennzeichen -- "Maus" im Deskriptor UND relatives X;
Grafiktabletts (absolut) bleiben bei UsbDevN, sonst spraenge der Zeiger.

FreeBSD-Baum: dev/hid/hid.c, dev/hid/hidquirk.h, dev/usb/usb_hid.c
unveraendert dazu.  Unterlage: struct malloc_type deklariert (sonst zwei
verschiedene Typen in usb_hid.c), opt_hid.h, sys/kdb.h, hid_if.h von Hand
(HID-Bus-Methoden, alle "nicht vorhanden"), INT*_MAX, systm.h ueber
kernel.h wie in FreeBSD.

**Nachgewiesen:** "Berichtsdeskriptor: 3 Knoepfe, Rad, Bericht 0";
Druecken/Ziehen/Loslassen und Rad +1/-1 richtig dekodiert (Vorzeichen);
Tablett weiter bei UsbDevN; in Notepad fuenf Rastschritte = genau 15
Zeilen gerollt (3 je Schritt, Windows-Vorgabe) -- WM_MOUSEWHEEL kommt an.
Freigabebau: 5 Meldungen an mouclass.  Nicht nachgewiesen: Maeuse mit
Berichtsnummer oder 16-Bit-Bewegung (QEMUs Maus hat beides nicht).

**Nebenbei gefunden:** bau-clangcl.sh setzte DDK=../ddk fest und
ueberschrieb die Umgebungsvariable -- der fruehere Selbstbau lief nur
zufaellig durch.  Jetzt DDK=${DDK:-../ddk}; Selbstbau aus einem fremden
Verzeichnis: 38 Dateien, 0 gescheitert.

## Sechzehnte Runde: Maustasten 4 bis 8 belegen

Anregung: hat eine Maus mehr Tasten, koennen die mit Funktionen belegt
werden.  NT 4.0 kennt nur drei Maustasten (XBUTTON kam mit Windows 2000),
aber der Treiber sieht die weiteren jetzt im Berichtsdeskriptor.

Neu: `ober/ntmaustasten.c` liest Parameters\Taste4..Taste8 (REG_SZ) und
zerlegt "alt+links", "strg+c", "f5", "code:e05b", "mitte", "doppelklick",
"aus".  Buchstaben nach deutscher Belegung (Tastencodes sind Positionen).
Vorgabe: Taste 4 = alt+links (Zurueck), Taste 5 = alt+rechts
(Vorwaerts).  ntkbdport.c: `ntu_kbd_einspeisen` gibt Tasten ueber die
feste Tastatur-Verbindung an kbdclass -- die besteht ab dem Booten, auch
ohne USB-Tastatur.  ntums.c: bis 8 Tasten im Deskriptor; bei jedem Wechsel
von 4..8 die Belegung; gehalten ist gehalten.  "mitte" wirkt als Taste 3
(auch Ziehen), "doppelklick" schickt vier Meldungen (links nieder/los x2).
`usbinst taste [4..8 belegung]` zeigt/setzt, prueft die Namen sofort.
ntbasis: strchr, strncmp dazu.

**Nachgewiesen** (QEMU-Maus mit 5 Tasten, im Deskriptor erkannt):
Taste 4 -> in Windows Alt+Links (VK 12 und 25 je einmal nieder/los, ohne
Wiederholung), Taste 5 -> Alt+Rechts (Freigabebau); "mitte" auf Taste 5:
MIDDLE_DOWN, Bewegung mit gehaltener Taste, MIDDLE_UP; "doppelklick" auf
Taste 4: genau 6 Meldungen; usbinst taste zeigt, setzt, lehnt "xyz" ab;
Belegung nach Neustart uebernommen.
**Nicht nachgewiesen:** dass USER die vier Doppelklick-Meldungen als
Doppelklick erkennt (am Bildschirm nicht geprueft); Tasten 6..8 (QEMUs
Maus hat 5).

Gefunden beim Testen: die Tippfehler-Meldung zeigte woertlich "%.*s" --
ntbasis' printf kennt das Format nicht.  Behoben.

## Siebzehnte Runde: Gamepads und Joysticks -- der Kernteil

Neu: `ober/ntujoy.c` + `ober/ntujoy.h`.  Nimmt HID-Geraete, deren
Deskriptor "Joystick" oder "Gamepad" sagt, und Xbox-360-Controller
(Klasse FF/5D/01, wie FreeBSDs uhid -- auch kompatible anderer
Hersteller).  Der Xbox-Controller ist kein HID; FreeBSDs nachgebauter
Deskriptor (HID_XB360GP_REPORT_DESCR, dev/hid/hidrdesc.h, unveraendert
dazugenommen) laesst ihn durch denselben Parser laufen.  Liest fortlaufend
und haelt den Zustand bereit: bis 8 Achsen auf 0..65535 (aus logischem
Min/Max), bis 32 Knoepfe, Hutschalter oder Steuerkreuz als POV in
Hundertstel Grad.  \\.\UsbJoyN, IOCTL_NTUJ_INFO / _ZUSTAND.  Xbox: nur
Eingabeberichte (00 14 ...), Y/Ry umgedreht (winmm: oben = klein), beim
Einstecken LED 1 an (01 03 06).  `joytest roh [n]` zeigt den Zustand.

**Pruefaufbau:** `agent/xpad.py` -- ein kabelgebundener Xbox-360-Controller
in Python, den QEMU ueber usbredir als echtes USB-Geraet sieht
(Deskriptoren 045E:028E, 20-Byte-Berichte, LED-Befehle protokolliert),
steuerbar ueber /tmp/xpad_ctl.sock ("setze a=1 lx=32767 ...").  QEMU
bildet sonst keinen Gamepad nach.

**Nachgewiesen** (NEC-xHCI, Controller hinter QEMUs Hub): erkannt als
"6 Achsen, 11 Knoepfe, Steuerkreuz, Xbox-Protokoll"; LED-Befehl 01 03 06
kam am Controller an; alle Achsen in beiden Richtungen (Y umgedreht),
Trigger voll/halb, alle Knoepfe mit der Nummerierung wie Windows,
Steuerkreuz in Haupt- und Diagonalrichtungen; Abziehen sauber (UsbJoy0
weg, kein haengender Faden); Freigabebau ebenso.
**Nicht nachgewiesen:** HID-Gamepads (kein Nachbau dafuer), echter
Controller.  Beobachtet: einmal "uhub_explore: illegal enable change,
port 1" am QEMU-Hub direkt nach dem Anstecken, danach lief alles.

Noch offen: usbjoy.dll fuer winmm (joyGetPosEx, joy.cpl, DirectInput).
NT 4.0s winmm.dll sucht den Treiber als "joystick" unter Drivers32 --
im Abbild nachgesehen.

## Achtzehnte Runde: Gamepads bei Windows -- usbjoy.dll

Neu: `werkzeug/usbjoy.c` -- installierbarer Joystick-Treiber fuer winmm
(DriverProc).  NT 4.0s winmm.dll laedt ihn als "joystick" aus Drivers32
(im Abbild nachgesehen).  Er fragt beim Kerntreiber nur den Zustand ab
(IOCTL_NTUJ_ZUSTAND) und verteilt die Achsen nach Art: X Y Z Rz Rx Ry ->
X Y Z R U V.  Eingebautes Protokoll (liegt C:\TEMP\usbjoy.log, wird jede
Nachricht angehaengt) -- damit nachgesehen, wie winmm wirklich arbeitet:

- es oeffnet den Treiber genau ZWEIMAL (DRV_OPEN mit 0 und 1) und ruft
  ihn fuer hoehere Nummern nie; joyGetDevCaps meldet dort "kein Fehler"
  mit leerer Struktur.  JDD_GETNUMDEVS antwortet deshalb 2, nicht 16.
- JOYCAPS kommt als Unicode-Fassung (728 Byte).

`usbinst ein` kopiert usbjoy.dll, traegt sie ein (ein fremder Eintrag
wird gesichert) und legt zwei Joystick-Typen an (OEM: "Xbox 360
Controller (USB)", "USB-Gamepad / -Joystick (usbjoy)") -- ohne Typ bleibt
in der Systemsteuerung "Testen" grau.  `usbinst aus` nimmt alles zurueck.
`joytest` fragt wie ein Spiel ueber winmm; `joytest folge` in EINEM
Prozess ueber Ab- und Anstecken hinweg.

**Nachgewiesen** (nachgebauter Xbox-Controller ueber usbredir):
joyGetNumDevs 2; Joystick 1 "Controller", 6 Achsen, 11 Knoepfe, Z R U V
POV; alle Achsen, Knoepfe, POV ueber joyGetPosEx richtig.  In einem
laufenden Prozess: nicht da -> da -> Knopf -> abgezogen (167) -> wieder
da -> Knopf.  Systemsteuerung "Joystick": Typ waehlbar, Testen zeigt
Fadenkreuz, Schub, Ruder, U, V, Coolie-Hat, Tasten -- und folgt, wenn sich
Stick, Trigger, Knoepfe, Steuerkreuz aendern.  usbinst ein/aus:
eintragen und restlos austragen.

**Nicht nachgewiesen:** DirectInput; das Zurueckschreiben eines fremden
Joystick-Treibers (keiner vorhanden); echter Controller.

Gefunden am eigenen Pruefaufbau: xpad.py bedient eine QEMU-Verbindung
zur Zeit -- ein vergessenes altes usb-redir-Geraet liess einen Abziehtest
falsch aussehen.  Beim Testen immer nur eines anstecken.

## Neunzehnte Runde: alle Tasten, und der Xbox-One-Controller

**"Nur 4 Tasten"** -- das war der Testdialog der Systemsteuerung von
NT 4.0: er zeigt grundsaetzlich Taste 1-4.  joyGetPosEx liefert bis 32,
und usbjoy.dll fuellt alle.  Nachgewiesen: alle 11 Tasten des 360 auf
einmal -> Maske 7FF im Kerntreiber UND ueber winmm.  Anleitung sagt es.

**Xbox One** (kabelgebunden, Klasse FF/47/D0, Schnittstelle 0): GIP statt
360-Protokoll.  Nach dem Linux-Treiber xpad:
- Einschalten beim Einstecken (05 20 seq 01 00, dazu 05 20 seq 0F 06 fuer
  den One S) -- ohne das schweigt er;
- Eingaben Typ 20 durch einen nachgebauten Deskriptor (wie FreeBSD beim
  360): Trigger 0..1023, Sticks 16 Bit, Y umgedreht; DIESELBEN
  Knopfnummern wie beim 360;
- Guide als eigene Nachricht Typ 07 -> Knopf 11; verlangt sie eine
  Quittung, geht sie zurueck (wie xpadone_ack_mode_report), sonst
  wiederholt der Controller sie.
Sendeschlange statt "LED einmal": LED (360), Einschalten, Quittungen.

xpad.py: `--one` bildet einen Xbox One S nach (045E:02EA), der bis zum
Einschalten schweigt und Guide mit Quittungswunsch schickt.

**Nachgewiesen:** erkannt als "Xbox-One-Protokoll (GIP)"; beide
Einschalt-Pakete am Controller; Stick, 10-Bit-Trigger (voll/halb), A +
Menue = 1 + 8, Steuerkreuz; alle Tasten + Guide = 7FF, Guide los = 3FF;
Quittungen mit der richtigen Folgenummer; Abziehen sauber; Freigabebau
(B RB RS Guide = 2 6 10 11).
**Nicht nachgewiesen:** echter Xbox-One-Controller (Firmware-Staende
unterscheiden sich; xpad kennt je nach Modell weitere Init-Pakete);
Funk-Adapter.

## Zwanzigste Runde: eigene Systemsteuerung "USB-Gamepads"

Der Testdialog der Joystick-Systemsteuerung von NT 4.0 zeigt nur Taste
1-4 und am Coolie-Hat nur vier Richtungen.  Neu: `werkzeug/usbjoycpl.c`
-> usbjoy.cpl (CPlApplet; NT 4.0 listet jede .cpl aus system32).  Zeigt
beide Sticks als Fadenkreuz mit Werten, LT/RT als Balken (voll = voll),
das Steuerkreuz mit acht Richtungen und Winkel, alle Tasten bis 32 --
beim Xbox-Controller mit Namen (Xbox-Art aus IOCTL_NTUJ_INFO; die Werte
selbst ueber joyGetPosEx, wie ein Spiel).  Joystick 1/2 umschaltbar
(auch mit den Tasten 1/2).  Symbol: 16 Farben, klassisches ICO -- NT 4.0
kennt weder PNG- noch 32-Bit-Symbole.  Doppelt gepuffert gezeichnet.

`usbinst ein` legt usbjoy.dll und usbjoy.cpl ab; sind sie belegt (ein
Spiel haelt die DLL, die Systemsteuerung die CPL), wird die neue Fassung
beim naechsten Start eingesetzt.  `usbinst aus` entfernt beide, belegt
ebenfalls beim naechsten Start.

**Nachgewiesen** (Xbox-One-Nachbau): zwei gespiegelte Zustaende --
Sticks, Trigger 100/50/29 %, Steuerkreuz 45 und 225 Grad, die jeweils
gesetzten Tasten mit Namen grün; Umschalten auf Joystick 2 ("nicht
angeschlossen") und zurueck; usbinst ein/aus mit belegten Dateien, nach
dem Neustart sind beide weg.

**Gefunden beim Testen:** (1) WS_CLIPCHILDREN fehlte -- das Zeichnen
uebermalte die Umschalter.  (2) Ohne SetFocus ging Alt+2 ins Fenster
dahinter.  Beides behoben.

**Nicht gezeigt:** das Symbol in der Liste der Systemsteuerung.  Die
Systemsteuerung stuerzt in der Test-VM grundsaetzlich ab -- explorer.exe,
Nullzeiger in WININET.DLL (Web-Ansicht des IE4) --, und zwar bevor sie
irgendein Element laedt: laut Dr.-Watson-Protokoll war in keinem der drei
Abstuerze usbjoy.cpl oder usbjoy.dll unter den 67 geladenen Modulen.

## Einundzwanzigste Runde: neues NT4-Abbild

Vom Anwender: das Abbild seiner VirtualBox-VM (VDI, 512 MB; NT 4.0
SP6a 4.0.1381.335, Einprozessor-Kern und -HAL, IE 6 SP1 6.0.2800.1106
durchgehend stimmig).  einbau.py nimmt es mit NT4_BASIS=neu; das Original
bleibt unberuehrt, gearbeitet wird auf Kopien.

In QEMU scheiterten dort nur VirtualBox-eigene Teile -- in den
Arbeitskopien abgeschaltet: AMDPCN (PCnet-Netzkarte), VBoxGuest, der
Computer-Suchdienst (haengt am Server-Dienst), und VBoxService/VBoxTray,
die abstuerzen: Ausnahme c000001e auf 0F AE, also SSE -- NT 4.0 schaltet
SSE nie frei, QEMUs Prozessor meldet es aber.  Danach startet es sauber.

**Nachgewiesen:** Systemsteuerung oeffnet ohne Absturz (die WININET-
Mischung des alten Abbilds gibt es hier nicht); "USB-Gamepads" steht in
der Liste, mit Symbol und Beschreibung; aus der Systemsteuerung heraus
geoeffnet zeigt es den Xbox-One-Nachbau richtig (Sticks, 68/100 %,
135 Grad, A B RB Back Guide).  Damit ist der Punkt der Pruefliste erledigt.

## Zweiundzwanzigste Runde: USB-Drucker -- der Anschluss

Nur die Anschluss-Seite; Spooler/Monitor/Druckertreiber gehen in einer
eigenen Arbeit weiter.  Beschreibung fuer die Uebergabe:
`DRUCKER-ANSCHLUSS.md`.

Neu: `ober/ntuprn.c`, `ober/ntuprn.h`.  Klasse 07/01/01 und 07/01/02 ->
\Device\UsbDruckN, \DosDevices\USBPRNn.  Schreiben per Bulk in 64-kB-
Stuecken ueber einen Arbeitsfaden; GET_DEVICE_ID, GET_PORT_STATUS,
SOFT_RESET als Steuercodes; 30 s je Stueck; IRP_MJ_CLEANUP bricht ab.
`usbdruck` zeigt Kennung und Status.  `agent/udruck.py` bildet einen
USB-Drucker ueber usbredir nach und schreibt Empfangenes in eine Datei.

**Drei Fehler beim Testen gefunden und behoben:**
1. CLEANUP brach ALLES ab -- ein Statusprogramm, das seinen Griff schloss,
   zerstoerte den laufenden Druck eines anderen.  Jetzt nur die Anfragen
   des schliessenden Dateiobjekts.
2. Nach einem Fehler setzte der Treiber den Endpunkt nur bei STALL
   zurueck; jetzt nach jedem Fehler ausser gewolltem Abbruch, wie FreeBSDs
   ulpt.
3. Im Nachbau (nicht im Treiber): abgebrochene Pakete wurden stumm
   verworfen statt mit "cancelled" beantwortet, wie usbredir es verlangt
   -- QEMU wartete darauf, und nach einer Zeitueberschreitung kamen je
   Druck nur zwei Stuecke durch.

Dazu im Pruefaufbau: agent.py brach das Hochladen grosser Dateien nach
2 s Stillstand ab (die serielle Leitung schafft ~11 kB/s); der Agent im
Gast wartete dann auf den Rest.  Senden jetzt ohne Zeitlimit -- auch im
Agentenpaket.

**Nachgewiesen:** siehe DRUCKER-ANSCHLUSS.md (1 MB byte-gleich; Papier
leer mit Statusabfrage dazwischen; Zeitueberschreitung und danach drei
Drucke; Abziehen im Druck; Freigabebau).

## Dreiundzwanzigste Runde: Rueckmeldung von CUPS4NT

Antwort Punkt fuer Punkt in DRUCKER-ANSCHLUSS.md (Fassung 2).  Kurz:

- **Nachbauten direkt am xHCI** (4.1): QEMU 8.2.2 verlangt dort 64-Bit-
  Kennungen.  Bei uns lief es nur, weil QEMU bei fast vollen Anschluessen
  selbst einen USB-1.1-Hub einschob.  udruck.py und xpad.py bieten jetzt
  64bits_ids (Bit 5) an und warten QEMUs HELLO ab, bevor sie sich melden --
  erst danach gilt der 16-Byte-Kopf.  agent/geraete_direkt.sh: xHCI mit
  freien Anschluessen.
- **Ein schreibender Griff** (4.3): zweiter -> 32; Griffe ohne
  Schreibzugriff immer.  Oeffnen/Schliessen gehen jetzt an den Drucker.
- **IOCTL_NTUP_INFO** (4.4, Code 3): VID/PID/bcdDevice, Seriennummer,
  Pfad, Protokoll, Wartezeit, Fassung 2, Aenderungszaehler.
- **Win32-Codes gemessen** (4.5): 121 Zeitueberschreitung, 995 CancelIo,
  21 Abziehen waehrend WriteFile, 2 CreateFile ohne Drucker.
- **SchreibWartezeit** (4.6), 0 = unbegrenzt; dafuer **CancelIo** mit
  Abbruch-Routine (vorher wirkungslos).  Sperrfolge Abbruch-Sperre vor
  x->Sperre; die Routine setzt fuer die laufende Anfrage nur sc_storno, der
  Faden sieht alle 250 ms nach (KMUTEX nicht bei DISPATCH_LEVEL).
- **Dot4** (4.8): Einstellung mit Protokoll 02 vor 01, wie ulpt.
- **Lokale Anschluesse** (4.2): USBINST ein legt \\.\USBPRN1..4 an (nur
  fehlende), merkt sich die eigenen, aus entfernt nur eigene und
  unbenutzte.  Spooler-Neustart: der sofort neu gestartete endete mit
  Fehler 12 -- jetzt warten, pruefen, dass er laeuft, bis zu zwei weitere
  Versuche.  Nach "aus" ohne Neustart: verstaendliche Meldung statt
  "CreateService: Fehler 1072".

Beim Umbau gefunden: eine beim Abziehen schon herausgenommene
Schreibanfrage wurde nie abgeschlossen.  Behoben.

Nachgewiesen: siehe DRUCKER-ANSCHLUSS.md; Freigabebau mit Info, einzelnem
Schreiber, 1 MB byte-gleich.

## Vierundzwanzigste Runde: Rueckkanal, Dot4 und High Speed geprueft

**Nachgeprueft** mit neuen Betriebsarten von udruck.py: `--dot4` (Einstellung
0 = Protokoll 03, 1 = 02 -- der Treiber schaltet per SET_INTERFACE auf 1;
der Nachbau meldet dabei wie libusbredirhost Schnittstelle und Endpunkte
neu), `--nurdot4` (bleibt UsbDev0), `--hs` (480 Mbit/s, 512 Byte).  Alle
byte-gleich bzw. wie erwartet.

**Rueckkanal** (Rueckmeldung 4.7): ReadFile bei Protokoll 02, eigener
Faden, eigene Bulk-IN-Uebertragung, eigene Warteschlange; kehrt zurueck,
sobald etwas da ist; nach LeseWartezeit (Vorgabe 2000 ms) mit 0 Byte ohne
Fehler.  Keine Antwort ist beim Rueckkanal der Normalfall -- der Endpunkt
wird dann nicht zurueckgesetzt.  Abbruch-Routine unterscheidet laufendes
Schreiben (1) und Lesen (2).  NTUP_FASSUNG 3, NTUP_INFO.Rueckkanal (aus
einem freien Byte).  udruck.py beantwortet Leseanfragen aus
`antwort <text>` und von selbst auf "@PJL INFO STATUS".  usbdruck: lies,
pjl, lesestorno.

Nachgewiesen: 0 Byte nach ~2 s ohne Antwort; PJL-Antwort vollstaendig
(auch "ohne Papier"); CancelIo -> 995; Abziehen -> 21; Lesen waehrend eines
stehenden Drucks sofort, Druck danach byte-gleich; Freigabebau mit
Dot4-Nachbau.  Beim Durchsehen korrigiert: cv_init des Lesezweigs vor dem
ersten Fehlerausgang in attach.

## Fuenfundzwanzigste Runde: xHCI ausgebaut

**Low Speed am xHCI** -- `agent/lsgeraet.py` bildet eine Low-Speed-Maus und
-Tastatur nach (EP0 8 Byte, 10 ms), wie echte Geraete; QEMU hat nur Full
Speed.  Direkt am xHCI fehlerfrei: 106/106 Mausberichte, Tasten, LEDs.

**Tastatur-Leuchten** -- IOCTL_KEYBOARD_SET_INDICATORS wurde verworfen.
Jetzt: SET_REPORT je USB-Tastatur (eigene Steuerübertragung wie FreeBSDs
ukbd), angestossen per Arbeitsauftrag (die IOCTL-Ebene ist nicht
zugesagt, der KMUTEX braucht < DISPATCH); QUERY_INDICATORS meldet den
Stand; neue Tastaturen bekommen ihn beim Anstecken.  Aus dem DDK-
Quelltext von kbdclass: jede Geraeteanforderung geht an genau EINEN
Anschluss (UnitId 0) -- mit PS/2 an i8042prt.  Deshalb schaltet der
Treiber die Leuchten bei Caps/Num/Scroll auf einer USB-Tastatur selbst
um.  Grenze: von Windows selbst gesetzter Zustand und PS/2-Tasten.

**VirtualBox-Spur** -- VirtualBox meldet 8086:1e31 (Panther Point); dafuer
schaltet FreeBSDs xhci_pci den Schrittmodus fuer Steuertransfers ein.
QEMUs xHCI buendelt SETUP..STATUS und beginnt erst, wenn alle TRBs
gueltig sind (hw/usb/hcd-xhci.c: "hack to bundle together ..."): mit
Schrittmodus wartet jeder auf den anderen -- per QEMU-Ablaufprotokoll
belegt (nach der Tuerklingel liest QEMU den Ring nicht).  Hypothese:
VirtualBox ebenso.  Neu dafuer:
- `ober/ntxhcisonder.c`: COM1-Zeile "usb: xHCI vvvv:dddd Intel-
  Sonderbehandlung: Schrittmodus ..., Anschlussumschaltung ...";
  Parameters\XhciSchritt (0 aus, 1 ein) -- in beide Richtungen geprueft.
- `nt/ntsysctl.c`: SYSCTL_INT legt Name -> Adresse in die Binder-Sektion
  NTUSYS (wie DRIVER_MODULE); Parameters\Sysctl setzt FreeBSD-Schalter
  ("hw.usb.xhci.polling=1;..."), FreeBSD-Quelltext unveraendert.

**SuperSpeed** -- Stick an einem USB-3-Anschluss (QEMU-Anschluss 1..4,
5000 Mbit/s): 1 MB geschrieben, im Abbild auf dem Wirt ohne Treiber
gelesen byte-gleich, Dateisystem fehlerfrei.  agent/geraete_ss.sh.

**Fehler im Pruefaufbau, die Scheinbefunde erzeugten:** (1) einbau.py
legte fuer usbnt_diag.sys einen zweiten Dienst neben dem usbnt des
Abbilds an -- zwei Treiber an einem xHCI: Resets, "host controller
error".  Jetzt schaltet es die uebrigen Dienste der Familie ab (USBINST
tat das immer).  (2) stick.img ist seit einem Rohschreibtest kein FAT
mehr; SuperSpeed-Test mit frischem stick_ss.img.

Nachgewiesen im Freigabebau: xHCI-Zeile, LEDs 02/03 an der Low-Speed-
Tastatur, SuperSpeed-Schreiben im Abbild byte-gleich.

## Sechsundzwanzigste Runde: Mehrprozessor

Mit sp6i386.exe vom Anwender: ntkrnlmp.exe und halmps.dll (4.0.1381.335,
gleicher Stand wie das Abbild -- ntoskrnl.exe im Paket byte-gleich mit dem
im Abbild).  einbau.py MP=1 legt beide unter eigenem Namen nach system32
und setzt einen ersten boot.ini-Eintrag mit /KERNEL= /HAL=; der
Einprozessor-Eintrag bleibt.  Drei Stolpersteine auf dem Weg:

- **QEMU:** "-smp 4" ist dort EIN Sockel mit vier Kernen; SeaBIOS schreibt
  einen Prozessor je Sockel in die MPS-Tabelle, und NT 4.0 kennt nur die.
  Also "-smp 4,sockets=4,cores=1,threads=1" (agent/geraete_mp.sh).
- **UP-Installation:** Setup hatte in NTDLL.DLL, KERNEL32.DLL und
  WIN32K.SYS die LOCK-Praefixe durch NOP ersetzt (und die Pruefsumme
  nachgerechnet) -> STOP c0000249.  Durch Vergleich mit dem Paket
  gefunden; MP=1 setzt die unveraenderten ein.
- **Lizenz:** NT 4.0 Workstation: RegisteredProcessors = 2 -- es laufen 2.
- Nebenbei korrigiert: das Abbild laeuft mit halapic.dll; und auch ein
  MP-Kern traegt "ntoskrnl.exe" als Originalnamen.

**Pruefaufbau:** grosse Uebertragungen ueber COM2 bleiben unter dem MP-HAL
in QEMU haengen (64 kB geht, 1 MB nie) -- ohne USB-Last.  Deshalb legt
einbau.py Testdaten mit DATEIEN="wirt=gast;..." gleich ins Abbild.

**Treiber:** alle Diagnosezaehler (Poolwache, ntu_zaehler) waren "+= 1" --
auf zwei Prozessoren nicht atomar; die Leck- und Verlustpruefung haette
Schritte verlieren koennen.  Jetzt Interlocked*.  Einfuhren gegen UP- und
MP-Kern und -HAL geprueft.

**Nachgewiesen auf 2 Prozessoren** (Herzschlag-Bau, dann Freigabe):
8x 1 MB Stick schreiben/lesen/vergleichen gleich, bei gleichzeitig ~2500
Mausbewegungen und ~250 Tastendruecken; Stick 3x abgezogen/angesteckt
unter Last; Drucker (1 MB mit Pause, Status, CancelIo 995, PJL, Abziehen
im Druck 21) und Xbox-One-Gamepad gleichzeitig, beide abgezogen und
wieder angesteckt.  Durchweg: kein haengender Faden, keine Resets, alle
Unterbrechungen abgeholt, offene Vergaben vorher = nachher (393 bzw. 587).

## Siebenundzwanzigste Runde: Tastatur auf zwei Prozessoren

**Leuchten-Fehler gefunden und behoben.**  11x Caps schnell hintereinander
-> Tastatur zeigte 00 statt 02; erst der naechste Anlass holte den Stand
nach.  Ursache: kam ein neuer Zustand, waehrend ein SET_REPORT noch lief,
tat usbd_transfer_start nichts, und der Rueckruf sah nur beim Start nach.
Jetzt wie FreeBSDs ukbd: USB_ST_TRANSFERRED faellt in USB_ST_SETUP durch.
Auf 2 Prozessoren nachgeprueft: 11/22/33x Caps -> 02/00/02, dazu 5x Num
-> 03; im Freigabebau ebenso.

**Tastenwiederholung auf 2 Prozessoren:** funktioniert (Druecken,
Wiederholungen, Loslassen; nichts klemmt).  Gemessen weniger als auf einem
(34 statt 55 in 3 s).  Aufgeklaert:
- Treiberzaehler "Anschlaege geliefert" = bei der Anwendung angekommen,
  auf beiden Kernen (57 = 57, 36 = 36) -- dahinter geht nichts verloren;
- 5 s Gastwartezeit dauern unter QEMU mit 2 Prozessoren 7,5-8 s echte Zeit;
  34/55 = 0,62 ~ 5/8.  In Gastzeit also gleich -- eine Eigenheit der
  Nachbildung, nicht des Treibers.

**Wiederholrate der Systemsteuerung** erreicht USB-Tastaturen nicht, wenn
es einen PS/2-Anschluss gibt: kbdclass schickt IOCTL_KEYBOARD_SET_TYPEMATIC
wie die Leuchten nur an Anschluss 0.  Dann gilt die Vorgabe 30/s nach
500 ms.  In LIESMICH vermerkt.

## Achtundzwanzigste Runde: grafischer Installer

Neu: `werkzeug/usbsetup.c` -> USBSETUP.EXE (Win32-Fenster, nur kernel32/
user32/gdi32/advapi32, kein CRT, 14 kB).  Ein Fenster VOR USBINST.EXE: es
ruft USBINST mit den gewaehlten Einstellungen auf (ein/probe/aus/taste,
-seriell, -neustart) und zeigt dessen Ausgabe -- die Einrichtung bleibt
eine gepruefte Logik.  USBINST laeuft ohne Fenster (DETACHED_PROCESS), die
Ausgabe kommt ueber eine Pipe in ein Protokollfeld, in einem eigenen Faden;
waehrenddessen sind die Knoepfe gesperrt.  Treiberwahl, Diagnoseausgabe,
Maustasten 4/5, VirtualBox-Haekchen (XhciSchritt = 0, abgewaehlt wieder
entfernt), Frage nach Neustart.  Prueft Administratorrechte gleich beim
Start.  Passt auf 640 x 480.  Symbol (16 Farben) und Versionsangabe.

Beim Bau gefunden: mit "-s" beim Binden meldet ld fuer die Ressourcen
"(.rsrc) is too large" -- ohne -s binden, danach strip.

**Nachgewiesen** (NT 4.0 SP6a, ohne Treiber vorbereitet, Bedienung nur ueber
die Tastatur): Fenster mit Zustandsuebersicht; Enter = Einrichten ->
usbinst ein, taste 4, taste 5, Frage nach Neustart; "Ja" -> Neustart ->
Treiber geladen, Stick als E:, Dienst "System, laeuft"; VirtualBox-
Haekchen -> "XhciSchritt = 0" im Protokoll.  Einfuhren gegen die NT4-DLLs
des Abbilds geprueft.

## Neunundzwanzigste Runde: Maustasten 1 bis 8 in der Systemsteuerung

**Treiber:** alle acht Tasten haben eine Belegung (Parameters\Taste1..8);
neu klick-links / klick-rechts ("links"/"rechts" allein bleiben Pfeil-
tasten).  Vorgaben unveraendert: 1 links, 2 rechts, 3 Mitte, 4 Alt+Links,
5 Alt+Rechts, 6..8 aus.  Was NT als Tasten 1..3 sieht, ergibt sich aus
Zaehlern (wie viele Tasten gerade links/rechts/Mitte halten).  Die
Belegung wird beim DRUECKEN festgehalten und beim Loslassen genau diese
geloest -- nachgewiesen: Taste 6 als "rechts" gedrueckt, gehalten auf
"aus" umbelegt, Loslassen gibt trotzdem RIGHT_UP.
Steuergeraet: IOCTL_NTU_MAUS_ZUSTAND (Tasten roh, auch 4..8), _HOLEN,
_SETZEN (wirkt sofort unter Spinlock, schreibt TasteN per
RtlWriteRegistryValue).

**usbmaus.cpl** (Systemsteuerung "USB-Maus", 12 kB): acht Zeilen mit
Laempchen (Abfrage alle 30 ms) und Belegung (Liste oder getippt),
Uebernehmen / Vorgabe / Schliessen; fragt nach, wenn keine Taste mehr
links ist; meldet ungueltige Angaben mit Tastennummer.  USBINST ein legt
es ab, aus raeumt es weg.

**USBINST taste:** Tasten 1..8, und wirkt sofort, wenn der Treiber laeuft.
Kleinigkeit: nach "vorgabe" schreibt der Treiber beim sofortigen Setzen
den Vorgabewert selbst zurueck -- die Liste zeigt ihn dann ohne
"(Vorgabe)"; die Wirkung stimmt.

**Nachbau:** lsgeraet.py --maus hat jetzt 8 Tasten.

Nachgewiesen (Diagnose- und Freigabebau): Laempchen fuer 6+8 bzw. 4+7;
Taste 6 -> klick-rechts ueber die Systemsteuerung, sofort RIGHT_DOWN/UP;
Umbelegen unter gedrueckter Taste; Vorgabe; Registry-Eintraege; USBINST
taste 2 klick-links sofort LEFT_DOWN/UP; USBINST ein legt usbmaus.cpl ab.

## Dreissigste Runde: Mausklicks auf echter Hardware Sekunden zu spaet

Gemeldet (AMD SB7x0, OHCI, USB-Maus): Laempchen in "USB-Maus" sofort,
Reaktion in Windows 2-3 s; MAUSLATENZ zeigte eine wachsende Verzoegerung
bis ~11 s; Tastatur einwandfrei.  In QEMU in keiner Lage nachstellbar
(UP, MP, ohne PS/2, Maus mit 1000 leeren Berichten/s).

Ursache (sehr wahrscheinlich): die Diagnosezeile JE BERICHT, im
Herzschlag-Bau (empfohlen zur Fehlersuche!) fuer Maus (~110 Zeichen) und
Tastatur (~40).  Sie wird nach dem Laempchen-Stand und vor der Uebergabe
an mouclass geschrieben, abgefragt, bis zu 100000 LSR-Runden je Zeichen
(auf echter Hardware ~0,1 s).  Daher Laempchen sofort, Windows spaet,
Tastatur (kurze Zeilen, seltene Berichte) unauffaellig.

Behoben: Zeilen je Bericht nur noch im Diagnosebau; UART-Warten je
Zeichen hoechstens 3000 Runden (lieber Zeichen verlieren als den Treiber
aufhalten); leere Mausberichte gehen nicht mehr an mouclass (Zaehler
ntu_maus_leer); Herzschlag zeigt "maus b/m/l".  Neu: MAUSLATENZ.EXE
(k-ter Druck im Treiber zu k-tem WM_LBUTTONDOWN; die erste Fassung paarte
"den letzten" und mass den Klickabstand).  Geprueft in QEMU: 2425 leere
Berichte verworfen, 12 Meldungen mit Inhalt geliefert, Klicks ~0 ms.
Auf der echten Maschine noch zu bestaetigen.

## Einunddreissigste Runde: Ringpuffer fuer die Diagnoseausgabe

ntu_printf schrieb jede Zeile sofort, unter ntu_buf_lock (DISPATCH_LEVEL),
abgefragt Zeichen fuer Zeichen.  Jetzt: Zeile in einen Ring (256 x 200
Byte) kopieren, KeSetEvent; ein Faden mit Prioritaet 4 schreibt ohne
Sperre.  Ring voll -> Zeile gezaehlt, spaeter "usb: N Zeilen verloren".
Vor dem Fadenstart (fruehes DriverEntry) und mit -DNTU_OHNE_RING direkt.
Bluescreen: KeRegisterBugCheckCallback leert den Ring direkt; ntu_panic
ebenso.  Entladen: erst alles hinaus, dann Faden beenden.

Nachgestellt mit -DNTU_UART_BREMSE (1 ms je Zeichen nach dem Leistungs-
zaehler -- KeStallExecutionProcessor ist unter QEMU falsch geeicht und
bremste kaum), Diagnosebau, Maus dauernd bewegt, MAUSLATENZ:
- 1 Prozessor: alter Weg 0 ms -- die Ausgabe haelt den GANZEN Prozessor
  an, auch den Messfaden; so ist die Verzoegerung unsichtbar.
- 2 Prozessoren, alter Weg: Klick 2 nach 359 ms, Klicks 3-6 nach 1,5 min
  nicht angekommen (hinter >1300 Mauszeilen).
- 2 Prozessoren, Ringpuffer: alle Klicks ~0 ms; 174 Zeilen geschrieben,
  1059 als verloren gemeldet.
- Freigabe mit Ringpuffer, 2 Prozessoren, ungebremst: Klicks ~0 ms,
  Meldungen erscheinen auf COM1.
Neue Einfuhren (KeRegister/DeregisterBugCheckCallback, KeSetPriorityThread,
PsTerminateSystemThread) gegen UP- und MP-Kern geprueft.  Testpaket
usb-ringtest.zip (A Freigabe, B Diagnose).  Auf echter Hardware offen.

## Zweiunddreissigste Runde: Klickverzoegerung geloest, Reiter in der Maus-Systemsteuerung

Ursache der ~3 s Klickverzoegerung auf echter Hardware: die Logitech-
Maussoftware (MouseWare + Filtertreiber) -- nicht usbnt.  Abgeschaltet:
Klicks in Echtzeit.  Befund per MAUSDATEN: Treiber gab LINKS AB/AUF sofort
an mouclass; Windows meldete sie gebuendelt ~3 s spaeter.  Die dabei
entstandenen Verbesserungen bleiben: Ringpuffer fuer die Ausgabe, keine
Zeile je Bericht ausser im Diagnosebau, leere Mausberichte verworfen,
Mausprotokoll (IOCTL_NTU_MAUS_PROTOKOLL), MausWeg 0-4 (Vorgabe 0),
Kernvariablen nur ueber __imp_-Zeiger (KeTickCount-Makro -> Bluescreen).

Neu: USBMAUSX.DLL -- Reiter "USB-Maustasten" in main.cpl ("Maus"), als
Eigenschaftsseiten-Erweiterung (IShellPropSheetExt, Controls Folder\Mouse\
shellex\PropertySheetHandlers).  Geprueft in NT 4.0 SP6a: Reiter erscheint,
Laempchen fuer Tasten 2 und 4, Uebernehmen/OK der Systemsteuerung setzt die
Belegung (Taste 8 = f5 nach erneutem Oeffnen aus dem Treiber gelesen).
Ausserdem: Versionsressource in USBNT.SYS (FileVersion Jahr.Monat.Tag.Bau),
Druckerschnittstelle Fassung 4 (IPP over USB, Zeitgrenze je Griff,
usbprint.sys-Codes) fuer CUPS4NT.

## Dreiunddreissigste Runde: Sondertasten, Xbox-Start

- Sondertasten (ntsonder.h, ntsondertasten.c, ntumedien.c): F13..F24 von
  der Haupt-Schnittstelle (ntukbd), Medien-/Systemtasten von der zweiten
  HID-Schnittstelle (Consumer 0x0C, System Control 0x01/0x81..0x83; Liste
  und Bitfeld, Berichtsnummern).  Belegung je Taste in
  Parameters\Sonder_<Kennung>: Tasten (Treiber speist ein), lauter/leiser/
  stumm/programm: (Auftrag -> USBTASTEN.EXE ueber Ereignis
  "UsbNtSondertasten").  IOCTL_NTU_SONDER_ZUSTAND/HOLEN/SETZEN/ABHOLEN.
- Falle: IoCreateNotificationEvent in DriverEntry scheitert, weil es
  \BaseNamedObjects beim Laden noch nicht gibt (OpenEvent: Fehler 2,
  Auftraege blieben liegen) -> Ereignis beim ersten IOCTL anlegen,
  USBTASTEN.EXE fragt ohne Ereignis alle 250 ms nach.
- USBTASTX.DLL: Reiter "Sondertasten" in main.cpl/Tastatur
  (Controls Folder\Keyboard\shellex); Liste springt zur gedrueckten Taste.
- USBTASTEN.EXE (Autostart): Mixer-Lautstaerke, Stumm, Programmstart.
- SONDERSET.EXE: Belegungen per Befehl.
- Xbox One/Series: herstellerabhaengige Startnachrichten wie xpad (PDP,
  Hori, PowerA, Microsoft-Zusatztasten); COM1 meldet VID:PID und was
  geschickt wurde.
- Geprueft (QEMU, lsgeraet.py --tastatur --medien): F13/F24 -> VK_F13/F24,
  WebVor -> Alt+Rechts, Lauter -> Auftrag, programm: fuehrt aus, Reiter
  springt und uebernimmt.  Nicht geprueft: Lautstaerke (keine Soundkarte).

## Vierunddreissigste Runde: ein Symbol "Joystick"

usbjoy.cpl heisst "Joystick", Eigenschaftsfenster mit Reitern Gamepad
(bisherige Testanzeige als Kindfenster), Vibration (IOCTL_NTUJ_RUETTELN,
neu im Treiber: Xbox 360 00 08 .., Xbox One GIP 09 .. wie xpad; Nutzer-
Zaehler, Abziehen wartet), Gameport (joy.cpl ueber CPlApplet im eigenen
Prozess -- Control_RunDLL oeffnete nichts; joy.cpl verweigert CPL_INIT
ohne Joystick-Treiber in Drivers32).  joy.cpl ausgeblendet ueber
HKCU\Control Panel\don't load (IniFileMapping von control.ini, geprueft).
Seiten 372x222 DLU: passt samt OK auf 640x480.

## Xbox One S (Model 1708): Einschalten wie xpad

S-Start nur 045e:02ea/0b00, Zusatztasten (4D) nur 0b00 -- vorher an
alle bzw. auch an 02ea; der 1708 blinkte und ging aus.  Neu: Antwort auf
Ankuendigung (02) mit erneutem Einschalten; Wecker (callout, 1 s): ohne
Eingabe (20/07) bis zu dreimal erneut.  Geprueft am OHCI mit xpad.py
--one --spaet 1 [--anmelden].  Am echten 1708 offen.

## Fuenfunddreissigste Runde: ResourceMap, Joysticks, neuer Installer

- ResourceMap: IoReportResourceUsage auf Treiberebene, ein Eintrag je
  Hostbaustein (Register exklusiv, Unterbrechung geteilt) unter
  HKLM\HARDWARE\RESOURCEMAP\USB\usbnt; beim Entladen leere Liste.
- i8042prt 15/17/19/20/26: PS/2-TASTATUR (Reset/Wiederholrate/LEDs),
  seit die BIOS-Tastatur-Emulation aus ist -- harmlos, nicht usbnt.
- usbjoy.cpl, Reiter "Test": Beschriftung nach der Achse, die Spiele
  sehen (Verteilung wie usbjoy.dll): Knueppel, Schub, Drehung,
  Coolie-Hat; fehlende Achsen grau.  Pruefstand: lsgeraet.py --joystick
  (Falle: der Nachbau meldete QEMU MPS 4 statt 8, QEMU verwarf die
  7-Byte-Berichte -- der Treiber war richtig).
- USBINST: pruefen / abschalten <fund> / wieder (Sicherung unter
  HKLM\SOFTWARE\UsbNt\Sicherung, "aus" stellt zurueck); ein richtet
  Maus-/Tastatur-Reiter (DllRegisterServer), USBTASTEN im Autostart,
  SONDERSET und "don't load joy.cpl" ein, aus entfernt sie.
- USBSETUP: Pruef-Fenster mit Haekchen vor dem Einrichten (FUND-Zeilen;
  Falle: Zerlegung verlangte drei Trennzeichen statt zwei), Fassung aus
  der Versionsressource, Diagnoseausgabe Vorgabe AUS.
- ntbasis printf: %s ueber 63 Zeichen direkt schreiben statt kuerzen.
- werkzeug/bau-werkzeuge.sh: alle 19 Werkzeuge aus dem aktuellen Stand.
- Alle 5 Bauarten neu, Fassung 2026.9.27.1; Einfuhren aller Dateien gegen
  NT 4.0 geprueft.  Installer mit PRUEFSUMMEN.TXT.
- Geprueft (QEMU): pruefen/abschalten/wieder mit vorgetaeuschter
  Logitech-Installation, Pruef-Fenster, Einrichten, Entfernen.

## Aufgeraeumt (Fassung 2026.9.27.2)

- MausWeg 1..4 (DPC, DPC auf festem Prozessor, Mini-Bewegung) entfernt:
  Ursache der Klickverzoegerung war die Logitech-Maussoftware, keiner der
  Wege half.  Uebergabe an mouclass wieder nur direkt.  Parameters\MausWeg
  wird nicht mehr gelesen.  Mausprotokoll (IOCTL_NTU_MAUS_PROTOKOLL,
  MAUSDATEN.EXE) bleibt.
- Geprueft (QEMU, 2 CPUs, OHCI): Klicks sofort, Bewegung, Rad; Abziehen und
  Wiederanstecken.
- Anwender: USB 3.0 am NEC uPD720200 laeuft auf echter Hardware.

## Fassung 2026.9.27.3: unauffaelligere Dateien fuer Virenscanner

Anlass: heuristische Treffer (BitDefender-Familie "Gen:Trojan.Heur2.FU" --
sieben Scanner, ein Urteil -- dazu ML-Scanner).  Keine Funktion geaendert:
- Versionsangaben (Hersteller, Beschreibung, Produkt, Lizenz, Originalname)
  in ALLEN Programmen und DLLs (bau-werkzeuge.sh: ver ...); vorher nur in
  den Treibern und USBSETUP.
- Werkzeuge ohne Abwicklungstabellen (-fno-asynchronous-unwind-tables):
  kein .eh_frame mehr.
- Treiber: eigener Abschnitt NTUSYS mit .rdata verschmolzen (/merge) --
  nur noch .text .rdata .data .rsrc .reloc.  Geprueft: sysctl-Tabelle,
  usbtest, Tastatur/Maus/Speicher.
- Messwerkzeuge in eigenes Paket USBNT-DIAGNOSE; Pruefsummen als SHA-256;
  LIESMICH erklaert die heuristischen Meldungen.

## Pakete neu geschnitten (2026.9.27.3)

Virenscanner (per Gruppe geprueft): Treiber, Einrichtung (USBSETUP,
USBINST) und Systemsteuerung sauber; heuristisch gemeldet: USBTASTEN und
die fuenf Befehlszeilenwerkzeuge.  Die sechs liegen jetzt im optionalen
Paket USBNT-WERKZEUGE; USBINST richtet sie mit ein, wenn sie daneben liegen.

## Fassung 2026.9.27.4: Geraetemanager mit USB, zwei Installer

- Treiber: IOCTL_NTU_USBGERAETE (ober/ntusbliste.c) -- jedes USB-Geraet
  einmal wie lsusb, unter der Aufzaehlungssperre des Wurzelhubs.
- Geraetemanager (geraetemanager/): USB-Zweig, Eigenschaften, Symbole,
  Reiter- und Fensterkorrekturen; DEVMGR.CPL richtet USBINST mit ein.
- installer-ohne-werkzeugen/ (Treiber, Einrichtung, Systemsteuerung,
  Geraetemanager) und installer-mit-werkzeugen/ (dazu USBTASTEN, SONDERSET,
  USBTEST, USBGERAET, USBDRUCK, JOYTEST, DIAGNOSE\).
- Geprueft (QEMU): USB-Baum, Eigenschaften (beide Reiter), PCI-Eigenschaften
  (drei Reiter), Systemsteuerung, usbinst ein mit Geraetemanager.

## PCI.IDS im Geraetemanager aktualisieren (Geraetemanager 1.9)

Knopf "PCI.IDS...": aus dem Internet (http) oder aus Datei, mit Pruefung,
.ALT-Sicherung und sofortigem Neuladen.  PCI.IDS 2026.09.25 liegt beiden
Installern bei; USBINST kopiert sie nur, wenn noch keine da ist.  Geprueft
(QEMU): ohne Netz klare Meldung, aus Datei uebernehmen und ersetzen.

## HTTPS: PCIHOLE (wolfSSL) fuer den Geraetemanager

pcihole/: TLS 1.2 mit Ketten- und Namenspruefung, CACERT.PEM (Mozilla).
Geraetemanager: Vorgabe https://, PCIHOLE ueber Pipe, sonst http://.
Beide Abrufe HTTP/1.1 + chunked.  USBINST richtet PCIHOLE + CACERT ein.
Geprueft: Wine (18 Faelle), echtes Netz (GitHub), NT 4.0 (Start, Zertifikate).

## Fassung 2026.9.27.5: USB-Sticks mit GPT (Patch "usb-gpt-fix", ueberarbeitet)

Befund (Anwender): 64-GB-Stick mit GPT/NTFS ohne Dateisystem -- ntum_medium
nahm den Schutz-MBR-Eintrag (0xEE, ab Sektor 1 = GPT-Kopf) als Partition.
Uebernommen aus dem Patch: Schutz-MBR erkennen, GPT-Kopf ("EFI PART") und
Tabelle lesen; NTFS/FAT16/FAT12 am Bootsektor erkennen (Typ 07/06).
Ueberarbeitet:
- MBR-Puffer wiederverwendet statt 2 x 512 Byte mehr auf dem Kernelstapel.
- Obere 32 Bit der 64-Bit-LBAs muessen 0 sein; Eintragsgroesse 128..512
  und Vielfaches von 128; Tabellen-LBA auf der Platte; Ende begrenzt.
- Nicht der ERSTE belegte Eintrag: Windows legt die Microsoft-Reserved-
  Partition (16 MB, ohne Dateisystem) vor die Daten.  Rang: Basic Data >
  Linux-Daten > sonst; MSR, EFI-System, Wiederherstellung nie.
- COM1: "umass: LUN n GPT, Partition ab X, Y Bloecke".
Geprueft (QEMU, NT 4.0): GPT mit MSR + NTFS -> MSR uebersprungen, F:
lesbar; GPT mit einer NTFS-Partition -> F: lesbar und beschreibbar (von
aussen: Datei da, ntfsfix OK, GPT unversehrt); MBR/FAT-Stick unveraendert.

## Fehlalarme: USBTASTEN sichtbar, PCIHOLE bereinigt (2026.9.27.6)

- USBTASTEN: Symbol im Infobereich (Kurzinfo, Menue Beenden), Arbeit im
  eigenen Faden; ohne CRT (eigener Einstieg ruft WinMain).
- PCIHOLE: VERSIONINFO; .eh_frame der wolfSSL-Bibliothek entfernt (war
  nach strip Abschnitt "/4"); 581 -> 512 KB; TLS-Tests unveraendert.
- Versuch zurueckgenommen: Werkzeuge mit MinGW-w64-CRT-Start haengen unter
  echtem NT 4.0 SP6a (Wine gut, Importe vollstaendig).  Bleiben bei ntbasis.c.

## SONDERSET.EXE abgeschafft -- Funktion in USBTASTX.DLL (2026.9.27.7)

SONDERSET.EXE (winzige EXE, von Elastic/Panda heuristisch gemeldet) faellt
weg.  Seine Funktion war ohnehin doppelt in USBTASTX.DLL vorhanden und ist
ueber rundll32 erreichbar:
    rundll32 usbtastx.dll,Sondertaste                 alle Belegungen
    rundll32 usbtastx.dll,Sondertaste Rechner         eine lesen
    rundll32 usbtastx.dll,Sondertaste Rechner calc.exe  setzen
Die DLL ist bei den Scannern sauber; damit ist ein Fehlalarm-Ziel weg,
ohne Funktionsverlust (Ausgabe im Meldungsfenster statt auf der Konsole).
USBINST richtet SONDERSET nicht mehr ein.

## USBGERAET.EXE -> USBGERAET.DLL (2026.9.27.8)

Letztes verbliebenes Fehlalarm-Ziel (USBGERAET.EXE, Elastic) nach dem
bewaehrten Muster in eine DLL verwandelt (wie SONDERSET -> USBTASTX.DLL).
Die gesamte Logik (usbgeraet.c) bleibt; usbg_main wird ueber rundll32
aufgerufen:
    rundll32 usbgeraet.dll,Geraete [Zahl|auswerfen X:|...]   (Ausgabe im Fenster)
    rundll32 usbgeraet.dll,GeraeteInDatei <Datei> [...]      (Ausgabe in Datei)
ntbasis.c: schreib() ist jetzt in einen Puffer umleitbar (ntb_puffer),
ntb_start/main nur ohne -DNTB_DLL.  Die Anzeige gibt es ohnehin im
Geraetemanager.  USBINST kopiert usbgeraet.dll statt der EXE.
Gebaut, exportiert Geraete/GeraeteInDatei, Importe NT-4.0-tauglich; der
Lauf im Gast liess sich wegen des Anzeige-Dialogs des Pruefstands nicht
abschliessend ablichten (Logik unveraendert ggue. der bewaehrten EXE).

## DEVMGR.CPL ohne Dropper-Muster (2026.9.27.9)

Der Geraetemanager legte den PCI-Lesetreiber (PCISCAN) frueher bei jedem
Start selbst nach system32\drivers und startete ihn als Dienst -- ein
Dropper-Muster, das Bkav/Kingsoft/Trellix an DEVMGR.CPL/EXE anschlugen.
Jetzt:
- PCISCAN.SYS liegt als normale Datei bei; der Dienst wird EINMALIG von
  USBINST eingerichtet (lokaler SC-Manager-Griff in zusatz_ein).
- Bauschalter NUR_ANSEHEN: die CPL enthaelt KEIN CreateService/StartService/
  OpenSCManager/CreateProcess und keinen eingebetteten Treiber mehr
  (an den Importen der gebauten CPL geprueft).  Die verzichtbare EXE
  behaelt "Treiber einrichten" und den HTTPS-Abruf ueber PCIHOLE.
- USB-Anzeige braucht PCISCAN nicht; ohne den Dienst fehlen nur die
  PCI-Geraete.
VM-Nachweis der PciScan-Einrichtung steht wegen des Anzeige-Dialogs des
Pruefstands noch aus; die CPL-Importe sind belegt.

## Fassung 2026.10.4.1: Abschnittsattribute (Hinweis von Ray Hinchliffe, SIV)

Gemeldet: ".text + .data are marked as paged, they should not be, further
paged code should be in PAGE ... and DriverEntry() should be in INIT."
Am gebauten Treiber bestaetigt: .text war 60000020, .data c0000040 --
beide ohne NOT_PAGED; weder PAGE noch INIT vorhanden.  Ausgelagerter
Treibercode bei erhoehtem IRQL = Bugcheck.

Behoben, ausgerichtet an seinem eigenen SIVNT4.SYS (bitgleich):
    .text  68000020   Code, NOT_PAGED, nicht schreibbar
    .data  c8000040   NOT_PAGED
    INIT   e2000020   verwerfbar, DriverEntry darin
PAGE wird bewusst nach .text gemergt: der Treiber arbeitet im ISR- und
DPC-Pfad, da darf nichts auslagerbar sein (strenger als das Vorbild).

Umsetzung:
- #pragma alloc_text(INIT, DriverEntry) in nt/ntusbnt.c
- /section:INIT,ERWD und /merge:PAGE=.text beim Binden
- abschnitte.py: setzt NOT_PAGED direkt im PE-Kopf (lld-link setzt das
  Bit im Gegensatz zu Microsofts Binder nicht), danach Pruefsumme neu
- betrifft auch PCISCAN.SYS (Geraetemanager), dort gleich behoben

Geprueft (NT 4.0 SP6a, QEMU): xHCI erkannt und angehaengt (5 Gbps),
Root-Hub 12 Anschluesse, USB-Tastatur -> kbdclass, USB-Maus (5 Knoepfe,
Rad) -> mouclass, USB-Stick als Laufwerk E:, \Device\UsbDev0 bereit.

Pruefstand (nach Umgebungsverlust neu aufgebaut), drei Fallen behoben:
- volle Platte beschaedigte das Abbild beim Zurueckschreiben
- NTFS-Dirty-Bit blockierte ntfscp -> einbau.py macht jetzt ntfsfix -d
- Hive-Folgenummer: NT hielt die bearbeitete Registry fuer veraltet

## Fassung 2026.10.7: Gamepads -- winmm, DirectInput, Win16 (Pruefstand mit Emulator)

Neu im Pruefstand: pruefstand/pademu.py spielt dem Gast ueber QEMUs usb-redir
einen echten USB-Controller vor (Xbox 360, Xbox One/Series per --pid, HID-
Gamepad); Knoepfe/Achsen ueber einen Steuer-Socket (pad.sh), alles, was der
Treiber schickt (LED, Rumble, GIP), steht im Protokoll.  QEMU verlangt am
xHCI 64-Bit-Kennungen, Bulk-Streams und 32-Bit-Bulk-Laengen; ausserdem
qemu-xhci,p2=8,p3=8 -- QEMUs automatischer Hub scheitert am xHCI
(USB_ERR_BAD_ADDRESS).

Befunde und Korrekturen:
- usbjoy.log war ASCII, nicht UTF-16: "p2=0x8d20" war ein Lesefehler.
  Tatsaechlich 0x2d8 = sizeof(JOYCAPSW); faehigkeiten() war richtig.
- szRegKey: usbjoy.dll meldete "USBJOY", die Einstellungen lagen unter
  "usbjoy.dll".  DirectInput 3 und joy.cpl bauen damit den Pfad
  MediaResources\Joystick\%s\CurrentJoystickSettings.  Jetzt "usbjoy.dll".
- Joystick<n>Configuration ist JOYREGHWCONFIG (112 Byte, binaer) mit
  JOY_US_PRESENT, kein Text (USBJOY.REG war hier falsch -- nicht mehr
  benutzen).  dwType >= 12 (OEM), sonst zeigt DirectInput nur
  "6 Achse, 11 Taste Joystick" statt des OEM-Namens (DINPUT.DLL: cmp $0xc).
  USBINST schreibt das jetzt selbst und ersetzt Text- und Vorfassungswerte.
- Win16 (WOW): winmm!joy32Message (winmm.dll 4.00, NT 4.0 SP6a) legt in der
  16-Bit-JOYCAPS szPname und alle Folgefelder 2 Byte zu weit hinten ab
  (lea -0x5a statt -0x5c).  Win16-Spiele sehen wNumButtons = 0xFFFF (-1):
  "Joystick must support at least 2 buttons" (SinkSub Pro).  Trifft jeden
  Joystick-Treiber.  usbjoy.dll gleicht das aus -- nur in WOW-Prozessen und
  nur, wenn joy32Message die fehlerhafte Anweisung enthaelt.  Die zwei
  Stapelbytes vor dem Namen bleiben (kosmetisch).
- Beide Installer: USBJOY.DLL und USBJOY.CPL fehlten -- joystick_ein()
  brach still ab.  Jetzt drin.
- joytest ruetteln <stark> <schwach> [n]; diprobe (DirectInput-Liste);
  werkzeug16/joy16.c (Win16-Pruefprogramm, Open Watcom).

Gemessen (NT 4.0 SP6a, QEMU, Treiber .8): zwei Controller gleichzeitig
(360 + One, 360 + Series) ueber winmm, DirectInput und Win16 ohne
Vertauschung; usbjoy.cpl Test- und Vibrationsreiter; Vibration 360 (0..255),
One/Series (GIP 0..127), HID meldet ERROR_NOT_SUPPORTED; Abziehen/Anstecken.

UNGEBAUT (kein DDK in dieser Umgebung): ober/ntujoy.c -- Spieler-LED nach
UsbJoy-Nummer (06..09) statt immer 06.  Vor dem naechsten Treiberbau pruefen.

## Fassung 2026.10.7.3: JOY16 -- joySetCapture unter WOW; Trigger-Ruhelage

JOY16 fang <s>: Win16-Fenster, joySetCapture auf beide Joysticks (20 ms,
nur Aenderungen), joySetThreshold 1000, protokolliert jede MM_JOYn-Nachricht.
Gemessen mit Plan am Emulator: Bewegung, ZMOVE (LT), Knopf ab/auf mit
richtigen JOY_BUTTONnCHG-Bits (z. B. B+Y gehalten = 080a), beide Joysticks
getrennt; Abziehen/Anstecken waehrend des Einfangens: Programm laeuft
weiter, Eingaben danach wieder richtig.  Win16 kennt nur 4 Knoepfe (API).

Behoben (usbjoy.dll): vor dem ersten Bericht nach dem Anstecken meldete der
Kern alle Achsen in der Mitte -- fuer Xbox-Trigger (Z, Rz) hiess das "halb
gedrueckt" (MM_JOY1ZMOVE 32768; mit pademu --stumm 4 vier Sekunden lang).
usbjoy.dll meldet bei Zaehler 0 fuer Xbox-Trigger jetzt 0.
pademu: --stumm <s> verzoegert den ersten Eingabebericht nach dem Einstecken.

## Fassung 2026.10.7.1 (Treiber): Spieler-LED; Bau mit neuem FreeBSD-Stand

Mit dem NT-4.0-DDK des Anwenders gebaut (clang-cl/lld-link, bau-clangcl.sh).
- ober/ntujoy.c: Spieler-LED nach UsbJoy-Nummer (06..09).  Gemessen: zwei
  360er -> Emulator meldet "LED-Muster 6" und "LED-Muster 7".
- FreeBSD-Ausschnitt jetzt im Paket: freebsd/ (HERKUNFT.txt mit Commit,
  PRUEFSUMMEN.md5).  Der Stand von .8 liess sich nicht mehr ermitteln (der
  Ordner fehlte im Paket); gebaut gegen main 139fa2c12 (2026-10-07).
  Einfuhren identisch zu .8, Groesse +512 Byte.
- nt/ntunterlage.c: usbd_dummy_timeout heisst ntu_dummy_timeout und springt
  per /alternatename nur ein, wenn usb_core.c ihn nicht mitbringt (neuere
  FreeBSD-Staende definieren ihn selbst: doppeltes Symbol beim Binden).
- bau-clangcl.sh: DDK, UNT, SRC aus der Umgebung (waren fest eingetragen;
  "DDK=... ./bau-clangcl.sh" wurde stillschweigend uebergangen).
- Geprueft (QEMU): xHCI, EHCI, UHCI, OHCI; Tastatur, Maus, Stick (lesen,
  schreiben, fc), Gamepads (winmm, DirectInput, Win16), Vibration.
- Nur USBNT.SYS ist neu.  Die Einzelbauformen (UHCINT/OHCINT/EHCINT/XHCINT)
  bleiben auf .8 -- sie wurden mit dem neuen Stand nicht gebaut und
  nicht geprueft.

## 7. Oktober, Abschluss: SinkSub Pro mit Controller -- bestaetigt

Mit dem Abbild des Anwenders (SSPRO unter C:\SSPRO\SSPRO), Treiber
2026.10.7.1 und emuliertem Xbox-360-Controller: Game -> Setup -> "Use
Joystick" laesst sich ankreuzen (vorher "at least 2 buttons"), SSPRO.INI
JoyUse=1.  Im Spiel: Stick lenkt das Boot, Knopf 1/2 werfen Bomben links/
rechts.  usbjoy.log: SSPRO fragt joyGetNumDevs, joyGetPos, joyGetDevCaps --
dort griff der Ausgleich des winmm-Fehlers.  SSPRO startet sich nach dem
Setup nicht selbst neu (beendet sich nur); BWCC.DLL fehlt, wird nicht gebraucht.

## Fassung 2026.10.8.1: Gamepads als eigener Treiber (USBXBOX.SYS) und eigenes Paket

Getrennt, auf Wunsch des Anwenders:
- usbnt.sys (und UHCINT/OHCINT/EHCINT/XHCINT) enthalten ober/ntujoy.c nicht
  mehr.  usbnt.sys exportiert die 40 Stapelfunktionen, die ntujoy braucht;
  die Liste bildet bau-clangcl.sh selbst aus den offenen Bezuegen der
  USBXBOX-Objekte (usbnt.def undekoriert, Einfuhr-.def dekoriert mit
  dlltool -k -- lld-link sucht die @N-Fassung selbst, dlltool nicht).
- nt/ntnewbus.c: device_probe_and_attach prueft neben der eingebauten
  NTUDRV-Liste eine Tabelle von Zusatztreibern (ntu_zusatz_hinzu).
- nt/ntzusatz.c: Export ntu_treiber_anmelden.  Zaehlt Geraete, an denen
  ntugen (BUS_PROBE_HOOVER) haengt, per usbd_start_re_enumerate neu auf --
  sonst bekaeme ein spaet geladener Zusatztreiber die schon belegten
  Schnittstellen nie.  Dazu ntu_hz (hz ohne Datenimport).
- xbox/ntxbox.c: Rahmen fuer USBXBOX.SYS -- eigenes Treiberobjekt (ntujoy
  legt \Device\UsbJoyN daran an), IRP-Verteilung, Anmeldung des eigenen
  NTUDRV-Abschnitts.  ntujoy.c unveraendert.
- ober/ntkbdport.c: Zweig NTU_PORT_JOY entfernt.
- werkzeug/xboxinst.c (neu): ein/aus/Zustand.  Schaltet VOR dem Einrichten
  das Alte ab: bricht ab bei usbnt.sys ohne Export ntu_treiber_anmelden
  (alte Fassung mit eingebautem Joystick-Teil) oder bei Einzelbauformen;
  sichert und ersetzt Drivers32\joystick (uebernimmt die Sicherung der
  kombinierten Vorfassung), schaltet den Gameport-Dienst "Joystick" ab,
  blendet joy.cpl aus.  "aus" schreibt alles zurueck.
- werkzeug/usbinst.c: Joystick-Teil entfernt; Hinweise auf XBOXINST.
- installer-xbox/ (neu): XBOXINST, USBXBOX.SYS, USBJOY.DLL/CPL, JOYTEST,
  DIPROBE, JOY16, EINRICHT/ENTFERNE.BAT, LIESMICH.TXT.  Die USB-Installer
  enthalten keine Joystick-Dateien mehr.

Geprueft (QEMU, Abbild des Anwenders, 360 + One emuliert):
 1. XBOXINST ein mit alter usbnt.sys (24.09.): Abbruch, nichts geaendert.
 2. neues USB-Paket: Controller nur noch als UsbDev (ntugen).
 3. XBOXINST ein + Neustart: usbxbox meldet sich an, UsbJoy0/1.
 4. XBOXINST ein + "net start usbxbox" im Betrieb: beide Controller werden
    neu aufgezaehlt, ntugen geloest, ntujoy uebernimmt.
 5. winmm, DirectInput, Win16 (JOY16), Vibration 360/GIP, Abziehen/
    Anstecken, SinkSub Pro (lenken, Bomben links/rechts).
 6. XBOXINST aus + Neustart: alles zurueck; joystick.dll und Gameport-
    Dienst werden gesichert und wiederhergestellt.
bin/diagnose und bin/herzschlag sind noch alte Fassungen MIT eingebautem
Joystick-Teil -- vertragen sich nicht mit USBXBOX.

## Fassung 2026.10.8.2: Einzelbauformen geprueft; zwei Fehler in USBINST

Geprueft (QEMU, je eigener Controller): UHCINT (PIIX3), OHCINT, EHCINT
(nur Stick: QEMUs EHCI ohne Begleitcontroller nimmt kein Full Speed;
1 MB schreiben + fc), XHCINT -- Tastatur, Maus, Stick.  XBOXINST lehnt bei
Einzelbauform ab.  Ohne jeden USB-Treiber (nach USBINST aus) startet NT
normal.  bin/diagnose und bin/herzschlag neu gebaut (ohne Joystick-Teil,
Exporte gleich der Freigabe); Diagnosebau + USBXBOX im Gast geprueft.

USBINST, behoben:
1. Wechsel der Bauart (usbnt <-> uhcint ...) scheiterte immer mit
   CreateService-Fehler 1078: gleicher Anzeigename fuer alle Bauarten, und
   der alte Dienst behaelt seinen bis zum Neustart (nur zum Loeschen
   vorgemerkt).  Jetzt "FreeBSD-USB fuer NT 4.0 (<dienst>)".
2. dienst_entfernen hielt den laufenden USB-Treiber an.  NT 4.0 versucht
   dann, einen Systemstart-Treiber mit DriverUnload zu entladen, an dem
   Tastatur und Maus haengen -- USBINST blieb haengen, mit ihm Agent und
   Dienstverwaltung.  Jetzt nur noch bei Start 3 (von Hand gestartet).
   Gegenprobe: Wechsel usbnt -> uhcint -> usbnt mit laufender alter
   usbnt.sys, je 6-8 s, beide Neustarts in Ordnung.
Mit dem USBINST.EXE der Pakete 2026.10.8.1 NICHT zwischen Bauarten wechseln.

## Fassung 2026.10.9.1: USB-Audio -- USBAUDIO.SYS und USBTON.EXE (UAC1, Wiedergabe)

Grundlage: Recherche UAC 1/2/3 (FreeBSD uaudio.c, Linux snd-usb-audio,
QEMU dev-audio.c, Windows usbaudio/usbaudio2).

USBTON.EXE (werkzeug/usbton.c):
- liste: AudioControl-Schnittstelle finden, Konfiguration auswerten
  (Feature Units, AS-Alternativen, Format, Raten, Sync-Art, Rate setzbar).
  Version ueber bcdADC im AC-Header -- QEMUs usb-audio meldet
  bInterfaceProtocol 0x04 statt 0x00 (nicht normgerecht); Linux nimmt jeden
  unbekannten Wert als UAC1, wir auch.
- stumm 0|1, laut DB: SET_CUR an die Feature Unit, ueber ntugen (AC-
  Schnittstelle).  QEMU bildet den Wert linear ab, nicht in dB.
- ton HZ S: Sinus ueber USBAUDIO.SYS.  tonroh: ueber ntugen, zum Vergleich.

Gemessen ueber ntugen (tonroh): jeder ISO-Aufruf laeuft synchron unter
einer Sperre; der naechste kommt erst nach dem Ende des vorigen, sein
Startrahmen ist vorbei, der Stapel verlegt ihn: fest 2-3 ms Luecke je
Aufruf (1, 5, 20, 50, 100, 120 Rahmen: kuerzeste Dauer = Rahmen + 2..3 ms).
Aus dem Benutzermodus ueber ntugen also nicht lueckenlos.

USBAUDIO.SYS (ober/ntuaudio.c + audio/ntaudio.c), Zusatztreiber wie USBXBOX:
- uebernimmt AudioStreaming-Schnittstellen (01/02), \Device\UsbAudioN;
  AudioControl bleibt bei ntugen (Steuerung aus dem Benutzermodus).
- NTUA_START waehlt Alternative, setzt ggf. die Rate (UAC1 SET_CUR am
  Endpunkt, Fehler toleriert wie FreeBSD), zwei isochrone Uebertragungen
  zu je 8 Rahmen, beide sofort gestartet; jeder Rueckruf befuellt aus dem
  Ring und schickt sofort wieder ab (FreeBSD-uaudio-Muster).
  Bruchteil-Akkumulator fuer die Rahmenlaengen (44,1 kHz -> 44/45).
  Fehlende Daten: Stille, Unterlauf gezaehlt (erst nach den ersten Daten).
- NTUA_SCHREIBEN nimmt, was in den Ring passt, kehrt sofort zurueck.
- Schliessen des spielenden Griffs stoppt (IRP_MJ_CLEANUP).
- usbd_get_isoc_fps nimmt in diesem FreeBSD-Stand das GERAET, nicht die
  Uebertragung (Warnung beim Uebersetzen hat es gezeigt).
bau-clangcl.sh: Liste ZUSATZ (usbxbox, usbaudio); Exportliste von usbnt.sys
aus den offenen Bezuegen aller Zusatztreiber (jetzt 52 Namen).

Geprueft (QEMU usb-audio, -audiodev wav): xHCI, OHCI, UHCI je 0 Unterlaeufe
waehrend des Tons, Sinus ohne eine Unstetigkeit (pruefstand/sinuspruef.py);
Nachladen im Betrieb (net start usbaudio): AS-Schnittstelle von ntugen
geloest, neu aufgezaehlt, an ntuaudio.
Noch nicht: winmm-Treiber, Installer, Aufnahme, Rueckmelde-Endpunkt
(asynchrone Senken), UAC2, echte Hardware.

## Fassung 2026.10.9.2: USB-Audio fuer Programme -- usbaudio.dll und AUDIOINST

- werkzeug/uac.c, uac.h (neu): Geraeteauswertung aus usbton herausgeloest,
  fuer mehrere Geraete, ohne Ausgaben; gemeinsam fuer usbton und
  usbaudio.dll.
- werkzeug/usbaudio.c (neu, usbaudio.dll): winmm-Wave-Treiber (DriverProc,
  wodMessage).  Jedes PCM-Format 8/16 Bit, Mono/Stereo, 4..96 kHz wird auf
  16 Bit Stereo in der Geraeterate gebracht (dieselbe, wenn das Geraet sie
  kann, sonst 48 kHz); lineare Interpolation fortlaufend ueber
  Puffergrenzen.  Ein Faden je Oeffnung fuellt den Ring von USBAUDIO.SYS.
  WOM_DONE, sobald ein Puffer im Ring liegt; Schliessen wartet, bis der
  Ring leer ist.  GETPOS rechnet die gespielten Byte zurueck.  Reset:
  Faden gibt alle Puffer zurueck, IOCTL_NTUA_LEEREN.  Lautstaerke:
  20*log10 auf die Feature Unit (Kanal 1/2, sonst Master).  GETDEVCAPS
  in A- und W-Fassung (an der Groesse erkannt).
- ober/ntuaudio.c/.h: IOCTL_NTUA_LEEREN (Ring verwerfen, Strom laeuft
  weiter).
- werkzeug/audioinst.c (neu): ein/aus/Zustand.  Prueft usbnt.sys an den
  Dateien: jede Funktion, die usbaudio.sys aus usbnt.sys importiert, muss
  dort exportiert sein -- nennt sonst den fehlenden Namen.  Dienst
  usbaudio wie usbxbox; Drivers32: erster freier Name wave, wave1..9.
- werkzeug/usbton.c: auf uac.c umgestellt; neu winmm, wav, wmreset,
  wmlaut, lautlesen.
- installer-audio/ (neu).  USB-Installer mit usbnt.sys 2026.10.9.2 (52
  Exporte), Xbox-Installer mit neu gebautem usbxbox.sys.

Geprueft (QEMU usb-audio, Mitschnitt):
- AUDIOINST gegen usbnt.sys von 2026.10.8 (40 Exporte): Abbruch, nennt
  "ntu_amdpll_betrifft"; mit 2026.10.9.2: eingerichtet, Drivers32 "wave1"
  (wave = mmdrv.dll blieb).
- winmm ueber den Audio-Mapper: 44,1 kHz 16 Bit Stereo, 22 kHz 8 Bit Mono,
  11 kHz 16 Bit Mono, 48 kHz -- je 0 Unstetigkeiten.  NT-Systemklaenge
  "Der Microsoft Sound" (6,12 s) und "tada" (1,26 s) in voller Laenge.
- waveOutReset nach 1 s: alle 4 Puffer zurueck, Schliessen ok, Position 0.
- waveOutSetVolume 50 %: Feature Unit -1502/256 dB (Soll -6,02 dB).
Pruefstand: sinuspruef.py misst die Amplitude nur noch in der mittleren
Haelfte -- QEMU schreibt den Mitschnitt verzoegert, an den Raendern stand
der vorige Ton (zweimal falscher Befund: "doppelte Amplitude", "1
Unstetigkeit" an der Naht zweier Toene).
Nicht geprueft: Win16-Programme (WOW), echte Hardware, Pause/Restart.

## Fassung 2026.10.9.3: USB-Audio -- Win16 und Pause geprueft

- werkzeug16/wave16.c (WAVE16.EXE, Open Watcom): Win16-Pruefprogramm.
  Unter WOW: waveOutGetNumDevs 1, WAVEOUTCAPS (16 Bit, 48 Byte) richtig
  ("USB: QEMU USB Audio", Formate 0xFFF, 2 Kanaele, Lautstaerke),
  waveOutOpen 22 kHz 8 Bit Mono, SetVolume/GetVolume, 2 s Ton in voller
  Laenge ohne Unstetigkeit.  Anders als beim Joystick (joy32Message)
  verschiebt winmm hier nichts.
- usbton wmpause: waveOutPause haelt die Position an (gleich nach 300 ms
  und 1 s), Mitschnitt 2,0 s Ton / 0,9 s Stille / 2,0 s Ton = alle 4 s,
  nichts verloren; ~85 ms Ring spielen nach dem Pausieren aus.

## Fassung 2026.10.9.4: USB-Audio -- Aufnahme

- ober/ntuaudio.c/.h: Aufnahmestrom (Endpunkt mit 0x80): zwei
  Uebertragungen im Wechsel, Rahmen mit Hoechstlaenge angefordert, Rahmen
  k bei k * mfl (FreeBSD uaudio); IOCTL_NTUA_LESEN (NTUA_STAND + Daten);
  Ueberlaufzaehler.  Lesepuffer je Geraet (nicht static).  usbnt.sys
  exportiert dafuer 53 Namen (usbd_copy_out, usbd_xfer_frame_len).
- werkzeug/usbaudio.c: widMessage (GETNUMDEVS/CAPS A+W, OPEN, ADDBUFFER,
  START, STOP, RESET, GETPOS, CLOSE); Faden liest den Ring, rechnet auf
  das Programmformat um (Rate, Mono/Stereo, 8/16 Bit).  Getrennte
  Nummerierung fuer Wiedergabe- und Aufnahmegeraete.
- werkzeug/uac.c: uac_aufnahme.  usbton: aufnahme, wmaufnahme.
- pruefstand/pademu.py: --art mikro (UAC1-Mikrofon, ISO IN ueber
  usb-redir, fortlaufender 1-kHz-Sinus); Verbindung kann ISO-Stroeme.
Geprueft: Aufnahme direkt 3 s = 288000 Byte, Amplitude 10000, 0
Unstetigkeiten, 0 Ueberlaeufe; ueber winmm 48k/16/1, 44,1k/16/2,
22k/8/1, 11k/16/1 je 0 Unstetigkeiten; Wiedergabe und Aufnahme
gleichzeitig (QEMU usb-audio + Mikrofon) beide sauber.

## Fassung 2026.10.9.5: Kein Ton am HyperX Cloud Stinger Core -- Ursache behoben

Anwender-Log: zwei Feature Units, FU 3 (Quelle 6, Mikrofon) VOR FU 2
(Quelle 1, Lautsprecher).  uac_fu nahm die erste mit Lautstaerke -- also
das Mikrofon; der Lautsprecherzweig blieb, wie das Geraet hochkommt
(viele: stumm oder leise).  Ausserdem schaltete die DLL beim Oeffnen nie
auf hoerbar.
- uac.c: Signalweg (IT/OT/Mixer/Selector/FU/Processing/Extension)
  merken; uac_fu_fuer: Wiedergabe vom Strom-Terminal vorwaerts, Aufnahme
  vom Ausgangsterminal rueckwaerts bis zur ersten FU.  Laut-/Stumm-
  Funktionen nehmen die FU ausdruecklich.
- usbaudio.dll: Lautstaerke auf die FU des Wiedergabezweigs; beim Oeffnen
  VOR dem Strom: stumm aus, zuletzt gesetzte Lautstaerke (anfangs 0 dB).
- usbton: liste zeigt Signalweg und Regler je Schnittstelle; lautlesen,
  roh (Deskriptor hex), laut/stumm mit "mikro"; ton schaltet vorher hoerbar.
- pademu.py --art headset: wie das Anwendergeraet aufgebaut (FU 3 vor
  FU 2, Mono/Stereo-Alternativen), Lautsprecher kommt STUMM und auf
  Minimum hoch, schreibt das Empfangene mit (stumm = Stille).
Geprueft: liste ordnet FU 2 dem Lautsprecher, FU 3 dem Mikrofon zu;
anfangs stumm=1, -48 dB; usbton ton und winmm am frisch angesteckten
Emulat: hoerbarer Sinus, 0 Unstetigkeiten, erstes Paket schon hoerbar.
Nebenbefund: der NT-Anmeldeklang lief ueber die alte DLL in den stummen
Zweig -- genau das Bild beim Anwender.
Pruefstand: mit usb-redir-Geraet im Gast laeuft die Gastzeit ohne KVM
stark verzerrt (3 s Ton "dauern" 24..39 s, auch ohne DLL); Ton und
Kontinuitaet stimmen.  Nicht als Treiberfehler werten.

## Geraetemanager: startet ohne PCISCAN, "Treiber einrichten" auch in der CPL

Anwender: "Der Geraetemanager muss auch starten, wenn PCISCAN nicht laeuft,
und Treiber einrichten fehlt -- warum?"
- Start ohne PCISCAN: GetList sollte laut eigenem Kommentar ohne Dienst
  nur die PCI-Liste leer lassen; Vorbereiten machte aus FALSE aber
  Fehlermeldung + Abbruch.  Jetzt gPciFehlt: der Baum zeigt einen
  Warnknoten ("Dienst PciScan laeuft nicht -- USBINST ein, dann
  Aktualisieren"), USB wird angezeigt.  Aktualisieren versucht GetList
  erneut (im Gast: net start PciScan, Aktualisieren -> PCI-Geraete da).
- "Treiber einrichten" fehlte in DEVMGR.CPL absichtlich: Bauschalter
  NUR_ANSEHEN nahm wegen der Virenscanner-Fehlalarme (Dropper-Muster) alle
  Dienstfunktionen UND den Knopf heraus -- ohne den Anwender zu fragen.
  Jetzt nach dem Helfer-Muster (av-fehlalarm, Rezept 6): der Knopf ist in
  der CPL wieder da und startet DEVMGR.EXE /einrichten aus system32; die
  EXE oeffnet sofort "Treiberdatei waehlen".  Die CPL importiert weiter
  keine Dienstfunktionen, neu ist nur CreateProcessA.  Im Gast geprueft.
  VirusTotal fuer die neue CPL steht aus.

## Fassung 2026.10.9.6: USB-Audio -- Mixer (Lautstaerkeregelung)

Anwender: "UAC Ton geht, nur noch ein Mixer bitte."
- usbaudio.dll: mxdMessage.  Je Audiogeraet ein Mixer: Ziel
  "Lautsprecher" (DST_SPEAKERS -> waveOut) mit Lautstaerke + Ton aus,
  Quelle "Wave"; Ziel "Aufnahme" (DST_WAVEIN) mit Quelle "Mikrofon"
  (Lautstaerke + Mikrofon aus).  Regler = Feature Unit des jeweiligen
  Zweigs (uac_fu_fuer), Werte im Geraet (fuer alle Prozesse gleich).
  0..65535 <-> dB: 20*log10 / 10^(dB/20) (eigene Potenzfunktion, keine
  CRT).  W- und A-Strukturen ueber cbStruct/cbmxctrl.  Kanaele des
  Lautsprecher-Ziels: 2 nur bei getrennten K1/K2-Reglern.
  MM_MIXM_CONTROL_CHANGE an offene Mixer desselben Prozesses.
- uac.c: uac_ac_oeffnen versucht es bei Freigabeverletzung bis zu 20 x
  (ntugen oeffnet exklusiv; Lautstaerkeregelung und Programm zugleich).
- audioinst.c: Drivers32 zusaetzlich "mixer"/mixer1..9 -- NT-4.0-winmm
  laedt Mixer-Treiber NUR ueber eigene mixer-Eintraege (gemessen:
  mixerGetNumDevs = 0, solange nur wave1 eingetragen war).
- usbton mixer [laut P | stumm 0|1].
Geprueft (Headset-Emulat wie HyperX): Mixer zeigt Lautsprecher/Wave/
Aufnahme/Mikrofon; 50 % -> FU 2 -1541/256 dB (-6,02 dB), Ton aus nur an
FU 2, Mikrofon unberuehrt.  sndvol32: Spalte "Lautsprecher", Regler
steht auf dem Geraetewert; "Alles aus" an/aus und Regler nach oben
wirken bis ins Geraet.  sndvol32 zeigt den Balance-Regler auch bei einer
Linie mit 1 Kanal (dort wirkungslos).  Wiedergabe danach weiter hoerbar.

## Nachtrag: echter Xbox-Controller -- vom Anwender bestaetigt

Der Anwender meldet: Xbox-Controller an echter Hardware geht, mit
Vibration.  Die Vermerke "Nicht nachgewiesen: echter Controller" (Runden
17/18) und "Am echten 1708 offen" (Xbox One S) sind damit ueberholt.  Am
echten Geraet geprueft hat der Anwender, nicht der Pruefstand; welches
Modell und welcher Treiberstand, ist hier nicht festgehalten.

## Fassung 2026.10.10.1: Auslagerbarer Code (PAGE), zweiter Hinweis Ray Hinchliffe

Ray Hinchliffe (SIV): "apart from DriverEntry() none of the code is
pageable ... routines are pageable assuming they stay at passive level
... should have PAGED_CODE() as the first statement, that includes
DriverEntry()."  Dazu der fruehere Hinweis: ".text + .data marked as
paged, they should not be, paged code should be in PAGE, DriverEntry in
INIT."

Umsetzung:
- nt/ntpaged.h (neu): NTU_PAGED(fn) = #pragma alloc_text(PAGE, fn);
  NTU_PAGED_CODE() = erste Anweisung (Debug: IRQL <= APC_LEVEL pruefen,
  frei leer).
- ntusbnt.c: DriverEntry PAGED_CODE() (liegt in INIT); NtuUnload nach
  PAGE + PAGED_CODE().
- ntuaudio.c: probe/attach/detach nach PAGE + PAGED_CODE() (passiv beim
  An-/Abstecken).  xbox/ntxbox.c, audio/ntaudio.c: DriverEntry PAGED_CODE().
- bau-clangcl.sh: PAGE NICHT mehr in .text gemergt (/merge:PAGE=.text
  raus), stattdessen /section:PAGE,ERP -> eigener auslagerbarer Abschnitt.
- abschnitte.py: PAGE = CODE|EXEC|READ (auslagerbar, KEIN NOT_PAGED).
  .text/.rdata/.data bleiben resident, INIT verwerfbar.
Nur nachweislich IMMER passive Routinen kommen nach PAGE; die IRP-/Strom-
/DPC-Pfade des FreeBSD-Stapels bleiben resident (ein falsch ausgelagerter
ISR-naher Pfad gaebe denselben Bugcheck wie nicht-residentes .text).

Abschnitte aller .sys geprueft: .text/.rdata/.data NOT_PAGED,
INIT+PAGE+.reloc auslagerbar.  usbnt/usbxbox/usbaudio und alle vier
Einzelbauformen gebaut; im Gast geladen, entladen, Tastatur/Maus/UsbDev
laufen.  PAGE-Groesse usbnt 0x91 (NtuUnload), usbaudio 0x385.

Leitfaden fuer kuenftige Treiber: TREIBER-BAU-NT4.md (Werkzeugkette,
beide Ray-Regeln, Pruef-Rezept).

## Fassung 2026.10.10.2: USB-Audio -- Lautstaerke springt nicht mehr, Regler synchron

Anwenderbericht: "Die Lautstaerke bleibt nicht gleich, sie springt zurueck.
Musik laeuft, ich klicke auf Start, dann erhoeht sich die allgemeine
Lautstaerke. Die Lautstaerkeregler sind nicht synchron."

Zwei Ursachen gefunden und behoben (werkzeug/usbaudio.c):
1. Beim Oeffnen des Wave-Geraets setzte die DLL die Lautstaerke auf den
   zuletzt gesetzten Wert, mangels eines solchen auf 100 %/0 dB
   (lautstaerke_setzen beim OPEN).  Ein Start-Klick riss die Lautstaerke
   also hoch.  Jetzt: nur_stumm_aus() hebt beim Oeffnen NUR die
   Stummschaltung auf, die Geraete-Lautstaerke bleibt unberuehrt.
2. WODM_GETVOLUME lieferte die zwischengespeicherte Variable
   d->lautstaerke, nicht den Geraetewert; der Mixer liest dagegen aus dem
   Geraet.  Dadurch liefen winmm-Regler und Mixer auseinander.  Jetzt:
   lautstaerke_lesen() holt den Wert frisch aus dem Geraet (dieselbe
   dB<->winmm-Umrechnung wie der Mixer, ueber hoch10).  Das Geraet ist die
   einzige Wahrheit.
usbton: neuer Befehl "wmlaut [PROZENT]" (waveOutSetVolume/GetVolume) zum
Pruefen der Synchronitaet.

Geprueft (Headset-Emulat): Geraet auf 40 %, Wiedergabe oeffnen -> Wert
bleibt 26219 (springt NICHT auf 65535).  winmm setzt 70 % -> Mixer liest
45877 (=0xB335).  Mixer setzt 25 % -> Geraetewert 16388.  Alle drei Leser
(winmm, Mixer, direkter Geraetewert) zeigen denselben Stand.

## Fassung 2026.10.10.3: USB-Audio -- Lautstaerke endgueltig richtig

Nachbesserung zu 2026.10.10.2.  Diese Fassung hatte beim Oeffnen GAR NICHT
mehr hoerbar gestellt -- Geraete, die stumm/am Minimum hochkommen (HyperX),
blieben still ("kein Systemklang").  Ausserdem fehlten zwei Meldewege.

Drei Korrekturen (werkzeug/usbaudio.c):
1. Hoerbar machen NUR wenn noetig: beim Oeffnen wird der Geraetewert
   gelesen; steht er praktisch am Minimum (< 2 %), wird auf einen hoerbaren
   Wert gesetzt, sonst bleibt die Einstellung des Anwenders unberuehrt.
   WICHTIG: das muss VOR uac_strom_oeffnen passieren -- danach haelt der
   Strom das Geraet exklusiv und das Steuergeraet laesst sich nicht mehr
   oeffnen (Freigabeverletzung), dann bliebe alles stumm.
2. WODM_SETVOLUME meldet dem Mixer jetzt MM_MIXM_CONTROL_CHANGE.  Vorher
   uebernahm sndvol32 die Lautstaerke eines Programms nicht.
3. Waechterfaden: solange ein Mixer offen ist, wird der Geraetewert alle
   400 ms gelesen und eine Aenderung gemeldet -- damit bemerkt sndvol32
   auch das Lautstaerkerad AM HEADSET.  Laeuft ein Strom, wird nicht
   gelesen (Zaehler GERAET.stroeme), damit die Wiedergabe Vorrang hat.

Falle (gekostet: eine halbe Sitzung): bau-werkzeuge.sh liegt in
quelle/werkzeug/, nicht im Projektwurzelverzeichnis.  Ein Aufruf mit
falschem Pfad baut NICHT neu, und man testet unbemerkt die alte DLL.
Gegenprobe: `strings usbaudio.dll | grep <neuer Text>`.

Geprueft (Headset-Emulat, kommt stumm und auf Minimum hoch):
- Geraet auf Minimum -> Oeffnen hebt an -> Ton 1,99 s, 0 Unstetigkeiten.
- Geraet bewusst auf 30 % -> Oeffnen laesst es -> bleibt 19662, Ton da.
- winmm setzt 70 % -> Mixer liest 45877 (=0xB335), gleicher Wert.
- Rad am Headset auf -6 dB gedreht (neuer Pruefstandsbefehl RAD) -> Mixer
  liest 32845 statt des alten Werts.
pademu.py: neuer Steuerbefehl "RAD <dB*256>" simuliert das Rad am Headset.
usbton: "wmlaut [PROZENT]" setzt/liest die waveOut-Lautstaerke.

## Fassung 2026.10.10.4 bis .6 -- an der ECHTEN Maschine gemessen

Ueber die GitHub-Fernsteuerung (crc_agent auf echtem NT 4.0)
liess sich erstmals am echten HyperX messen statt am Emulat.  Dabei kam
heraus, dass mehrere Dinge gleichzeitig falsch waren.

1. .4 -- Versionsnummer sichtbar: "usbton version" zeigt die Fassung von
   usbaudio.dll, usbaudio.sys, usbnt.sys und den Bauarten.  Damit fiel
   sofort auf, dass auf der Maschine noch die kaputte .2 lief, waehrend
   wir am Emulat .3 prueften.  (bau-werkzeuge.sh reicht FASSUNG jetzt als
   -DFASSUNG_TEXT durch; usbton selbst hatte vorher keine Versionsangabe.)
2. .5 -- Geraet blieb dauerhaft belegt.  schliessen() gab bei noch
   anstehenden Puffern WAVERR_STILLPLAYING zurueck und schloss NICHT.  Der
   Explorer (spielt die Systemklaenge) behandelt den Fehler nicht, behaelt
   den Griff, und da er nie endet, war UsbAudio1 bis zum Abmelden belegt
   -- START scheiterte mit 170/ERROR_BUSY, kein Klang mehr.  Jetzt werden
   offene Puffer zurueckgegeben und trotzdem geschlossen.
3. .6 -- Skala.  Umrechnung dB <-> Regler war physikalisch (10^(dB/20)).
   Bei einem Bereich von -64..0 dB landet ein angenehmer Pegel (-19 dB)
   damit bei 11 %; alles Brauchbare im obersten Zehntel ("die Skala haut
   nicht hin").  Jetzt gleichmaessig: db_nach_regler/regler_nach_db legen
   den dB-Bereich linear auf 0..65535.  Gemessen: -6 dB bei Bereich
   -64..0 -> 59391 (91 %), vorher 50 %.

Befunde am echten Geraet (Deskriptor, usbton roh):
- Die AudioControl-Schnittstelle hat NULL Endpunkte, also KEINEN
  Status-Interrupt-Endpunkt (UAC1 3.7.1.2).  Das Geraet kann
  Reglerbewegungen ueber USB-Audio prinzipiell nicht melden -- nur Pollen.
- Es gibt aber eine vierte Schnittstelle: Nr. 3, Klasse 03 (HID),
  Endpunkt 83 Interrupt IN, max 32 Byte, Berichtsdeskriptor 426 Byte.
  Dort meldet das Lautstaerkerad hoechstwahrscheinlich als HID Consumer
  Control (Volume Increment 0xE9 / Decrement 0xEA / Mute 0xE2).
- Gemessen: das Rad VERSTELLT Feature Unit 2 (vorher -4864, nach "ganz
  leise" -16384, spaeter -1536).  Pollen sieht die Aenderung also; der
  HID-Weg waere der schnelle, ereignisgesteuerte Ersatz.
- Die Maschine hat eine eingebaute Soundkarte als Wave-Geraet 0; das
  Headset ist Geraet 1.  Systemklaenge und Taskleisten-Lautstaerke gehen
  deshalb an die Soundkarte, nicht ans Headset (Multimedia ->
  bevorzugtes Geraet umstellen).

OFFEN: HID-Schnittstelle 3 uebernehmen und Endpunkt 83 auswerten, damit
Radbewegungen sofort gemeldet werden statt per 400-ms-Abfrage.  Zweitens:
Mixer-Benachrichtigung ueber Prozessgrenzen -- mxdMessage-DLLs werden je
Prozess geladen, mx_melden erreicht nur Mixer im eigenen Prozess; andere
Prozesse erfahren Aenderungen nur ueber den Waechterfaden (Pollen).

fernsteuerung/sende_datei.py: grosse Dateien stueckweise zur NT-Maschine
(PUT ueberschreibt und vertraegt nur 1400 Zeichen je Zeile -> Stuecke +
copy /b; >90 Stuecke werden auf mehrere Auftraege verteilt).

## Fassung 2026.10.11.4 (nachgetragen): Nachlauf, INF, HID

Diese drei Dinge waren gebaut und ausgeliefert, standen aber nicht hier.
- Nachlauf im Audiotreiber: nach dem letzten Klang sendet der Strom noch
  rund zwei Sekunden Stille (sc_leerlauf), statt sofort aufzuhoeren.  Ein
  Funk-Headset braucht nach dem Anlaufen Zeit; der Anfang kurzer Klaenge
  ging sonst verloren.  STOP/CLEANUP entfernen nur noch den Klienten, der
  Rueckruf beendet den Strom selbst.  Gemessen: Mitschnitt nach ding.wav
  2,54 s statt 0,54 s, Ton vollstaendig bis 0,524 s.
- INF-Dateien fuer die drei Pakete -- in dieser Fassung FEHLERHAFT, siehe
  unten.
- HID fuer NT 4.0: quelle/werkzeug/hiddll.c (HID.DLL, alle 21 Funktionen,
  die DirectInput laedt, aufgesetzt auf \\.\UsbGenN) und hidtest.c.

## Fassung 2026.11.11.1: Durchsicht auf Vollstaendigkeit und Fehler

Anlass: der Baum wurde nach GitHub gesichert und danach als Ganzes
geprueft.  Gefunden und behoben:

INF-Dateien (ausgeliefert, FEHLERHAFT -- nicht die aus 2026.10.11.4 nehmen):
- USBXBOX.INF trug den Dienst unter "usbaudio" mit usbaudio.sys ein
  (Textersetzung hatte nur Grossbuchstaben erfasst): kein Xbox-Treiber,
  verfaelschter Audio-Eintrag.  Joystick-Eintrag frei erfunden.
- USBAUDIO.INF: Start "manuell" und ErrorControl 1 statt Systemstart und 0.
- USBNT.INF: Gruppe "Extended base" statt "Keyboard Port"; es fehlten
  Parameters\Ausgabe und ConnectMultiplePorts fuer Kbdclass/Mouclass
  (ohne die arbeiten USB-Tastatur/-Maus nicht neben PS/2); Reste der
  Audio-Vorlage im Kopf.
  Ursache: Werte angenommen statt aus audioinst.c/xboxinst.c/usbinst.c
  abgelesen.  Alle drei von Hand neu geschrieben, Werte aus den Quellen;
  USBAUDIO.INF legt jetzt auch die Merkposten unter SOFTWARE\UsbNt\Audio
  an, damit "AUDIOINST aus" eine INF-Einrichtung entfernt.  Jede INF sagt
  im Kopf, was sie NICHT tut (suchen, sichern, pruefen).
  NICHT auf einer NT-4.0-Maschine erprobt.

Bau:
- quelle/werkzeug/res/werkzeug.ico und manifest.xml fehlten im Projekt
  (lagen nur im Arbeitsbereich).  windres scheiterte deshalb still, und
  14 Programme -- darunter USBINST, AUDIOINST, XBOXINST, USBTON -- wurden
  OHNE Versionsangabe gebaut und ausgeliefert.  Beide Dateien aus
  bin/programme/usbtest.exe zurueckgewonnen.
- HID.DLL, HIDTEST.EXE und DIPROBE.EXE standen in keinem Bauskript; jetzt
  in bau-werkzeuge.sh.  hidtest.c auf ntbasis.c umgestellt (zog msvcrt).
- pruefe-bau.py (neu): Abschnitte, PE-Pruefsumme, Versionsangabe,
  NT-4.0-fremde Importe, msvcrt.  Nach jedem Bau laufen lassen.

Audiotreiber (quelle/ober/ntuaudio.c):
- sc_leerlauf wurde beim Start nicht zurueckgesetzt.  Nach abgelaufenem
  Nachlauf stand er am Anschlag; der Rueckruf konnte den frisch
  gestarteten Strom sofort wieder beenden, wenn er lief, bevor der neue
  Klient als spielend eingetragen war (Wettlauf) -- der Klang bliebe
  stumm.  Im Pruefstand zufaellig nie aufgetreten.
- Ein laufender Strom hat EINE Rate.  START mit anderer Rate oder
  Abtastgroesse wird jetzt mit STATUS_DEVICE_BUSY abgewiesen.
- teil auf die Groesse von sc_mix/sc_mixaus begrenzt.
- ntua_detach gibt die Ringe aller Klienten frei (Leck beim Abziehen
  waehrend der Wiedergabe).

usbaudio.dll (quelle/werkzeug/usbaudio.c):
- Jede Oeffnung nahm die Rate ihres Programms, wenn das Geraet sie
  konnte.  Seit dem Mischer falsch: 44,1-kHz-Musik plus Systemklang, der
  auf 48 kHz umgerechnet wird, im selben Strom -- rund 8 % verstimmt.
  Jetzt immer die beim Auflisten ermittelte Geraeterate (rate48).
- Der Waechterfaden endete nie (g_waechter_aus wurde nirgends gesetzt).
  Jetzt waechter_beenden() beim letzten MXDM_CLOSE und bei DRV_FREE.

Stand der Binaerdateien -- WICHTIG:
- bin/treiber, bin/programme und alle installer-* enthalten weiter die
  Fassung 2026.10.11.4, die an der echten Maschine laeuft.  Geaendert
  wurden dort NUR die INF-Dateien.
- bin/ungeprueft-2026.11.11.2/ enthaelt den Neubau aus dem heutigen
  Quellstand: 7 Treiber, 25 Programme, pruefe-bau.py ohne Maengel.
  Gebaut und statisch geprueft, aber NICHT gelaufen -- weder im Pruefstand
  (mit dem Arbeitsbereich verloren) noch an der echten Maschine.  Erst
  nach einem Lauf in die Installer uebernehmen.

## Fassung 2026.11.11.2: Durchsicht des Host-Controller-Teils

Nur usbnt.sys und die vier Einzelbauformen; Audio blieb aussen vor.

Geprueft:
- Herkunft: alle 17 Dateien unter freebsd/dev/usb/controller/ (xhci, ehci,
  ohci, uhci, jeweils .c/_pci.c/.h/reg.h, usb_controller.c) sind bitgleich
  mit FreeBSD main, Commit 139fa2c1 -- HERKUNFT.txt stimmt.  Fehler koennen
  also nur in der NT-Unterlage (quelle/nt, ~4900 Zeilen) liegen.
- Uebersetzung der Unterlage und der Controller mit eingeschalteten
  Warnungen (der Bau laeuft sonst mit -w): keine ernsthafte Meldung in
  der Unterlage.  Die "may be uninitialized" in ehci.c/ohci.c/xhci.c
  stehen im unveraenderten FreeBSD-Code.
- Durchgelesen: PCI-Zugriff, Registerblock abbilden/freigeben,
  Unterbrechung anhaengen/abbauen, ISR, Erkennen/Sperren/Freigeben je
  Controllerart.

Behoben (quelle/nt/ntgeraet.c):
1. 64-Bit-Registerblock.  bus_alloc_resource_any las nur das untere
   BAR-Register (bus.HighPart = 0).  Liegt der Block oberhalb von 4 GB
   ("Above 4G Decoding", bei xHCI auf neueren Platinen ueblich), wurde
   die untere Adresshaelfte abgebildet -- fremder Speicher -- und als
   Registerblock gelesen.  Jetzt wird bei 64-Bit-BARs die obere Haelfte
   gelesen; ist sie nicht null, bricht das Anhaengen mit klarer Meldung
   ab (NT 4.0 kann dort nichts abbilden).  Dazu: ein E/A-BAR, wo Speicher
   erwartet wird, wird abgewiesen.
2. bus_teardown_intr: nach IoDisconnectInterrupt konnte noch ein DPC in
   der Warteschlange stehen und nach ntu_free(i) laufen.  Jetzt
   KeRemoveQueueDpc; dev->Intr wird geloescht.
3. bus_release_resource: dev->Res zeigte nach der Freigabe weiter auf den
   freigegebenen Block.

Befunde, NICHT geaendert (brauchen einen Lauf an echter Hardware):
a. Herunterfahren.  Es gibt weder IRP_MJ_SHUTDOWN noch
   IoRegisterShutdownNotification; bus_generic_shutdown ist leer.  Beim
   Neustart laufen die Controller weiter (Unterbrechungen frei, DMA
   aktiv).  FreeBSD haelt sie ueber usb_shutdown -> set_hw_power_sleep
   (USB_HW_POWER_SHUTDOWN) an.  Nicht blind einbauen: unter NT 4.0 kommt
   die Abschaltmeldung VOR dem Schreiben der Dateisysteme -- ein
   Controller-Halt dort kann Daten auf einem USB-Datentraeger kosten.
   Erst die Reihenfolge am Pruefstand klaeren.
b. xHCI, Host System Error.  FreeBSD schaltet USBCMD.HSEE ein; ein HSE
   zieht die Leitung ohne EINT/IMAN.IP.  Der Filter (ntu_intr_erkannt)
   meldet dann "nicht meins", die pegelgesteuerte Leitung bleibt stehen
   -> Dauerunterbrechung.  Abhilfe waere: HSE im Filter mitnehmen und
   beim Sperren HSEE loeschen.  Reiner Hardwarefehlerpfad.
c. UHCI-Filter nimmt USBSTS & 0x3F; Bit 5 (HCHalted) ist ein Zustand und
   keine Unterbrechungsquelle.  Ein angehaltener UHCI auf geteilter
   Leitung beansprucht damit die Unterbrechungen der Nachbarn zuerst
   (wird ueber "Gesperrt" wieder aufgeloest, kostet aber Umwege).  0x1F
   waere genauer.  FreeBSD prueft genauso (UHCI_STS_ALLINTRS), dort stoert
   es nicht, weil jeder Handler gerufen wird.
d. UHCI: HSE und HCPE sind nicht ueber USBINTR sperrbar.  Zwischen ISR
   und Faden bleibt die Leitung in diesem Fehlerfall stehen.

Gebaut: usbnt.sys, uhcint/ohcint/ehcint/xhcint.sys als 2026.11.11.2 in
bin/ungeprueft-2026.11.11.2/treiber, pruefe-bau.py ohne Maengel.
NICHT gelaufen.  Installer und bin/treiber bleiben 2026.10.11.4.


## Fassung 2026.11.15.52: CD/DVD-Laufwerke, Bau ohne DDK, Einbindung in die Installations-CD

**USB-CD/DVD-Laufwerke** (ober/ntumass.c).  Bisher lehnte der
Massenspeicher-Teil alles ausser 512-Byte-Sektoren ab.  Jetzt:
- INQUIRY Geraetetyp 5 -> eigenes NT-Geraet `\Device\CdRomN`
  (FILE_DEVICE_CD_ROM, FILE_READ_ONLY_DEVICE), N ab
  IoGetConfigurationInformation()->CdRomCount, das danach hochgezaehlt
  wird -- wie bei cdrom.sys.  Laufwerksbuchstabe wie bei den Platten.
- 2048-Byte-Bloecke, keine Partitionen, schreibgeschuetzt; CDFS bindet sich
  am VPB an.  Beim Einstecken bis zu 20 s zusaetzlich auf "wird bereit"
  (02/04) warten -- die Scheibe muss anlaufen.
- Steuercodes: IOCTL_CDROM_READ_TOC (READ TOC Format 0, MSF),
  GET_LAST_SESSION (Format 1, LBA) -- beide durch den Arbeitsfaden, damit
  sie sich nicht mit laufenden Bulk-Uebertragungen mischen;
  GET_DRIVE_GEOMETRY (2048, 64x32), CHECK_VERIFY wie bei Platten,
  EJECT/LOAD per START STOP UNIT, MEDIA_REMOVAL/RESERVE/RELEASE.
- Geprueft (QEMU, usb-bot + scsi-cd, an xHCI und an EHCI):
  `umass: F: LUN 0 = QEMU QEMU CD-ROM CD/DVD`, `dir F:` zeigt die
  Scheibe, `type F:\USBCD.TXT` liest die Datei.

**Bau ohne NT-4.0-DDK** (bau-frei.sh, einfuhrlib_frei.py,
quelle/nt/ntfreiekoepfe.h).  Mit den freien DDK-Koepfen von mingw-w64.
Die Einfuhrbibliotheken entstehen aus den tatsaechlichen Einfuhren der
Objekte und werden gegen die Exportliste eines echten NT-4.0-Kerns und HAL
geprueft -- was NT 4.0 nicht hat, bricht den Bau ab.  Ergebnis gegen den
DDK-Bau 2026.10.10.1 verglichen: gleiche Abschnitte, Groesse +-1 kB, gleiche
Einfuhren bis auf fuenf (Interlocked*, KeInitializeSpinLock jetzt als
eingebaute Befehle).  Wichtig dabei: clang-cl gilt fuer die mingw-Koepfe
nicht als GNU-C, die streichen dann `__attribute__` -- und damit `packed`.
Deshalb `-fgnuc-version`.

**Einbindung in die Installations-CD** (cd-einbindung/usb_einbinden.py),
ausprobiert mit der reinen NT 4.0 Server Enterprise (deutsch, ohne SP;
Kern laeuft also auch auf RTM):
Textsetup kopiert usbnt.sys, die Registry-Vorlage traegt den Dienst.
Erster Start (Umwandlung nach NTFS) und grafischer Teil schon mit USB;
Tastatur-Eingaben im grafischen Teil kamen ueber die USB-Tastatur,
Anmeldung (Strg+Alt+Entf) ueber eine USB-Tastatur, die an die Konsole
gebunden war (PS/2 bekam nichts).  Stick D:/E:, CD E:/F:, Maus klickt.
Controller: xHCI (qemu-xhci), EHCI (ICH4) und UHCI (PIIX3).

Bemerkt: "uhub_explore: illegal enable change, port 1" einmal nach dem
Anmelden an xHCI; Tastatur arbeitete weiter.

Gebaut aus dem Repo-Stand 2026.11.11.2 + obige Aenderungen als
2026.11.15.52 in bin/2026.11.15.52 (mit bau-frei.sh).  Dieser Bau ist
in QEMU gestartet (xHCI, alle vier Geraete erkannt, bis Anmeldung);
der volle Durchlauf oben lief mit dem gleichen ntumass.c auf dem Stand des
Pakets usb-nt4-komplett30.  Installer und bin/treiber unveraendert.

## 2026.11.16.59 -- umass: anlaufendes DVD-Laufwerk nicht als "leer" behandeln

HP EliteDesk 705 G5, grafisches Setup: nach den Dialogen (Scheibe steht)
"Auf D:\ kann nicht zugegriffen werden. Unzulaessige Funktion", auch beim
Wiederholen; Ab- und Anstecken des Laufwerks half.  Vermutete Ursache: das
Laufwerk meldet beim Anlaufen sekundenlang 02/04 ("wird bereit") bzw. UNIT
ATTENTION; der Treiber wartete nur gut 1 s, erklaerte das Medium fuer
entnommen und liess CDFS pruefen -- das gab den Datentraeger auf.

- ntum_lesen: bei 02/04 und Transportabriss bis 30 s warten; bei 06/xx
  bereit abwarten und Groesse vergleichen (gleich = dieselbe Scheibe,
  weiterlesen); "leer" nur noch 02/3A.
- ntum_leer: 02/04 zaehlt nicht als leer.
- Regelmaessige Abfrage: CD geduldig neu einlesen; gleiche Groesse -> kein
  DO_VERIFY_VOLUME.

Geprueft in QEMU (8 Prozessoren, xHCI, usb-bot + scsi-cd): Lesen, danach
"change" auf dieselbe ISO (UNIT ATTENTION) und weiterlesen.  Das Anhalten
der Scheibe kann QEMU nicht nachstellen -- auf echter Hardware noch offen.

## 2026.11.16.60 -- im grafischen Setup auf das USB-CD/DVD-Laufwerk warten

HP EliteDesk 705 G5: grafisches Setup endete mal mit "Interne
Setup-Datenstrukturen sind beschaedigt (Phase 0)", mal lief es durch.
Vermutung: setup.exe sucht sein Quelllaufwerk, bevor das USB-DVD-Laufwerk
nach dem Neustart angelegt ist.  DriverEntry wartet jetzt auch bei
System\Setup\SystemSetupInProgress != 0 bis zu 30 s auf ein USB-CD/DVD
(bisher nur im Textmodus-Setup).  In QEMU nur geprueft, dass der Start mit
gesetztem Wert durchlaeuft; die Wirkung auf echter Hardware ist offen.

## 2026.11.16.61 -- grafisches Setup: bis 3 Minuten auf das USB-CD/DVD warten, mit Anzeige

"Interne Setup-Datenstrukturen sind beschaedigt (Phase 0)" ist geklaert:
syssetup.dll liest aus $winnt$.inf den Quellpfad (\device\cdromN\...) und
sucht den Laufwerksbuchstaben dazu; gibt es das Laufwerk nicht, kommt diese
Meldung.  In QEMU nachgestellt: derselbe Start ohne angeschlossenes
USB-DVD-Laufwerk liefert genau diesen Fehler, mit Laufwerk laeuft Setup an
(4 von 4 Starts, 8 Prozessoren).

Auf dem HP 705 G5 reichten die 30 s aus .60 nicht.  Jetzt im grafischen
Setup bis 180 s, mit Zeilen auf dem Startbildschirm ("usbnt: Setup -- warte
...", alle 10 s eine Zeile, am Ende "gefunden" oder "KEIN ... nach 3
Minuten").  Warum das Laufwerk dort so spaet erscheint, ist offen.

## 2026.11.16.62 -- Sicherungstakt neben der Unterbrechung

HP EliteDesk 705 G5, MPS-HAL mit aus ACPI erzeugter Tabelle (ACPIMPS):
im grafischen Setup erschien das USB-DVD-Laufwerk auch nach 150 s nicht,
obwohl Uhr und Warteschleife stimmen (ACPIMPS-Messung: 500 ms = 494 ms,
100 ms = 100 ms).  Vermutung: der xHCI bekommt jetzt eine Leitung, auf der
nichts ankommt (vorher lief er dort im Abfragebetrieb).

- Auch mit angeschlossener Leitung weckt ein Takt alle 50 ms den
  Unterbrechungsfaden.  Freigegeben wird nur, wenn der ISR gesperrt hat.
- Setup-Zeile nennt jetzt Wartezeit, echte Unterbrechungen und Fadenlaeufe.

QEMU (8 Prozessoren, xHCI mit echter Leitung): Laufwerk nach 1 s,
92 echte Unterbrechungen, Setup-Assistent erscheint.
