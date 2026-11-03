/*
 *  werkzeug/uac.h -- USB-Audio (UAC1) im Benutzermodus: Geraet finden,
 *  Konfiguration auswerten, Steueranfragen.  Gemeinsam fuer usbton.exe
 *  und usbaudio.dll.
 *
 *  Ein Audiogeraet besteht fuer uns aus
 *    - der AudioControl-Schnittstelle an ntugen (\\.\UsbDevN): von dort die
 *      Konfiguration und die Steueranfragen (Lautstaerke, Stumm);
 *    - den AudioStreaming-Schnittstellen an USBAUDIO.SYS (\\.\UsbAudioN):
 *      der Strom.
 */

#ifndef _UAC_H_
#define _UAC_H_

#include <windows.h>
#include "../ober/ntusbgen.h"
#include "../ober/ntuaudio.h"

#define UAC_MAX_ALT    16
#define UAC_MAX_FU      8
#define UAC_MAX_RATE    8
#define UAC_MAX_IF      8

typedef struct {
    unsigned char  ifnr, alt;
    unsigned char  link;                     /* bTerminalLink */
    unsigned short format;                   /* wFormatTag, 1 = PCM */
    unsigned char  kanaele, bytes, bits;
    unsigned char  ratentyp;                 /* 0 = Bereich, n = Liste */
    unsigned long  rate[UAC_MAX_RATE];
    unsigned char  ep, epattr, intervall, cs_attr;
    unsigned short maxpaket;
} UAC_ALT;

typedef struct {
    unsigned char id, quelle, groesse, kanaele;
    unsigned long ctl[9];                    /* je Kanal, [0] = Master */
} UAC_FU;

/*  Ein Glied des Signalwegs (AudioControl): Terminal oder Unit.  */
#define UAC_MAX_ENT    24
typedef struct {
    unsigned char  id, art;                  /* art = Subtyp: 2 IT, 3 OT, 4 Mixer, 5 Selector, 6 FU, 7/8 ... */
    unsigned short typ;                      /* Terminaltyp (IT/OT) */
    unsigned char  nq, quelle[8];            /* Quellen (OT/FU: eine) */
} UAC_ENT;

typedef struct {
    unsigned short vid, pid;
    char           name[64];
    unsigned char  protokoll;                /* bInterfaceProtocol der AC-Schnittstelle */
    unsigned short adc;                      /* bcdADC */
    int            uac;                      /* 1, 2, 3 */
    unsigned char  ac_if;
    int            ac_dev;                   /* UsbDevN der AC-Schnittstelle */
    int            nalt;
    UAC_ALT        alt[UAC_MAX_ALT];
    int            nfu;
    UAC_FU         fu[UAC_MAX_FU];
    int            nent;
    UAC_ENT        ent[UAC_MAX_ENT];
    unsigned long  konflen;
    unsigned char  konf[2048];               /* Konfigurationsdeskriptor (fuer usbton roh) */
    int            as_dev[UAC_MAX_IF];       /* UsbDevN (ohne USBAUDIO) */
    int            au_dev[UAC_MAX_IF];       /* UsbAudioN je Schnittstellennr. */
} UAC_GERAET;

/*  Das nr-te Audiogeraet (0, 1, ...) suchen und auswerten.  1 = gefunden.  */
int  uac_suchen (UAC_GERAET *g, int nr);

/*  Wiedergabe-Alternative: PCM, 16 Bit, Stereo, OUT; bevorzugt mit
 *  Rate 'wunsch', sonst 48 kHz, sonst die erste.  *rate = gewaehlte Rate.  */
UAC_ALT *uac_wiedergabe (UAC_GERAET *g, unsigned long wunsch, unsigned long *rate);

/*  Aufnahme-Alternative: PCM, 16 Bit, Mono oder Stereo, IN; bevorzugt mit
 *  Rate 'wunsch', sonst 48 kHz, sonst die erste.  */
UAC_ALT *uac_aufnahme (UAC_GERAET *g, unsigned long wunsch, unsigned long *rate);

/*  Kann die Alternative diese Rate?  */
int  uac_kann_rate (const UAC_ALT *a, unsigned long rate);

/*  Klassenanfrage ueber ntugen.  0 = gut, sonst USB-Fehler, -1 = Aufruf.  */
long uac_steuer (HANDLE h, unsigned char rt, unsigned char rq, unsigned short wv,
                 unsigned short wi, void *daten, unsigned short n);

HANDLE uac_ac_oeffnen (const UAC_GERAET *g);     /* \\.\UsbDevN der AC-Schnittstelle */
HANDLE uac_strom_oeffnen (int au_dev);            /* \\.\UsbAudioN */

/*  Feature Unit mit Stumm (bit 1) bzw. Lautstaerke (bit 2) -- die erste
 *  im Geraet.  Fuer Geraete mit mehreren (Headset: Lautsprecher und
 *  Mikrofon) besser uac_fu_fuer.  */
UAC_FU *uac_fu (UAC_GERAET *g, int bit);

/*  Feature Unit im Signalweg einer Alternative: Wiedergabe -- vom
 *  Strom-Terminal (bTerminalLink) vorwaerts; Aufnahme -- vom Ausgangs-
 *  terminal rueckwaerts.  Ueber Mixer/Selector hinweg.  0 = keine.  */
UAC_FU *uac_fu_fuer (UAC_GERAET *g, const UAC_ALT *a);

/*  Lautstaerke an einer bestimmten Feature Unit: Bereich lesen, setzen,
 *  lesen.  Werte in 1/256 dB.  Kanal 0 = Master.  */
int  uac_laut_bereich (UAC_GERAET *g, UAC_FU *f, HANDLE h, int kanal, short *mn, short *mx, short *res);
int  uac_laut_setzen (UAC_GERAET *g, UAC_FU *f, HANDLE h, int kanal, short wert);
int  uac_laut_lesen (UAC_GERAET *g, UAC_FU *f, HANDLE h, int kanal, short *wert);
int  uac_stumm_setzen (UAC_GERAET *g, UAC_FU *f, HANDLE h, int an);
int  uac_stumm_lesen (UAC_GERAET *g, UAC_FU *f, HANDLE h, int *an);

#endif
