/*
 *  ober/ntugen.c -- jedes USB-Geraet bereitstellen.
 *
 *  Tastatur und Maus haben ihren festen Weg zu kbdclass und mouclass.
 *  Alles andere -- Joystick, Gamepad, Drucker, Scanner, Kartenleser,
 *  serieller Wandler, Massenspeicher -- landet hier und bekommt ein
 *  eigenes NT-Geraet:
 *
 *      \Device\UsbDevN          \DosDevices\UsbDevN
 *      HKLM\HARDWARE\DEVICEMAP\USB\UsbDevN = "VID_...&PID_...&MI_.."
 *
 *  mit der Schnittstelle aus ober/ntusbgen.h.  Was ein Geraet BEDEUTET,
 *  weiss dieser Code nicht und soll es nicht wissen; das entscheidet der
 *  Treiber darueber.  Der Unterschied zu FreeBSDs ugen ist nur, dass
 *  dort ein Zeichengeraet mit read/write/poll steht und hier ein
 *  NT-Geraet mit sechs Steueraufrufen.
 *
 *  Gebunden wird je SCHNITTSTELLE, nicht je Geraet.  Ein
 *  zusammengesetztes Geraet -- Tastatur mit Medientasten, Drucker mit
 *  Scanner -- bekommt so fuer jeden Teil einen eigenen Eintrag, und
 *  der Tastaturteil geht trotzdem an ukbd.
 *
 *  Die Gueteziffer im probe ist die niedrigste, die es gibt.  Jeder
 *  andere Treiber, der sich fuer eine Schnittstelle meldet, gewinnt;
 *  hier landet nur, was sonst niemand nimmt.  FreeBSD nennt diesen
 *  Wert BUS_PROBE_HOOVER -- der Staubsauger.
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
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_request.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_endian.h>
#include <dev/usb/usbhid.h>
#include <ober/ntkbdmou.h>
#include <ober/ntusbioctl.h>
#include <ober/ntusbgen.h>

#define NTUG_ZEIT_VORGABE   5000

/*  Die oeffentlichen Fehlerwerte muessen die des Stapels sein -- ein
 *  Treiber darueber vergleicht gegen die Konstanten in ntusbgen.h, und
 *  diese Datei reicht usb_error_t unveraendert durch.  */
CTASSERT(NTUG_FEHLER_INVAL == USB_ERR_INVAL);
CTASSERT(NTUG_FEHLER_IO    == USB_ERR_IOERROR);
CTASSERT(NTUG_FEHLER_ZEIT  == USB_ERR_TIMEOUT);
CTASSERT(NTUG_FEHLER_KURZ  == USB_ERR_SHORT_XFER);
CTASSERT(NTUG_FEHLER_STALL == USB_ERR_STALLED);
CTASSERT(NTUG_MAX_ISO_RAHMEN <= USB_MAX_FS_ISOC_FRAMES_PER_XFER);

/*  wcslen ist ein NTOSKRNL-Export (gegen den Kern der VM geprueft), steht
 *  aber nur in der string.h der Laufzeitbibliothek, die hier nicht
 *  eingebunden wird.  */
size_t __cdecl wcslen(const unsigned short *);
#define NTUG_MAX_GERAETE    64


/* ====================================================================
 *  Zustand
 *
 *  Zwei Lebensdauern, die nicht zusammenfallen, und das ist der ganze
 *  Grund fuer die Bruecke:
 *
 *      softc     lebt, solange das Geraet steckt.  Beim Abziehen gibt
 *                FreeBSD es frei.
 *      NT-Geraet lebt, solange ein Griff offen ist.  Ein Treiber, der
 *                gerade liest, haelt es fest -- auch ueber das Abziehen
 *                hinaus.
 *
 *  Die Bruecke liegt deshalb in der Geraeteerweiterung, also im
 *  Speicher, den NT mit dem Geraeteobjekt verwaltet.  Sie zeigt auf das
 *  softc, solange es eines gibt; beim Abziehen wird der Zeiger unter
 *  ihrer Sperre geloescht.  Jeder Aufruf nimmt dieselbe Sperre und
 *  prueft ihn zuerst.
 * ==================================================================== */

struct ntug_softc;

typedef struct ntug_erweiterung {
    ntu_port            Port;        /* MUSS vorn stehen: die gemeinsame
                                        Verteilung liest Port.Kind   */
    KMUTEX              Sperre;
    struct ntug_softc  *Sc;          /* 0 = abgezogen */
    ULONG               Nummer;      /* N in UsbDevN */
} ntug_erweiterung;

typedef struct ntug_auftrag {
    uint8_t   *daten;
    uint32_t   laenge;
    uint32_t   ist;
    int        fehler;
    int        fertig;
    int        raus;                 /* 1 = OUT */

    /*  Nur isochron: eine Laenge je Rahmen statt einer fuer alles.  */
    uint32_t   rahmen;               /* 0 = kein isochroner Auftrag */
    uint32_t  *laengen;              /* OUT: gewuenscht, je Rahmen   */
    uint32_t  *ist_je;               /* tatsaechlich, je Rahmen      */
    uint32_t   mfl;                  /* Rahmenschrittweite, vom Stapel */
} ntug_auftrag;

struct ntug_softc {
    device_t              sc_dev;
    struct usb_device    *sc_udev;
    uint8_t               sc_iface_index;
    uint8_t               sc_iface_no;
    struct mtx            sc_mtx;
    struct cv             sc_cv;
    ntug_auftrag         *sc_auftrag;
    PDEVICE_OBJECT        sc_nt;
    ULONG                 sc_nummer;

    /*  Wann auf welchem isochronen Endpunkt zuletzt uebertragen wurde,
     *  in ticks; 0 = noch nie.  Index: Endpunktnummer, +16 fuer IN.  */
    int                   sc_iso_zeit[32];

    /*  Gehaltene Uebertragungen, eine je Endpunkt.  Siehe ntug_holen.  */
    struct {
        uint8_t           adresse;
        uint8_t           amdpll;   /* 1 = hat die PLL-Abhilfe eingeschaltet */
        struct usb_xfer  *x;
    }                     sc_gehalten[NTUG_MAX_EP];
};

static LONG       ntug_belegt[NTUG_MAX_GERAETE];
static KSPIN_LOCK  ntug_belegt_sperre;
static int         ntug_belegt_init;


/* ====================================================================
 *  Kleinkram fuer Namen
 * ==================================================================== */

static VOID
ntug_hex (PWCHAR z, ULONG wert, ULONG stellen)
{
    static const WCHAR h[] = L"0123456789ABCDEF";
    ULONG i;

    for (i = 0; i < stellen; i += 1) {
        z[i] = h[(wert >> ((stellen - 1 - i) * 4)) & 0xF];
    }
}

static VOID
ntug_kennung (PWCHAR z, ULONG max, struct ntug_softc *sc)
{
    /*  "VID_046D&PID_C215&MI_00 03/00/00"  -- Hersteller, Produkt,
     *  Schnittstellennummer, dann Klasse/Unterklasse/Protokoll der
     *  Schnittstelle.  Dieselbe Form, die Windows 2000 fuer
     *  USB-Geraete-IDs benutzt; ein Treiberschreiber kennt sie.  */

    struct usb_device_descriptor    *dd;
    struct usb_interface            *ifc;
    struct usb_interface_descriptor *id;
    PCWSTR muster = L"VID_xxxx&PID_xxxx&MI_xx xx/xx/xx";
    ULONG  i;

    for (i = 0; muster[i] != 0 && i + 1 < max; i += 1) {
        z[i] = muster[i];
    }

    z[i] = 0;

    if (max < 34) {
        return;
    }

    dd  = usbd_get_device_descriptor(sc->sc_udev);
    ifc = usbd_get_iface(sc->sc_udev, sc->sc_iface_index);
    id  = (ifc != 0) ? usbd_get_interface_descriptor(ifc) : 0;

    ntug_hex(z + 4,  UGETW(dd->idVendor),  4);
    ntug_hex(z + 13, UGETW(dd->idProduct), 4);
    ntug_hex(z + 21, sc->sc_iface_no, 2);

    if (id != 0) {
        ntug_hex(z + 24, id->bInterfaceClass,    2);
        ntug_hex(z + 27, id->bInterfaceSubClass, 2);
        ntug_hex(z + 30, id->bInterfaceProtocol, 2);
    }
}


/* ====================================================================
 *  Das NT-Geraet
 * ==================================================================== */

static int
ntug_nt_anlegen (struct ntug_softc *sc)
{
    WCHAR            dname[32], lname[40], text[40], basis[8];
    UNICODE_STRING   un, ul;
    PDEVICE_OBJECT   dev = 0;
    ntug_erweiterung *x;
    NTSTATUS         st;
    ULONG            n;

    /*  Kleinste freie Nummer.  Anders als bei KeyboardPort zaehlt hier
     *  niemand -- ein Treiber sucht im DEVICEMAP nach seiner Kennung --,
     *  also sind Luecken nach dem Abziehen harmlos.  Die kleinste freie
     *  zu nehmen haelt die Namen trotzdem kurz und stabil.  */

    {
        KIRQL alt;

        /*  Aufgezaehlt wird je Bus in einem eigenen Faden; bei zwei
         *  Hostbausteinen laufen also zwei attach gleichzeitig.  Die
         *  Nummernvergabe muss darum gesperrt sein.  Die Sperre selbst
         *  wird beim ersten Mal angelegt -- DriverEntry laeuft vor jedem
         *  attach, also ohne Wettlauf.  */

        if (!ntug_belegt_init) {
            KeInitializeSpinLock(&ntug_belegt_sperre);
            ntug_belegt_init = 1;
        }

        KeAcquireSpinLock(&ntug_belegt_sperre, &alt);

        for (n = 0; n < NTUG_MAX_GERAETE; n += 1) {
            if (ntug_belegt[n] == 0) {
                ntug_belegt[n] = 1;
                break;
            }
        }

        KeReleaseSpinLock(&ntug_belegt_sperre, alt);
    }

    if (n == NTUG_MAX_GERAETE) {
        device_printf(sc->sc_dev, "keine freie UsbDev-Nummer\n");
        return ENOSPC;
    }

    ntu_wsprintf(dname, 32, L"\\Device\\UsbDev%u", n);
    ntu_wsprintf(lname, 40, L"\\DosDevices\\UsbDev%u", n);
    RtlInitUnicodeString(&un, dname);
    RtlInitUnicodeString(&ul, lname);

    st = IoCreateDevice(NtuDriverObject, sizeof(ntug_erweiterung), &un,
                        0x00000022 /* FILE_DEVICE_UNKNOWN */, 0, FALSE,
                        &dev);

    if (!NT_SUCCESS(st)) {
        ntug_belegt[n] = 0;
        device_printf(sc->sc_dev, "IoCreateDevice %08x\n", st);
        return ENXIO;
    }

    x = (ntug_erweiterung *)dev->DeviceExtension;
    memset(x, 0, sizeof(*x));
    x->Port.Kind = NTU_PORT_UGEN;
    x->Port.Suffix = n;
    x->Nummer = n;
    x->Sc = sc;
    KeInitializeMutex(&x->Sperre, 0);

    IoCreateSymbolicLink(&ul, &un);

    dev->Flags |= DO_BUFFERED_IO;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    sc->sc_nt     = dev;
    sc->sc_nummer = n;

    /*  DEVICEMAP-Eintrag: so findet ein Treiber sein Geraet, ohne alle
     *  UsbDevN der Reihe nach oeffnen zu muessen.  */

    ntug_kennung(text, 40, sc);
    basis[0] = L'U'; basis[1] = L'S'; basis[2] = L'B'; basis[3] = 0;
    RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP, basis, dname, REG_SZ,
                          text, (ULONG)((wcslen(text) + 1) * sizeof(WCHAR)));

    ntu_printf("usb: \\Device\\UsbDev%u  %S\n", n, text);
    return 0;
}

static VOID
ntug_nt_abbauen (struct ntug_softc *sc)
{
    WCHAR             dname[32], lname[40], basis[8];
    UNICODE_STRING    ul;
    ntug_erweiterung *x;

    if (sc->sc_nt == 0) {
        return;
    }

    x = (ntug_erweiterung *)sc->sc_nt->DeviceExtension;

    /*  Erst die Bruecke kappen -- unter ihrer Sperre, damit ein Aufruf,
     *  der gerade laeuft, zu Ende laeuft, und jeder spaetere
     *  NTUG_FEHLER_WEG bekommt.  Dann erst den Namen weg.  Das Geraet
     *  selbst gibt NT frei, wenn der letzte Griff zu ist.  */

    ntu_wart_rein("bruecke", "ugen detach", 0);
    KeWaitForSingleObject(&x->Sperre, Executive, KernelMode, FALSE, NULL);
    ntu_wart_raus();
    x->Sc = 0;
    KeReleaseMutex(&x->Sperre, FALSE);

    ntu_wsprintf(dname, 32, L"\\Device\\UsbDev%u", sc->sc_nummer);
    ntu_wsprintf(lname, 40, L"\\DosDevices\\UsbDev%u", sc->sc_nummer);
    basis[0] = L'U'; basis[1] = L'S'; basis[2] = L'B'; basis[3] = 0;
    RtlDeleteRegistryValue(RTL_REGISTRY_DEVICEMAP, basis, dname);

    RtlInitUnicodeString(&ul, lname);
    IoDeleteSymbolicLink(&ul);
    IoDeleteDevice(sc->sc_nt);

    ntug_belegt[sc->sc_nummer] = 0;
    sc->sc_nt = 0;
}


/* ====================================================================
 *  Gehaltene Uebertragungen
 *
 *  Eine Uebertragung wird je Endpunkt EINMAL eingerichtet und danach
 *  wiederverwendet -- so, wie FreeBSDs eigene Treiber es tun.  Die erste
 *  Fassung richtete sie je Aufruf ein und baute sie danach ab, und das
 *  ist am xHCI nicht nur langsam, sondern falsch:
 *
 *  Nach einer Uebertragung zeigt der Ringzeiger des Bausteins auf das
 *  abschliessende LINK-TRB der letzten TD -- also in den Speicher DIESER
 *  Uebertragung.  Wird sie abgebaut, ist der Speicher frei.  Die naechste
 *  Einrichtung bekam unter QEMU denselben Speicher mit derselben
 *  Aufteilung; an derselben Stelle stand nun ein LINK auf den
 *  UEBERnaechsten Ringplatz, die neue TD hing aber am naechsten.  Der
 *  Baustein folgte dem Zeiger, uebersprang sie und meldete "Ring leer".
 *  Nach dem Zeitablauf setzte FreeBSD den Ring zurueck, und der Aufruf
 *  danach ging wieder -- Erfolg und Zeitlimit im Wechsel, reproduzierbar,
 *  im QEMU-Ereignisprotokoll an einem ep_kick ohne ein einziges
 *  fetch_trb zu sehen.  Echte xHCI-Hardware folgt demselben Zeiger.
 *
 *  Abgebaut wird nur, wo es sein muss: beim Wechsel der alternativen
 *  Einstellung (usbd_set_alt_interface_index lehnt sonst mit
 *  USB_ERR_IN_USE ab) und beim Abziehen.
 * ==================================================================== */

static void ntug_rueckruf(struct usb_xfer *, usb_error_t);
static struct usb_endpoint_descriptor *ntug_ep_suchen(struct ntug_softc *,
                                                      uint8_t);

static struct usb_xfer *
ntug_holen (struct ntug_softc *sc, uint8_t adresse, int *fehler)
{
    struct usb_endpoint_descriptor *ed;
    struct usb_config               cfg;
    struct usb_xfer                *xfer[1];
    usb_error_t                     err;
    uint8_t                         art;
    int                             k, frei = -1;

    *fehler = 0;

    for (k = 0; k < NTUG_MAX_EP; k += 1) {
        if (sc->sc_gehalten[k].x != 0 && sc->sc_gehalten[k].adresse == adresse) {
            return sc->sc_gehalten[k].x;
        }
        if (sc->sc_gehalten[k].x == 0 && frei < 0) {
            frei = k;
        }
    }

    ed = ntug_ep_suchen(sc, adresse);

    if (ed == 0 || frei < 0) {
        *fehler = NTUG_FEHLER_ENDPUNKT;
        return 0;
    }

    art = ed->bmAttributes & UE_XFERTYPE;

    if (art == UE_CONTROL) {
        *fehler = NTUG_FEHLER_ENDPUNKT;
        return 0;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.type      = art;
    cfg.endpoint  = UE_GET_ADDR(adresse);
    cfg.direction = UE_GET_DIR(adresse) ? UE_DIR_IN : UE_DIR_OUT;
    cfg.timeout   = NTUG_ZEIT_VORGABE;
    cfg.callback  = &ntug_rueckruf;
    cfg.flags.short_xfer_ok = UE_GET_DIR(adresse) ? 1 : 0;
    cfg.flags.pipe_bof      = 1;

    if (art == UE_ISOCHRONOUS) {

        /*  So viele Rahmen, wie in den Arbeitspuffer passen.  Die
         *  Rahmenlaenge schaetzen wir hier aus wMaxPacketSize samt der
         *  Zusatztransaktionen in Bit 11 und 12; die genaue liefert der
         *  Stapel nach der Einrichtung.  */

        uint32_t w   = UGETW(ed->wMaxPacketSize);
        uint32_t mfl = (w & 0x7FF) * (1 + ((w >> 11) & 3));
        uint32_t n   = (mfl != 0) ? NTUG_MAX_DATEN / mfl : NTUG_MAX_ISO_RAHMEN;

        if (n > NTUG_MAX_ISO_RAHMEN) {
            n = NTUG_MAX_ISO_RAHMEN;
        }
        if (n == 0) {
            n = 1;
        }

        cfg.frames  = n;
        cfg.bufsize = 0;
        cfg.flags.short_xfer_ok   = 1;
        cfg.flags.short_frames_ok = 1;
        cfg.flags.pipe_bof        = 0;
    } else {
        cfg.bufsize = NTUG_MAX_DATEN;
    }

    err = usbd_transfer_setup(sc->sc_udev, &sc->sc_iface_index, xfer, &cfg,
                              1, sc, &sc->sc_mtx);

    if (err != 0) {
        *fehler = (int)err;
        return 0;
    }

    sc->sc_gehalten[frei].adresse = adresse;
    sc->sc_gehalten[frei].x       = xfer[0];
    sc->sc_gehalten[frei].amdpll  = 0;

    /*  Ein isochroner Strom beginnt: auf einer AMD-Suedbruecke die
     *  PLL-Stromsparfunktion abschalten, solange er gehalten wird.  Linux
     *  tut das, sobald ein isochroner Endpunkt in den Zeitplan kommt; bei
     *  uns ist das der Moment, in dem die gehaltene Uebertragung entsteht.  */

    if (art == UE_ISOCHRONOUS && ntu_amdpll_betrifft(sc->sc_udev->bus->parent)) {
        ntu_amdpll_isoc_an();
        sc->sc_gehalten[frei].amdpll = 1;
    }

    return xfer[0];
}

static void
ntug_alle_abbauen (struct ntug_softc *sc)
{
    int k;

    for (k = 0; k < NTUG_MAX_EP; k += 1) {
        if (sc->sc_gehalten[k].x != 0) {
            usbd_transfer_unsetup(&sc->sc_gehalten[k].x, 1);
            sc->sc_gehalten[k].x = 0;

            if (sc->sc_gehalten[k].amdpll) {
                ntu_amdpll_isoc_aus();
                sc->sc_gehalten[k].amdpll = 0;
            }
        }
    }
}

/*  Einen Auftrag auf einer gehaltenen Uebertragung laufen lassen und auf
 *  das Ende warten.  */

static int
ntug_laufen (struct ntug_softc *sc, struct usb_xfer *x, ntug_auftrag *a,
             uint32_t zeit)
{
    mtx_lock(&sc->sc_mtx);
    usbd_xfer_set_timeout(x, (int)zeit);
    sc->sc_auftrag = a;
    usbd_transfer_start(x);

    while (!a->fertig) {
        cv_wait(&sc->sc_cv, &sc->sc_mtx);
    }

    sc->sc_auftrag = 0;
    mtx_unlock(&sc->sc_mtx);
    return a->fehler;
}


/* ====================================================================
 *  Bulk und Interrupt, synchron
 *
 *  Der Stapel ist durch und durch asynchron: man stellt eine
 *  Uebertragung ein, und der Rueckruf meldet sie fertig.  Ein
 *  Steueraufruf aus NT will dagegen warten, bis die Daten da sind.
 *  Dazwischen steht eine Bedingungsvariable -- genau das Muster, das
 *  FreeBSD selbst in usb_msctest.c benutzt.
 *
 *  Die Uebertragung wird je Aufruf eingerichtet und wieder abgebaut.
 *  Das kostet eine DMA-Anforderung pro Aufruf; dafuer gibt es keinen
 *  Zustand, der zwischen zwei Aufrufen verrutschen kann, und keine
 *  offene Uebertragung, wenn niemand liest.  Wer viel Durchsatz braucht
 *  -- ein Scanner beim Einzug --, bekommt spaeter eine gehaltene
 *  Fassung; fuer den ersten Schritt ist Einfachheit mehr wert.
 * ==================================================================== */

static void
ntug_rueckruf (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntug_softc     *sc = usbd_xfer_softc(xfer);
    ntug_auftrag          *a  = sc->sc_auftrag;
    struct usb_page_cache *pc;
    int                    ist;

    usbd_xfer_status(xfer, &ist, NULL, NULL, NULL);

    if (a == 0) {
        return;                     /* Abbau ohne Auftrag */
    }

    switch (USB_GET_STATE(xfer)) {

        case USB_ST_SETUP:
            pc = usbd_xfer_get_frame(xfer, 0);

            if (a->rahmen != 0) {

                /*  Isochron.  Beim Lesen jeden Rahmen mit der
                 *  Hoechstlaenge anfordern -- dann liegt Rahmen n bei
                 *  n * mfl, so wie FreeBSDs uaudio es liest.  Beim
                 *  Schreiben die gewuenschten Laengen, und die Daten
                 *  dicht hintereinander.  */

                uint32_t k, summe = 0;

                a->mfl = usbd_xfer_max_framelen(xfer);
                usbd_xfer_set_frames(xfer, a->rahmen);

                for (k = 0; k < a->rahmen; k += 1) {
                    uint32_t l = a->raus ? a->laengen[k] : a->mfl;

                    if (l > a->mfl) {
                        l = a->mfl;
                    }

                    usbd_xfer_set_frame_len(xfer, k, l);
                    summe += l;
                }

                if (a->raus && summe != 0) {
                    usbd_copy_in(pc, 0, a->daten, summe);
                }

                usbd_transfer_submit(xfer);
                break;
            }

            if (a->raus && a->laenge != 0) {
                usbd_copy_in(pc, 0, a->daten, a->laenge);
            }

            usbd_xfer_set_frame_len(xfer, 0, a->laenge);
            usbd_transfer_submit(xfer);
            break;

        case USB_ST_TRANSFERRED:

            if (a->rahmen != 0) {

                uint32_t k, summe = 0;

                for (k = 0; k < a->rahmen; k += 1) {
                    a->ist_je[k] = usbd_xfer_frame_len(xfer, k);
                    summe += a->ist_je[k];
                }

                if (!a->raus) {
                    pc = usbd_xfer_get_frame(xfer, 0);
                    usbd_copy_out(pc, 0, a->daten, a->rahmen * a->mfl);
                }

                ist = (int)summe;
            } else if (!a->raus && ist > 0) {
                pc = usbd_xfer_get_frame(xfer, 0);
                usbd_copy_out(pc, 0, a->daten, ist);
            }

            a->ist    = (uint32_t)ist;
            a->fehler = 0;
            a->fertig = 1;
            cv_signal(&sc->sc_cv);
            break;

        default:

            /*  Kein Wiederholen und kein Stall-Aufheben von selbst: der
             *  Treiber darueber soll sehen, was passiert ist, und
             *  entscheiden.  Ein Drucker, der beim Papierende sperrt,
             *  will das melden und nicht ueberspielt bekommen.  */

            a->ist    = (uint32_t)ist;
            a->fehler = (int)error;
            a->fertig = 1;
            cv_signal(&sc->sc_cv);
            break;
    }
}

static struct usb_endpoint_descriptor *
ntug_ep_suchen (struct ntug_softc *sc, uint8_t adresse)
{
    struct usb_config_descriptor    *cd;
    struct usb_interface            *ifc;
    struct usb_interface_descriptor *id;
    struct usb_endpoint_descriptor  *ed;

    cd  = usbd_get_config_descriptor(sc->sc_udev);
    ifc = usbd_get_iface(sc->sc_udev, sc->sc_iface_index);
    id  = (ifc != 0) ? usbd_get_interface_descriptor(ifc) : 0;

    if (cd == 0 || id == 0) {
        return 0;
    }

    /*  Nur die Endpunkte DIESER Schnittstelle.  Ein Treiber darf nicht
     *  ueber seinen Eintrag hinweg an einem Nachbarn lesen, der einem
     *  anderen Treiber gehoert.  */

    ed = (struct usb_endpoint_descriptor *)id;

    while ((ed = usb_edesc_foreach(cd, ed)) != 0) {
        if (ed->bEndpointAddress == adresse) {
            return ed;
        }
    }

    return 0;
}

static int
ntug_uebertragen (struct ntug_softc *sc, uint8_t adresse, uint8_t *daten,
                  uint32_t laenge, uint32_t zeit, uint32_t *ist)
{
    struct usb_endpoint_descriptor *ed;
    struct usb_xfer                *x;
    ntug_auftrag                    a;
    uint8_t                         art;
    int                             err;

    *ist = 0;
    ed = ntug_ep_suchen(sc, adresse);

    if (ed == 0) {
        return NTUG_FEHLER_ENDPUNKT;
    }

    art = ed->bmAttributes & UE_XFERTYPE;

    if (art != UE_BULK && art != UE_INTERRUPT) {
        return NTUG_FEHLER_ENDPUNKT;     /* isochron: IOCTL_NTUG_ISO */
    }

    x = ntug_holen(sc, adresse, &err);

    if (x == 0) {
        return err;
    }

    memset(&a, 0, sizeof(a));
    a.daten  = daten;
    a.laenge = laenge;
    a.raus   = UE_GET_DIR(adresse) ? 0 : 1;

    err = ntug_laufen(sc, x, &a, zeit);
    *ist = a.ist;
    return err;
}


/* ====================================================================
 *  Isochron
 *
 *  Dieselbe Machart wie Bulk und Interrupt -- einrichten, starten,
 *  warten, abbauen --, nur dass eine Uebertragung aus vielen Rahmen
 *  besteht, einer je Millisekunde (Full Speed) oder Mikrorahmen (High
 *  Speed), und jeder seine eigene Laenge hat.
 *
 *  Die Rahmenschrittweite fuer den Lesepuffer kommt vom Stapel
 *  (usbd_xfer_max_framelen), nicht aus wMaxPacketSize.  Bei High-Speed-
 *  Endpunkten mit mehreren Transaktionen je Mikrorahmen stehen dort in
 *  den Bits 11 und 12 zusaetzliche Pakete; der Stapel rechnet sie ein,
 *  eine eigene Rechnung waere eine zweite Stelle, die falsch sein kann.
 * ==================================================================== */

static int
ntug_bus_ist_ohci (struct ntug_softc *sc)
{
    device_t hc = sc->sc_udev->bus->parent;
    const char *n = (hc != 0) ? device_get_nameunit(hc) : 0;   /* "ohci1" */

    return n != 0 && n[0] == 'o' && n[1] == 'h' && n[2] == 'c' && n[3] == 'i';
}

static int
ntug_iso_uebertragen (struct ntug_softc *sc, uint8_t adresse, uint32_t rahmen,
                      uint32_t *laengen, uint32_t *ist_je, uint8_t *daten,
                      uint32_t zeit, uint32_t *mfl_aus, uint32_t *summe,
                      uint32_t *geschaetzt)
{
    struct usb_endpoint_descriptor *ed;
    struct usb_xfer                *x;
    ntug_auftrag                    a;

    *summe = 0;
    *mfl_aus = 0;
    ed = ntug_ep_suchen(sc, adresse);

    if (ed == 0 || (ed->bmAttributes & UE_XFERTYPE) != UE_ISOCHRONOUS) {
        return NTUG_FEHLER_ENDPUNKT;
    }

    {
        int e2;
        x = ntug_holen(sc, adresse, &e2);

        if (x == 0) {
            return e2;
        }
    }

    /*  Passt die Anfrage nicht in den Arbeitspuffer, weniger Rahmen.
     *  Der Aufrufer sieht in der Antwort, wie viele es waren.  */

    *mfl_aus = usbd_xfer_max_framelen(x);

    if (*mfl_aus != 0 && rahmen * *mfl_aus > NTUG_MAX_DATEN) {
        rahmen = NTUG_MAX_DATEN / *mfl_aus;
    }

    /*  Nie mehr Rahmen, als die gehaltene Uebertragung hat.  */

    if (rahmen > usbd_xfer_max_frames(x)) {
        rahmen = usbd_xfer_max_frames(x);
    }

    memset(&a, 0, sizeof(a));
    a.daten   = daten;
    a.raus    = UE_GET_DIR(adresse) ? 0 : 1;
    a.rahmen  = rahmen;
    a.laengen = laengen;
    a.ist_je  = ist_je;

    /*  Planung frisch aufsetzen, wenn die letzte Uebertragung auf diesem
     *  Endpunkt zu lange her ist.
     *
     *  Der Stapel merkt sich je Endpunkt die naechste Rahmennummer und
     *  entscheidet mit "delta > frame_mask / 2", ob sie vorbei ist.  Der
     *  Rahmenzaehler laeuft aber rund -- beim xHCI alle 2,048 s, beim
     *  EHCI alle 1,024 s.  Nach einer laengeren Pause ist nicht mehr zu
     *  unterscheiden, ob die gemerkte Nummer vorbei ist oder noch kommt,
     *  und mit etwa halber Wahrscheinlichkeit plant der Stapel in eine
     *  Zukunft, die in Wahrheit Vergangenheit ist.  Der Baustein wartet
     *  dann bis zum naechsten Umlauf.  Im QEMU-Lauf: 100 Rahmen gingen,
     *  der naechste Aufruf Sekunden spaeter lief ins Zeitlimit.
     *
     *  FreeBSD selbst trifft das nie, weil dort ein Audiotreiber
     *  ununterbrochen streamt.  Erst ein Aufruf je Anfrage erzeugt
     *  beliebige Pausen.  250 ms liegen unter der kuerzesten
     *  Mehrdeutigkeit aller vier Bausteine (EHCI: 512 ms); dicht
     *  aufeinanderfolgende Aufrufe -- ein Treiber, der Ton streamt --
     *  behalten ihre durchgehende Planung.  */

    {
        int idx = (adresse & 0x0F) | ((adresse & 0x80) ? 16 : 0);
        int her = ticks - sc->sc_iso_zeit[idx];

        if (sc->sc_iso_zeit[idx] == 0 || her < 0 || her > hz / 4) {
            struct usb_endpoint *ep = usbd_get_ep_by_addr(sc->sc_udev,
                                                          adresse);
            if (ep != 0) {
                ep->is_synced = 0;
            }
        }
    }

    ntug_laufen(sc, x, &a, zeit);

    sc->sc_iso_zeit[(adresse & 0x0F) | ((adresse & 0x80) ? 16 : 0)] =
        ticks | 1;                  /* nie 0 -- das heisst "noch nie" */

    *summe      = a.ist;
    *mfl_aus    = a.mfl ? a.mfl : *mfl_aus;
    *geschaetzt = 0;

    /*  OHCI, OUT, ohne harten Fehler: die Laengen sind nach der Norm 0
     *  und damit nichtssagend.  Die angeforderten einsetzen und es
     *  kennzeichnen -- siehe NTUG_ISO_ANTWORT.Geschaetzt.  */

    if (a.raus && ntug_bus_ist_ohci(sc) &&
        (a.fehler == 0 || a.fehler == USB_ERR_SHORT_XFER)) {

        uint32_t k, s2 = 0;

        for (k = 0; k < rahmen; k += 1) {
            uint32_t l = (laengen[k] < a.mfl) ? laengen[k] : a.mfl;
            ist_je[k] = l;
            s2 += l;
        }

        *summe      = s2;
        *geschaetzt = 1;
        return 0;
    }

    return a.fehler;
}

static ULONG
ntug_iso (struct ntug_softc *sc, const uint8_t *anfrage, ULONG ein,
          NTUG_ERGEBNIS *e, ULONG platz, uint8_t *arbeit)
{
    NTUG_ISO          q;
    NTUG_ISO_ANTWORT *ant;
    uint32_t          laengen[NTUG_MAX_ISO_RAHMEN];
    uint32_t          ist_je[NTUG_MAX_ISO_RAHMEN];
    uint32_t          n, k, mfl = 0, summe = 0, datenlaenge = 0, geschaetzt = 0;
    uint32_t          zeit;
    int               lesen, err;
    ULONG             kopf;

    if (ein < sizeof(q) || platz < sizeof(*e) + sizeof(*ant)) {
        return 0;
    }

    memcpy(&q, anfrage, sizeof(q));
    n     = q.Rahmen;
    zeit  = q.Zeitlimit ? q.Zeitlimit : NTUG_ZEIT_VORGABE;
    lesen = (q.Endpunkt & 0x80) != 0;

    if (n == 0 || n > NTUG_MAX_ISO_RAHMEN) {
        e->Fehler = NTUG_FEHLER_ANFRAGE;
        e->Laenge = 0;
        return sizeof(*e);
    }

    memset(laengen, 0, sizeof(laengen));
    memset(ist_je, 0, sizeof(ist_je));

    if (!lesen) {

        /*  Laengen und Daten aus der Eingabe retten, bevor die Antwort
         *  den Puffer ueberschreibt -- METHOD_BUFFERED.  */

        if (ein < sizeof(q) + n * sizeof(uint32_t)) {
            e->Fehler = NTUG_FEHLER_ANFRAGE;
            e->Laenge = 0;
            return sizeof(*e);
        }

        memcpy(laengen, anfrage + sizeof(q), n * sizeof(uint32_t));

        for (k = 0; k < n; k += 1) {
            datenlaenge += laengen[k];
        }

        if (datenlaenge > NTUG_MAX_DATEN ||
            ein < sizeof(q) + n * sizeof(uint32_t) + datenlaenge) {
            e->Fehler = NTUG_FEHLER_ANFRAGE;
            e->Laenge = 0;
            return sizeof(*e);
        }

        memcpy(arbeit, anfrage + sizeof(q) + n * sizeof(uint32_t),
               datenlaenge);
    }

    err = ntug_iso_uebertragen(sc, q.Endpunkt, n, laengen, ist_je, arbeit,
                               zeit, &mfl, &summe, &geschaetzt);

    /*  Wurden weniger Rahmen uebertragen, als verlangt, ist das in ist_je
     *  an den Nullen hinten zu sehen; die tatsaechliche Zahl steht in der
     *  Antwort.  */

    if (mfl != 0 && n * mfl > NTUG_MAX_DATEN) {
        n = NTUG_MAX_DATEN / mfl;
    }

    ant = (NTUG_ISO_ANTWORT *)(e + 1);
    ant->Rahmen       = n;
    ant->RahmenLaenge = mfl;
    ant->Geschaetzt   = geschaetzt;
    kopf = sizeof(*e) + sizeof(*ant) + n * sizeof(uint32_t);

    if (platz < kopf) {
        e->Fehler = NTUG_FEHLER_ANFRAGE;
        e->Laenge = 0;
        return sizeof(*e);
    }

    memcpy((uint8_t *)(ant + 1), ist_je, n * sizeof(uint32_t));

    e->Fehler = err;
    e->Laenge = summe;

    if (lesen && err == 0) {

        uint32_t menge = n * mfl;

        if (kopf + menge > platz) {
            menge = platz - kopf;
        }

        memcpy((uint8_t *)e + kopf, arbeit, menge);
        return kopf + menge;
    }

    return kopf;
}


/* ====================================================================
 *  Die sechs Aufrufe
 * ==================================================================== */

static VOID
ntug_text (char *ziel, ULONG max, const char *quelle)
{
    if (quelle == 0) {
        ziel[0] = '\0';
        return;
    }

    strlcpy(ziel, quelle, max);
}

static ULONG
ntug_info (struct ntug_softc *sc, NTUG_ERGEBNIS *e, ULONG platz)
{
    NTUG_INFO                       *i = (NTUG_INFO *)(e + 1);
    struct usb_device_descriptor    *dd;
    struct usb_interface            *ifc;
    struct usb_interface_descriptor *id;
    struct usb_config_descriptor    *cd;
    struct usb_endpoint_descriptor  *ed;

    if (platz < sizeof(*e) + sizeof(*i)) {
        return 0;
    }

    memset(i, 0, sizeof(*i));

    dd  = usbd_get_device_descriptor(sc->sc_udev);
    cd  = usbd_get_config_descriptor(sc->sc_udev);
    ifc = usbd_get_iface(sc->sc_udev, sc->sc_iface_index);
    id  = (ifc != 0) ? usbd_get_interface_descriptor(ifc) : 0;

    i->Fassung         = NTUG_FASSUNG;
    i->Hersteller      = UGETW(dd->idVendor);
    i->Produkt         = UGETW(dd->idProduct);
    i->Ausgabe         = UGETW(dd->bcdDevice);
    i->GeraetKlasse    = dd->bDeviceClass;
    i->GeraetUnter     = dd->bDeviceSubClass;
    i->GeraetProtokoll = dd->bDeviceProtocol;
    i->Adresse         = sc->sc_udev->address;
    i->Anschluss       = sc->sc_udev->port_no;
    i->Tiefe           = sc->sc_udev->depth;

    switch (usbd_get_speed(sc->sc_udev)) {
        case USB_SPEED_LOW:   i->Geschwindigkeit = NTUG_GESCHW_LOW;   break;
        case USB_SPEED_FULL:  i->Geschwindigkeit = NTUG_GESCHW_FULL;  break;
        case USB_SPEED_HIGH:  i->Geschwindigkeit = NTUG_GESCHW_HIGH;  break;
        case USB_SPEED_SUPER: i->Geschwindigkeit = NTUG_GESCHW_SUPER; break;
        default:              i->Geschwindigkeit = 0;                 break;
    }

    if (id != 0) {
        i->IfKlasse      = id->bInterfaceClass;
        i->IfUnter       = id->bInterfaceSubClass;
        i->IfProtokoll   = id->bInterfaceProtocol;
        i->IfNummer      = id->bInterfaceNumber;
        i->IfAlternative = id->bAlternateSetting;

        ed = (struct usb_endpoint_descriptor *)id;

        while (cd != 0 && (ed = usb_edesc_foreach(cd, ed)) != 0 &&
               i->AnzahlEp < NTUG_MAX_EP) {

            NTUG_ENDPUNKT *p = &i->Ep[i->AnzahlEp];

            p->Adresse   = ed->bEndpointAddress;
            p->Art       = ed->bmAttributes & UE_XFERTYPE;
            p->MaxPaket  = UGETW(ed->wMaxPacketSize) & 0x7FF;
            p->Intervall = ed->bInterval;
            i->AnzahlEp += 1;
        }
    }

    ntug_text(i->HerstellerText, sizeof(i->HerstellerText),
              sc->sc_udev->manufacturer);
    ntug_text(i->ProduktText, sizeof(i->ProduktText),
              sc->sc_udev->product);
    ntug_text(i->Seriennummer, sizeof(i->Seriennummer),
              sc->sc_udev->serial);

    e->Fehler = NTUG_OK;
    e->Laenge = sizeof(*i);
    return sizeof(*e) + sizeof(*i);
}

static ULONG
ntug_deskriptor (struct ntug_softc *sc, const NTUG_DESK *q, NTUG_ERGEBNIS *e,
                 ULONG platz)
{
    uint8_t  *ziel = (uint8_t *)(e + 1);
    uint16_t  n    = q->Laenge;
    uint16_t  ist  = 0;
    usb_error_t err;

    if (platz < sizeof(*e)) {
        return 0;
    }

    if (n > platz - sizeof(*e)) {
        n = (uint16_t)(platz - sizeof(*e));
    }

    if (q->Typ == UDESC_HID || q->Typ == UDESC_REPORT) {

        /*  HID-Deskriptoren liegen an der Schnittstelle, nicht am Geraet
         *  -- bmRequestType 0x81 statt 0x80.  Wer das vergisst, bekommt
         *  vom Joystick einen Stall statt der Bedeutung seiner Bits.  */

        struct usb_device_request r;

        r.bmRequestType = UT_READ_INTERFACE;
        r.bRequest      = UR_GET_DESCRIPTOR;
        USETW2(r.wValue, q->Typ, q->Index);
        USETW(r.wIndex,  sc->sc_iface_no);
        USETW(r.wLength, n);

        err = usbd_do_request_flags(sc->sc_udev, NULL, &r, ziel,
                                    USB_SHORT_XFER_OK, &ist,
                                    NTUG_ZEIT_VORGABE);
    } else if (q->Typ == UDESC_DEVICE || q->Typ == UDESC_CONFIG) {

        /*  Geraete- und Konfigurationsdeskriptor hat der Stapel beim
         *  Aufzaehlen gelesen und haelt sie vor.  Kein Grund, den Bus zu
         *  bemuehen -- und die vorgehaltene Konfiguration ist genau die,
         *  mit der der Stapel arbeitet.  */

        const uint8_t *q2;
        uint16_t       voll;

        if (q->Typ == UDESC_DEVICE) {
            q2   = (const uint8_t *)usbd_get_device_descriptor(sc->sc_udev);
            voll = (q2 != 0) ? q2[0] : 0;
        } else {
            q2   = (const uint8_t *)usbd_get_config_descriptor(sc->sc_udev);
            voll = (q2 != 0) ? (uint16_t)(q2[2] | (q2[3] << 8)) : 0;
        }

        if (q2 == 0) {
            err = USB_ERR_INVAL;
        } else {
            ist = (voll < n) ? voll : n;
            memcpy(ziel, q2, ist);
            err = 0;
        }

    } else {

        /*  Alles andere vom Geraet.  min_len muss mindestens 2 sein --
         *  sonst meldet der Stapel INVAL, ohne den Bus anzufassen.  Das
         *  war der Fehler 3 im ersten Lauf.  */

        err = usbd_req_get_desc(sc->sc_udev, NULL, &ist, ziel,
                                (n < 2) ? n : 2, n,
                                q->Sprache, q->Typ, q->Index, 3);

        if (err == 0 && ist == 0 && n >= 2) {
            ist = ziel[0];
            if (ist > n) {
                ist = n;
            }
        }
    }

    e->Fehler = (long)err;
    e->Laenge = ist;
    return sizeof(*e) + ist;
}

static ULONG
ntug_steuer (struct ntug_softc *sc, const NTUG_STEUER *q, ULONG ein,
             NTUG_ERGEBNIS *e, ULONG platz, uint8_t *arbeit)
{
    struct usb_device_request r;
    uint16_t    ist = 0;
    uint16_t    n   = q->wLength;
    uint32_t    zeit = q->Zeitlimit ? q->Zeitlimit : NTUG_ZEIT_VORGABE;
    usb_error_t err;
    int         hinein = (q->bmRequestType & 0x80) != 0;

    if (platz < sizeof(*e)) {
        return 0;
    }

    /*  Richtung OUT: die Nutzdaten folgen der Anfrage im Eingabepuffer.
     *  Sie werden vorher in einen eigenen Puffer gerettet, weil
     *  METHOD_BUFFERED Ein- und Ausgabe in DENSELBEN Puffer legt -- die
     *  Antwort wuerde sonst die Daten ueberschreiben, bevor sie gesendet
     *  sind.  */

    if (!hinein) {
        if (ein < sizeof(*q) + n) {
            e->Fehler = NTUG_FEHLER_ANFRAGE;
            e->Laenge = 0;
            return sizeof(*e);
        }
        memcpy(arbeit, (const uint8_t *)(q + 1), n);
    } else if (n > platz - sizeof(*e)) {
        n = (uint16_t)(platz - sizeof(*e));
    }

    r.bmRequestType = q->bmRequestType;
    r.bRequest      = q->bRequest;
    USETW(r.wValue,  q->wValue);
    USETW(r.wIndex,  q->wIndex);
    USETW(r.wLength, n);

    err = usbd_do_request_flags(sc->sc_udev, NULL, &r, arbeit,
                                USB_SHORT_XFER_OK, &ist, zeit);

    if (hinein && ist != 0) {
        memcpy((uint8_t *)(e + 1), arbeit, ist);
    }

    e->Fehler = (long)err;
    e->Laenge = hinein ? ist : ist;
    return sizeof(*e) + (hinein ? ist : 0);
}

static ULONG
ntug_daten (struct ntug_softc *sc, ULONG code, const NTUG_DATEN *q, ULONG ein,
            NTUG_ERGEBNIS *e, ULONG platz, uint8_t *arbeit)
{
    uint32_t n    = q->Laenge;
    uint32_t zeit = q->Zeitlimit ? q->Zeitlimit : NTUG_ZEIT_VORGABE;
    uint32_t ist  = 0;
    int      err;
    int      lesen = (code == IOCTL_NTUG_LESEN);

    if (platz < sizeof(*e)) {
        return 0;
    }

    if (n > NTUG_MAX_DATEN) {
        n = NTUG_MAX_DATEN;
    }

    /*  Die Richtung steht im Endpunkt, nicht im Aufruf.  Passen beide
     *  nicht zusammen, ist das ein Fehler des Aufrufers -- lieber
     *  melden als stillschweigend in die falsche Richtung schicken.  */

    if (lesen != ((q->Endpunkt & 0x80) != 0)) {
        e->Fehler = NTUG_FEHLER_ENDPUNKT;
        e->Laenge = 0;
        return sizeof(*e);
    }

    if (lesen) {
        if (n > platz - sizeof(*e)) {
            n = platz - sizeof(*e);
        }
    } else {
        if (ein < sizeof(*q) + n) {
            e->Fehler = NTUG_FEHLER_ANFRAGE;
            e->Laenge = 0;
            return sizeof(*e);
        }
        memcpy(arbeit, (const uint8_t *)(q + 1), n);
    }

    err = ntug_uebertragen(sc, q->Endpunkt, arbeit, n, zeit, &ist);

    if (lesen && ist != 0) {
        memcpy((uint8_t *)(e + 1), arbeit, ist);
    }

    e->Fehler = err;
    e->Laenge = ist;
    return sizeof(*e) + (lesen ? ist : 0);
}

NTSTATUS
ntu_ugen_ioctl (PDEVICE_OBJECT dev, ULONG code, PVOID puffer, ULONG ein,
                ULONG aus, PULONG len)
{
    ntug_erweiterung  *x = (ntug_erweiterung *)dev->DeviceExtension;
    struct ntug_softc *sc;
    NTUG_ERGEBNIS     *e = (NTUG_ERGEBNIS *)puffer;
    uint8_t           *arbeit;
    uint8_t            anfrage[64];

    *len = 0;

    if (aus < sizeof(NTUG_ERGEBNIS)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /*  Die Anfrage retten, bevor die Antwort sie ueberschreibt --
     *  METHOD_BUFFERED legt beides in denselben Puffer.  */

    memset(anfrage, 0, sizeof(anfrage));
    memcpy(anfrage, puffer, (ein < sizeof(anfrage)) ? ein : sizeof(anfrage));

    ntu_wart_rein("bruecke", "ugen ioctl", 0);
    KeWaitForSingleObject(&x->Sperre, Executive, KernelMode, FALSE, NULL);
    ntu_wart_raus();

    sc = x->Sc;

    if (sc == 0) {
        KeReleaseMutex(&x->Sperre, FALSE);
        e->Fehler = NTUG_FEHLER_WEG;
        e->Laenge = 0;
        *len = sizeof(*e);
        return STATUS_SUCCESS;
    }

    /*  Arbeitspuffer fuer die Nutzdaten.  Er kommt aus dem Pool und
     *  nicht vom Stapel -- 64 kB auf dem Kernstapel waeren der sichere
     *  Weg in einen Doppelfehler.  */

    arbeit = (uint8_t *)ntu_malloc(NTUG_MAX_DATEN, 0);

    if (arbeit == 0) {
        KeReleaseMutex(&x->Sperre, FALSE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    switch (code) {

        case IOCTL_NTUG_INFO:
            *len = ntug_info(sc, e, aus);
            break;

        case IOCTL_NTUG_DESKRIPTOR:
            if (ein < sizeof(NTUG_DESK)) { *len = 0; break; }
            *len = ntug_deskriptor(sc, (NTUG_DESK *)anfrage, e, aus);
            break;

        case IOCTL_NTUG_STEUER:
            if (ein < sizeof(NTUG_STEUER)) { *len = 0; break; }
            /*  Bei OUT liegen die Daten hinter der Anfrage im Puffer;
             *  die sind noch unversehrt, weil noch nichts geantwortet
             *  wurde.  ntug_steuer kopiert sie vor dem Senden weg.  */
            *len = ntug_steuer(sc, (NTUG_STEUER *)puffer, ein, e, aus, arbeit);
            break;

        case IOCTL_NTUG_LESEN:
        case IOCTL_NTUG_SCHREIBEN:
            if (ein < sizeof(NTUG_DATEN)) { *len = 0; break; }
            *len = ntug_daten(sc, code, (NTUG_DATEN *)puffer, ein, e, aus,
                              arbeit);
            break;

        case IOCTL_NTUG_ISO:
            *len = ntug_iso(sc, (const uint8_t *)puffer, ein, e, aus, arbeit);
            break;

        case IOCTL_NTUG_ALTERNATIVE:
            if (ein < 1) { *len = 0; break; }

            /*  Gehaltene Uebertragungen zuerst abbauen -- sie gehoeren zu
             *  Endpunkten der alten Einstellung, und der Stapel lehnt den
             *  Wechsel sonst mit USB_ERR_IN_USE ab.  */

            ntug_alle_abbauen(sc);
            e->Fehler = (long)usbd_set_alt_interface_index(sc->sc_udev,
                                            sc->sc_iface_index, anfrage[0]);
            e->Laenge = 0;
            *len = sizeof(*e);
            break;

        case IOCTL_NTUG_STALL_WEG: {

            struct usb_device_request r;
            struct usb_endpoint      *ep;

            if (ein < 1) { *len = 0; break; }

            r.bmRequestType = UT_WRITE_ENDPOINT;
            r.bRequest      = UR_CLEAR_FEATURE;
            USETW(r.wValue,  UF_ENDPOINT_HALT);
            USETW(r.wIndex,  anfrage[0]);
            USETW(r.wLength, 0);

            e->Fehler = (long)usbd_do_request_flags(sc->sc_udev, NULL, &r,
                                   NULL, 0, NULL, NTUG_ZEIT_VORGABE);

            /*  Mit der Sperre ist auch das Datenumschaltbit auf DATA0
             *  zurueck -- im Geraet.  Der Stapel muss es ebenfalls
             *  vergessen, sonst ist das naechste Paket aus seiner Sicht
             *  eine Wiederholung und wird verworfen.  */

            ep = usbd_get_ep_by_addr(sc->sc_udev, anfrage[0]);

            if (ep != 0) {
                usbd_clear_data_toggle(sc->sc_udev, ep);
            }

            e->Laenge = 0;
            *len = sizeof(*e);
            break;
        }

        default:
            ntu_free(arbeit);
            KeReleaseMutex(&x->Sperre, FALSE);
            return STATUS_INVALID_DEVICE_REQUEST;
    }

    ntu_free(arbeit);
    KeReleaseMutex(&x->Sperre, FALSE);

    return (*len != 0) ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}


/* ====================================================================
 *  probe, attach, detach
 * ==================================================================== */

static int
ntug_probe (device_t dev)
{
    struct usb_attach_arg *uaa = device_get_ivars(dev);

    if (uaa->usb_mode != USB_MODE_HOST) {
        return ENXIO;
    }

    /*  Nur Schnittstellen.  Auf Geraeteebene meldet sich hier niemand --
     *  sonst wuerde ein zusammengesetztes Geraet als Ganzes geschluckt,
     *  und ukbd kaeme an den Tastaturteil nie heran.  */

    if (uaa->iface == 0) {
        return ENXIO;
    }

    return BUS_PROBE_HOOVER;
}

static int
ntug_attach (device_t dev)
{
    struct ntug_softc     *sc  = device_get_softc(dev);
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    struct usb_interface_descriptor *id;

    sc->sc_dev         = dev;
    sc->sc_udev        = uaa->device;
    sc->sc_iface_index = uaa->info.bIfaceIndex;
    sc->sc_iface_no    = uaa->info.bIfaceNum;

    id = usbd_get_interface_descriptor(uaa->iface);

    device_set_desc(dev, "USB-Schnittstelle (allgemein)");
    mtx_init(&sc->sc_mtx, "ntugen", NULL, MTX_DEF);
    cv_init(&sc->sc_cv, "ntugen");

    /*  Die Bindung merkt sich der Stapel selbst, sobald dieses attach
     *  gelingt (usb_probe_and_attach_sub setzt iface->subdev).  Ein
     *  eigenes usbd_set_parent_iface waere hier falsch -- das ist der
     *  Weg, ueber den FreeBSDs ugen Schnittstellen beansprucht, die
     *  schon einem Treiber gehoeren.  */

    if (ntug_nt_anlegen(sc) != 0) {
        cv_destroy(&sc->sc_cv);
        mtx_destroy(&sc->sc_mtx);
        return ENXIO;
    }

    (void)id;
    return 0;
}

static int
ntug_detach (device_t dev)
{
    struct ntug_softc *sc = device_get_softc(dev);

    /*  Erst die Bruecke kappen (wartet, bis ein laufender Aufruf fertig
     *  ist), dann die gehaltenen Uebertragungen abbauen -- danach kann
     *  keiner mehr eine benutzen.  */

    ntu_wart_rein("schritt", "ugen detach: nt_abbauen", 0);
    ntug_nt_abbauen(sc);
    ntu_wart_rein("schritt", "ugen detach: alle_abbauen", 0);
    ntug_alle_abbauen(sc);
    ntu_wart_raus();
    cv_destroy(&sc->sc_cv);
    mtx_destroy(&sc->sc_mtx);
    return 0;
}

static device_method_t ntug_methods[] = {
    DEVMETHOD(device_probe,  ntug_probe),
    DEVMETHOD(device_attach, ntug_attach),
    DEVMETHOD(device_detach, ntug_detach),
    DEVMETHOD_END
};

static driver_t ntug_driver = {
    .name    = "ntugen",
    .methods = ntug_methods,
    .size    = sizeof(struct ntug_softc),
};

DRIVER_MODULE(ntugen, uhub, ntug_driver, 0, 0);
