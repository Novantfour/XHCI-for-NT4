/*
 *  ober/ntumass.c -- USB-Massenspeicher als Laufwerk.
 *
 *  Sticks, Kartenleser, USB-Festplatten: alles, was sich als
 *  Massenspeicher mit Bulk-Only-Transport meldet (Klasse 08, Protokoll
 *  50), bekommt ein NT-Plattengeraet und einen Laufwerksbuchstaben.  Das
 *  Dateisystem -- FAT, NTFS -- bindet sich beim ersten Zugriff selbst an.
 *
 *  Warum kein SCSI-Miniport: NT 4.0 hat kein Plug and Play, und disk.sys
 *  sucht Platten nur beim Booten.  Ein nachtraeglich eingesteckter Stick
 *  wuerde erst nach einem Neustart zum Laufwerk.  Ein eigenes
 *  Plattengeraet -- nach dem Muster des RAM-Disk-Beispiels im DDK -- kann
 *  beim Einstecken entstehen und beim Abziehen verschwinden.
 *
 *  Warum nicht FreeBSDs umass: der haengt an CAM, dem SCSI-Unterbau von
 *  FreeBSD, und den gibt es hier nicht.  Bulk-Only und die paar
 *  SCSI-Befehle, die ein Stick braucht, sind ueberschaubar; die Oberkante
 *  wird neu geschrieben, der Stapel darunter bleibt unveraendert.
 *
 *  Aufbau:
 *
 *    NT-Plattengeraet  \\Device\\UsbPlatteN, \\DosDevices\\X:
 *        |  IRP_MJ_READ/WRITE -> Warteschlange
 *    Arbeitsfaden (PASSIVE_LEVEL)
 *        |  SCSI: READ(10), WRITE(10), TEST UNIT READY, ...
 *    Bulk-Only: Befehl (31 Byte) -> Daten -> Quittung (13 Byte)
 *        |  zwei gehaltene Bulk-Uebertragungen
 *    FreeBSD-USB-Stapel
 *
 *  Einheiten: ein Geraet kann mehrere LUNs haben -- ein Kombi-Kartenleser
 *  meldet je Schacht eine.  Jede Einheit bekommt ihr eigenes Medium, ihr
 *  eigenes NT-Geraet und ihren eigenen Buchstaben, auch ohne Karte, wie
 *  unter spaeteren Windows-Versionen.  Alle teilen sich den einen
 *  Bulk-Only-Kanal; USB-Speicher fuehren Befehle ohnehin nacheinander aus.
 *
 *  Kartenwechsel: im Leerlauf fragt der Faden alle zwei Sekunden jede
 *  Einheit mit TEST UNIT READY ab.  Karte weg, neu oder gewechselt ->
 *  DO_VERIFY_VOLUME; das Dateisystem prueft beim naechsten Zugriff und
 *  gibt die alte frei bzw. bindet die neue an.
 *
 *  Grenzen: nur die erste primaere Partition (oder keine
 *  Partitionstabelle), nur 512-Byte-Sektoren, hoechstens 8 Einheiten.
 *
 *  CD/DVD-Laufwerke (INQUIRY: Geraetetyp 5): eigenes NT-Geraet
 *  \\Device\\CdRomN vom Typ FILE_DEVICE_CD_ROM -- daran bindet sich
 *  CDFS --, 2048-Byte-Sektoren, keine Partitionen, nur lesen.  Dazu die
 *  CD-Steuercodes, die CDFS und Programme benutzen (Inhaltsverzeichnis,
 *  letzte Sitzung, Geometrie, Pruefen, Sperren, Auswerfen).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/callout.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_endian.h>
#include <ober/ntkbdmou.h>
#include <ntdddisk.h>

#define NTUM_PUFFER       65536         /* je Uebertragung */

/*  CD-Steuercodes (ntddcdrm.h des DDK), hier selbst gebildet -- die
 *  Werte sind seit NT 3.1 dieselben.  */
#define NTUM_CD(f, z)  CTL_CODE(FILE_DEVICE_CD_ROM, (f), METHOD_BUFFERED, (z))
#define NTUM_CD_READ_TOC           NTUM_CD(0x0000, FILE_READ_ACCESS)
#define NTUM_CD_GET_LAST_SESSION   NTUM_CD(0x000E, FILE_READ_ACCESS)
#define NTUM_CD_GET_DRIVE_GEOMETRY NTUM_CD(0x0013, FILE_READ_ACCESS)
#define NTUM_CD_CHECK_VERIFY       NTUM_CD(0x0200, FILE_READ_ACCESS)
#define NTUM_CD_MEDIA_REMOVAL      NTUM_CD(0x0201, FILE_READ_ACCESS)
#define NTUM_CD_EJECT_MEDIA        NTUM_CD(0x0202, FILE_READ_ACCESS)
#define NTUM_CD_LOAD_MEDIA         NTUM_CD(0x0203, FILE_READ_ACCESS)
#define NTUM_CD_RESERVE            NTUM_CD(0x0204, FILE_READ_ACCESS)
#define NTUM_CD_RELEASE            NTUM_CD(0x0205, FILE_READ_ACCESS)
#define NTUM_CD_FIND_NEW_DEVICES   NTUM_CD(0x0206, FILE_READ_ACCESS)
#define NTUM_CD_TOC_GROESSE        804       /* sizeof(CDROM_TOC) */
#define NTUM_ZEIT_BEFEHL  5000
#define NTUM_ZEIT_DATEN   20000

enum { NTUM_AUS, NTUM_EIN, NTUM_N };


/* ====================================================================
 *  Zustand
 *
 *  Wie bei den allgemeinen Geraeten zwei Lebensdauern: das softc lebt,
 *  solange der Stick steckt; das NT-Geraet, solange das Dateisystem es
 *  festhaelt.  Die Erweiterung des NT-Geraets haelt deshalb Sperre und
 *  Warteschlange selbst und zeigt nur auf das softc, solange es eines
 *  gibt.
 * ==================================================================== */

#define NTUM_MAX_LUN  8

struct ntum_softc;

/*  Eine Einheit = eine LUN = ein Schacht = ein Laufwerk.  */

typedef struct ntum_einheit {
    struct ntum_softc    *sc;
    uint8_t               lun;
    int                   medium;         /* 1 = bereit */
    uint32_t              bloecke;
    uint32_t              blockgroesse;
    int                   schreibschutz;
    uint32_t              part_start;     /* in Bloecken */
    uint32_t              part_laenge;
    uint8_t               part_typ;
    char                  name[40];       /* Hersteller + Produkt */
    const char           *fremd;          /* Dateisystem, das NT 4.0 nicht kennt */
    int                   cd;             /* Geraetetyp 5: CD/DVD */
    PDEVICE_OBJECT        nt;
} ntum_einheit;

typedef struct ntum_erweiterung {
    ntu_port            Port;        /* MUSS vorn stehen */
    KSPIN_LOCK          Sperre;
    ntum_einheit       *E;           /* 0 = abgezogen */
    LIST_ENTRY          Warteschlange;
    ULONG               Nummer;
    WCHAR               Buchstabe;
    int                 Cd;          /* \\Device\\CdRomN statt UsbPlatteN */
} ntum_erweiterung;

typedef struct ntum_auftrag {
    uint8_t   *daten;
    uint32_t   laenge;
    uint32_t   ist;
    int        fehler;
    int        fertig;
    int        raus;
    int        aktiv;
} ntum_auftrag;

struct ntum_softc {
    device_t              sc_dev;
    struct usb_device    *sc_udev;
    uint8_t               sc_iface_index;
    uint8_t               sc_iface_no;
    struct mtx            sc_mtx;
    struct cv             sc_cv;
    struct usb_xfer      *sc_xfer[NTUM_N];
    ntum_auftrag          sc_auftrag;
    uint32_t              sc_tag;
    volatile int          sc_weg;
    uint8_t               sc_sense[3];        /* des letzten Befehls */

    ntum_einheit          sc_e[NTUM_MAX_LUN];
    int                   sc_anzahl;

    ntu_thread            sc_faden;
    uint8_t              *sc_puffer;
};

static LONG       ntum_belegt[16];
static KSPIN_LOCK ntum_belegt_sperre;
static int        ntum_bereit;


/* ====================================================================
 *  Bulk -- synchron, auf zwei gehaltenen Uebertragungen
 * ==================================================================== */

static void
ntum_rueckruf (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntum_softc     *sc = usbd_xfer_softc(xfer);
    ntum_auftrag          *a  = &sc->sc_auftrag;
    struct usb_page_cache *pc;
    int                    ist;

    usbd_xfer_status(xfer, &ist, NULL, NULL, NULL);

    switch (USB_GET_STATE(xfer)) {

        case USB_ST_SETUP:

            if (!a->aktiv) {
                return;
            }

            pc = usbd_xfer_get_frame(xfer, 0);

            if (a->raus && a->laenge != 0) {
                usbd_copy_in(pc, 0, a->daten, a->laenge);
            }

            usbd_xfer_set_frame_len(xfer, 0, a->laenge);
            usbd_transfer_submit(xfer);
            break;

        case USB_ST_TRANSFERRED:

            if (!a->raus && ist > 0) {
                pc = usbd_xfer_get_frame(xfer, 0);
                usbd_copy_out(pc, 0, a->daten, ist);
            }

            a->ist    = (uint32_t)ist;
            a->fehler = 0;
            a->fertig = 1;
            cv_signal(&sc->sc_cv);
            break;

        default:

            /*  Ein gesperrter Endpunkt wird beim naechsten Start
             *  entsperrt -- so macht es FreeBSDs umass auch: der Stapel
             *  schickt dann selbst CLEAR_FEATURE(ENDPOINT_HALT) und setzt
             *  die Datenumschaltung zurueck.  */

            if (error == USB_ERR_STALLED) {
                usbd_xfer_set_stall(xfer);
            }

            a->ist    = (uint32_t)ist;
            a->fehler = (int)error;
            a->fertig = 1;
            cv_signal(&sc->sc_cv);
            break;
    }
}

static int
ntum_bulk (struct ntum_softc *sc, int raus, uint8_t *daten, uint32_t laenge,
           uint32_t zeit, uint32_t *ist)
{
    struct usb_xfer *x = sc->sc_xfer[raus ? NTUM_AUS : NTUM_EIN];
    ntum_auftrag    *a = &sc->sc_auftrag;

    /*  sc_weg unter der Sperre pruefen: detach setzt es und bricht unter
     *  derselben Sperre ab.  Draussen geprueft, koennte zwischen Pruefen
     *  und Starten abgezogen werden, und die Uebertragung hinge dann bis
     *  zu ihrem Zeitlimit.  */

    mtx_lock(&sc->sc_mtx);

    if (sc->sc_weg) {
        mtx_unlock(&sc->sc_mtx);
        return USB_ERR_CANCELLED;
    }

    memset(a, 0, sizeof(*a));
    a->daten  = daten;
    a->laenge = laenge;
    a->raus   = raus;
    a->aktiv  = 1;
    usbd_xfer_set_timeout(x, (int)zeit);
    usbd_transfer_start(x);

    while (!a->fertig) {
        cv_wait(&sc->sc_cv, &sc->sc_mtx);
    }

    a->aktiv = 0;
    mtx_unlock(&sc->sc_mtx);

    if (ist != 0) {
        *ist = a->ist;
    }

    return a->fehler;
}


/* ====================================================================
 *  Bulk-Only
 *
 *  Befehl (CBW, 31 Byte) -> Datenphase -> Quittung (CSW, 13 Byte).  Geht
 *  etwas schief, der Rueckweg nach der Norm: Massenspeicher-Reset
 *  (Klassenanfrage FF) und beide Endpunkte entsperren.
 *
 *  Die Quittung wird bei Zeitlimit oder Sperre einmal wiederholt -- die
 *  Norm sieht das vor, und QEMUs Speichernachbildung liefert sie nach
 *  einer Datenphase manchmal erst beim zweiten Versuch.
 * ==================================================================== */

static void
ntum_reset (struct ntum_softc *sc)
{
    struct usb_device_request r;

    r.bmRequestType = UT_WRITE_CLASS_INTERFACE;
    r.bRequest      = 0xFF;                /* Bulk-Only Mass Storage Reset */
    USETW(r.wValue,  0);
    USETW(r.wIndex,  sc->sc_iface_no);
    USETW(r.wLength, 0);
    (void)usbd_do_request_flags(sc->sc_udev, NULL, &r, NULL, 0, NULL, 2000);

    mtx_lock(&sc->sc_mtx);
    usbd_xfer_set_stall(sc->sc_xfer[NTUM_AUS]);
    usbd_xfer_set_stall(sc->sc_xfer[NTUM_EIN]);
    mtx_unlock(&sc->sc_mtx);
}

/*  0 = Befehl ausgefuehrt (Status in *status), sonst Transportfehler.  */

static int
ntum_befehl (struct ntum_softc *sc, uint8_t lun, const uint8_t *cdb, int cdblen,
             int raus, uint8_t *daten, uint32_t laenge, uint32_t *ist,
             uint8_t *status)
{
    uint8_t  cbw[31], csw[13];
    uint32_t n = 0, tag, k;
    int      e;

    *status = 0xFF;
    if (ist != 0) {
        *ist = 0;
    }

    tag = ++sc->sc_tag;
    memset(cbw, 0, sizeof(cbw));
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    cbw[4] = (uint8_t)tag;           cbw[5] = (uint8_t)(tag >> 8);
    cbw[6] = (uint8_t)(tag >> 16);   cbw[7] = (uint8_t)(tag >> 24);
    cbw[8] = (uint8_t)laenge;        cbw[9] = (uint8_t)(laenge >> 8);
    cbw[10] = (uint8_t)(laenge >> 16); cbw[11] = (uint8_t)(laenge >> 24);
    cbw[12] = raus ? 0x00 : 0x80;
    cbw[13] = lun & 0x0F;
    cbw[14] = (uint8_t)cdblen;
    memcpy(cbw + 15, cdb, (size_t)cdblen);

    e = ntum_bulk(sc, 1, cbw, 31, NTUM_ZEIT_BEFEHL, &n);

    if (e != 0) {
        if (!sc->sc_weg) ntum_reset(sc);
        return e;
    }

    if (laenge != 0) {

        e = ntum_bulk(sc, raus, daten, laenge, NTUM_ZEIT_DATEN, &n);

        if (ist != 0) {
            *ist = n;
        }

        /*  Kurz oder gesperrt ist in der Datenphase kein Grund zum
         *  Abbruch -- die Quittung sagt, was los ist.  Alles andere ist
         *  ein Transportfehler.  */

        if (e != 0 && e != USB_ERR_SHORT_XFER && e != USB_ERR_STALLED) {
            if (!sc->sc_weg) ntum_reset(sc);
            return e;
        }
    }

    for (k = 0; k < 2; k += 1) {
        e = ntum_bulk(sc, 0, csw, 13, NTUM_ZEIT_BEFEHL, &n);
        if (e == 0 && n == 13) {
            break;
        }
        if (sc->sc_weg) {
            return USB_ERR_CANCELLED;
        }
    }

    if (e != 0 || n != 13 || csw[0] != 'U' || csw[1] != 'S' ||
        csw[2] != 'B' || csw[3] != 'S' ||
        (uint32_t)(csw[4] | (csw[5] << 8) | (csw[6] << 16) |
                   ((uint32_t)csw[7] << 24)) != tag) {
        if (!sc->sc_weg) ntum_reset(sc);
        return (e != 0) ? e : USB_ERR_IOERROR;
    }

    if (csw[12] == 2) {                      /* Phasenfehler */
        ntum_reset(sc);
        return USB_ERR_IOERROR;
    }

    *status = csw[12];
    return 0;
}


/* ====================================================================
 *  SCSI
 * ==================================================================== */

static void
ntum_sense (ntum_einheit *E)
{
    struct ntum_softc *sc = E->sc;
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t s[18], st;
    uint32_t ist;

    memset(s, 0, sizeof(s));
    sc->sc_sense[0] = sc->sc_sense[1] = sc->sc_sense[2] = 0;

    if (ntum_befehl(sc, E->lun, cdb, 6, 0, s, 18, &ist, &st) == 0 && ist >= 14) {
        sc->sc_sense[0] = s[2] & 0x0F;
        sc->sc_sense[1] = s[12];
        sc->sc_sense[2] = s[13];
    }
}

/*  0 = gut; sonst: 1 fehlgeschlagen (Sense steht in sc_sense), 2 Transport.  */

static int
ntum_scsi (ntum_einheit *E, const uint8_t *cdb, int cdblen, int raus,
           uint8_t *daten, uint32_t laenge, uint32_t *ist)
{
    uint8_t st;

    E->sc->sc_sense[0] = 0;

    if (ntum_befehl(E->sc, E->lun, cdb, cdblen, raus, daten, laenge, ist,
                    &st) != 0) {
        return 2;
    }

    if (st != 0) {
        ntum_sense(E);
        return 1;
    }

    return 0;
}

static int
ntum_lesen (ntum_einheit *E, uint32_t lba, uint32_t bloecke, uint8_t *ziel)
{
    uint8_t  cdb[10];
    uint32_t ist, soll = bloecke * E->blockgroesse;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(bloecke >> 8); cdb[8] = (uint8_t)bloecke;

    if (ntum_scsi(E, cdb, 10, 0, ziel, soll, &ist) != 0 || ist != soll) {
        return 0;
    }

    return 1;
}

static int
ntum_schreiben (ntum_einheit *E, uint32_t lba, uint32_t bloecke,
                uint8_t *quelle)
{
    uint8_t  cdb[10];
    uint32_t ist, soll = bloecke * E->blockgroesse;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x2A;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(bloecke >> 8); cdb[8] = (uint8_t)bloecke;

    return ntum_scsi(E, cdb, 10, 1, quelle, soll, &ist) == 0 && ist == soll;
}


/* ====================================================================
 *  Medium einlesen -- je Einheit
 *
 *  Getrennt von INQUIRY: ein Kartenwechsel betrifft nur das Medium, der
 *  Schacht bleibt derselbe.  "geduld": beim Einstecken bis zu zehn
 *  Sekunden auf "bereit" warten (Sticks brauchen einen Moment); beim
 *  Nachsehen im Leerlauf nur kurz.
 * ==================================================================== */

/*  Heisst der letzte Sense "hier ist nichts lesbar"?
 *
 *    02/xx  NOT READY -- echte Kartenleser melden einen leeren Schacht
 *           meist als 02/3A (MEDIUM NOT PRESENT)
 *    05/25  LOGICAL UNIT NOT SUPPORTED -- so meldet QEMUs SCSI-Platte
 *           einen leeren Schacht, und manche Leser tun es ebenso
 *
 *  Die erste Fassung kannte nur 02/3A; unter QEMU blieb eine entnommene
 *  Karte deshalb unbemerkt.  */

static int
ntum_leer (struct ntum_softc *sc)
{
    return sc->sc_sense[0] == 0x02 ||
           (sc->sc_sense[0] == 0x05 && sc->sc_sense[1] == 0x25);
}

/*  memcmp gibt es im Kern nicht.  */

static int
ntum_gleich (const uint8_t *a, const char *b, int n)
{
    int k;

    for (k = 0; k < n; k += 1) {
        if (a[k] != (uint8_t)b[k]) {
            return 0;
        }
    }

    return 1;
}

/*  32 Bit Little Endian vom Datentraeger (GPT).  */
static uint32_t
ntum_le32 (const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int
ntum_medium (ntum_einheit *E, int geduld)
{
    struct ntum_softc *sc = E->sc;
    uint8_t  cdb[10], d[36];
    uint32_t ist, k, versuche = geduld ? 20 : 3;
    /*  Ein CD/DVD-Laufwerk braucht nach dem Einlegen gut 10-20 Sekunden,
     *  bis die Scheibe laeuft und gelesen ist (02/04 "wird bereit").  */
    uint8_t *mbr = sc->sc_puffer;

    E->medium = 0;

    if (E->name[0] == '\0') {

        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x12; cdb[4] = 36;
        memset(d, 0, sizeof(d));

        if (ntum_scsi(E, cdb, 6, 0, d, 36, &ist) == 0 && ist >= 36) {
            E->cd = (d[0] & 0x1F) == 0x05;        /* CD/DVD (MMC) */
            for (k = 0; k < 24; k += 1) {
                E->name[k] = (d[8 + k] >= 0x20 && d[8 + k] < 0x7F)
                           ? (char)d[8 + k] : ' ';
            }
            E->name[24] = '\0';
        }
    }

    for (k = 0; k < versuche; k += 1) {

        memset(cdb, 0, sizeof(cdb));

        if (ntum_scsi(E, cdb, 6, 0, NULL, 0, &ist) == 0) {
            break;
        }

        if (sc->sc_weg) {
            return 0;
        }

        /*  Kein Medium (02/3A): ein leerer Schacht.  Nicht weiter warten.
         *  UNIT ATTENTION (06): Medium neu oder gewechselt -- gleich noch
         *  einmal fragen, das zweite TEST UNIT READY geht dann durch.  */

        /*  "wird bereit" (02/04) ist Warten wert -- ein Stick kurz nach
         *  dem Einstecken; jedes andere "leer" nicht.  */

        if (ntum_leer(sc) &&
            !(sc->sc_sense[0] == 0x02 && sc->sc_sense[1] == 0x04)) {
            return 0;
        }

        if (sc->sc_sense[0] != 0x06) {
            ntu_pause_ticks(hz / 2);
        }
    }

    if (k == versuche && E->cd && geduld) {
        for (k = 0; k < 40; k += 1) {           /* noch bis 20 s */
            memset(cdb, 0, sizeof(cdb));
            if (ntum_scsi(E, cdb, 6, 0, NULL, 0, &ist) == 0) break;
            if (sc->sc_weg) return 0;
            if (!(sc->sc_sense[0] == 0x02 && sc->sc_sense[1] == 0x04) &&
                sc->sc_sense[0] != 0x06) return 0;
            ntu_pause_ticks(hz / 2);
        }
        if (k == 40) return 0;
    } else if (k == versuche) {
        return 0;
    }

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    memset(d, 0, sizeof(d));

    if (ntum_scsi(E, cdb, 10, 0, d, 8, &ist) != 0 || ist < 8) {
        return 0;
    }

    E->bloecke      = ((uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]) + 1;
    E->blockgroesse = (uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];

    if (E->cd) {

        /*  CD/DVD: immer 2048 Byte (Mode-1/Form-1-Nutzdaten).  Manche
         *  Laufwerke melden bei Audio- oder gemischten Scheiben 2352;
         *  gelesen wird mit READ(10) trotzdem in 2048er-Bloecken.  Keine
         *  Partitionen, kein Bootsektor -- CDFS liest ab Sektor 16 selbst.  */

        E->blockgroesse  = 2048;
        E->schreibschutz = 1;
        E->part_start    = 0;
        E->part_laenge   = E->bloecke;
        E->part_typ      = 0;
        E->fremd         = 0;
        E->medium        = 1;
        return 1;
    }

    if (E->blockgroesse != 512) {
        ntu_printf("umass: LUN %u Blockgroesse %u -- nur 512 unterstuetzt\n",
                   E->lun, E->blockgroesse);
        return 0;
    }

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1A; cdb[2] = 0x3F; cdb[4] = 4;
    memset(d, 0, sizeof(d));
    E->schreibschutz = 0;

    if (ntum_scsi(E, cdb, 6, 0, d, 4, &ist) == 0 && ist >= 4) {
        E->schreibschutz = (d[2] & 0x80) != 0;
    }

    if (!ntum_lesen(E, 0, 1, mbr)) {
        return 0;
    }

    E->part_start  = 0;
    E->part_laenge = E->bloecke;
    E->part_typ    = 0x06;

    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {

        /*  Ein FAT-Bootsektor gleich am Anfang -- Sprungbefehl und 512
         *  Byte je Sektor im BPB -- ist ein Datentraeger ohne
         *  Partitionstabelle ("Superfloppy").  Sonst der erste belegte
         *  Eintrag der Partitionstabelle.  */

        int superfloppy = (mbr[0] == 0xEB || mbr[0] == 0xE9) &&
                          mbr[11] == 0x00 && mbr[12] == 0x02;

        if (!superfloppy) {
            int nur_schutz = 0;

            /*  GPT:  Ein Schutz-MBR traegt genau einen Eintrag vom Typ 0xEE,
             *  der die ganze Platte belegt.  Die echten Partitionen stehen in
             *  der GPT ab Sektor 1 (Kopf) bzw. Sektor 2 (Eintraege).  Wer nur
             *  den MBR liest, landet mitten im GPT-Kopf und findet kein
             *  Dateisystem -- genau so geschehen mit einem 64-GB-Stick.       */

            for (k = 0; k < 4; k += 1) {
                if (mbr[446 + k * 16 + 4] == 0xEE) { nur_schutz = 1; break; }
            }

            if (nur_schutz) {

                /*  GPT (Patch "usb-gpt-fix", ueberarbeitet):
                 *  - mbr wird als Puffer fuer Kopf und Tabelle wiederverwendet
                 *    (kein weiteres KB auf dem Kernelstapel); der Bootsektor
                 *    wird unten ohnehin neu hineingelesen.
                 *  - 64-Bit-LBAs: obere Haelfte muss 0 sein (READ CAPACITY(10)
                 *    reicht nur bis 2 TB).  Eintragsgroesse 128..512, Vielfaches
                 *    von 128 (Spezifikation) -- sie kommt vom Datentraeger.
                 *  - NICHT der erste belegte Eintrag: Windows legt auf GPT-
                 *    Platten die Microsoft-Reserved-Partition (16 MB, ohne
                 *    Dateisystem) VOR die Datenpartition.  Rang: Basic Data 3,
                 *    Linux-Daten 2, sonst 1; MSR, EFI-System und Windows-
                 *    Wiederherstellung nie.                                    */

                int      rang_best = 0;
                uint32_t best_start = 0, best_laenge = 0;

                if (ntum_lesen(E, 1, 1, mbr) && ntum_gleich(mbr, "EFI PART", 8)) {

                    uint32_t tab_lba   = ntum_le32(mbr + 72);
                    uint32_t tab_hoch  = ntum_le32(mbr + 76);
                    uint32_t eintraege = ntum_le32(mbr + 80);
                    uint32_t groesse   = ntum_le32(mbr + 84);

                    if (tab_hoch == 0 && tab_lba >= 2 && tab_lba < E->bloecke &&
                        groesse >= 128 && groesse <= 512 && (groesse % 128) == 0 &&
                        eintraege > 0) {

                        uint32_t je_sektor = 512 / groesse, sektoren, sek, i, nr;

                        if (eintraege > 128) eintraege = 128;
                        sektoren = (eintraege + je_sektor - 1) / je_sektor;

                        for (sek = 0; sek < sektoren; sek += 1) {
                            if (!ntum_lesen(E, tab_lba + sek, 1, mbr)) break;

                            for (i = 0; i < je_sektor; i += 1) {
                                uint8_t *p = mbr + i * groesse;
                                uint32_t erste, letzte;
                                int      rang, j, leer = 1;

                                nr = sek * je_sektor + i;
                                if (nr >= eintraege) break;
                                for (j = 0; j < 16; j += 1) if (p[j]) { leer = 0; break; }
                                if (leer) continue;

                                erste  = ntum_le32(p + 32);
                                letzte = ntum_le32(p + 40);
                                if (ntum_le32(p + 36) != 0 || ntum_le32(p + 44) != 0) continue;
                                if (erste == 0 || letzte < erste || erste >= E->bloecke) continue;
                                if (letzte >= E->bloecke) letzte = E->bloecke - 1;

                                /*  Typ-GUID, erste 4 Byte (Little Endian abgelegt)  */
                                if      (p[0]==0xA2 && p[1]==0xA0 && p[2]==0xD0 && p[3]==0xEB) rang = 3; /* Basic Data  */
                                else if (p[0]==0xAF && p[1]==0x3D && p[2]==0xC6 && p[3]==0x0F) rang = 2; /* Linux-Daten */
                                else if (p[0]==0x16 && p[1]==0xE3 && p[2]==0xC9 && p[3]==0xE3) rang = 0; /* MSR         */
                                else if (p[0]==0x28 && p[1]==0x73 && p[2]==0x2A && p[3]==0xC1) rang = 0; /* EFI-System  */
                                else if (p[0]==0xA4 && p[1]==0xBB && p[2]==0x94 && p[3]==0xDE) rang = 0; /* Wiederherst.*/
                                else rang = 1;

                                if (rang > rang_best) {
                                    rang_best   = rang;
                                    best_start  = erste;
                                    best_laenge = letzte - erste + 1;
                                }
                            }
                        }
                    }
                }

                if (rang_best > 0) {
                    E->part_start  = best_start;
                    E->part_laenge = best_laenge;
                    E->part_typ    = 0x07;      /* unten am Bootsektor genauer */
                    ntu_printf("umass: LUN %u GPT, Partition ab %u, %u Bloecke\n",
                               E->lun, best_start, best_laenge);
                } else {
                    /*  GPT nicht lesbar oder nichts Brauchbares -- lieber die
                     *  ganze Einheit anbieten als auf den Schutzeintrag
                     *  hereinzufallen.  */
                    E->part_start  = 0;
                    E->part_laenge = E->bloecke;
                    E->part_typ    = 0x06;
                    ntu_printf("umass: LUN %u GPT ohne brauchbare Partition -- ganze Einheit\n", E->lun);
                }
            } else {
                for (k = 0; k < 4; k += 1) {
                    uint8_t *e = mbr + 446 + k * 16;
                    uint32_t start = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
                    uint32_t lang  = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;

                    if (e[4] != 0 && lang != 0 && start + lang <= E->bloecke) {
                        E->part_start  = start;
                        E->part_laenge = lang;
                        E->part_typ    = e[4];
                        break;
                    }
                }
            }
        }
    }

    /*  Dateisystem, das NT 4.0 nicht lesen kann?  Fast jeder Stick ab
     *  etwa 4 GB kommt ab Werk mit FAT32 oder exFAT; NT 4.0 kennt nur
     *  FAT12/16 und NTFS (FAT32 kam mit Windows 2000).  Das Laufwerk
     *  entsteht trotzdem -- aber ohne Hinweis steht man vor einem
     *  Buchstaben, der sich nicht oeffnen laesst.  So geschehen beim
     *  ersten Test mit einem echten Stick.
     *
     *  Erkannt am Bootsektor der Partition, nicht allein am
     *  Partitionstyp: der stimmt bei Sticks nicht immer.  */

    E->fremd = 0;

    if (E->part_start == 0 || ntum_lesen(E, E->part_start, 1, mbr)) {

        if (ntum_gleich(mbr + 3, "NTFS    ", 8)) {
            E->part_typ = 0x07;          /* NTFS -- NT 4.0 liest das */
        } else if (ntum_gleich(mbr + 3, "EXFAT   ", 8)) {
            E->fremd = "exFAT";
        } else if (ntum_gleich(mbr + 0x52, "FAT32   ", 8) ||
                   E->part_typ == 0x0B || E->part_typ == 0x0C) {
            E->fremd = "FAT32";
        } else if (ntum_gleich(mbr + 0x36, "FAT16   ", 8) ||
                   ntum_gleich(mbr + 0x36, "FAT12   ", 8) ||
                   ntum_gleich(mbr + 0x36, "FAT     ", 8)) {
            E->part_typ = 0x06;          /* FAT16/12 */
        }
    }

    E->medium = 1;
    return 1;
}

static void
ntum_verify (ntum_einheit *E)
{
    /*  Ist ein Dateisystem gebunden, soll es beim naechsten Zugriff
     *  pruefen.  So gibt es eine entnommene Karte frei und bindet eine
     *  neue an, statt mit dem alten Inhalt weiterzuarbeiten.  */

    if (E->nt != 0 && E->nt->Vpb != 0 && (E->nt->Vpb->Flags & VPB_MOUNTED)) {
        E->nt->Flags |= DO_VERIFY_VOLUME;
    }
}

static void
ntum_bericht (ntum_einheit *E, const char *was)
{
    ntum_erweiterung *x = E->nt ? (ntum_erweiterung *)E->nt->DeviceExtension : 0;
    char b = (x && x->Buchstabe) ? (char)x->Buchstabe : '?';

    if (E->medium && E->cd) {
        ntu_printf("umass: %c: LUN %u %s  %s  CD/DVD, %u MB\n",
                   b, E->lun, was, E->name, E->bloecke / 512);
    } else if (E->medium) {
        ntu_printf("umass: %c: LUN %u %s  %s  %u MB%s, Partition ab %u, Typ %02x\n",
                   b, E->lun, was, E->name, E->bloecke / 2048,
                   E->schreibschutz ? " schreibgeschuetzt" : "",
                   E->part_start, E->part_typ);

        if (E->fremd != 0) {
            ntu_printf("umass: %c: ist %s formatiert -- das kann NT 4.0 nicht "
                       "lesen.  Mit FAT16 (bis 2 GB) oder NTFS formatieren.\n",
                       b, E->fremd);
        }
    } else {
        ntu_printf("umass: %c: LUN %u %s  %s  kein Medium\n", b, E->lun, was,
                   E->name);
    }
}


/* ====================================================================
 *  Das NT-Plattengeraet
 * ==================================================================== */

static int
ntum_nt_anlegen (ntum_einheit *E)
{
    WCHAR             dname[32], lname[24];
    UNICODE_STRING    un, ul;
    PDEVICE_OBJECT    dev = 0;
    ntum_erweiterung *x;
    NTSTATUS          st;
    ULONG             n;
    WCHAR             b;
    KIRQL             alt;

    if (!ntum_bereit) {
        KeInitializeSpinLock(&ntum_belegt_sperre);
        ntum_bereit = 1;
    }

    /*  Freie Nummer und Geraet in einem.  Haelt ein Dateisystem das
     *  Geraet eines eben abgezogenen Sticks noch fest, lebt dessen Name
     *  bis zum letzten Verweis weiter -- IoCreateDevice scheitert dann an
     *  genau dieser Nummer.  In dem Fall die naechste nehmen.  */

    if (E->cd) {

        /*  CD/DVD: \\Device\\CdRomN, wie cdrom.sys es anlegt -- so findet
         *  jedes Programm (und Setup) das Laufwerk unter dem gewohnten
         *  Namen.  N ab der Zahl der schon vorhandenen CD-Laufwerke;
         *  CdRomCount danach hochzaehlen, wie es ein Klassentreiber tut.  */

        PCONFIGURATION_INFORMATION ci = IoGetConfigurationInformation();
        ULONG k0 = ci ? ci->CdRomCount : 0;

        for (n = k0; n < k0 + 16; n += 1) {
            ntu_wsprintf(dname, 32, L"\\Device\\CdRom%u", n);
            RtlInitUnicodeString(&un, dname);
            st = IoCreateDevice(NtuDriverObject, sizeof(ntum_erweiterung), &un,
                                FILE_DEVICE_CD_ROM,
                                FILE_REMOVABLE_MEDIA | FILE_READ_ONLY_DEVICE,
                                FALSE, &dev);
            if (NT_SUCCESS(st)) break;
            if (st != STATUS_OBJECT_NAME_COLLISION) {
                ntu_printf("umass: IoCreateDevice (CD) %08x\n", st);
                return 0;
            }
        }
        if (n == k0 + 16) {
            return 0;
        }
        if (ci && ci->CdRomCount <= n) {
            ci->CdRomCount = n + 1;
        }
    }

    for (n = 0; !E->cd && n < 16; n += 1) {

        KeAcquireSpinLock(&ntum_belegt_sperre, &alt);
        if (ntum_belegt[n] != 0) {
            KeReleaseSpinLock(&ntum_belegt_sperre, alt);
            continue;
        }
        ntum_belegt[n] = 1;
        KeReleaseSpinLock(&ntum_belegt_sperre, alt);

        ntu_wsprintf(dname, 32, L"\\Device\\UsbPlatte%u", n);
        RtlInitUnicodeString(&un, dname);

        /*  FILE_DEVICE_DISK bringt einen VPB mit -- daran bindet sich das
         *  Dateisystem.  FILE_REMOVABLE_MEDIA sagt ihm, dass das Medium
         *  verschwinden kann.  */

        st = IoCreateDevice(NtuDriverObject, sizeof(ntum_erweiterung), &un,
                            FILE_DEVICE_DISK, FILE_REMOVABLE_MEDIA, FALSE, &dev);

        if (NT_SUCCESS(st)) {
            break;
        }

        ntum_belegt[n] = 0;

        if (st != STATUS_OBJECT_NAME_COLLISION) {
            ntu_printf("umass: IoCreateDevice %08x\n", st);
            return 0;
        }
    }

    if (!E->cd && n == 16) {
        return 0;
    }

    x = (ntum_erweiterung *)dev->DeviceExtension;
    memset(x, 0, sizeof(*x));
    x->Port.Kind   = NTU_PORT_PLATTE;
    x->Port.Suffix = n;
    x->Nummer      = n;
    x->Cd          = E->cd;
    x->E           = E;
    KeInitializeSpinLock(&x->Sperre);
    InitializeListHead(&x->Warteschlange);

    dev->Flags         |= DO_DIRECT_IO;
    dev->AlignmentRequirement = FILE_WORD_ALIGNMENT;
    dev->SectorSize     = E->cd ? 2048 : 512;
    dev->Flags         &= ~DO_DEVICE_INITIALIZING;

    /*  Laufwerksbuchstabe: der erste freie ab D:.  IoCreateSymbolicLink
     *  scheitert an einem belegten Namen -- das ist die Pruefung, ohne
     *  Wettlauf zwischen Nachsehen und Anlegen.  */

    for (b = L'D'; b <= L'Z'; b += 1) {

        lname[0] = L'\\'; lname[1] = L'D'; lname[2] = L'o'; lname[3] = L's';
        lname[4] = L'D'; lname[5] = L'e'; lname[6] = L'v'; lname[7] = L'i';
        lname[8] = L'c'; lname[9] = L'e'; lname[10] = L's'; lname[11] = L'\\';
        lname[12] = b; lname[13] = L':'; lname[14] = 0;
        RtlInitUnicodeString(&ul, lname);

        if (NT_SUCCESS(IoCreateSymbolicLink(&ul, &un))) {
            x->Buchstabe = b;
            break;
        }
    }

    E->nt = dev;
    return 1;
}

static void
ntum_nt_abbauen (ntum_einheit *E)
{
    ntum_erweiterung *x;
    PDEVICE_OBJECT    dev = E->nt;
    WCHAR             lname[16];
    UNICODE_STRING    ul;
    KIRQL             alt;

    if (dev == 0) {
        return;
    }

    x = (ntum_erweiterung *)dev->DeviceExtension;

    KeAcquireSpinLock(&x->Sperre, &alt);
    x->E = 0;
    KeReleaseSpinLock(&x->Sperre, alt);

    /*  Ist ein Dateisystem gebunden, soll es beim naechsten Zugriff
     *  pruefen -- und dabei feststellen, dass das Medium fehlt.  So gibt
     *  es den Datentraeger frei, statt mit einem Stick weiterzuarbeiten,
     *  den es nicht mehr gibt.  */

    if (dev->Vpb != 0 && (dev->Vpb->Flags & VPB_MOUNTED)) {
        dev->Flags |= DO_VERIFY_VOLUME;
    }

    if (x->Buchstabe != 0) {
        lname[0] = L'\\'; lname[1] = L'D'; lname[2] = L'o'; lname[3] = L's';
        lname[4] = L'D'; lname[5] = L'e'; lname[6] = L'v'; lname[7] = L'i';
        lname[8] = L'c'; lname[9] = L'e'; lname[10] = L's'; lname[11] = L'\\';
        lname[12] = x->Buchstabe; lname[13] = L':'; lname[14] = 0;
        RtlInitUnicodeString(&ul, lname);
        IoDeleteSymbolicLink(&ul);
    }

    ntu_printf("umass: %c: abgezogen\n", x->Buchstabe ? (char)x->Buchstabe : '?');

    if (!x->Cd) {
        ntum_belegt[x->Nummer] = 0;
    }
    IoDeleteDevice(dev);
    E->nt = 0;
}


/* ====================================================================
 *  Anfragen des Dateisystems
 * ==================================================================== */

static NTSTATUS
ntum_fertig (PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status      = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, (st == STATUS_SUCCESS) ? IO_DISK_INCREMENT : IO_NO_INCREMENT);
    return st;
}

NTSTATUS
ntum_irp (PDEVICE_OBJECT dev, PIRP irp)
{
    ntum_erweiterung  *x  = (ntum_erweiterung *)dev->DeviceExtension;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    KIRQL              alt;

    if (sl->MajorFunction == IRP_MJ_FLUSH_BUFFERS) {

        /*  Geschrieben wird ohnehin durchgehend -- jede Schreibanfrage
         *  ist fertig, wenn der Stick sie quittiert hat.  */

        return ntum_fertig(irp, STATUS_SUCCESS, 0);
    }

    /*  Dateisysteme pruefen nach einem Wechsel selbst; bis dahin nichts
     *  annehmen, ausser die Anfrage darf ausdruecklich durch.  */

    if ((dev->Flags & DO_VERIFY_VOLUME) && !(sl->Flags & SL_OVERRIDE_VERIFY_VOLUME)) {
        return ntum_fertig(irp, STATUS_VERIFY_REQUIRED, 0);
    }

    KeAcquireSpinLock(&x->Sperre, &alt);

    if (x->E == 0) {
        KeReleaseSpinLock(&x->Sperre, alt);
        return ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
    }

    IoMarkIrpPending(irp);
    InsertTailList(&x->Warteschlange, &irp->Tail.Overlay.ListEntry);
    KeSetEvent(&x->E->sc->sc_faden.Wake, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&x->Sperre, alt);

    return STATUS_PENDING;
}

/*  Im Arbeitsfaden: READ TOC fuer IOCTL_CDROM_READ_TOC (Format 0,
 *  MSF-Adressen -- so liefert es cdrom.sys, und so erwartet es CDFS) und
 *  IOCTL_CDROM_GET_LAST_SESSION (Format 1, LBA).  Die Antwort des
 *  Laufwerks hat schon den Aufbau von CDROM_TOC bzw.
 *  CDROM_TOC_SESSION_DATA; sie wird unveraendert weitergereicht.
 *  EJECT/LOAD: START STOP UNIT mit LoEj.  */

static void
ntum_cd_befehl (ntum_einheit *E, PIRP irp)
{
    struct ntum_softc *sc = E->sc;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    ULONG              code = sl->Parameters.DeviceIoControl.IoControlCode;
    ULONG              aus  = sl->Parameters.DeviceIoControl.OutputBufferLength;
    uint8_t            cdb[10];
    uint32_t           ist = 0, soll;
    int                r;

    memset(cdb, 0, sizeof(cdb));

    if (code == NTUM_CD_EJECT_MEDIA || code == NTUM_CD_LOAD_MEDIA) {
        cdb[0] = 0x1B;                              /* START STOP UNIT */
        cdb[4] = (code == NTUM_CD_EJECT_MEDIA) ? 0x02 : 0x03;
        r = ntum_scsi(E, cdb, 6, 0, NULL, 0, &ist);
        if (code == NTUM_CD_EJECT_MEDIA && r == 0) {
            E->medium = 0;
            ntum_verify(E);
        }
        ntum_fertig(irp, r == 0 ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR, 0);
        return;
    }

    if (!E->medium) {
        ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
        return;
    }

    soll = (code == NTUM_CD_READ_TOC) ? NTUM_CD_TOC_GROESSE : 12;
    if (aus < 4) {
        ntum_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
        return;
    }
    if (soll > aus) soll = aus;

    cdb[0] = 0x43;                                  /* READ TOC */
    cdb[1] = (code == NTUM_CD_READ_TOC) ? 0x02 : 0x00;   /* MSF */
    cdb[2] = (code == NTUM_CD_READ_TOC) ? 0x00 : 0x01;   /* Format */
    cdb[7] = (uint8_t)(soll >> 8);
    cdb[8] = (uint8_t)soll;

    memset(sc->sc_puffer, 0, soll);
    r = ntum_scsi(E, cdb, 10, 0, sc->sc_puffer, soll, &ist);

    if (r != 0 || ist < 4) {
        ntum_fertig(irp, ntum_leer(sc) ? STATUS_NO_MEDIA_IN_DEVICE
                                       : STATUS_IO_DEVICE_ERROR, 0);
        return;
    }

    if (ist > soll) ist = soll;
    memcpy(irp->AssociatedIrp.SystemBuffer, sc->sc_puffer, ist);
    ntum_fertig(irp, STATUS_SUCCESS, ist);
}

static void
ntum_bearbeiten (ntum_einheit *E, PIRP irp)
{
    struct ntum_softc *sc = E->sc;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    int                schreiben = (sl->MajorFunction == IRP_MJ_WRITE);

    if (sl->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        ntum_cd_befehl(E, irp);
        return;
    }

    {
    LONGLONG           ab   = schreiben ? sl->Parameters.Write.ByteOffset.QuadPart
                                        : sl->Parameters.Read.ByteOffset.QuadPart;
    ULONG              lang = schreiben ? sl->Parameters.Write.Length
                                        : sl->Parameters.Read.Length;
    uint32_t           bs = E->blockgroesse, lba, bloecke, stueck, fertig = 0;
    uint8_t           *mem;

    if (!E->medium) {
        ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
        return;
    }

    if (schreiben && E->schreibschutz) {
        ntum_fertig(irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
        return;
    }

    if ((ab % bs) != 0 || (lang % bs) != 0 || ab < 0 ||
        (ULONGLONG)ab + lang > (ULONGLONG)E->part_laenge * bs) {
        ntum_fertig(irp, STATUS_INVALID_PARAMETER, 0);
        return;
    }

    if (lang == 0) {
        ntum_fertig(irp, STATUS_SUCCESS, 0);
        return;
    }

    mem = (uint8_t *)MmGetSystemAddressForMdl(irp->MdlAddress);

    if (mem == 0) {
        ntum_fertig(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return;
    }

    lba     = E->part_start + (uint32_t)(ab / bs);
    bloecke = lang / bs;

    while (bloecke != 0) {

        stueck = (bloecke > NTUM_PUFFER / bs) ? NTUM_PUFFER / bs : bloecke;

        if (schreiben) {
            memcpy(sc->sc_puffer, mem + fertig, stueck * bs);
            if (!ntum_schreiben(E, lba, stueck, sc->sc_puffer)) break;
        } else {
            if (!ntum_lesen(E, lba, stueck, sc->sc_puffer)) break;
            memcpy(mem + fertig, sc->sc_puffer, stueck * bs);
        }

        lba     += stueck;
        bloecke -= stueck;
        fertig  += stueck * bs;
    }

    if (bloecke != 0) {

        /*  Medium weg (Karte gezogen) oder Geraet weg: das Dateisystem
         *  soll pruefen.  Ein gewoehnlicher Lesefehler bleibt ein
         *  Lesefehler.  */

        NTSTATUS st = STATUS_IO_DEVICE_ERROR;

        if (sc->sc_weg || ntum_leer(sc)) {
            E->medium = 0;
            ntum_verify(E);
            st = STATUS_NO_MEDIA_IN_DEVICE;
        } else if (sc->sc_sense[0] == 0x06) {
            ntum_medium(E, 0);
            E->nt->Flags |= DO_VERIFY_VOLUME;
            st = STATUS_VERIFY_REQUIRED;
        } else if (sc->sc_sense[0] == 0x07) {
            st = STATUS_MEDIA_WRITE_PROTECTED;
        }

        ntum_fertig(irp, st, fertig);
        return;
    }

    ntum_fertig(irp, STATUS_SUCCESS, lang);
    }
}

NTSTATUS
ntum_ioctl (PDEVICE_OBJECT dev, PIRP irp)
{
    ntum_erweiterung  *x  = (ntum_erweiterung *)dev->DeviceExtension;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    ULONG              aus = sl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID              p   = irp->AssociatedIrp.SystemBuffer;
    ntum_einheit      *E;
    uint32_t           bloecke, start, laenge, bs;
    uint8_t            typ;
    int                schutz, medium;
    KIRQL              alt;

    /*  Die Werte unter der Sperre abgreifen -- danach darf das softc weg
     *  sein, ohne dass hier etwas zeigt.  */

    KeAcquireSpinLock(&x->Sperre, &alt);
    E = x->E;
    medium  = (E != 0) && E->medium && !E->sc->sc_weg;
    bloecke = E ? E->bloecke : 0;
    start   = E ? E->part_start : 0;
    laenge  = E ? E->part_laenge : 0;
    bs      = E ? E->blockgroesse : 512;
    typ     = E ? E->part_typ : 0;
    schutz  = E ? E->schreibschutz : 0;
    KeReleaseSpinLock(&x->Sperre, alt);

    switch (sl->Parameters.DeviceIoControl.IoControlCode) {

        /*  CD/DVD -- Befehle an das Laufwerk gehen durch den Arbeitsfaden.  */

        case NTUM_CD_READ_TOC:
        case NTUM_CD_GET_LAST_SESSION:
        case NTUM_CD_EJECT_MEDIA:
        case NTUM_CD_LOAD_MEDIA:
            if (!x->Cd) {
                return ntum_fertig(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
            }
            return ntum_irp(dev, irp);

        case NTUM_CD_GET_DRIVE_GEOMETRY: {

            DISK_GEOMETRY *g = (DISK_GEOMETRY *)p;

            if (aus < sizeof(*g)) {
                return ntum_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
            }
            if (!medium) {
                return ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
            }
            g->MediaType          = RemovableMedia;
            g->TracksPerCylinder  = 64;             /* wie cdrom.sys */
            g->SectorsPerTrack    = 32;
            g->BytesPerSector     = 2048;
            g->Cylinders.QuadPart = (bloecke / (64 * 32)) ? (bloecke / (64 * 32)) : 1;
            return ntum_fertig(irp, STATUS_SUCCESS, sizeof(*g));
        }

        case NTUM_CD_CHECK_VERIFY:
        case IOCTL_DISK_CHECK_VERIFY:

            /*  Wie disk.sys: steht DO_VERIFY_VOLUME, erst "Pruefung
             *  noetig" -- nur daran merkt das Dateisystem einen Wechsel.  */

            if ((dev->Flags & DO_VERIFY_VOLUME) &&
                !(sl->Flags & SL_OVERRIDE_VERIFY_VOLUME)) {
                return ntum_fertig(irp, STATUS_VERIFY_REQUIRED, 0);
            }

            if (!medium) {
                return ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
            }
            return ntum_fertig(irp, STATUS_SUCCESS, 0);

        case IOCTL_DISK_GET_DRIVE_GEOMETRY: {

            DISK_GEOMETRY *g = (DISK_GEOMETRY *)p;

            if (aus < sizeof(*g)) {
                return ntum_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
            }
            if (!medium) {
                return ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
            }

            /*  Eine erfundene, aber uebliche Geometrie: 255 Koepfe,
             *  63 Sektoren.  Das Dateisystem braucht davon nur die
             *  Sektorgroesse und dass die Rechnung aufgeht.  */

            g->MediaType         = RemovableMedia;
            g->TracksPerCylinder = 255;
            g->SectorsPerTrack   = 63;
            g->BytesPerSector    = bs;
            g->Cylinders.QuadPart = (bloecke / (255 * 63)) ? (bloecke / (255 * 63)) : 1;
            return ntum_fertig(irp, STATUS_SUCCESS, sizeof(*g));
        }

        case IOCTL_DISK_GET_PARTITION_INFO: {

            PARTITION_INFORMATION *pi = (PARTITION_INFORMATION *)p;

            if (aus < sizeof(*pi)) {
                return ntum_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
            }
            if (!medium) {
                return ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
            }

            memset(pi, 0, sizeof(*pi));
            pi->StartingOffset.QuadPart  = (LONGLONG)start * bs;
            pi->PartitionLength.QuadPart = (LONGLONG)laenge * bs;
            pi->HiddenSectors            = start;
            pi->PartitionNumber          = 1;
            pi->PartitionType            = typ;
            pi->BootIndicator            = FALSE;
            pi->RecognizedPartition      = TRUE;
            pi->RewritePartition         = FALSE;
            return ntum_fertig(irp, STATUS_SUCCESS, sizeof(*pi));
        }

        case IOCTL_DISK_IS_WRITABLE:
            if (!medium) {
                return ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
            }
            return ntum_fertig(irp, schutz ? STATUS_MEDIA_WRITE_PROTECTED
                                           : STATUS_SUCCESS, 0);

        case NTUM_CD_MEDIA_REMOVAL:
        case NTUM_CD_RESERVE:
        case NTUM_CD_RELEASE:
        case NTUM_CD_FIND_NEW_DEVICES:
            return ntum_fertig(irp, STATUS_SUCCESS, 0);

        case IOCTL_DISK_VERIFY:
        case IOCTL_DISK_SET_PARTITION_INFO:
        case IOCTL_DISK_MEDIA_REMOVAL:
        case IOCTL_DISK_EJECT_MEDIA:
            return ntum_fertig(irp, medium ? STATUS_SUCCESS
                                           : STATUS_NO_MEDIA_IN_DEVICE, 0);

        default:
            return ntum_fertig(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}


/* ====================================================================
 *  Der Arbeitsfaden
 * ==================================================================== */

static int
ntum_max_lun (struct ntum_softc *sc)
{
    struct usb_device_request r;
    uint8_t                   n = 0;

    /*  GET MAX LUN (Klassenanfrage FE).  Geraete mit nur einer LUN
     *  duerfen die Anfrage sperren -- dann ist es eben eine.  */

    r.bmRequestType = UT_READ_CLASS_INTERFACE;
    r.bRequest      = 0xFE;
    USETW(r.wValue,  0);
    USETW(r.wIndex,  sc->sc_iface_no);
    USETW(r.wLength, 1);

    if (usbd_do_request_flags(sc->sc_udev, NULL, &r, &n, 0, NULL, 2000) != 0) {
        n = 0;
    }

    return (n < NTUM_MAX_LUN) ? n + 1 : NTUM_MAX_LUN;
}

/*  Im Leerlauf: Karte eingelegt, entnommen, gewechselt?  */

static void
ntum_pruefen (ntum_einheit *E)
{
    struct ntum_softc *sc = E->sc;
    uint8_t            cdb[6];
    uint32_t           ist;
    int                r;

    memset(cdb, 0, sizeof(cdb));
    r = ntum_scsi(E, cdb, 6, 0, NULL, 0, &ist);


    if (r == 2 || sc->sc_weg) {
        return;                         /* Transport -- abziehen regelt das */
    }

    if (r == 0) {
        if (!E->medium && ntum_medium(E, 0)) {
            ntum_verify(E);
            ntum_bericht(E, "eingelegt");
        }
        return;
    }

    if (sc->sc_sense[0] == 0x06) {

        /*  UNIT ATTENTION: das Medium ist neu oder gewechselt -- auch
         *  wenn es zwischen zwei Abfragen herausgenommen und ein anderes
         *  hineingesteckt wurde.  Neu einlesen und pruefen lassen.  */

        int vorher = E->medium;

        ntum_medium(E, 0);
        ntum_verify(E);

        /*  Nach dem Einlesen sagen, was es wirklich war -- QEMU meldet
         *  auch beim blossen Entnehmen zuerst UNIT ATTENTION.  */

        ntum_bericht(E, !E->medium ? "entnommen" :
                        vorher     ? "gewechselt" : "eingelegt");
        return;
    }

    if (ntum_leer(sc) && E->medium) {
        E->medium = 0;
        ntum_verify(E);
        ntum_bericht(E, "entnommen");
    }
}

static int
ntum_einen_bearbeiten (ntum_einheit *E)
{
    ntum_erweiterung *x;
    PLIST_ENTRY       e;
    KIRQL             alt;

    if (E->nt == 0) {
        return 0;
    }

    x = (ntum_erweiterung *)E->nt->DeviceExtension;

    KeAcquireSpinLock(&x->Sperre, &alt);
    e = IsListEmpty(&x->Warteschlange) ? 0 : RemoveHeadList(&x->Warteschlange);
    KeReleaseSpinLock(&x->Sperre, alt);

    if (e == 0) {
        return 0;
    }

    {
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);

        if (E->sc->sc_faden.Stop || E->sc->sc_weg) {
            ntum_fertig(irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
        } else {
            ntum_bearbeiten(E, irp);
        }
    }

    return 1;
}

static VOID
ntum_faden (PVOID ctx)
{
    struct ntum_softc *sc = (struct ntum_softc *)ctx;
    LARGE_INTEGER      takt;
    int                k;

    ntu_wart_name("umass");

    sc->sc_anzahl = ntum_max_lun(sc);

    /*  Jede Einheit bekommt ein Laufwerk, auch ohne Medium -- ein leerer
     *  Kartenschacht hat seinen Buchstaben, und "kein Datentraeger" ist
     *  die richtige Antwort, bis eine Karte steckt.  */

    for (k = 0; k < sc->sc_anzahl && !sc->sc_weg; k += 1) {

        ntum_einheit *E = &sc->sc_e[k];

        E->sc  = sc;
        E->lun = (uint8_t)k;

        ntum_medium(E, k == 0);         /* Geduld nur beim ersten */

        if (ntum_nt_anlegen(E)) {
            ntum_bericht(E, "=");
        }
    }

    if (sc->sc_anzahl > 1) {
        ntu_printf("umass: %d Einheiten (Kartenleser mit %d Schaechten)\n",
                   sc->sc_anzahl, sc->sc_anzahl);
    }

    takt.QuadPart = -2LL * 10000000LL;   /* zwei Sekunden */

    while (!sc->sc_faden.Stop) {

        NTSTATUS w = KeWaitForSingleObject(&sc->sc_faden.Wake, Executive,
                                           KernelMode, FALSE, &takt);
        int gearbeitet = 0, weiter;

        /*  Reihum je Einheit eine Anfrage -- so kommt kein Schacht zu
         *  kurz, wenn auf einem anderen gerade viel kopiert wird.  */

        do {
            weiter = 0;
            for (k = 0; k < sc->sc_anzahl; k += 1) {
                if (ntum_einen_bearbeiten(&sc->sc_e[k])) {
                    weiter = 1;
                    gearbeitet = 1;
                }
            }
        } while (weiter && !sc->sc_faden.Stop);

        if (w == STATUS_TIMEOUT && !gearbeitet && !sc->sc_faden.Stop &&
            !sc->sc_weg) {
            for (k = 0; k < sc->sc_anzahl; k += 1) {
                ntum_pruefen(&sc->sc_e[k]);
            }
        }
    }

    /*  Was noch wartet, als abgezogen zurueckgeben.  */

    for (k = 0; k < sc->sc_anzahl; k += 1) {
        while (ntum_einen_bearbeiten(&sc->sc_e[k])) {
            ;
        }
    }
}


/* ====================================================================
 *  probe, attach, detach
 * ==================================================================== */

static const struct usb_config ntum_konfig[NTUM_N] = {
    [NTUM_AUS] = {
        .type      = UE_BULK,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_OUT,
        .bufsize   = NTUM_PUFFER,
        .flags     = { .proxy_buffer = 1, .pipe_bof = 1 },
        .callback  = &ntum_rueckruf,
        .timeout   = NTUM_ZEIT_DATEN,
    },
    [NTUM_EIN] = {
        .type      = UE_BULK,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_IN,
        .bufsize   = NTUM_PUFFER,
        .flags     = { .proxy_buffer = 1, .short_xfer_ok = 1, .pipe_bof = 1 },
        .callback  = &ntum_rueckruf,
        .timeout   = NTUM_ZEIT_DATEN,
    },
};

static int
ntum_probe (device_t dev)
{
    struct usb_attach_arg *uaa = device_get_ivars(dev);

    if (uaa->usb_mode != USB_MODE_HOST || uaa->iface == 0) {
        return ENXIO;
    }

    /*  Massenspeicher, Bulk-Only.  Unterklasse 06 (SCSI) ist der
     *  Normalfall; 05 (SFF-8070i) und 02 (ATAPI) sprechen bei Sticks und
     *  Kartenlesern in der Praxis dieselben Befehle.  CBI (Protokoll 00
     *  und 01, alte Diskettenlaufwerke) bleibt bei der allgemeinen
     *  Bereitstellung.  */

    if (uaa->info.bInterfaceClass == UICLASS_MASS &&
        uaa->info.bInterfaceProtocol == 0x50 &&
        (uaa->info.bInterfaceSubClass == 0x06 ||
         uaa->info.bInterfaceSubClass == 0x05 ||
         uaa->info.bInterfaceSubClass == 0x02)) {
        return BUS_PROBE_DEFAULT;
    }

    return ENXIO;
}

static int
ntum_attach (device_t dev)
{
    struct ntum_softc     *sc  = device_get_softc(dev);
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    usb_error_t            err;

    sc->sc_dev         = dev;
    sc->sc_udev        = uaa->device;
    sc->sc_iface_index = uaa->info.bIfaceIndex;
    sc->sc_iface_no    = uaa->info.bIfaceNum;

    device_set_desc(dev, "USB-Massenspeicher");
    mtx_init(&sc->sc_mtx, "umass", NULL, MTX_DEF);
    cv_init(&sc->sc_cv, "umass");

    sc->sc_puffer = (uint8_t *)ntu_malloc(NTUM_PUFFER, 0);

    if (sc->sc_puffer == 0) {
        goto weg;
    }

    err = usbd_transfer_setup(sc->sc_udev, &sc->sc_iface_index, sc->sc_xfer,
                              ntum_konfig, NTUM_N, sc, &sc->sc_mtx);

    if (err != 0) {
        device_printf(dev, "usbd_transfer_setup: %s\n", usbd_errstr(err));
        goto weg;
    }

    /*  Die Einrichtung des Mediums dauert -- bis zu zehn Sekunden, bis
     *  ein Stick bereit ist.  Das gehoert nicht in attach, das im
     *  Aufzaehlungsfaden laeuft und alle anderen Geraete aufhielte.  */

    if (!NT_SUCCESS(ntu_thread_start(&sc->sc_faden, ntum_faden, sc, "umass"))) {
        usbd_transfer_unsetup(sc->sc_xfer, NTUM_N);
        goto weg;
    }

    return 0;

weg:
    if (sc->sc_puffer != 0) {
        ntu_free(sc->sc_puffer);
    }
    cv_destroy(&sc->sc_cv);
    mtx_destroy(&sc->sc_mtx);
    return ENXIO;
}

static int
ntum_detach (device_t dev)
{
    struct ntum_softc *sc = device_get_softc(dev);

    /*  Reihenfolge:
     *    1. als weg markieren und laufende Uebertragungen abbrechen --
     *       der Faden kommt dann aus seinem Warten zurueck, statt bis zum
     *       Zeitlimit zu haengen;
     *    2. den Faden anhalten; er gibt Wartendes als "kein Medium"
     *       zurueck;
     *    3. das NT-Geraet abbauen (Bruecke kappen, Buchstabe weg);
     *    4. die Uebertragungen abbauen.  */

    mtx_lock(&sc->sc_mtx);
    sc->sc_weg = 1;
    usbd_transfer_stop(sc->sc_xfer[NTUM_AUS]);
    usbd_transfer_stop(sc->sc_xfer[NTUM_EIN]);
    sc->sc_auftrag.fertig = 1;
    sc->sc_auftrag.fehler = USB_ERR_CANCELLED;
    cv_broadcast(&sc->sc_cv);
    mtx_unlock(&sc->sc_mtx);

    ntu_thread_stop(&sc->sc_faden);

    {
        int k;
        for (k = 0; k < NTUM_MAX_LUN; k += 1) {
            if (sc->sc_e[k].nt != 0) {
                ntum_nt_abbauen(&sc->sc_e[k]);
            }
        }
    }

    usbd_transfer_unsetup(sc->sc_xfer, NTUM_N);

    ntu_free(sc->sc_puffer);
    cv_destroy(&sc->sc_cv);
    mtx_destroy(&sc->sc_mtx);
    return 0;
}

static device_method_t ntum_methods[] = {
    DEVMETHOD(device_probe,  ntum_probe),
    DEVMETHOD(device_attach, ntum_attach),
    DEVMETHOD(device_detach, ntum_detach),
    DEVMETHOD_END
};

static driver_t ntum_driver = {
    .name    = "ntumass",
    .methods = ntum_methods,
    .size    = sizeof(struct ntum_softc),
};

DRIVER_MODULE(ntumass, uhub, ntum_driver, 0, 0);
