/*
 *  ober/ntuaudio.h -- Schnittstelle von USBAUDIO.SYS (USB-Audio, UAC1).
 *
 *  USBAUDIO.SYS uebernimmt die AudioStreaming-Schnittstellen (Klasse 01,
 *  Unterklasse 02) und haelt den isochronen Strom am Laufen: zwei
 *  Uebertragungen im Wechsel, jede wird in ihrem Rueckruf sofort neu
 *  befuellt und abgeschickt -- so wie FreeBSDs uaudio.  Gespeist wird
 *  aus einem Ringpuffer, in den der Benutzermodus schreibt.  Warum nicht
 *  ueber \\.\UsbDevN (ntugen): dort laeuft jeder ISO-Aufruf synchron,
 *  und zwischen zwei Aufrufen fehlen gemessen 2-3 ms je Aufruf (usbton).
 *
 *  Steuerung (Lautstaerke, Stumm) geht weiter ueber die AudioControl-
 *  Schnittstelle an ntugen (\\.\UsbDevN) -- sie ist nicht zeitkritisch.
 *  Die Deskriptoren liest der Benutzermodus ebenfalls dort.
 *
 *  Aus dem Benutzermodus:  CreateFile("\\\\.\\UsbAudio0", ...)
 *  Alle Aufrufe METHOD_BUFFERED.
 *
 *  Ablauf Aufnahme (Endpunkt mit 0x80):
 *      NTUA_START, dann immer wieder NTUA_LESEN; was der Ring nicht mehr
 *      fasst, zaehlt als Ueberlauf (niemand holt ab).
 *
 *  Ablauf Wiedergabe:
 *      NTUA_START  (Alternative, Endpunkt, Rate, Bytes je Abtastung)
 *      immer wieder NTUA_SCHREIBEN, solange Platz ist (sonst kurz warten)
 *      NTUA_STOP   (oder Griff schliessen)
 *  Fehlen Daten, sendet der Treiber Stille und zaehlt Unterlaeufe.
 */

#ifndef _OBER_NTUAUDIO_H_
#define _OBER_NTUAUDIO_H_

#define NTUA_FASSUNG     1

#define NTUA_IOCTL(fn)  ((0x22 << 16) | ((fn) << 2) | 0)

#define IOCTL_NTUA_INFO        NTUA_IOCTL(0xA00)
#define IOCTL_NTUA_START       NTUA_IOCTL(0xA01)
#define IOCTL_NTUA_SCHREIBEN   NTUA_IOCTL(0xA02)
#define IOCTL_NTUA_STAND       NTUA_IOCTL(0xA03)
#define IOCTL_NTUA_STOP        NTUA_IOCTL(0xA04)
#define IOCTL_NTUA_LEEREN      NTUA_IOCTL(0xA05)
#define IOCTL_NTUA_LESEN       NTUA_IOCTL(0xA06)

/* ---- IOCTL_NTUA_INFO -- Ausgabe: NTUA_INFO -------------------------- */

typedef struct _NTUA_INFO {
    unsigned long  Fassung;            /* NTUA_FASSUNG */
    unsigned short Hersteller, Produkt;
    unsigned char  IfNummer;           /* AudioStreaming-Schnittstelle */
    unsigned char  Geschwindigkeit;    /* NTUG_GESCHW_* */
    unsigned char  Fuell[2];
    char           ProduktText[64];
} NTUA_INFO;

/* ---- IOCTL_NTUA_START -- Eingabe: NTUA_START, Ausgabe: NTUA_STAND ----
 *  Waehlt die Alternative, setzt die Abtastrate am Endpunkt (wenn
 *  RateSetzen, UAC1 SET_CUR SAMPLING_FREQ_CONTROL), legt den Ring an und
 *  startet den Strom -- anfangs Stille, bis Daten kommen.
 */

typedef struct _NTUA_START {
    unsigned char  Alternative;
    unsigned char  Endpunkt;           /* mit 0x80: Aufnahme (IN) */
    unsigned char  RateSetzen;         /* 1 = SET_CUR an den Endpunkt */
    unsigned char  Fuell;
    unsigned long  Rate;               /* Hz */
    unsigned long  BytesJeAbtastung;   /* Kanaele * Bytes, z. B. 4 */
    unsigned long  RingByte;           /* 0 = Vorgabe (64 KB) */
} NTUA_START;

/* ---- IOCTL_NTUA_SCHREIBEN -- Eingabe: PCM-Daten, Ausgabe: NTUA_STAND --
 *  Nimmt so viel, wie in den Ring passt, und kehrt sofort zurueck.
 *  NTUA_STAND.Angenommen sagt, wie viel.
 */

/* ---- IOCTL_NTUA_STAND -- Ausgabe: NTUA_STAND ------------------------ */

typedef struct _NTUA_STAND {
    unsigned long  Laeuft;             /* 1 = Strom aktiv */
    unsigned long  Angenommen;         /* nur SCHREIBEN: Byte uebernommen */
    unsigned long  Fuellstand;         /* Byte im Ring */
    unsigned long  Platz;              /* freie Byte im Ring */
    unsigned long  GespieltLo;         /* Byte an den Bus gegeben (64 Bit) */
    unsigned long  GespieltHi;
    unsigned long  Unterlaeufe;        /* Rahmen ganz oder teils mit Stille */
    unsigned long  Rahmen;             /* Rahmen insgesamt */
    unsigned long  Fehler;             /* letzter USB-Fehler im Strom */
    unsigned long  RahmenJeSekunde;    /* 1000 Full Speed, 8000 High Speed */
    unsigned long  Ueberlaeufe;        /* Aufnahme: Byte verworfen, Ring voll */
    unsigned long  Aufnahme;           /* 1 = Strom ist IN */
} NTUA_STAND;

/* ---- IOCTL_NTUA_LESEN -- Ausgabe: NTUA_STAND, dahinter die Daten -----
 *  Aufnahme: holt, was im Ring liegt (hoechstens Ausgabegroesse minus
 *  sizeof(NTUA_STAND)), und kehrt sofort zurueck.  NTUA_STAND.Angenommen
 *  = Anzahl Daten-Byte hinter dem Kopf.
 */

/* ---- IOCTL_NTUA_STOP -- Ausgabe: NTUA_STAND ------------------------- */

/* ---- IOCTL_NTUA_LEEREN -- Ausgabe: NTUA_STAND -----------------------
 *  Verwirft, was im Ring steht; der Strom laeuft mit Stille weiter
 *  (waveOutReset: sofort still, ohne Neuaufbau des Stroms).
 */

#endif
