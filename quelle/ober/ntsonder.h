/*
 *  ober/ntsonder.h -- Sondertasten der USB-Tastatur: F13 bis F24,
 *  Medientasten, Systemtasten.  Gemeinsam fuer Treiber und Werkzeuge
 *  (Reiter "Sondertasten" in der Tastatur-Systemsteuerung, USBTASTEN.EXE).
 *
 *  Die Tabelle legt Reihenfolge (= Index, = Bit in Gedrueckt/Gesehen),
 *  Registry-Namen (Parameters\Sonder_<Kennung>), Anzeige und Vorgabe
 *  fest.  NUR hier aendern; Treiber und Werkzeuge muessen zusammen
 *  neu gebaut werden.
 *
 *  Woher die Tasten kommen:
 *    Seite 0x07 (Tastatur): F13..F24 kommen auf der Haupt-Schnittstelle
 *      der Tastatur, im Startprotokoll-Bericht (Nutzung 0x68..0x73).
 *    Seite 0x0C (Consumer) und 0x01/0x81..0x83 (System Control): auf der
 *      ZWEITEN HID-Schnittstelle der Tastatur ("Consumer Control").
 *
 *  Belegungen (Text, klein geschrieben):
 *    aus                    nichts
 *    f13, strg+c, alt+f4    Tasten, mit + verbunden (wie bei den Maustasten)
 *    lauter, leiser, stumm  Lautstaerke   -- fuehrt USBTASTEN.EXE aus
 *    programm:<befehl>      Programm starten -- fuehrt USBTASTEN.EXE aus
 *
 *  NT 4.0 kennt keine Medientasten (VK_MEDIA_* kamen mit Windows 2000);
 *  ohne Belegung tun sie unter NT 4.0 nichts.  F13..F24 kennt NT 4.0
 *  (VK_F13..VK_F24).
 */

#ifndef _NTSONDER_H_
#define _NTSONDER_H_

#define NTU_SONDER_ANZAHL  32
#define NTU_SONDER_TEXT    160

typedef struct {
    const char     *kennung;     /* Registry: Parameters\Sonder_<kennung> */
    const char     *name;        /* Anzeige */
    unsigned short  seite;       /* HID-Nutzungsseite */
    unsigned short  nutzung;     /* HID-Nutzung */
    const char     *vorgabe;     /* Belegung ohne Registry-Eintrag */
} NTU_SONDER_INFO;



/*  IOCTL_NTU_SONDER_ZUSTAND  */
typedef struct _NTU_SONDER_ZUSTAND {
    unsigned long Anzahl;        /* NTU_SONDER_ANZAHL */
    unsigned long Gedrueckt;     /* Bit i: Taste i gerade gedrueckt */
    unsigned long Gesehen;       /* Bit i: Taste i seit dem Start einmal gedrueckt */
    unsigned long Tastaturen;    /* USB-Tastaturen mit Sondertasten-Schnittstelle */
    unsigned long Wartend;       /* Auftraege fuer USBTASTEN.EXE, noch nicht abgeholt */
} NTU_SONDER_ZUSTAND;

/*  IOCTL_NTU_SONDER_HOLEN (Eingabe: Index) / _SETZEN (Index + Text)  */
typedef struct _NTU_SONDER_BELEGUNG {
    unsigned long Index;
    char          Text[NTU_SONDER_TEXT];
} NTU_SONDER_BELEGUNG;

/*  IOCTL_NTU_SONDER_ABHOLEN: ein Auftrag fuer USBTASTEN.EXE ("lauter",
 *  "programm:calc.exe" ...), oder 0 Byte, wenn keiner da ist.  Das
 *  Ereignis NTU_SONDER_EREIGNIS ist gesetzt, solange Auftraege warten.  */
typedef struct _NTU_SONDER_AUFTRAG {
    unsigned long Index;
    char          Text[NTU_SONDER_TEXT];
} NTU_SONDER_AUFTRAG;

#define NTU_SONDER_EREIGNIS      "UsbNtSondertasten"          /* OpenEvent */
#define NTU_SONDER_EREIGNIS_NT  L"\\BaseNamedObjects\\UsbNtSondertasten"

#endif

/*  Die Tabelle: nur wo NTU_SONDER_TABELLE vor dem Einbinden steht --
 *  eigener Waechter, weil ntkbdmou.h diese Datei schon ohne Tabelle
 *  einbindet.  */
#if defined(NTU_SONDER_TABELLE) && !defined(_NTSONDER_TABELLE_)
#define _NTSONDER_TABELLE_
static const NTU_SONDER_INFO ntu_sonder_tabelle[NTU_SONDER_ANZAHL] = {
    { "F13", "F13", 0x07, 0x68, "f13" },
    { "F14", "F14", 0x07, 0x69, "f14" },
    { "F15", "F15", 0x07, 0x6A, "f15" },
    { "F16", "F16", 0x07, 0x6B, "f16" },
    { "F17", "F17", 0x07, 0x6C, "f17" },
    { "F18", "F18", 0x07, 0x6D, "f18" },
    { "F19", "F19", 0x07, 0x6E, "f19" },
    { "F20", "F20", 0x07, 0x6F, "f20" },
    { "F21", "F21", 0x07, 0x70, "f21" },
    { "F22", "F22", 0x07, 0x71, "f22" },
    { "F23", "F23", 0x07, 0x72, "f23" },
    { "F24", "F24", 0x07, 0x73, "f24" },
    { "Play",          "Wiedergabe/Pause",  0x0C, 0x0CD, "aus" },
    { "Stopp",         "Stopp",             0x0C, 0x0B7, "aus" },
    { "Weiter",        "N\xe4" "chster Titel", 0x0C, 0x0B5, "aus" },
    { "Zurueck",       "Voriger Titel",     0x0C, 0x0B6, "aus" },
    { "Stumm",         "Stumm",             0x0C, 0x0E2, "stumm" },
    { "Lauter",        "Lauter",            0x0C, 0x0E9, "lauter" },
    { "Leiser",        "Leiser",            0x0C, 0x0EA, "leiser" },
    { "Mail",          "E-Mail",            0x0C, 0x18A, "aus" },
    { "Rechner",       "Rechner",           0x0C, 0x192, "programm:calc.exe" },
    { "Arbeitsplatz",  "Arbeitsplatz",      0x0C, 0x194, "programm:explorer.exe" },
    { "Browser",       "Browser",           0x0C, 0x196, "aus" },
    { "WebSuche",      "Web: Suche",        0x0C, 0x221, "aus" },
    { "WebStart",      "Web: Startseite",   0x0C, 0x223, "aus" },
    { "WebZurueck",    "Web: Zur\xfc" "ck", 0x0C, 0x224, "alt+links" },
    { "WebVor",        "Web: Vorw\xe4rts",  0x0C, 0x225, "alt+rechts" },
    { "WebAktual",     "Web: Aktualisieren",0x0C, 0x227, "f5" },
    { "Favoriten",     "Web: Favoriten",    0x0C, 0x22A, "aus" },
    { "Ausschalten",   "Ausschalten",       0x01, 0x081, "aus" },
    { "Ruhezustand",   "Ruhezustand",       0x01, 0x082, "aus" },
    { "Aufwachen",     "Aufwachen",         0x01, 0x083, "aus" },
};
#endif
