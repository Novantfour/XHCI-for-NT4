/*
 *  ober/ntuprn.c -- USB-Drucker.
 *
 *  Nimmt Drucker der USB-Klasse 07 (Unterklasse 01; Protokoll 01 nur
 *  hinaus, 02 in beide Richtungen) und stellt jeden als Geraet bereit:
 *
 *      \\Device\\UsbDruckN   \\DosDevices\\USBPRNn   (n = N + 1, wie LPT1)
 *
 *  Druckdaten gehen per Bulk hinaus -- was an \\.\USBPRN1 geschrieben
 *  wird, kommt beim Drucker an.  Der Spooler schreibt ueber den Monitor
 *  "Lokaler Anschluss" auf einen Anschluss dieses Namens.
 *
 *  Die drei Klassenanfragen der Druckerklasse:
 *      GET_DEVICE_ID    IEEE-1284-Kennung ("MFG:...;MDL:...;CMD:...;")
 *      GET_PORT_STATUS  Papier leer, ausgewaehlt, kein Fehler
 *      SOFT_RESET       Puffer leeren, Endpunkte zuruecksetzen
 *
 *  Ein Drucker ohne Papier nimmt einfach keine Daten mehr an -- die
 *  Uebertragung steht.  Heraus kommt man ueber: CancelIo (Abbruch-
 *  Routine), Schliessen des Griffs (IRP_MJ_CLEANUP, nur die Anfragen DIESES
 *  Dateiobjekts), Abziehen, oder das Zeitlimit je 64-kB-Stueck
 *  (Parameters\SchreibWartezeit in Sekunden, Vorgabe 30, 0 = unbegrenzt).
 *
 *  Nur EIN schreibender Griff: ein zweites Oeffnen mit Schreibzugriff
 *  scheitert mit STATUS_SHARING_VIOLATION -- zwei Auftraege, deren 64-kB-
 *  Stuecke sich abwechseln, waeren Datensalat.  Griffe ohne Schreibzugriff
 *  (fuer Kennung, Status, Info) gehen immer.
 *
 *  Sperrfolge: Abbruch-Sperre (IoAcquireCancelSpinLock) vor x->Sperre.
 *  Die Abbruch-Routine erledigt alles unter der Abbruch-Sperre; so weiss,
 *  wer danach die Abbruch-Sperre bekommt, dass sie fertig ist.  Den KMUTEX
 *  sc_mtx darf sie nicht nehmen (DISPATCH_LEVEL) -- fuer die LAUFENDE
 *  Anfrage setzt sie nur sc_storno, und der wartende Arbeitsfaden stoppt
 *  die Uebertragung selbst (er sieht alle 250 ms nach).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/callout.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/sx.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>

/*  Nur fuer den Anschlusspfad (parent_hub, port_no, bus): FreeBSDs innere
 *  Geraetestruktur -- in derselben Reihenfolge wie usb_hub.c.  */
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#include <ober/ntkbdmou.h>
#include <ober/ntuprn.h>

#define NTUP_PUFFER     65536
#define NTUP_ZEIT_VORGABE 30             /* s je Stueck; Parameters\SchreibWartezeit */
#define NTUP_MAX        8

enum { NTUP_AUS, NTUP_EIN, NTUP_N };

#define NTUP_LESEPUFFER 4096
#define NTUP_LESEZEIT_VORGABE 2000      /* ms; Parameters\\LeseWartezeit */

struct ntup_softc;

typedef struct ntup_erweiterung {
    ntu_port            Port;           /* MUSS vorn stehen */
    KSPIN_LOCK          Sperre;
    struct ntup_softc  *Sc;             /* 0 = abgezogen */
    LIST_ENTRY          Warteschlange;
    LIST_ENTRY          Leseschlange;   /* ReadFile, eigener Faden */
    ULONG               Nummer;
    PFILE_OBJECT        Schreiber;      /* der eine schreibende Griff, 0 = frei */
    int                 Ipp;            /* 1 = \\.\USBIPPn (IPP over USB) */
} ntup_erweiterung;

/*  Zeitgrenzen je Griff (IOCTL_NTUP_ZEIT), an FileObject->FsContext2.  */
#define NTUP_ZEIT_MAGIE  0x5A454954ul          /* "ZEIT" */
typedef struct {
    ULONG Magie;
    ULONG LesenMs;
    ULONG SchreibenMs;
} ntup_griffzeit;

#define NTUP_LAEUFT(irp)  ((irp)->Tail.Overlay.DriverContext[0])

struct ntup_softc {
    int                 sc_ipp;          /* IPP over USB (Protokoll 04) */
    ULONG               sc_akt_lesen_ms;     /* fuer die laufende Anfrage */
    ULONG               sc_akt_schreiben_ms;
    device_t            sc_dev;
    struct usb_device  *sc_udev;
    uint8_t             sc_iface_index;
    uint8_t             sc_iface_no;
    uint8_t             sc_alt;
    struct mtx          sc_mtx;
    struct cv           sc_cv;
    struct usb_xfer    *sc_xfer[NTUP_N];
    ntu_thread          sc_faden;

    /*  Rueckkanal: eigener Faden, eigene Uebertragung -- ein Lesender, der
     *  auf eine Antwort wartet, haelt keinen Druck auf.  */
    int                 sc_hat_ein;
    ntu_thread          sc_lfaden;
    struct cv           sc_lcv;
    uint8_t            *sc_lpuffer;
    uint32_t            sc_llaenge, sc_list;
    int                 sc_lfehler, sc_lfertig, sc_laktiv;
    volatile int        sc_lstorno, sc_labbruch;
    PFILE_OBJECT        sc_akt_lfo;
    uint8_t            *sc_puffer;
    PDEVICE_OBJECT      sc_nt;
    volatile int        sc_weg;
    volatile int        sc_abbruch;     /* CLEANUP: laufendes abbrechen */
    PFILE_OBJECT        sc_akt_fo;      /* zu wem der laufende Auftrag gehoert */
    volatile int        sc_storno;      /* CancelIo auf die laufende Anfrage */
    uint8_t             sc_proto;       /* genutzte Einstellung: 1 oder 2 */

    /* ein Auftrag an die Bulk-Uebertragung */
    uint32_t            sc_laenge, sc_ist;
    int                 sc_fehler, sc_fertig, sc_aktiv;

    char                sc_kennung[128]; /* IEEE-1284, gekuerzt */
    unsigned long       sc_bytes;
};

static LONG       ntup_belegt[2][NTUP_MAX];   /* [0] USBPRN, [1] USBIPP */
static KSPIN_LOCK ntup_sperre;
static int        ntup_bereit;
static ULONG      ntup_wartezeit = NTUP_ZEIT_VORGABE;   /* s, 0 = unbegrenzt */
static ULONG      ntup_lesezeit  = NTUP_LESEZEIT_VORGABE; /* ms, 0 = unbegrenzt */
static LONG       ntup_aenderungen;


/*  Aus DriverEntry: Parameters\SchreibWartezeit (REG_DWORD, Sekunden).  */

VOID
ntup_parameter_lesen (PUNICODE_STRING reg)
{
    WCHAR                    pfad[260];
    RTL_QUERY_REGISTRY_TABLE t[2];
    ULONG                    n = reg->Length / sizeof(WCHAR), wert = 0xFFFFFFFFul;

    if (n >= 240) {
        return;
    }
    memcpy(pfad, reg->Buffer, n * sizeof(WCHAR));
    memcpy(pfad + n, L"\\Parameters", 12 * sizeof(WCHAR));
    pfad[n + 11] = 0;

    memset(t, 0, sizeof(t));
    t[0].Flags        = RTL_QUERY_REGISTRY_DIRECT;
    t[0].Name         = L"SchreibWartezeit";
    t[0].EntryContext = &wert;

    if (NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, pfad, t, 0, 0)) &&
        wert != 0xFFFFFFFFul) {
        ntup_wartezeit = (wert > 86400) ? 86400 : wert;
        ntu_printf("uprn: SchreibWartezeit = %lu s%s\n", ntup_wartezeit,
                   ntup_wartezeit ? "" : " (unbegrenzt)");
    }

    /*  XhciSchritt: Intel-Schrittmodus des xHCI (ober/ntxhcisonder.c).  */

    wert = 0xFFFFFFFFul;
    t[0].Name = L"XhciSchritt";
    if (NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, pfad, t, 0, 0)) &&
        wert != 0xFFFFFFFFul) {
        ntu_xhci_schritt = wert;
    }

    /*  LeseWartezeit (ms): so lange wartet ReadFile auf Antwort des
     *  Druckers; danach kehrt es mit 0 Byte zurueck.  */

    wert = 0xFFFFFFFFul;
    t[0].Name = L"LeseWartezeit";
    if (NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, pfad, t, 0, 0)) &&
        wert != 0xFFFFFFFFul) {
        ntup_lesezeit = (wert > 3600000) ? 3600000 : wert;
        ntu_printf("uprn: LeseWartezeit = %lu ms%s\n", ntup_lesezeit,
                   ntup_lesezeit ? "" : " (unbegrenzt)");
    }
}


/* ====================================================================
 *  Bulk hinaus
 * ==================================================================== */

static void
ntup_rueckruf (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntup_softc     *sc = usbd_xfer_softc(xfer);
    struct usb_page_cache *pc;
    int                    ist;

    usbd_xfer_status(xfer, &ist, NULL, NULL, NULL);

    switch (USB_GET_STATE(xfer)) {

        case USB_ST_SETUP:
            if (!sc->sc_aktiv) {
                return;
            }
            pc = usbd_xfer_get_frame(xfer, 0);
            usbd_copy_in(pc, 0, sc->sc_puffer, sc->sc_laenge);
            usbd_xfer_set_frame_len(xfer, 0, sc->sc_laenge);
            usbd_transfer_submit(xfer);
            break;

        case USB_ST_TRANSFERRED:
            sc->sc_ist    = (uint32_t)ist;
            sc->sc_fehler = 0;
            sc->sc_fertig = 1;
            cv_signal(&sc->sc_cv);
            break;

        default:

            /*  Nach JEDEM Fehler ausser einem gewollten Abbruch den
             *  Endpunkt zuruecksetzen -- beim naechsten Start schickt der
             *  Stapel CLEAR_FEATURE(ENDPOINT_HALT), und Endpunkt und
             *  Datenumschaltung sind wieder im Takt.  So macht es FreeBSDs
             *  ulpt.  Die erste Fassung tat das nur bei STALL: nach einer
             *  Zeitueberschreitung kamen beim naechsten Druck noch zwei
             *  Stuecke an, dann stand der Endpunkt.  */

            if (error != USB_ERR_CANCELLED) {
                usbd_xfer_set_stall(xfer);
            }
            sc->sc_ist    = (uint32_t)ist;
            sc->sc_fehler = (int)error;
            sc->sc_fertig = 1;
            cv_signal(&sc->sc_cv);
            break;
    }
}

/*  Ein Stueck (bis 64 kB) aus sc_puffer hinaus.  0 = gut.
 *
 *  Gewartet wird in Schritten von 250 ms: zwischendurch nachsehen, ob
 *  CancelIo die laufende Anfrage abgebrochen hat (sc_storno) -- die
 *  Abbruch-Routine darf den KMUTEX nicht nehmen, also stoppt der Faden
 *  hier selbst.  */

/*  Die Zeitgrenzen dieses Griffs (IOCTL_NTUP_ZEIT), sonst die Vorgaben.  */

static void
ntup_zeiten (PFILE_OBJECT fo, ULONG *lesen, ULONG *schreiben)
{
    ntup_griffzeit *g = (fo != 0) ? (ntup_griffzeit *)fo->FsContext2 : 0;

    *lesen     = ntup_lesezeit;
    *schreiben = ntup_wartezeit * 1000;
    if (g != 0 && g->Magie == NTUP_ZEIT_MAGIE) {
        *lesen     = g->LesenMs;
        *schreiben = g->SchreibenMs;
    }
}

static int
ntup_senden (struct ntup_softc *sc, uint32_t n, uint32_t *ist)
{
    struct usb_xfer *x = sc->sc_xfer[NTUP_AUS];
    int              gestoppt = 0;

    mtx_lock(&sc->sc_mtx);

    if (sc->sc_weg || sc->sc_abbruch || sc->sc_storno) {
        mtx_unlock(&sc->sc_mtx);
        return USB_ERR_CANCELLED;
    }

    sc->sc_laenge = n;
    sc->sc_ist    = 0;
    sc->sc_fertig = 0;
    sc->sc_fehler = 0;
    sc->sc_aktiv  = 1;
    usbd_xfer_set_timeout(x, (int)sc->sc_akt_schreiben_ms);   /* 0 = keine Grenze */
    usbd_transfer_start(x);

    while (!sc->sc_fertig) {
        cv_timedwait(&sc->sc_cv, &sc->sc_mtx, hz / 4);
        if (sc->sc_storno && !gestoppt && !sc->sc_fertig) {
            gestoppt = 1;
            usbd_transfer_stop(x);
            if (!sc->sc_fertig) {
                sc->sc_fertig = 1;
                sc->sc_fehler = USB_ERR_CANCELLED;
            }
        }
    }

    sc->sc_aktiv = 0;
    *ist = sc->sc_ist;
    mtx_unlock(&sc->sc_mtx);
    return sc->sc_fehler;
}

/*  Laufendes abbrechen -- aus CLEANUP und detach.  */

static void
ntup_abbrechen (struct ntup_softc *sc)
{
    mtx_lock(&sc->sc_mtx);
    sc->sc_abbruch = 1;
    usbd_transfer_stop(sc->sc_xfer[NTUP_AUS]);
    if (sc->sc_aktiv && !sc->sc_fertig) {
        sc->sc_fertig = 1;
        sc->sc_fehler = USB_ERR_CANCELLED;
        cv_broadcast(&sc->sc_cv);
    }
    mtx_unlock(&sc->sc_mtx);
}


/* ====================================================================
 *  Bulk herein: der Rueckkanal (nur Protokoll 02)
 * ==================================================================== */

static void
ntup_ein_rueckruf (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntup_softc     *sc = usbd_xfer_softc(xfer);
    struct usb_page_cache *pc;
    int                    ist;

    usbd_xfer_status(xfer, &ist, NULL, NULL, NULL);

    switch (USB_GET_STATE(xfer)) {

        case USB_ST_SETUP:
            if (!sc->sc_laktiv) {
                return;
            }
            usbd_xfer_set_frame_len(xfer, 0, sc->sc_llaenge);
            usbd_transfer_submit(xfer);
            break;

        case USB_ST_TRANSFERRED:
            if (ist > 0) {
                pc = usbd_xfer_get_frame(xfer, 0);
                usbd_copy_out(pc, 0, sc->sc_lpuffer, ist);
            }
            sc->sc_list    = (uint32_t)ist;
            sc->sc_lfehler = 0;
            sc->sc_lfertig = 1;
            cv_signal(&sc->sc_lcv);
            break;

        default:

            /*  Keine Antwort in der Lesezeit ist beim Rueckkanal der
             *  Normalfall, kein Fehler -- dann den Endpunkt nicht
             *  zuruecksetzen.  Andere Fehler: zuruecksetzen wie beim
             *  Schreiben.  */

            if (error != USB_ERR_CANCELLED && error != USB_ERR_TIMEOUT) {
                usbd_xfer_set_stall(xfer);
            }
            sc->sc_list    = 0;
            sc->sc_lfehler = (int)error;
            sc->sc_lfertig = 1;
            cv_signal(&sc->sc_lcv);
            break;
    }
}

static int
ntup_lesen_stueck (struct ntup_softc *sc, uint32_t n, uint32_t *ist)
{
    struct usb_xfer *x = sc->sc_xfer[NTUP_EIN];
    int              gestoppt = 0;

    mtx_lock(&sc->sc_mtx);

    if (sc->sc_weg || sc->sc_labbruch || sc->sc_lstorno) {
        mtx_unlock(&sc->sc_mtx);
        return USB_ERR_CANCELLED;
    }

    sc->sc_llaenge = n;
    sc->sc_list    = 0;
    sc->sc_lfertig = 0;
    sc->sc_lfehler = 0;
    sc->sc_laktiv  = 1;
    usbd_xfer_set_timeout(x, (int)sc->sc_akt_lesen_ms);
    usbd_transfer_start(x);

    while (!sc->sc_lfertig) {
        cv_timedwait(&sc->sc_lcv, &sc->sc_mtx, hz / 4);
        if (sc->sc_lstorno && !gestoppt && !sc->sc_lfertig) {
            gestoppt = 1;
            usbd_transfer_stop(x);
            if (!sc->sc_lfertig) {
                sc->sc_lfertig = 1;
                sc->sc_lfehler = USB_ERR_CANCELLED;
            }
        }
    }

    sc->sc_laktiv = 0;
    *ist = sc->sc_list;
    mtx_unlock(&sc->sc_mtx);
    return sc->sc_lfehler;
}

static void
ntup_lesen_abbrechen (struct ntup_softc *sc)
{
    if (!sc->sc_hat_ein) {
        return;
    }
    mtx_lock(&sc->sc_mtx);
    sc->sc_labbruch = 1;
    usbd_transfer_stop(sc->sc_xfer[NTUP_EIN]);
    if (sc->sc_laktiv && !sc->sc_lfertig) {
        sc->sc_lfertig = 1;
        sc->sc_lfehler = USB_ERR_CANCELLED;
        cv_broadcast(&sc->sc_lcv);
    }
    mtx_unlock(&sc->sc_mtx);
}


/* ====================================================================
 *  Klassenanfragen
 * ==================================================================== */

static int
ntup_klasse (struct ntup_softc *sc, uint8_t anfrage, uint16_t wert,
             uint16_t index, void *daten, uint16_t laenge, int ein)
{
    struct usb_device_request r;
    uint16_t                  ist = 0;

    r.bmRequestType = ein ? UT_READ_CLASS_INTERFACE : UT_WRITE_CLASS_INTERFACE;
    r.bRequest      = anfrage;
    USETW(r.wValue,  wert);
    USETW(r.wIndex,  index);
    USETW(r.wLength, laenge);

    if (usbd_do_request_flags(sc->sc_udev, NULL, &r, daten,
                              USB_SHORT_XFER_OK, &ist, 2000) != 0) {
        return -1;
    }
    return (int)ist;
}

/*  GET_DEVICE_ID: 2 Byte Laenge (grosses Ende, schliesst sich selbst
 *  ein), dann die Kennung.  wIndex = Schnittstelle << 8 | Alternative.  */

static int
ntup_kennung (struct ntup_softc *sc, char *ziel, int max)
{
    static uint8_t b[1024];
    int            n, k;

    n = ntup_klasse(sc, 0, 0, (uint16_t)((sc->sc_iface_no << 8) | sc->sc_alt),
                    b, sizeof(b), 1);
    if (n < 2) {
        return 0;
    }

    k = (b[0] << 8) | b[1];
    if (k > n) k = n;
    k -= 2;
    if (k > max - 1) k = max - 1;
    if (k < 0) k = 0;

    memcpy(ziel, b + 2, (size_t)k);
    ziel[k] = '\0';
    return k;
}


/* ====================================================================
 *  Anfragen
 * ==================================================================== */

static NTSTATUS
ntup_fertig (PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status      = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/*  Abbruch-Routine (CancelIo, Thread-Ende).  Laeuft mit der Abbruch-
 *  Sperre und erledigt alles darunter -- siehe Kopf.  */

static VOID
ntup_storno (PDEVICE_OBJECT dev, PIRP irp)
{
    ntup_erweiterung *x = (ntup_erweiterung *)dev->DeviceExtension;
    int               wartend;

    KeAcquireSpinLockAtDpcLevel(&x->Sperre);
    wartend = (NTUP_LAEUFT(irp) == 0);
    if (wartend) {
        RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    } else if (x->Sc != 0) {
        if (NTUP_LAEUFT(irp) == (PVOID)2) x->Sc->sc_lstorno = 1;   /* Lesen */
        else                              x->Sc->sc_storno  = 1;   /* Schreiben */
    }
    KeReleaseSpinLockFromDpcLevel(&x->Sperre);
    IoReleaseCancelSpinLock(irp->CancelIrql);

    if (wartend) {
        ntup_fertig(irp, STATUS_CANCELLED, 0);
    }
}

/*  Wartende Anfragen herausnehmen und zurueckgeben -- alle (fo == 0) oder
 *  nur die eines Dateiobjekts.  */

static void
ntup_zurueck (ntup_erweiterung *x, PFILE_OBJECT fo, NTSTATUS st)
{
    LIST_ENTRY  weg;
    PLIST_ENTRY e, n;
    KIRQL       alt;

    InitializeListHead(&weg);

    IoAcquireCancelSpinLock(&alt);
    KeAcquireSpinLockAtDpcLevel(&x->Sperre);
    {
        PLIST_ENTRY kopf[2];
        int         k;
        kopf[0] = &x->Warteschlange;
        kopf[1] = &x->Leseschlange;
        for (k = 0; k < 2; k += 1) {
            for (e = kopf[k]->Flink; e != kopf[k]; e = n) {
                PIRP w = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
                n = e->Flink;
                if (fo == 0 || IoGetCurrentIrpStackLocation(w)->FileObject == fo) {
                    RemoveEntryList(e);
                    IoSetCancelRoutine(w, NULL);
                    InsertTailList(&weg, e);
                }
            }
        }
    }
    KeReleaseSpinLockFromDpcLevel(&x->Sperre);
    IoReleaseCancelSpinLock(alt);

    while (!IsListEmpty(&weg)) {
        e = RemoveHeadList(&weg);
        ntup_fertig(CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry), st, 0);
    }
}

static void
ntup_alle_zurueck (ntup_erweiterung *x, NTSTATUS st)
{
    ntup_zurueck(x, 0, st);
}

NTSTATUS
ntup_irp (PDEVICE_OBJECT dev, PIRP irp)
{
    ntup_erweiterung  *x  = (ntup_erweiterung *)dev->DeviceExtension;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    KIRQL              alt;

    switch (sl->MajorFunction) {

        case IRP_MJ_CREATE: {

            /*  Nur ein schreibender Griff.  Ohne Schreibzugriff (fuer
             *  Kennung, Status, Info) immer.  */

            ACCESS_MASK will = (sl->Parameters.Create.SecurityContext != 0)
                             ? sl->Parameters.Create.SecurityContext->DesiredAccess : 0;
            NTSTATUS    st   = STATUS_SUCCESS;

            if (will & (FILE_WRITE_DATA | FILE_APPEND_DATA)) {
                KeAcquireSpinLock(&x->Sperre, &alt);
                if (x->Schreiber != 0) {
                    st = STATUS_SHARING_VIOLATION;
                } else {
                    x->Schreiber = sl->FileObject;
                }
                KeReleaseSpinLock(&x->Sperre, alt);
            }
            return ntup_fertig(irp, st, 0);
        }

        case IRP_MJ_CLOSE: {
            ntup_griffzeit *g = (ntup_griffzeit *)sl->FileObject->FsContext2;
            if (g != 0 && g->Magie == NTUP_ZEIT_MAGIE) {
                sl->FileObject->FsContext2 = 0;
                g->Magie = 0;
                ExFreePool(g);
            }
            return ntup_fertig(irp, STATUS_SUCCESS, 0);
        }

        case IRP_MJ_WRITE:
            IoAcquireCancelSpinLock(&alt);
            if (irp->Cancel) {
                IoReleaseCancelSpinLock(alt);
                return ntup_fertig(irp, STATUS_CANCELLED, 0);
            }
            KeAcquireSpinLockAtDpcLevel(&x->Sperre);
            if (x->Sc == 0) {
                KeReleaseSpinLockFromDpcLevel(&x->Sperre);
                IoReleaseCancelSpinLock(alt);
                return ntup_fertig(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
            }
            IoMarkIrpPending(irp);
            NTUP_LAEUFT(irp) = 0;
            IoSetCancelRoutine(irp, ntup_storno);
            InsertTailList(&x->Warteschlange, &irp->Tail.Overlay.ListEntry);
            KeSetEvent(&x->Sc->sc_faden.Wake, IO_NO_INCREMENT, FALSE);
            KeReleaseSpinLockFromDpcLevel(&x->Sperre);
            IoReleaseCancelSpinLock(alt);
            return STATUS_PENDING;

        case IRP_MJ_READ:

            /*  Rueckkanal: nur bei Protokoll 02 (sonst gibt es keinen
             *  Endpunkt herein).  */

            IoAcquireCancelSpinLock(&alt);
            if (irp->Cancel) {
                IoReleaseCancelSpinLock(alt);
                return ntup_fertig(irp, STATUS_CANCELLED, 0);
            }
            KeAcquireSpinLockAtDpcLevel(&x->Sperre);
            if (x->Sc == 0 || !x->Sc->sc_hat_ein) {
                NTSTATUS st = (x->Sc == 0) ? STATUS_DEVICE_NOT_CONNECTED
                                           : STATUS_INVALID_DEVICE_REQUEST;
                KeReleaseSpinLockFromDpcLevel(&x->Sperre);
                IoReleaseCancelSpinLock(alt);
                return ntup_fertig(irp, st, 0);
            }
            IoMarkIrpPending(irp);
            NTUP_LAEUFT(irp) = 0;
            IoSetCancelRoutine(irp, ntup_storno);
            InsertTailList(&x->Leseschlange, &irp->Tail.Overlay.ListEntry);
            KeSetEvent(&x->Sc->sc_lfaden.Wake, IO_NO_INCREMENT, FALSE);
            KeReleaseSpinLockFromDpcLevel(&x->Sperre);
            IoReleaseCancelSpinLock(alt);
            return STATUS_PENDING;

        case IRP_MJ_CLEANUP: {

            /*  Ein Griff wurde geschlossen.  Abbrechen darf nur, was zu
             *  DIESEM Dateiobjekt gehoert -- die erste Fassung brach alles
             *  ab, und ein Statusprogramm, das kurz nachsah, zerstoerte den
             *  Druck eines anderen.  War es der Schreiber, ist der Platz
             *  danach frei.  */

            PFILE_OBJECT       fo = sl->FileObject;
            struct ntup_softc *sc;

            ntup_zurueck(x, fo, STATUS_CANCELLED);

            KeAcquireSpinLock(&x->Sperre, &alt);
            sc = (x->Sc != 0 && x->Sc->sc_akt_fo == fo) ? x->Sc : 0;
            KeReleaseSpinLock(&x->Sperre, alt);
            if (sc != 0) {
                ntup_abbrechen(sc);
            }
            KeAcquireSpinLock(&x->Sperre, &alt);
            sc = (x->Sc != 0 && x->Sc->sc_akt_lfo == fo) ? x->Sc : 0;
            KeReleaseSpinLock(&x->Sperre, alt);
            if (sc != 0) {
                ntup_lesen_abbrechen(sc);
            }

            KeAcquireSpinLock(&x->Sperre, &alt);
            if (x->Schreiber == fo) {
                x->Schreiber = 0;
            }
            KeReleaseSpinLock(&x->Sperre, alt);
            return ntup_fertig(irp, STATUS_SUCCESS, 0);
        }

        case IRP_MJ_FLUSH_BUFFERS:
            return ntup_fertig(irp, STATUS_SUCCESS, 0);

        default:
            return ntup_fertig(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static void
ntup_schreiben (struct ntup_softc *sc, PIRP irp)
{
    PIO_STACK_LOCATION sl   = IoGetCurrentIrpStackLocation(irp);
    ULONG              lang = sl->Parameters.Write.Length, fertig = 0;
    uint8_t           *mem;
    NTSTATUS           st = STATUS_SUCCESS;
    KIRQL              alt;
    ULONG              unbenutzt;

    ntup_zeiten(sl->FileObject, &unbenutzt, &sc->sc_akt_schreiben_ms);

    /*  Merken, zu wem dieser Auftrag gehoert (fuer CLEANUP); einen
     *  frueheren Abbruch vergessen -- der galt einem anderen Auftrag.  Ist
     *  diese Anfrage schon abgebrochen, bevor sie lief (CancelIo zwischen
     *  Herausnehmen und hier), hat die Abbruch-Routine sc_storno gesetzt --
     *  das bleibt stehen.  */

    mtx_lock(&sc->sc_mtx);
    sc->sc_akt_fo  = sl->FileObject;
    sc->sc_abbruch = 0;
    mtx_unlock(&sc->sc_mtx);

    mem = (lang != 0) ? (uint8_t *)MmGetSystemAddressForMdl(irp->MdlAddress) : 0;
    if (lang != 0 && mem == 0) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        lang = 0;
    }

    while (fertig < lang) {

        uint32_t stueck = (lang - fertig > NTUP_PUFFER) ? NTUP_PUFFER : lang - fertig;
        uint32_t ist = 0;
        int      e;

        memcpy(sc->sc_puffer, mem + fertig, stueck);
        e = ntup_senden(sc, stueck, &ist);
        fertig += ist;

        if (e != 0) {
            st = (e == USB_ERR_CANCELLED) ? (sc->sc_weg ? STATUS_DEVICE_NOT_CONNECTED
                                                         : STATUS_CANCELLED)
               : (e == USB_ERR_TIMEOUT)   ? STATUS_IO_TIMEOUT
                                          : STATUS_IO_DEVICE_ERROR;
            break;
        }
    }

    /*  Abbruch-Routine abmelden.  Unter der Abbruch-Sperre: laeuft sie
     *  gerade, ist sie danach fertig; sonst wird sie nicht mehr gerufen.  */

    IoAcquireCancelSpinLock(&alt);
    IoSetCancelRoutine(irp, NULL);
    IoReleaseCancelSpinLock(alt);

    mtx_lock(&sc->sc_mtx);
    sc->sc_akt_fo = 0;
    sc->sc_storno = 0;
    mtx_unlock(&sc->sc_mtx);

    sc->sc_bytes += fertig;
    ntup_fertig(irp, st, fertig);
}

/*  "Controller-Anschluss.Hub-Anschluss...", z. B. "0-3" oder "0-1.4":
 *  bleibt gleich, solange der Drucker an derselben Buchse steckt -- als
 *  Ersatz, wenn es keine Seriennummer gibt.  */

static void
ntup_zahl (char **z, char *ende, unsigned v)
{
    char t[4];
    int  n = 0;

    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v != 0 && n < 4);
    while (n > 0 && *z < ende) { *(*z)++ = t[--n]; }
}

static void
ntup_pfad (struct usb_device *u, char *ziel, int max)
{
    uint8_t            ports[8];
    int                n = 0;
    struct usb_device *d = u;
    char              *z = ziel, *ende = ziel + max - 1;

    while (d != NULL && d->parent_hub != NULL && n < 8) {
        ports[n++] = d->port_no;
        d = d->parent_hub;
    }

    ntup_zahl(&z, ende, (unsigned)device_get_unit(u->bus->bdev));
    if (z < ende) *z++ = '-';
    while (n > 0) {
        ntup_zahl(&z, ende, ports[--n]);
        if (n > 0 && z < ende) *z++ = '.';
    }
    *z = '\0';
}

NTSTATUS
ntup_ioctl (PDEVICE_OBJECT dev, PIRP irp)
{
    ntup_erweiterung  *x   = (ntup_erweiterung *)dev->DeviceExtension;
    PIO_STACK_LOCATION sl  = IoGetCurrentIrpStackLocation(irp);
    ULONG              aus = sl->Parameters.DeviceIoControl.OutputBufferLength;
    char              *p   = (char *)irp->AssociatedIrp.SystemBuffer;
    struct ntup_softc *sc  = x->Sc;
    int                n;

    /*  Klassenanfragen warten auf den Drucker -- nur bei PASSIVE_LEVEL,
     *  wie sie aus DeviceIoControl kommen.  */

    if (sc == 0) {
        return ntup_fertig(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    }

    switch (sl->Parameters.DeviceIoControl.IoControlCode) {

        case IOCTL_NTUP_KENNUNG:
            n = ntup_kennung(sc, sc->sc_kennung, sizeof(sc->sc_kennung));
            if (aus < (ULONG)n + 1) {
                return ntup_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
            }
            memcpy(p, sc->sc_kennung, (size_t)n + 1);
            return ntup_fertig(irp, STATUS_SUCCESS, (ULONG)n + 1);

        case IOCTL_NTUP_STATUS: {
            uint8_t s = 0;
            if (aus < sizeof(NTUP_STATUS)) {
                return ntup_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
            }
            n = ntup_klasse(sc, 1, 0, sc->sc_iface_no, &s, 1, 1);
            ((NTUP_STATUS *)p)->Gelesen      = (n == 1);
            ((NTUP_STATUS *)p)->PapierLeer   = (s & 0x20) != 0;
            ((NTUP_STATUS *)p)->Ausgewaehlt  = (s & 0x10) != 0;
            ((NTUP_STATUS *)p)->KeinFehler   = (s & 0x08) != 0;
            ((NTUP_STATUS *)p)->Roh          = s;
            ((NTUP_STATUS *)p)->Bytes        = sc->sc_bytes;
            return ntup_fertig(irp, STATUS_SUCCESS, sizeof(NTUP_STATUS));
        }

        case IOCTL_NTUP_INFO: {
            NTUP_INFO                    *i = (NTUP_INFO *)p;
            struct usb_device_descriptor *dd;
            const char                   *sn;
            int                           k;

            if (aus < sizeof(NTUP_INFO)) {
                return ntup_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
            }
            memset(i, 0, sizeof(*i));
            dd = usbd_get_device_descriptor(sc->sc_udev);
            i->Fassung       = NTUP_FASSUNG;
            i->Vid           = UGETW(dd->idVendor);
            i->Pid           = UGETW(dd->idProduct);
            i->BcdDevice     = UGETW(dd->bcdDevice);
            i->Protokoll     = sc->sc_proto;
            i->Schnittstelle = sc->sc_iface_no;
            i->Alternative   = sc->sc_alt;
            i->Rueckkanal    = (unsigned char)(sc->sc_hat_ein != 0);
            i->Aenderungen   = (unsigned long)ntup_aenderungen;
            i->Wartezeit     = ntup_wartezeit;
            sn = usb_get_serial(sc->sc_udev);
            for (k = 0; sn != NULL && sn[k] != '\0' && k < (int)sizeof(i->Seriennummer) - 1; k += 1) {
                i->Seriennummer[k] = sn[k];
            }
            ntup_pfad(sc->sc_udev, i->Pfad, sizeof(i->Pfad));
            return ntup_fertig(irp, STATUS_SUCCESS, sizeof(NTUP_INFO));
        }

        case IOCTL_NTUP_RESET:
        case IOCTL_USBPRINT_SOFT_RESET:
            n = ntup_klasse(sc, 2, 0, sc->sc_iface_no, NULL, 0, 0);
            return ntup_fertig(irp, (n >= 0) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR, 0);

        case IOCTL_USBPRINT_GET_LPT_STATUS: {
            uint8_t s = 0;
            if (aus < 1) {
                return ntup_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
            }
            n = ntup_klasse(sc, 1, 0, sc->sc_iface_no, &s, 1, 1);
            if (n != 1) {
                return ntup_fertig(irp, STATUS_IO_DEVICE_ERROR, 0);
            }
            *(uint8_t *)p = s;                  /* 0x20 Papier leer, 0x10 ausgewaehlt, 0x08 kein Fehler */
            return ntup_fertig(irp, STATUS_SUCCESS, 1);
        }

        case IOCTL_USBPRINT_GET_1284_ID:
            /*  Wie usbprint.sys: 2 Byte Laenge (big endian, einschliesslich
             *  dieser 2 Byte), dann die Kennung ohne Null.  */
            n = ntup_kennung(sc, sc->sc_kennung, sizeof(sc->sc_kennung));
            if (aus < (ULONG)n + 2) {
                return ntup_fertig(irp, STATUS_BUFFER_TOO_SMALL, 0);
            }
            ((uint8_t *)p)[0] = (uint8_t)((n + 2) >> 8);
            ((uint8_t *)p)[1] = (uint8_t)((n + 2) & 0xFF);
            memcpy(p + 2, sc->sc_kennung, (size_t)n);
            return ntup_fertig(irp, STATUS_SUCCESS, (ULONG)n + 2);

        case IOCTL_NTUP_ZEIT: {
            ULONG            ein = sl->Parameters.DeviceIoControl.InputBufferLength;
            PFILE_OBJECT     fo  = sl->FileObject;
            ntup_griffzeit  *g   = (ntup_griffzeit *)fo->FsContext2;
            ULONG            l, w;

            ntup_zeiten(fo, &l, &w);
            if (ein >= sizeof(NTUP_ZEIT)) {
                NTUP_ZEIT *z = (NTUP_ZEIT *)p;
                if (g == 0 || g->Magie != NTUP_ZEIT_MAGIE) {
                    g = (ntup_griffzeit *)ExAllocatePoolWithTag(NonPagedPool, sizeof(*g), 'ZpuN');
                    if (g == 0) {
                        return ntup_fertig(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
                    }
                    g->Magie = NTUP_ZEIT_MAGIE;
                    fo->FsContext2 = g;
                }
                if (z->LesenMs != NTUP_ZEIT_BLEIBT) {
                    l = (z->LesenMs > 3600000) ? 3600000 : z->LesenMs;
                }
                if (z->SchreibenMs != NTUP_ZEIT_BLEIBT) {
                    w = (z->SchreibenMs > 86400000) ? 86400000 : z->SchreibenMs;
                }
                g->LesenMs     = l;
                g->SchreibenMs = w;
            }
            if (aus >= sizeof(NTUP_ZEIT)) {
                ((NTUP_ZEIT *)p)->LesenMs     = l;
                ((NTUP_ZEIT *)p)->SchreibenMs = w;
                return ntup_fertig(irp, STATUS_SUCCESS, sizeof(NTUP_ZEIT));
            }
            return ntup_fertig(irp, STATUS_SUCCESS, 0);
        }
    }

    return ntup_fertig(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}


/* ====================================================================
 *  Arbeitsfaden
 * ==================================================================== */

/*  Eine Leseanfrage: EIN Stueck (bis 4 kB) herein.  Kommt in der
 *  Lesezeit nichts, kehrt sie mit 0 Byte ohne Fehler zurueck -- ein
 *  Statusabfrager haengt so nie.  */

static void
ntup_lesen (struct ntup_softc *sc, PIRP irp)
{
    PIO_STACK_LOCATION sl   = IoGetCurrentIrpStackLocation(irp);
    ULONG              lang = sl->Parameters.Read.Length, info = 0;
    uint8_t           *mem;
    NTSTATUS           st = STATUS_SUCCESS;
    uint32_t           ist = 0;
    KIRQL              alt;
    int                e;

    {
        ULONG unbenutzt;
        ntup_zeiten(sl->FileObject, &sc->sc_akt_lesen_ms, &unbenutzt);
    }
    mtx_lock(&sc->sc_mtx);
    sc->sc_akt_lfo  = sl->FileObject;
    sc->sc_labbruch = 0;
    mtx_unlock(&sc->sc_mtx);

    mem = (lang != 0) ? (uint8_t *)MmGetSystemAddressForMdl(irp->MdlAddress) : 0;

    if (lang != 0 && mem == 0) {
        st = STATUS_INSUFFICIENT_RESOURCES;
    } else if (lang != 0) {
        e = ntup_lesen_stueck(sc, (lang > NTUP_LESEPUFFER) ? NTUP_LESEPUFFER : lang, &ist);
        if (e == 0) {
            memcpy(mem, sc->sc_lpuffer, ist);
            info = ist;
        } else if (e == USB_ERR_TIMEOUT) {
            info = 0;                   /* nichts gekommen: kein Fehler */
        } else {
            st = (e == USB_ERR_CANCELLED) ? (sc->sc_weg ? STATUS_DEVICE_NOT_CONNECTED
                                                         : STATUS_CANCELLED)
                                          : STATUS_IO_DEVICE_ERROR;
        }
    }

    IoAcquireCancelSpinLock(&alt);
    IoSetCancelRoutine(irp, NULL);
    IoReleaseCancelSpinLock(alt);

    mtx_lock(&sc->sc_mtx);
    sc->sc_akt_lfo = 0;
    sc->sc_lstorno = 0;
    mtx_unlock(&sc->sc_mtx);

    ntup_fertig(irp, st, info);
}

static VOID
ntup_lfaden (PVOID ctx)
{
    struct ntup_softc *sc = (struct ntup_softc *)ctx;
    ntup_erweiterung  *x  = (ntup_erweiterung *)sc->sc_nt->DeviceExtension;
    KIRQL              alt;

    ntu_wart_name("uprn-lesen");

    while (!sc->sc_lfaden.Stop) {

        PLIST_ENTRY e;

        KeWaitForSingleObject(&sc->sc_lfaden.Wake, Executive, KernelMode, FALSE, NULL);

        for (;;) {

            PIRP irp;

            IoAcquireCancelSpinLock(&alt);
            KeAcquireSpinLockAtDpcLevel(&x->Sperre);
            e = IsListEmpty(&x->Leseschlange) ? 0 : RemoveHeadList(&x->Leseschlange);
            if (e != 0) {
                NTUP_LAEUFT(CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry)) = (PVOID)2;
            }
            KeReleaseSpinLockFromDpcLevel(&x->Sperre);
            IoReleaseCancelSpinLock(alt);

            if (e == 0) {
                break;
            }
            irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);

            if (sc->sc_lfaden.Stop) {
                IoAcquireCancelSpinLock(&alt);
                IoSetCancelRoutine(irp, NULL);
                IoReleaseCancelSpinLock(alt);
                ntup_fertig(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
                continue;
            }
            ntup_lesen(sc, irp);
        }
    }

    ntup_alle_zurueck(x, STATUS_DEVICE_NOT_CONNECTED);
}

static VOID
ntup_faden (PVOID ctx)
{
    struct ntup_softc *sc = (struct ntup_softc *)ctx;
    ntup_erweiterung  *x  = (ntup_erweiterung *)sc->sc_nt->DeviceExtension;
    KIRQL              alt;

    ntu_wart_name("uprn");

    while (!sc->sc_faden.Stop) {

        PLIST_ENTRY e;

        KeWaitForSingleObject(&sc->sc_faden.Wake, Executive, KernelMode, FALSE, NULL);

        for (;;) {

            PIRP irp;

            /*  Unter Abbruch-Sperre und x->Sperre herausnehmen und als
             *  laufend kennzeichnen: eine Abbruch-Routine, die danach
             *  kommt, setzt nur noch sc_storno.  */

            IoAcquireCancelSpinLock(&alt);
            KeAcquireSpinLockAtDpcLevel(&x->Sperre);
            e = IsListEmpty(&x->Warteschlange) ? 0 : RemoveHeadList(&x->Warteschlange);
            if (e != 0) {
                NTUP_LAEUFT(CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry)) = (PVOID)1;
            }
            KeReleaseSpinLockFromDpcLevel(&x->Sperre);
            IoReleaseCancelSpinLock(alt);

            if (e == 0) {
                break;
            }
            irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);

            /*  Angehalten (abgezogen), waehrend eine Anfrage schon heraus
             *  war: auch die zurueckgeben.  Die erste Fassung brach hier
             *  einfach ab -- die Anfrage wurde nie abgeschlossen.  */

            if (sc->sc_faden.Stop) {
                IoAcquireCancelSpinLock(&alt);
                IoSetCancelRoutine(irp, NULL);
                IoReleaseCancelSpinLock(alt);
                ntup_fertig(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
                continue;
            }
            ntup_schreiben(sc, irp);
        }
    }

    ntup_alle_zurueck(x, STATUS_DEVICE_NOT_CONNECTED);
}


/* ====================================================================
 *  NT-Geraet
 * ==================================================================== */

static void
ntup_namen (WCHAR *d, WCHAR *l, ULONG n, int ipp)
{
    if (ipp) {
        ntu_wsprintf(d, 32, L"\\Device\\UsbIpp%u", n);
        ntu_wsprintf(l, 32, L"\\DosDevices\\USBIPP%u", n + 1);
    } else {
        ntu_wsprintf(d, 32, L"\\Device\\UsbDruck%u", n);
        ntu_wsprintf(l, 32, L"\\DosDevices\\USBPRN%u", n + 1);
    }
}

static int
ntup_nt_anlegen (struct ntup_softc *sc)
{
    WCHAR             dn[32], ln[32];
    UNICODE_STRING    ud, ul;
    PDEVICE_OBJECT    dev = 0;
    ntup_erweiterung *x;
    NTSTATUS          st;
    ULONG             n;
    KIRQL             alt;

    if (!ntup_bereit) {
        KeInitializeSpinLock(&ntup_sperre);
        ntup_bereit = 1;
    }

    for (n = 0; n < NTUP_MAX; n += 1) {
        KeAcquireSpinLock(&ntup_sperre, &alt);
        if (ntup_belegt[sc->sc_ipp][n]) {
            KeReleaseSpinLock(&ntup_sperre, alt);
            continue;
        }
        ntup_belegt[sc->sc_ipp][n] = 1;
        KeReleaseSpinLock(&ntup_sperre, alt);

        ntup_namen(dn, ln, n, sc->sc_ipp);
        RtlInitUnicodeString(&ud, dn);
        st = IoCreateDevice(NtuDriverObject, sizeof(ntup_erweiterung), &ud,
                            FILE_DEVICE_PRINTER, 0, FALSE, &dev);
        if (NT_SUCCESS(st)) {
            break;
        }
        ntup_belegt[sc->sc_ipp][n] = 0;
        if (st != STATUS_OBJECT_NAME_COLLISION) {
            return 0;
        }
    }
    if (n == NTUP_MAX) {
        return 0;
    }

    x = (ntup_erweiterung *)dev->DeviceExtension;
    memset(x, 0, sizeof(*x));
    x->Port.Kind   = NTU_PORT_DRUCK;
    x->Port.Suffix = n;
    x->Nummer      = n;
    x->Ipp         = sc->sc_ipp;
    x->Sc          = sc;
    KeInitializeSpinLock(&x->Sperre);
    InitializeListHead(&x->Warteschlange);
    InitializeListHead(&x->Leseschlange);

    dev->Flags |= DO_DIRECT_IO;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    RtlInitUnicodeString(&ul, ln);
    IoCreateSymbolicLink(&ul, &ud);
    sc->sc_nt = dev;
    return 1;
}

static void
ntup_nt_abbauen (struct ntup_softc *sc)
{
    ntup_erweiterung *x;
    WCHAR             dn[32], ln[32];
    UNICODE_STRING    ul;
    KIRQL             alt;

    if (sc->sc_nt == 0) {
        return;
    }
    x = (ntup_erweiterung *)sc->sc_nt->DeviceExtension;

    KeAcquireSpinLock(&x->Sperre, &alt);
    x->Sc = 0;
    KeReleaseSpinLock(&x->Sperre, alt);

    ntup_namen(dn, ln, x->Nummer, x->Ipp);
    RtlInitUnicodeString(&ul, ln);
    IoDeleteSymbolicLink(&ul);

    ntu_printf("uprn: %s%u abgezogen (%lu Byte %s)\n", x->Ipp ? "USBIPP" : "USBPRN",
               x->Nummer + 1, sc->sc_bytes, x->Ipp ? "gesendet" : "gedruckt");

    ntup_belegt[x->Ipp ? 1 : 0][x->Nummer] = 0;
    IoDeleteDevice(sc->sc_nt);
    sc->sc_nt = 0;
}


/* ====================================================================
 *  probe, attach, detach
 * ==================================================================== */

static void ntup_ein_rueckruf(struct usb_xfer *, usb_error_t);

static const struct usb_config ntup_konfig[NTUP_N] = {
    [NTUP_EIN] = {
        .type      = UE_BULK,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_IN,
        .bufsize   = NTUP_LESEPUFFER,
        .flags     = { .proxy_buffer = 1, .pipe_bof = 1, .short_xfer_ok = 1 },
        .callback  = &ntup_ein_rueckruf,
        .timeout   = 0,                 /* je Uebertragung: LeseWartezeit */
    },
    [NTUP_AUS] = {
        .type      = UE_BULK,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_OUT,
        .bufsize   = NTUP_PUFFER,
        .flags     = { .proxy_buffer = 1, .pipe_bof = 1 },
        .callback  = &ntup_rueckruf,
        .timeout   = 0,                 /* je Uebertragung gesetzt: SchreibWartezeit */
    },
};

/*  Die beste alternative Einstellung dieser Schnittstelle suchen, wie
 *  FreeBSDs ulpt: Protokoll 02 (beide Richtungen) vor 01 (nur hinaus).
 *  Protokoll 03 (IEEE 1284.4, "Dot4") spricht ein eigenes Paketprotokoll;
 *  viele solche Geraete haben aber zusaetzlich eine Einstellung mit 02 --
 *  dann die.  Rueckgabe: 2, 1 oder 0 (keine brauchbare).  */

static int
ntup_suche_einstellung (struct usb_attach_arg *uaa, uint8_t *index,
                        struct usb_interface_descriptor **wahl, int ipp)
{
    struct usb_interface_descriptor *id = usbd_get_interface_descriptor(uaa->iface);
    uint8_t                          k = 0xFF;
    int                              beste = 0;

    while (id != NULL) {
        if (id->bDescriptorType == UDESC_INTERFACE && id->bLength >= sizeof(*id)) {
            int rang;
            if (id->bInterfaceNumber != uaa->info.bIfaceNum) {
                break;
            }
            k += 1;
            if (id->bInterfaceClass != UICLASS_PRINTER || id->bInterfaceSubClass != 0x01) {
                rang = 0;
            } else if (ipp) {
                rang = (id->bInterfaceProtocol == 0x04) ? 4 : 0;
            } else {
                rang = (id->bInterfaceProtocol == 0x02) ? 2
                     : (id->bInterfaceProtocol == 0x01) ? 1 : 0;
            }
            if (rang > beste) {
                beste  = rang;
                *index = k;
                *wahl  = id;
            }
        }
        id = (void *)usb_desc_foreach(usbd_get_config_descriptor(uaa->device), (void *)id);
    }
    return beste;
}

/*  Hat diese Schnittstelle eine Einstellung 07/01/<proto>?  */

static int
ntup_hat (struct usb_attach_arg *uaa, uint8_t proto)
{
    struct usb_config_descriptor    *cd = usbd_get_config_descriptor(uaa->device);
    struct usb_interface_descriptor *id = NULL;

    while ((id = (void *)usb_desc_foreach(cd, (void *)id)) != NULL) {
        if (id->bDescriptorType == UDESC_INTERFACE && id->bLength >= sizeof(*id) &&
            id->bInterfaceNumber == uaa->info.bIfaceNum &&
            id->bInterfaceClass == UICLASS_PRINTER && id->bInterfaceSubClass == 0x01 &&
            id->bInterfaceProtocol == proto) {
            return 1;
        }
    }
    return 0;
}

/*  Die niedrigste Schnittstellennummer des Geraets mit einer Einstellung
 *  07/01/01 oder 02 -- sie bleibt USBPRN.  0xFF: keine.  */

static uint8_t
ntup_erste_klassisch (struct usb_attach_arg *uaa)
{
    struct usb_config_descriptor    *cd = usbd_get_config_descriptor(uaa->device);
    struct usb_interface_descriptor *id = NULL;
    uint8_t                          erste = 0xFF;

    while ((id = (void *)usb_desc_foreach(cd, (void *)id)) != NULL) {
        if (id->bDescriptorType == UDESC_INTERFACE && id->bLength >= sizeof(*id) &&
            id->bInterfaceClass == UICLASS_PRINTER && id->bInterfaceSubClass == 0x01 &&
            (id->bInterfaceProtocol == 0x01 || id->bInterfaceProtocol == 0x02) &&
            id->bInterfaceNumber < erste) {
            erste = id->bInterfaceNumber;
        }
    }
    return erste;
}

/*  IPP over USB oder klassisch?  Eine Schnittstelle mit Einstellung 04
 *  wird USBIPP -- ausser sie ist die ERSTE des Geraets, die auch 01/02
 *  kann: die bleibt USBPRN, damit jedes Geraet einen klassischen
 *  Druckzugang behaelt.  IPP-USB-Geraete haben mindestens zwei
 *  04-Schnittstellen; die uebrigen reichen fuer gleichzeitige Verbindungen.  */

static int
ntup_ist_ipp (struct usb_attach_arg *uaa)
{
    int klassisch = ntup_hat(uaa, 0x01) || ntup_hat(uaa, 0x02);

    if (!ntup_hat(uaa, 0x04)) {
        return 0;
    }
    if (!klassisch) {
        return 1;
    }
    return uaa->info.bIfaceNum != ntup_erste_klassisch(uaa);
}

static int
ntup_probe (device_t dev)
{
    struct usb_attach_arg           *uaa = device_get_ivars(dev);
    struct usb_interface_descriptor *wahl = NULL;
    uint8_t                          index = 0;

    if (uaa->usb_mode != USB_MODE_HOST ||
        uaa->info.bInterfaceClass != UICLASS_PRINTER ||
        uaa->info.bInterfaceSubClass != 0x01) {
        return ENXIO;
    }

    /*  Protokoll 01/02 in irgendeiner Einstellung -- auch wenn die
     *  aktuelle 03 ist -- oder 04 (IPP over USB).  Nur Dot4: bleibt bei
     *  der allgemeinen Bereitstellung.  */

    return ntup_suche_einstellung(uaa, &index, &wahl, ntup_ist_ipp(uaa)) ? BUS_PROBE_DEFAULT : ENXIO;
}

static int
ntup_attach (device_t dev)
{
    struct ntup_softc     *sc  = device_get_softc(dev);
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    int                    err;
    struct usb_interface_descriptor *wahl = NULL;
    uint8_t                          index = 0;

    sc->sc_dev         = dev;
    sc->sc_udev        = uaa->device;
    sc->sc_iface_index = uaa->info.bIfaceIndex;
    sc->sc_iface_no    = uaa->info.bIfaceNum;

    sc->sc_ipp   = ntup_ist_ipp(uaa);
    sc->sc_proto = (uint8_t)ntup_suche_einstellung(uaa, &index, &wahl, sc->sc_ipp);
    if (sc->sc_proto == 0 || wahl == NULL) {
        return ENXIO;
    }
    if (index != 0 &&
        usbd_set_alt_interface_index(uaa->device, sc->sc_iface_index, index) != 0) {
        device_printf(dev, "Einstellung %u nicht setzbar\n", index);
        return ENXIO;
    }
    sc->sc_alt = wahl->bAlternateSetting;

    device_set_desc(dev, sc->sc_ipp ? "USB-Drucker (IPP over USB)" : "USB-Drucker");
    mtx_init(&sc->sc_mtx, "ntuprn", NULL, MTX_DEF);
    cv_init(&sc->sc_cv, "ntuprn");
    cv_init(&sc->sc_lcv, "ntuprn-l");   /* vor jedem "goto weg" */

    sc->sc_puffer = (uint8_t *)ntu_malloc(NTUP_PUFFER, 0);
    if (sc->sc_puffer == 0) {
        goto weg;
    }

    /*  Rueckkanal nur bei Protokoll 02 -- bei 01 gibt es keinen Endpunkt
     *  herein, dann nur die Uebertragung hinaus einrichten.  */

    sc->sc_hat_ein = (sc->sc_proto == 2 || sc->sc_proto == 4);
    if (sc->sc_hat_ein) {
        sc->sc_lpuffer = (uint8_t *)ntu_malloc(NTUP_LESEPUFFER, 0);
        if (sc->sc_lpuffer == 0) {
            goto weg;
        }
    }

    err = usbd_transfer_setup(sc->sc_udev, &sc->sc_iface_index, sc->sc_xfer,
                              ntup_konfig, sc->sc_hat_ein ? NTUP_N : 1, sc, &sc->sc_mtx);
    if (err != 0 && sc->sc_hat_ein && !sc->sc_ipp) {

        /*  Protokoll 02, aber kein Endpunkt herein: ohne Rueckkanal.  */

        sc->sc_hat_ein = 0;
        err = usbd_transfer_setup(sc->sc_udev, &sc->sc_iface_index, sc->sc_xfer,
                                  ntup_konfig, 1, sc, &sc->sc_mtx);
    }
    if (err != 0) {
        device_printf(dev, "usbd_transfer_setup: %s\n", usbd_errstr(err));
        goto weg;
    }

    if (!ntup_nt_anlegen(sc)) {
        usbd_transfer_unsetup(sc->sc_xfer, NTUP_N);
        goto weg;
    }

    if (sc->sc_ipp) {

        /*  IPP over USB: das Geraet meldet sich per IPP; die 1284-Kennung
         *  braucht es dafuer nicht (und nicht jedes Geraet beantwortet
         *  GET_DEVICE_ID auf einer 04-Schnittstelle).  */

        ntu_printf("uprn: USBIPP%u = Schnittstelle %u (IPP over USB)\n",
                   ((ntup_erweiterung *)sc->sc_nt->DeviceExtension)->Nummer + 1,
                   sc->sc_iface_no);
    } else {
        ntup_kennung(sc, sc->sc_kennung, sizeof(sc->sc_kennung));
        ntu_printf("uprn: USBPRN%u = %s\n",
                   ((ntup_erweiterung *)sc->sc_nt->DeviceExtension)->Nummer + 1,
                   sc->sc_kennung[0] ? sc->sc_kennung : "(keine Kennung)");
    }
    if (!sc->sc_ipp && (sc->sc_alt != 0 || sc->sc_proto != 2)) {
        ntu_printf("uprn:   Einstellung %u, Protokoll %02u\n", sc->sc_alt, sc->sc_proto);
    }
    InterlockedIncrement(&ntup_aenderungen);

    if (!NT_SUCCESS(ntu_thread_start(&sc->sc_faden, ntup_faden, sc, "uprn"))) {
        ntup_nt_abbauen(sc);
        usbd_transfer_unsetup(sc->sc_xfer, NTUP_N);
        goto weg;
    }
    if (sc->sc_hat_ein &&
        !NT_SUCCESS(ntu_thread_start(&sc->sc_lfaden, ntup_lfaden, sc, "uprn-lesen"))) {
        sc->sc_hat_ein = 0;             /* ohne Rueckkanal weiter */
    }

    return 0;

weg:
    if (sc->sc_puffer != 0) {
        ntu_free(sc->sc_puffer);
    }
    if (sc->sc_lpuffer != 0) {
        ntu_free(sc->sc_lpuffer);
    }
    cv_destroy(&sc->sc_lcv);
    cv_destroy(&sc->sc_cv);
    mtx_destroy(&sc->sc_mtx);
    return ENXIO;
}

static int
ntup_detach (device_t dev)
{
    struct ntup_softc *sc = device_get_softc(dev);

    /*  Wie beim Speicher: erst als weg markieren und Laufendes abbrechen,
     *  dann den Faden anhalten (er gibt Wartendes als "nicht verbunden"
     *  zurueck), dann das NT-Geraet, dann die Uebertragung.  */

    mtx_lock(&sc->sc_mtx);
    sc->sc_weg = 1;
    mtx_unlock(&sc->sc_mtx);
    ntup_abbrechen(sc);
    ntup_lesen_abbrechen(sc);

    if (sc->sc_hat_ein) {
        ntu_thread_stop(&sc->sc_lfaden);
    }
    ntu_thread_stop(&sc->sc_faden);
    ntup_nt_abbauen(sc);
    InterlockedIncrement(&ntup_aenderungen);
    usbd_transfer_unsetup(sc->sc_xfer, NTUP_N);

    ntu_free(sc->sc_puffer);
    if (sc->sc_lpuffer != 0) {
        ntu_free(sc->sc_lpuffer);
    }
    cv_destroy(&sc->sc_lcv);
    cv_destroy(&sc->sc_cv);
    mtx_destroy(&sc->sc_mtx);
    return 0;
}

static device_method_t ntup_methods[] = {
    DEVMETHOD(device_probe,  ntup_probe),
    DEVMETHOD(device_attach, ntup_attach),
    DEVMETHOD(device_detach, ntup_detach),
    DEVMETHOD_END
};

static driver_t ntup_driver = {
    .name    = "ntuprn",
    .methods = ntup_methods,
    .size    = sizeof(struct ntup_softc),
};

DRIVER_MODULE(ntuprn, uhub, ntup_driver, 0, 0);
