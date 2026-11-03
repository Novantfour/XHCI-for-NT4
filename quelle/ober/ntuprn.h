/*
 *  ober/ntuprn.h -- USB-Drucker: Kern <-> Werkzeuge (usbdruck.exe)
 *
 *      \\.\USBPRN1 ..     schreiben = drucken (07/01/01 oder 02)
 *      \\.\USBIPP1 ..     IPP over USB (07/01/04): Bytestrom in beide
 *                         Richtungen, WriteFile = Bulk hinaus, ReadFile =
 *                         Bulk herein (ab Schnittstellenfassung 4)
 *      IOCTL_NTUP_KENNUNG IEEE-1284-Kennung als Zeichenkette
 *      IOCTL_NTUP_STATUS  Papier leer / ausgewaehlt / kein Fehler
 *      IOCTL_NTUP_RESET   SOFT_RESET an den Drucker
 */
#ifndef _NTUPRN_H_
#define _NTUPRN_H_

#define NTUP_IOCTL(n)  (((0x00000022) << 16) | ((0x8C0 + (n)) << 2))

#define IOCTL_NTUP_KENNUNG  NTUP_IOCTL(0)
#define IOCTL_NTUP_STATUS   NTUP_IOCTL(1)
#define IOCTL_NTUP_RESET    NTUP_IOCTL(2)
#define IOCTL_NTUP_INFO     NTUP_IOCTL(3)   /* ab Schnittstellenfassung 2 */
#define IOCTL_NTUP_ZEIT     NTUP_IOCTL(4)   /* ab Schnittstellenfassung 4 */

#define NTUP_FASSUNG        4   /* 3: ReadFile-Rueckkanal (Protokoll 02)
                                   4: IPP over USB (\\.\USBIPPn), Zeitgrenze
                                      je Griff, usbprint.sys-Codes */

/*  Dieselben Steuercodes wie Windows' usbprint.sys (Windows 2000 bis 11),
 *  zusaetzlich -- wer sie dort benutzt, braucht keinen zweiten Weg.  */
#define IOCTL_USBPRINT_GET_LPT_STATUS  0x00220030   /* 1 Byte wie Parallelport */
#define IOCTL_USBPRINT_GET_1284_ID     0x00220034   /* 2 Byte Laenge (big endian,
                                                       einschliesslich) + Kennung */
#define IOCTL_USBPRINT_SOFT_RESET      0x00220040

/*  IOCTL_NTUP_ZEIT: Zeitgrenzen fuer DIESEN Griff (Dateiobjekt), getrennt
 *  fuer Lesen und Schreiben, in ms.  0 = unbegrenzt, NTUP_ZEIT_BLEIBT =
 *  nicht aendern.  Eingabe optional (nur abfragen: ohne Eingabe); die
 *  Antwort enthaelt die gueltigen Werte.  Ohne Einstellung gelten
 *  Parameters\LeseWartezeit (ms) und \SchreibWartezeit (s); Schreiben
 *  gilt je 64-kB-Stueck.  */
#define NTUP_ZEIT_BLEIBT  0xFFFFFFFFul
typedef struct _NTUP_ZEIT {
    unsigned long LesenMs;
    unsigned long SchreibenMs;
} NTUP_ZEIT;

typedef struct _NTUP_STATUS {
    unsigned long Gelesen;       /* GET_PORT_STATUS beantwortet */
    unsigned long PapierLeer;
    unsigned long Ausgewaehlt;
    unsigned long KeinFehler;
    unsigned long Roh;
    unsigned long Bytes;         /* seit dem Einstecken gedruckt */
} NTUP_STATUS;

/*  IOCTL_NTUP_INFO.  Codes 0..2 und NTUP_STATUS bleiben unveraendert;
 *  wer IOCTL_NTUP_INFO bekommt, hat mindestens Fassung 2.  */

typedef struct _NTUP_INFO {
    unsigned long  Fassung;          /* NTUP_FASSUNG */
    unsigned short Vid, Pid;
    unsigned short BcdDevice;
    unsigned char  Protokoll;        /* genutzte Einstellung: 1, 2, oder 4 (IPP) */
    unsigned char  Schnittstelle;    /* bInterfaceNumber */
    unsigned char  Alternative;      /* bAlternateSetting */
    unsigned char  Rueckkanal;       /* 1 = ReadFile geht (ab Fassung 3) */
    unsigned char  Frei[2];
    unsigned long  Aenderungen;      /* zaehlt jedes An-/Abstecken eines Druckers */
    unsigned long  Wartezeit;        /* Sekunden je 64-kB-Stueck, 0 = unbegrenzt */
    char           Seriennummer[64]; /* iSerialNumber, "" wenn keine */
    char           Pfad[32];         /* Controller-Anschluss[.Hub-Anschluss...], z. B. "0-3.2" */
} NTUP_INFO;                         /* 120 Byte */

#endif
