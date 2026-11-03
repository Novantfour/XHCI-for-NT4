/*
 *  ober/ntusbioctl.h -- die Schnittstelle zwischen Treiber und Werkzeug.
 *
 *  Dieselbe Datei wird von beiden Seiten uebersetzt: vom Treiber mit
 *  der Unterlage, vom Werkzeug mit den Win32-Kopfdateien.  Deshalb
 *  stehen hier nur feste Breiten und keine Zeiger -- alles, was in
 *  einem Puffer ueber die Grenze geht, muss auf beiden Seiten dieselbe
 *  Lage haben, und ein Zeiger hat sie nie.
 *
 *  Geraetename:  \Device\UsbNt   und  \DosDevices\UsbNt
 *  Aus Win32:    CreateFile("\\\\.\\UsbNt", ...)
 *
 *  FILE_DEVICE_UNKNOWN ist 0x22, METHOD_BUFFERED ist 0 -- damit kopiert
 *  NT selbst und wir muessen keine Benutzeradresse anfassen.  Das ist
 *  bei einem Diagnosewerkzeug die richtige Wahl: die Puffer sind klein,
 *  und ein falsch geprobter Zeiger waere ein Bugcheck aus dem
 *  Benutzermodus heraus.
 */

#ifndef _OBER_NTUSBIOCTL_H_
#define _OBER_NTUSBIOCTL_H_

/*  Fuenf Baufassungen, fuenf Namen.
 *
 *  Der Treiberquelltext ist derselbe; nur der Suchlauf laesst weg, was
 *  nicht zur Bauart gehoert.  Getrennte Geraete- und Dienstnamen sind
 *  die Voraussetzung dafuer, dass sie nebeneinander liegen und einzeln
 *  geladen werden koennen -- und dass ein Fehler in einer Bauart nicht
 *  wie ein Fehler in einer anderen aussieht.
 *
 *      usbnt.sys   \Device\UsbNt    alle vier
 *      uhcint.sys  \Device\UhciNt   nur UHCI   (USB 1.1, E/A-Raum)
 *      ohcint.sys  \Device\OhciNt   nur OHCI   (USB 1.1, Speicher)
 *      ehcint.sys  \Device\EhciNt   nur EHCI   (USB 2.0)
 *      xhcint.sys  \Device\XhciNt   nur xHCI   (USB 3.x)
 *
 *  Gleichzeitig darf immer nur einer liegen, der denselben Baustein
 *  beansprucht.  DriverEntry prueft das und bricht sonst ab.
 */

/*  Die Bauart als Zahl -- dieselbe Schnittstellenkennung, die im
 *  PCI-Konfigraum steht, damit der Suchlauf sie unmittelbar vergleichen
 *  kann.  NTU_EIGENER_NAME ist der Platz in der Namensliste, die
 *  DriverEntry gegen die anderen Bauformen prueft.  */

#if   defined(NTU_NUR_UHCI)
#define NTU_EINE_BAUART   0x00
#define NTU_EIGENER_NAME  1
#elif defined(NTU_NUR_OHCI)
#define NTU_EINE_BAUART   0x10
#define NTU_EIGENER_NAME  2
#elif defined(NTU_NUR_EHCI)
#define NTU_EINE_BAUART   0x20
#define NTU_EIGENER_NAME  3
#elif defined(NTU_NUR_XHCI)
#define NTU_EINE_BAUART   0x30
#define NTU_EIGENER_NAME  4
#else
#define NTU_EIGENER_NAME  0
#endif

#if   defined(NTU_NUR_UHCI)
#define NTU_DEV_NAME_W   L"\\Device\\UhciNt"
#define NTU_LINK_NAME_W  L"\\DosDevices\\UhciNt"
#define NTU_BAUART       "uhci"
#elif defined(NTU_NUR_OHCI)
#define NTU_DEV_NAME_W   L"\\Device\\OhciNt"
#define NTU_LINK_NAME_W  L"\\DosDevices\\OhciNt"
#define NTU_BAUART       "ohci"
#elif defined(NTU_NUR_EHCI)
#define NTU_DEV_NAME_W   L"\\Device\\EhciNt"
#define NTU_LINK_NAME_W  L"\\DosDevices\\EhciNt"
#define NTU_BAUART       "ehci"
#elif defined(NTU_NUR_XHCI)
#define NTU_DEV_NAME_W   L"\\Device\\XhciNt"
#define NTU_LINK_NAME_W  L"\\DosDevices\\XhciNt"
#define NTU_BAUART       "xhci"
#else
#define NTU_DEV_NAME_W   L"\\Device\\UsbNt"
#define NTU_LINK_NAME_W  L"\\DosDevices\\UsbNt"
#define NTU_BAUART       "usb"
#endif

/*  Die Namen, unter denen ein Werkzeug suchen kann -- in dieser
 *  Reihenfolge, damit der Vollbau zuerst gefunden wird.  */

#define NTU_WIN32_ALLE  "\\\\.\\UsbNt"
#define NTU_WIN32_UHCI  "\\\\.\\UhciNt"
#define NTU_WIN32_OHCI  "\\\\.\\OhciNt"
#define NTU_WIN32_EHCI  "\\\\.\\EhciNt"
#define NTU_WIN32_XHCI  "\\\\.\\XhciNt"

#define NTU_IOCTL(fn)  ((0x22 << 16) | ((fn) << 2) | 0)

#define IOCTL_NTU_HOSTBAUSTEINE  NTU_IOCTL(0x800)   /* PCI-Tabelle      */
#define IOCTL_NTU_GERAETE        NTU_IOCTL(0x801)   /* USB-Geraetebaum  */
#define IOCTL_NTU_REGISTER       NTU_IOCTL(0x802)   /* Registerabzug    */
#define IOCTL_NTU_ZAEHLER        NTU_IOCTL(0x803)   /* Betriebszaehler  */
#define IOCTL_NTU_VERSION        NTU_IOCTL(0x804)   /* Fassungsangabe   */
#define IOCTL_NTU_SPUR           NTU_IOCTL(0x805)   /* letzte Unterbr.  */
#define IOCTL_NTU_MAUS_ZUSTAND   NTU_IOCTL(0x806)   /* Maustasten roh   */
#define IOCTL_NTU_MAUS_HOLEN     NTU_IOCTL(0x807)   /* Belegung lesen   */
#define IOCTL_NTU_MAUS_SETZEN    NTU_IOCTL(0x808)   /* Belegung setzen  */
#define IOCTL_NTU_MAUS_PROTOKOLL NTU_IOCTL(0x809)   /* letzte Berichte  */
#define IOCTL_NTU_SONDER_ZUSTAND NTU_IOCTL(0x80A)   /* Sondertasten: gedrueckt/gesehen */
#define IOCTL_NTU_SONDER_HOLEN   NTU_IOCTL(0x80B)   /* Belegung lesen   */
#define IOCTL_NTU_SONDER_SETZEN  NTU_IOCTL(0x80C)   /* Belegung setzen  */
#define IOCTL_NTU_SONDER_ABHOLEN NTU_IOCTL(0x80D)   /* Auftrag fuer USBTASTEN.EXE */
#define IOCTL_NTU_USBGERAETE     NTU_IOCTL(0x80E)   /* USB-Geraete, wie lsusb */
#include "ntsonder.h"

/*  Systemsteuerung "USB-Maus".  Roh: Bit 0 = Taste 1 ... Bit 7 = Taste 8,
 *  so wie die zuletzt meldende USB-Maus sie liefert -- auch 4..8, die
 *  Windows nicht kennt.  Belegung: Taste 1..8, Text wie in
 *  Parameters\TasteN (klick-links, klick-rechts, mitte, doppelklick,
 *  alt+links, strg+c, aus, ...).  Setzen wirkt sofort und wird gespeichert.  */

#define NTU_MAUS_FASSUNG 1

typedef struct _NTU_MAUS_ZUSTAND {
    unsigned long Fassung;
    unsigned long Maeuse;        /* angesteckte USB-Maeuse */
    unsigned long Knoepfe;       /* groesste Tastenzahl */
    unsigned long Roh;
    unsigned long Berichte;      /* Zaehler: aendert er sich, kam etwas */
} NTU_MAUS_ZUSTAND;

/*  Die letzten 64 Mausberichte, wie der Treiber sie gesehen und was er an
 *  mouclass gegeben hat -- fuer MAUSDATEN.EXE.  Ms: Millisekunden nach dem
 *  Systemtakt (KeTickCount), vergleichbar mit GetTickCount.  */
typedef struct _NTU_MAUS_EINTRAG {
    unsigned long  Ms;
    unsigned short Flags;        /* ButtonFlags an mouclass (0 = keine Taste) */
    short          Dx, Dy;
    unsigned char  Len;          /* Laenge des Berichts */
    unsigned char  Alle;         /* erkannte Tasten 1..8, Bit 0 = links */
    unsigned char  Vorher;       /* Tasten beim vorigen Bericht */
    unsigned char  Geliefert;    /* 1 = an mouclass gegeben, 0 = leer verworfen */
    unsigned char  Roh[8];       /* die ersten 8 Byte des Berichts */
    unsigned char  Frei[2];
} NTU_MAUS_EINTRAG;

typedef struct _NTU_MAUS_PROTOKOLL {
    unsigned long    Anzahl;     /* gueltige Eintraege, aelteste zuerst */
    unsigned long    Gesamt;     /* Berichte seit dem Start */
    NTU_MAUS_EINTRAG E[64];
    unsigned long    TAnzahl;    /* nur Tastenwechsel, die letzten 32 --
                                    Bewegung verdraengt sie nicht */
    NTU_MAUS_EINTRAG T[32];
} NTU_MAUS_PROTOKOLL;

typedef struct _NTU_MAUS_BELEGUNG {
    unsigned long Taste;         /* 1..8 */
    char          Text[48];
} NTU_MAUS_BELEGUNG;

typedef struct _NTU_SPUR_EINTRAG {
    unsigned long Iman;
    unsigned long Usbsts;
    unsigned long Tick;
} NTU_SPUR_EINTRAG;

typedef struct _NTU_SPUR_ANTWORT {
    unsigned long    Anzahl;
    unsigned long    Kopf;
    NTU_SPUR_EINTRAG Eintrag[16];
} NTU_SPUR_ANTWORT;

#define NTU_ABI_FASSUNG  3

/* ------------------------------------------------------------------ *
 *  Hostbausteine
 * ------------------------------------------------------------------ */

#define NTU_MAX_HC_IOCTL  16

typedef struct _NTU_HC_EINTRAG {
    unsigned char  Bus;
    unsigned char  Slot;          /* Geraetenummer, nicht NT-Slotwort */
    unsigned char  Func;
    unsigned char  ProgIf;        /* 00 uhci, 10 ohci, 20 ehci, 30 xhci */
    unsigned short Vendor;
    unsigned short Device;
    unsigned short SubVendor;
    unsigned short SubDevice;
    unsigned long  Bar;
    unsigned short Command;
    unsigned char  Irq;
    unsigned char  Sbrn;
    unsigned short LegSup;        /* UHCI Konfig 0xC0, sonst 0 */
    unsigned short Revision;
    unsigned long  Angehaengt;    /* 0 = nein, 1 = ja, 2 = uebergangen */
    unsigned long  OpBase;        /* EHCI/xHCI: CAPLENGTH */
    unsigned long  RtsOff;        /* nur xHCI */
    char           Name[16];      /* "ohci0" */
} NTU_HC_EINTRAG;

typedef struct _NTU_HC_ANTWORT {
    unsigned long  Anzahl;
    NTU_HC_EINTRAG Eintrag[NTU_MAX_HC_IOCTL];
} NTU_HC_ANTWORT;

/* ------------------------------------------------------------------ *
 *  Geraete am Bus
 * ------------------------------------------------------------------ */

#define NTU_MAX_DEV_IOCTL  64

typedef struct _NTU_DEV_EINTRAG {
    unsigned char  Tiefe;         /* 0 = Hostbaustein, 1 = Wurzelhub, ... */
    unsigned char  Adresse;
    unsigned char  Anschluss;
    unsigned char  Geschwindigkeit;  /* 0 low, 1 full, 2 high, 3 super */
    unsigned short Vendor;
    unsigned short Product;
    unsigned char  Klasse;
    unsigned char  Unterklasse;
    unsigned char  Protokoll;
    unsigned char  Belegt;        /* 1 = ein Treiber haengt daran */
    char           Treiber[16];
    char           Text[64];
} NTU_DEV_EINTRAG;

typedef struct _NTU_DEV_ANTWORT {
    unsigned long   Anzahl;
    NTU_DEV_EINTRAG Eintrag[NTU_MAX_DEV_IOCTL];
} NTU_DEV_ANTWORT;

/* ------------------------------------------------------------------ *
 *  USB-Geraete, wie lsusb  (IOCTL_NTU_USBGERAETE)
 *
 *  Anders als IOCTL_NTU_GERAETE (der Treiberbaum: ein Knoten je
 *  Schnittstelle, USB-Angaben nur beim Hostbaustein) steht hier jedes
 *  USB-GERAET einmal, mit seinen Deskriptoren und Texten -- auch
 *  Wurzelhubs und Hubs.  Eltern ueber ElternAdresse (0 = keins, dann
 *  ist es der Wurzelhub des Busses).  Fuer den Geraetemanager.
 * ------------------------------------------------------------------ */

#define NTU_USB_MAX     64
#define NTU_USB_MAX_IF   8

typedef struct _NTU_USB_IF {
    unsigned char  Nummer;           /* bInterfaceNumber */
    unsigned char  Klasse;
    unsigned char  Unterklasse;
    unsigned char  Protokoll;
    char           Treiber[16];      /* "ukbd0", "" = keiner */
} NTU_USB_IF;

typedef struct _NTU_USB_GERAET {
    unsigned char  Bus;              /* Nummer des Hostbausteins, ab 0 */
    unsigned char  Adresse;
    unsigned char  ElternAdresse;    /* 0 = Wurzelhub */
    unsigned char  Anschluss;        /* am Elternhub */
    unsigned char  Tiefe;            /* 0 = Wurzelhub */
    unsigned char  Geschwindigkeit;  /* 1 low, 2 full, 3 high, 4 super */
    unsigned char  AnzahlIf;         /* belegte Eintraege in If[] */
    unsigned char  Konfigurationen;  /* bNumConfigurations */
    unsigned short Vendor;
    unsigned short Product;
    unsigned short Geraetefassung;   /* bcdDevice */
    unsigned short UsbFassung;       /* bcdUSB */
    unsigned char  Klasse;           /* bDeviceClass */
    unsigned char  Unterklasse;
    unsigned char  Protokoll;
    unsigned char  MaxPaket0;
    unsigned short StromMa;          /* aus bMaxPower der Konfiguration */
    unsigned char  Hub;              /* 1 = ist ein Hub */
    unsigned char  Frei;
    char           Hersteller[48];
    char           Produkt[64];
    char           Seriennummer[32];
    char           HostArt[8];       /* "OHCI", "EHCI", ... beim Wurzelhub */
    NTU_USB_IF     If[NTU_USB_MAX_IF];
} NTU_USB_GERAET;

typedef struct _NTU_USB_ANTWORT {
    unsigned long  Fassung;          /* 1 */
    unsigned long  Anzahl;
    NTU_USB_GERAET Geraet[NTU_USB_MAX];
} NTU_USB_ANTWORT;

/* ------------------------------------------------------------------ *
 *  Registerabzug
 *
 *  Eingabe ist die laufende Nummer aus der Hostbausteintabelle.  Der
 *  Treiber liest die Register selbst -- das Werkzeug kann es nicht, weil
 *  ein Benutzerprozess unter NT 4.0 keinen Speicher- oder E/A-Zugriff
 *  auf den Baustein bekommt.  Genau dafuer ist dieser Aufruf da.
 * ------------------------------------------------------------------ */

typedef struct _NTU_REG_ANFRAGE {
    unsigned long Nummer;
    unsigned long Anzahl;         /* wieviele 32-Bit-Worte, max 64 */
    unsigned long Versatz;        /* ab wo, relativ zum Registerblock */
} NTU_REG_ANFRAGE;

typedef struct _NTU_REG_ANTWORT {
    unsigned long Anzahl;
    unsigned long Breite;         /* 1, 2 oder 4 Byte je Register */
    unsigned long Wert[64];
} NTU_REG_ANTWORT;

/* ------------------------------------------------------------------ *
 *  Zaehler
 *
 *  Was ohne Kerndebugger sonst niemand sieht.  Alle vier sagen etwas,
 *  das aus der Anmeldemaske heraus unsichtbar ist.
 * ------------------------------------------------------------------ */

typedef struct _NTU_ZAEHLER {
    unsigned long Unterbrechungen;   /* ISR hat TRUE gemeldet        */
    unsigned long Fremdunterbr;      /* ISR hat FALSE gemeldet       */
    unsigned long Fadenlaeufe;       /* Handler im Faden gerufen     */
    unsigned long ArbeitVerloren;    /* callout-Ring war voll        */
    unsigned long AnschlaegeGesamt;  /* an kbdclass geliefert        */
    unsigned long AnschlaegeVerworf; /* von kbdclass nicht genommen  */
    unsigned long MausMeldungen;
    unsigned long Ticks;             /* eigener Taktzaehler          */
    unsigned long PoolOffen;         /* Bloecke, die noch stehen     */
    unsigned long PoolVergaben;      /* Vergaben seit dem Laden      */
    unsigned long PoolGemeckert;     /* Freigaben, die die Wache
                                        abgewiesen hat -- jede davon
                                        waere ein Bugcheck gewesen   */
} NTU_ZAEHLER;

typedef struct _NTU_VERSION {
    unsigned long Fassung;        /* NTU_ABI_FASSUNG */
    char          Bau[32];        /* Datum des Baus  */
} NTU_VERSION;

#endif
