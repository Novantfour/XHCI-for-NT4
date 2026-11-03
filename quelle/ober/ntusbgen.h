/*
 *  ober/ntusbgen.h -- die allgemeine USB-Geraeteschnittstelle.
 *
 *  Das ist der Vertrag fuer alles, was NICHT Tastatur oder Maus ist:
 *  Joystick und Gamepad (HID oder herstellereigen), Drucker, Scanner,
 *  Kartenleser, serielle Wandler, Massenspeicher -- jedes USB-Geraet,
 *  fuer das kein anderer Treiber zustaendig ist.
 *
 *  Der USB-Treiber selbst weiss nichts ueber Joysticks oder Drucker, und
 *  er soll es auch nicht.  Er stellt jedes Geraet bereit; was es
 *  bedeutet, entscheidet der Treiber darueber.  Den schreibt man gegen
 *  diese Datei -- im Kern oder im Benutzermodus, mit denselben Aufrufen.
 *
 *  ------------------------------------------------------------------
 *  Wie man ein Geraet findet
 *
 *      HKLM\HARDWARE\DEVICEMAP\USB
 *          \Device\UsbDev0 = "VID_046D&PID_C215&MI_00 03/00/00 ..."
 *          \Device\UsbDev1 = ...
 *
 *  Jede SCHNITTSTELLE eines Geraets ist ein eigener Eintrag.  Ein
 *  zusammengesetztes Geraet -- Tastatur mit Medientasten, Drucker mit
 *  Scanner -- erscheint also mehrfach, einmal je Schnittstelle, und
 *  jeder Teil kann einen eigenen Treiber bekommen.
 *
 *  Aus dem Benutzermodus:   CreateFile("\\\\.\\UsbDev0", ...)
 *  Aus dem Kern:            IoGetDeviceObjectPointer(L"\\Device\\UsbDev0")
 *                           und IoBuildDeviceIoControlRequest
 *
 *  ------------------------------------------------------------------
 *  Wie man spricht
 *
 *  Sechs Aufrufe, alle METHOD_BUFFERED.  Jede Antwort beginnt mit
 *  NTUG_ERGEBNIS; danach folgen, je nach Aufruf, die Daten.  Ein
 *  USB-Fehler ist KEIN Fehler des Aufrufs: DeviceIoControl gelingt, und
 *  der USB-Fehler steht in NTUG_ERGEBNIS.Fehler.  So bekommt man auch
 *  bei einem Kurzpaket oder einer Zeitueberschreitung, was angekommen
 *  ist.
 *
 *  Isochron (Kameras, USB-Audio) geht ueber einen eigenen Aufruf,
 *  IOCTL_NTUG_ISO, weil dort nicht eine Laenge zaehlt, sondern eine je
 *  Rahmen.  Auf SB7x0/SB8x0 fehlt noch die AMD-PLL-Abhilfe; siehe dort.
 *
 *  ------------------------------------------------------------------
 *  Lebensdauer
 *
 *  Wird ein Geraet abgezogen, verschwindet der DEVICEMAP-Eintrag, und
 *  jeder weitere Aufruf auf einen noch offenen Griff liefert
 *  NTUG_FEHLER_WEG.  Der Griff selbst bleibt gueltig, bis er
 *  geschlossen wird -- NT haelt das Geraeteobjekt so lange.  Ein
 *  Treiber darueber muss also nicht fuerchten, in freigegebenen
 *  Speicher zu greifen; er muss nur NTUG_FEHLER_WEG beachten.
 */

#ifndef _OBER_NTUSBGEN_H_
#define _OBER_NTUSBGEN_H_

#define NTUG_FASSUNG       2      /* 2: NTUG_ISO_ANTWORT.Geschaetzt */
#define NTUG_MAX_DATEN     65536       /* je Aufruf */

#define NTUG_IOCTL(fn)  ((0x22 << 16) | ((fn) << 2) | 0)

#define IOCTL_NTUG_INFO          NTUG_IOCTL(0x900)
#define IOCTL_NTUG_DESKRIPTOR    NTUG_IOCTL(0x901)
#define IOCTL_NTUG_STEUER        NTUG_IOCTL(0x902)
#define IOCTL_NTUG_LESEN         NTUG_IOCTL(0x903)
#define IOCTL_NTUG_SCHREIBEN     NTUG_IOCTL(0x904)
#define IOCTL_NTUG_ALTERNATIVE   NTUG_IOCTL(0x905)
#define IOCTL_NTUG_STALL_WEG     NTUG_IOCTL(0x906)
#define IOCTL_NTUG_ISO           NTUG_IOCTL(0x907)

/*  Fehlerwerte in NTUG_ERGEBNIS.Fehler.  0 bis 0x7F sind die
 *  USB-Fehler des Stapels (usb_error_t aus dev/usb/usbdi.h); die
 *  wichtigsten stehen hier.
 *
 *  Nachgesehen im Quelltext, nicht erinnert -- die erste Fassung hatte
 *  sie aus dem Gedaechtnis und alle drei falsch, am schlimmsten ZEIT
 *  und KURZ vertauscht: ein Treiber haette eine Zeitueberschreitung fuer
 *  ein harmloses Kurzpaket gehalten.  ober/ntugen.c sichert die Werte
 *  jetzt beim Uebersetzen gegen usb_error_t ab.  */

#define NTUG_OK                  0
#define NTUG_FEHLER_INVAL        3     /* USB_ERR_INVAL      */
#define NTUG_FEHLER_IO          18     /* USB_ERR_IOERROR    */
#define NTUG_FEHLER_ZEIT        20     /* USB_ERR_TIMEOUT    */
#define NTUG_FEHLER_KURZ        21     /* USB_ERR_SHORT_XFER, nicht schlimm */
#define NTUG_FEHLER_STALL       22     /* USB_ERR_STALLED    */
#define NTUG_FEHLER_WEG      0x100     /* Geraet abgezogen  */
#define NTUG_FEHLER_ENDPUNKT 0x101     /* kein solcher Endpunkt */
#define NTUG_FEHLER_ANFRAGE  0x102     /* Anfrage unvollstaendig */

#define NTUG_GESCHW_LOW          1     /* 1,5 MBit */
#define NTUG_GESCHW_FULL         2     /* 12 MBit  */
#define NTUG_GESCHW_HIGH         3     /* 480 MBit */
#define NTUG_GESCHW_SUPER        4     /* 5 GBit   */

typedef struct _NTUG_ERGEBNIS {
    long          Fehler;              /* NTUG_OK oder ein Fehlerwert */
    unsigned long Laenge;              /* Nutzdaten dahinter, in Byte */
} NTUG_ERGEBNIS;


/* ---- IOCTL_NTUG_INFO ------------------------------------------------
 *  Eingabe: keine.   Ausgabe: NTUG_ERGEBNIS, dann NTUG_INFO.
 */

#define NTUG_MAX_EP  16

typedef struct _NTUG_ENDPUNKT {
    unsigned char  Adresse;            /* mit Bit 0x80 fuer IN        */
    unsigned char  Art;                /* 0 Steuer 1 isochron 2 Bulk
                                          3 Interrupt                 */
    unsigned short MaxPaket;
    unsigned char  Intervall;
    unsigned char  Fuell[3];
} NTUG_ENDPUNKT;

typedef struct _NTUG_INFO {
    unsigned long  Fassung;            /* NTUG_FASSUNG */
    unsigned short Hersteller;         /* idVendor  */
    unsigned short Produkt;            /* idProduct */
    unsigned short Ausgabe;            /* bcdDevice */
    unsigned char  GeraetKlasse, GeraetUnter, GeraetProtokoll;
    unsigned char  IfKlasse,     IfUnter,     IfProtokoll;
    unsigned char  IfNummer;
    unsigned char  IfAlternative;
    unsigned char  Geschwindigkeit;    /* NTUG_GESCHW_* */
    unsigned char  Adresse;            /* USB-Adresse am Bus  */
    unsigned char  Anschluss;          /* Buchse am Elternhub */
    unsigned char  Tiefe;              /* 1 = am Wurzelhub    */
    unsigned char  AnzahlEp;
    unsigned char  Fuell;
    NTUG_ENDPUNKT  Ep[NTUG_MAX_EP];
    char           HerstellerText[64];
    char           ProduktText[64];
    char           Seriennummer[32];
} NTUG_INFO;


/* ---- IOCTL_NTUG_DESKRIPTOR ------------------------------------------
 *  Eingabe: NTUG_DESK.   Ausgabe: NTUG_ERGEBNIS, dann die Rohbytes.
 *
 *  Typ 1 Geraet, 2 Konfiguration (vollstaendig, mit allen Schnittstellen
 *  und Endpunkten), 3 Zeichenkette (Index, Sprache).
 *  Typ 0x21 und 0x22 (HID und HID-Bericht) werden automatisch an die
 *  SCHNITTSTELLE gerichtet statt an das Geraet -- so verlangt es die
 *  HID-Spezifikation, und genau dort liegt beim Joystick die Bedeutung
 *  jedes Bits.
 */

typedef struct _NTUG_DESK {
    unsigned char  Typ;
    unsigned char  Index;
    unsigned short Sprache;            /* nur fuer Zeichenketten, sonst 0 */
    unsigned short Laenge;             /* hoechstens so viel */
    unsigned short Fuell;
} NTUG_DESK;


/* ---- IOCTL_NTUG_STEUER ----------------------------------------------
 *  Eingabe: NTUG_STEUER, bei Richtung OUT gefolgt von wLength Byte.
 *  Ausgabe: NTUG_ERGEBNIS, bei Richtung IN gefolgt von den Daten.
 *
 *  Die Richtung steht in bmRequestType Bit 7, wie in der USB-Norm.  Ein
 *  Drucker liest so seine IEEE-1284-Kennung (Klassenanfrage 0), ein
 *  Scanner schickt so seine Herstellerbefehle.
 */

typedef struct _NTUG_STEUER {
    unsigned char  bmRequestType;
    unsigned char  bRequest;
    unsigned short wValue;
    unsigned short wIndex;
    unsigned short wLength;
    unsigned long  Zeitlimit;          /* ms, 0 = 5000 */
} NTUG_STEUER;


/* ---- IOCTL_NTUG_LESEN / IOCTL_NTUG_SCHREIBEN ------------------------
 *  Bulk oder Interrupt, je nachdem, was der Endpunkt ist.
 *
 *  LESEN     Eingabe: NTUG_DATEN.  Ausgabe: NTUG_ERGEBNIS + Daten.
 *            Ein Kurzpaket ist kein Fehler -- der Joystick liefert
 *            seinen Bericht, der Drucker seinen Status, und Laenge sagt,
 *            wieviel.
 *
 *  SCHREIBEN Eingabe: NTUG_DATEN, gefolgt von Laenge Byte.
 *            Ausgabe: NTUG_ERGEBNIS (Laenge = geschrieben).
 *
 *  Beim Joystick ist LESEN auf den Interrupt-IN-Endpunkt der ganze
 *  Treiber: jeder Aufruf liefert den naechsten Bericht, sobald sich
 *  etwas bewegt hat, oder nach Zeitlimit NTUG_FEHLER_ZEIT.
 */

typedef struct _NTUG_DATEN {
    unsigned char  Endpunkt;           /* Adresse, mit 0x80 fuer IN */
    unsigned char  Fuell[3];
    unsigned long  Laenge;             /* hoechstens NTUG_MAX_DATEN */
    unsigned long  Zeitlimit;          /* ms, 0 = 5000 */
} NTUG_DATEN;


/* ---- IOCTL_NTUG_ALTERNATIVE -----------------------------------------
 *  Eingabe: ein unsigned char, die alternative Einstellung.
 *  Ausgabe: NTUG_ERGEBNIS.  Danach liefert INFO die neuen Endpunkte.
 */


/* ---- IOCTL_NTUG_STALL_WEG -------------------------------------------
 *  Eingabe: ein unsigned char, die Endpunktadresse.
 *  Ausgabe: NTUG_ERGEBNIS.  Hebt eine Endpunktsperre auf (CLEAR_FEATURE
 *  ENDPOINT_HALT) -- noetig, wenn ein Geraet einen Befehl ablehnt und
 *  den Endpunkt dabei sperrt, wie es Drucker beim Papierende tun.
 */


/* ---- IOCTL_NTUG_ISO -------------------------------------------------
 *  Isochron, IN oder OUT, je nach Endpunkt.  Vorher mit
 *  IOCTL_NTUG_ALTERNATIVE die Einstellung waehlen, in der der Endpunkt
 *  liegt -- isochrone Endpunkte sitzen fast immer in Alternative 1 oder
 *  hoeher, weil Alternative 0 nach der Norm keine Bandbreite belegen
 *  darf.
 *
 *  Eingabe:  NTUG_ISO
 *            bei OUT danach  unsigned long Laenge[Rahmen]
 *                            und die Daten, dicht hintereinander
 *
 *  Ausgabe:  NTUG_ERGEBNIS          Laenge = Summe aller Rahmen
 *            NTUG_ISO_ANTWORT
 *            unsigned long Ist[Rahmen]      tatsaechliche Laengen
 *            bei IN: Rahmen * RahmenLaenge Byte Daten.  Rahmen n liegt
 *            bei n * RahmenLaenge, gueltig sind Ist[n] Byte davon --
 *            genau so, wie FreeBSD die Rahmen im Puffer anlegt.  Ein
 *            leerer Rahmen (Ist = 0) ist bei isochron nichts Besonderes:
 *            das Geraet hatte in dieser Millisekunde nichts.
 *
 *  Rahmen ist hoechstens NTUG_MAX_ISO_RAHMEN, und Rahmen * RahmenLaenge
 *  hoechstens NTUG_MAX_DATEN.  Passt die Anfrage nicht, werden weniger
 *  Rahmen uebertragen; NTUG_ISO_ANTWORT.Rahmen sagt, wie viele.
 */

/*  120 ist FreeBSDs Grenze fuer Full Speed (USB_MAX_FS_ISOC_FRAMES_PER_XFER
 *  in usb_freebsd.h); High Speed erlaubt mehr, aber eine gemeinsame Zahl
 *  fuer alle Geschwindigkeiten ist fuer den Treiber darueber einfacher.
 *  Die erste Fassung hatte 128 -- ungeprueft --, und die Einrichtung
 *  scheiterte an jedem Full-Speed-Geraet mit USB_ERR_INVAL.  */
#define NTUG_MAX_ISO_RAHMEN  120

typedef struct _NTUG_ISO {
    unsigned char  Endpunkt;
    unsigned char  Fuell[3];
    unsigned long  Rahmen;
    unsigned long  Zeitlimit;          /* ms, 0 = 5000 */
} NTUG_ISO;

typedef struct _NTUG_ISO_ANTWORT {
    unsigned long  Rahmen;             /* tatsaechlich uebertragen */
    unsigned long  RahmenLaenge;       /* Schrittweite im IN-Puffer */
    unsigned long  Geschaetzt;         /* 1 = Ist[] sind die angeforderten
                                          Laengen, nicht gemessene */
} NTUG_ISO_ANTWORT;

/*  Geschaetzt = 1 kommt nur bei OUT an einem OHCI vor.
 *
 *  Die OHCI-Norm setzt bei einem erfolgreich GESENDETEN isochronen
 *  Rahmen das Groessenfeld im Paketstatus auf 0 -- die Groesse zaehlt
 *  dort nur beim Empfangen.  FreeBSDs ohci_isoc_done unterscheidet die
 *  Richtung nicht und nimmt das Feld als Rahmenlaenge; jeder gesendete
 *  Rahmen galt so als leer.  Im QEMU-Lauf: 19200 Byte kamen bitgenau
 *  am Geraet an, und die Rueckmeldung sagte "0 Byte, Kurzuebertragung".
 *
 *  Welcher einzelne Rahmen am OHCI vielleicht wirklich ausgefallen ist,
 *  laesst sich auf dieser Ebene nicht mehr erkennen.  Deshalb meldet die
 *  Schnittstelle die angeforderten Laengen UND sagt es.  An UHCI, EHCI
 *  und xHCI sind die Laengen gemessen; Geschaetzt ist dort 0.  */

#endif
