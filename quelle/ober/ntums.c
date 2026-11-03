/*
 *  ober/ntums.c -- die Oberkante fuer Maeuse.
 *
 *  Wie bei der Tastatur neu geschrieben.  FreeBSDs ums.c liest den
 *  HID-Berichtsdeskriptor und kann damit jede Maus; das kostet den
 *  ganzen hid-Parser als weiteres Untersystem.  Das Startprotokoll
 *  kostet nichts und deckt jede Maus ab, die auch im BIOS funktioniert.
 *
 *  Der Bericht ist drei oder vier Byte:
 *
 *      [0]  Tasten, Bit 0 links, Bit 1 rechts, Bit 2 Mitte
 *      [1]  dx, vorzeichenbehaftet
 *      [2]  dy, vorzeichenbehaftet
 *      [3]  Rad, vorzeichenbehaftet -- nicht jede Maus liefert es
 *
 *  dy zeigt nach **unten**, genau wie NT es erwartet.  Kein Vorzeichen
 *  drehen -- das ist der Fehler, den man beim ersten Versuch macht und
 *  danach nie wieder.
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
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbhid.h>
#include <dev/hid/hid.h>
#include <ober/ntkbdmou.h>
#include <ober/ntusbioctl.h>

extern NTU_ZAEHLER ntu_zaehler;

enum { NTUMS_INTR_DT, NTUMS_N_TRANSFER };

struct ntums_softc {
    device_t           sc_dev;
    struct usb_device *sc_udev;
    struct mtx         sc_mtx;
    struct usb_xfer   *sc_xfer[NTUMS_N_TRANSFER];
    uint8_t            sc_iface_index;
    uint8_t            sc_last_buttons;
    PDEVICE_OBJECT     sc_pdo;
    NTU_MOU_CONNECT   *sc_conn;         /* der feste Anschluss */
    MOUSE_INPUT_DATA   sc_one;

    /*  Volles Berichtsformat, beschrieben vom Berichtsdeskriptor der Maus.
     *  sc_hid = 0: Startprotokoll wie bisher (3 Byte, Rad nur freiwillig).  */
    int                 sc_hid;
    uint8_t             sc_iid;          /* Berichtsnummer, 0 = keine */
    int                 sc_rad;          /* 1 = Rad vorhanden */
    int                 sc_n_knopf;      /* 1..8 */
    struct hid_location sc_loc_x, sc_loc_y, sc_loc_rad, sc_loc_knopf[8];

    /*  Tasten 4..8 (ntmaustasten.c): mitte-Belegung wirkt wie Taste 3.  */
    uint8_t             sc_last_basis;   /* Tasten 1..3, wie an NT gemeldet */

    /*  Belegung je Taste, festgehalten beim DRUECKEN: das Loslassen loest
     *  genau diese -- auch wenn in der Zwischenzeit umbelegt wurde.  */
    NTU_MAUSAKTION      sc_aktiv[NTU_MAUS_TASTEN];
    int                 sc_halten[3];    /* wie viele Tasten links/rechts/Mitte halten */
};

static usb_callback_t ntums_intr_callback;

static const struct usb_config ntums_config[NTUMS_N_TRANSFER] = {
    [NTUMS_INTR_DT] = {
        .type      = UE_INTERRUPT,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_IN,
        .flags     = { .pipe_bof = 1, .short_xfer_ok = 1 },
        .bufsize   = 0,
        .callback  = &ntums_intr_callback,
    },
};

/*  Uebergabe an mouclass: direkt, aus unserem Faden, kurz auf
 *  DISPATCH_LEVEL gehoben.
 *
 *  Es gab zeitweise vier andere Wege (DPC, DPC auf festem Prozessor, Klick
 *  mit Mini-Bewegung), weil Klicks auf echter Hardware ~3 s hingen.  Die
 *  Ursache war die Logitech-Maussoftware (MouseWare + Filtertreiber), nicht
 *  die Uebergabe -- keiner der Wege aenderte etwas, sie sind wieder raus.
 *  Messbar mit MAUSDATEN.EXE (IOCTL_NTU_MAUS_PROTOKOLL, unten).  */

/*  Die Importzeiger auf Kernvariablen.  Nie die Namen selbst benutzen:
 *  unser Bauweg bindet dafuer eine Funktionsbruecke ein, und man liest
 *  Maschinencode (Bluescreen 0x50 mit KeQueryTickCount).  */
extern volatile KSYSTEM_TIME *_imp__KeTickCount;

/*  Protokoll der letzten 64 Berichte (IOCTL_NTU_MAUS_PROTOKOLL).  Unter
 *  ntu_maus_sperre.  */
static NTU_MAUS_EINTRAG ntu_mprot[64];
static ULONG            ntu_mprot_n;
static NTU_MAUS_EINTRAG ntu_mtast[32];      /* nur Tastenwechsel */
static ULONG            ntu_mtast_n;

void
ntu_maus_protokoll_holen (NTU_MAUS_PROTOKOLL *p)
{
    KIRQL alt;
    ULONG k, n, erst;

    KeAcquireSpinLock(&ntu_maus_sperre, &alt);
    n = (ntu_mprot_n < 64) ? ntu_mprot_n : 64;
    erst = ntu_mprot_n - n;
    for (k = 0; k < n; k += 1) {
        p->E[k] = ntu_mprot[(erst + k) % 64];
    }
    p->Anzahl = n;
    p->Gesamt = ntu_mprot_n;
    n = (ntu_mtast_n < 32) ? ntu_mtast_n : 32;
    erst = ntu_mtast_n - n;
    for (k = 0; k < n; k += 1) {
        p->T[k] = ntu_mtast[(erst + k) % 32];
    }
    p->TAnzahl = n;
    KeReleaseSpinLock(&ntu_maus_sperre, alt);
}

static VOID
ntu_maus_protokoll_eintragen (const uint8_t *r, int len, uint8_t alle, uint8_t vorher,
                              USHORT flags, int dx, int dy, int geliefert)
{
    NTU_MAUS_EINTRAG e;
    LARGE_INTEGER    t;
    KIRQL            alt;
    int              k;

    /*  Zeit nach dem SYSTEMTAKT (KeTickCount), wie GetTickCount -- nicht
     *  nach dem Leistungszaehler: der liest unter halmps den Zeitstempel-
     *  zaehler des gerade laufenden Prozessors, und die laufen auf
     *  mehreren Prozessoren nicht gleich (in QEMU 22 s Versatz).
     *
     *  Und NICHT ueber das DDK-Makro KeQueryTickCount: es liest die
     *  exportierte VARIABLE ueber &KeTickCount, und ohne Importkenn-
     *  zeichnung bindet unser Bauweg dafuer eine Funktionsbruecke ein --
     *  das Makro las Maschinencode als Zeiger (Bluescreen 0x50).  Hier
     *  direkt der Importzeiger __imp__KeTickCount.  */
    {
        ULONG hoch, niedrig;
        do {
            hoch    = (ULONG)_imp__KeTickCount->High1Time;
            niedrig = _imp__KeTickCount->LowPart;
        } while (hoch != (ULONG)_imp__KeTickCount->High2Time);
        t.HighPart = (LONG)hoch;
        t.LowPart  = niedrig;
        memset(&e, 0, sizeof(e));
        e.Ms = (unsigned long)((t.QuadPart * KeQueryTimeIncrement()) / 10000);
    }
    e.Flags     = flags;
    e.Dx        = (short)dx;
    e.Dy        = (short)dy;
    e.Len       = (unsigned char)len;
    e.Alle      = alle;
    e.Vorher    = vorher;
    e.Geliefert = (unsigned char)geliefert;
    for (k = 0; k < 8 && k < len; k += 1) {
        e.Roh[k] = r[k];
    }
    KeAcquireSpinLock(&ntu_maus_sperre, &alt);
    ntu_mprot[ntu_mprot_n % 64] = e;
    ntu_mprot_n += 1;
    if (alle != vorher) {
        ntu_mtast[ntu_mtast_n % 32] = e;
        ntu_mtast_n += 1;
    }
    KeReleaseSpinLock(&ntu_maus_sperre, alt);
}

static VOID
ntums_liefern (struct ntums_softc *sc, MOUSE_INPUT_DATA *m, ULONG n)
{
    KIRQL old;
    ULONG verbraucht = 0;

    if (sc->sc_conn == 0 || sc->sc_conn->ClassService == 0 || n == 0) {
        return;
    }
    InterlockedExchangeAdd((LONG *)&ntu_zaehler.MausMeldungen, (LONG)n);

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    sc->sc_conn->ClassService(sc->sc_conn->ClassDeviceObject, &m[0], &m[n], &verbraucht);
    KeLowerIrql(old);
}

static VOID
ntums_melden (struct ntums_softc *sc)
{
    ntums_liefern(sc, &sc->sc_one, 1);
}

/*  Doppelklick fuer die Belegung "doppelklick": zwei vollstaendige
 *  Linksklicks als vier Meldungen am Stueck -- so kommen sie auch bei
 *  i8042prt an, und USER erkennt daran den Doppelklick.  */

static VOID
ntums_doppelklick (struct ntums_softc *sc)
{
    MOUSE_INPUT_DATA m[4];
    int              k;

    memset(m, 0, sizeof(m));
    for (k = 0; k < 4; k += 1) {
        m[k].Flags       = MOUSE_MOVE_RELATIVE;
        m[k].ButtonFlags = (k & 1) ? MOUSE_LEFT_BUTTON_UP : MOUSE_LEFT_BUTTON_DOWN;
    }
    ntums_liefern(sc, m, 4);
}

/*  Der gemeinsame Kern: Knoepfe, Bewegung, Rad -> MOUSE_INPUT_DATA.
 *  Gemeldet werden Knopf-WECHSEL (DOWN/UP), wie i8042prt es tut.  */

static VOID
ntums_werte (struct ntums_softc *sc, uint8_t alle, LONG dx, LONG dy, LONG rad,
             const uint8_t *r, int len)
{
    uint8_t vorher_alle = sc->sc_last_buttons;
    uint8_t jetzt, vorher = sc->sc_last_basis;
    USHORT  flags  = 0;
    int     doppel = 0, k;

    /*  Alle Tasten 1..8 laufen durch ihre Belegung.  Beim Druecken wird
     *  die geltende festgehalten, beim Loslassen genau diese geloest.
     *  Was NT als Tasten 1..3 sieht, ergibt sich aus Zaehlern: wie viele
     *  Tasten gerade "klick-links", "klick-rechts" oder "mitte" halten --
     *  zwei Tasten auf links: links bleibt, bis beide los sind.  */

    for (k = 0; k < NTU_MAUS_TASTEN; k += 1) {

        uint8_t         bit = (uint8_t)(1 << k);
        NTU_MAUSAKTION *a   = &sc->sc_aktiv[k];
        int             gedrueckt = (alle & bit) != 0;
        int             d = gedrueckt ? 1 : -1;
        KIRQL           irql;

        if ((alle & bit) == (vorher_alle & bit)) {
            continue;
        }

        if (gedrueckt) {
            KeAcquireSpinLock(&ntu_maus_sperre, &irql);
            *a = ntu_mausaktion[k];
            KeReleaseSpinLock(&ntu_maus_sperre, irql);
        }

        switch (a->art) {
            case NTU_MA_TASTEN:
                ntu_kbd_einspeisen(a->code, a->n, !gedrueckt);
                break;
            case NTU_MA_LINKS:
                sc->sc_halten[0] += d;
                break;
            case NTU_MA_RECHTS:
                sc->sc_halten[1] += d;
                break;
            case NTU_MA_MITTE:
                sc->sc_halten[2] += d;
                break;
            case NTU_MA_DOPPEL:
                doppel |= gedrueckt;
                break;
            default:
                break;
        }
    }

    for (k = 0; k < 3; k += 1) {
        if (sc->sc_halten[k] < 0) sc->sc_halten[k] = 0;
    }

    sc->sc_last_buttons = alle;

    /*  Fuer die Systemsteuerung "USB-Maus": die Tasten roh, wie sie von
     *  der Maus kommen -- auch 4..8, die Windows nicht kennt.  */

    InterlockedExchange((LONG *)&ntu_maus_roh, (LONG)alle);
    InterlockedIncrement((LONG *)&ntu_maus_berichte);

    jetzt = (uint8_t)((sc->sc_halten[0] > 0 ? 1 : 0) |
                      (sc->sc_halten[1] > 0 ? 2 : 0) |
                      (sc->sc_halten[2] > 0 ? 4 : 0));

    if ((jetzt & 1) != (vorher & 1)) {
        flags |= (jetzt & 1) ? MOUSE_LEFT_BUTTON_DOWN
                             : MOUSE_LEFT_BUTTON_UP;
    }

    if ((jetzt & 2) != (vorher & 2)) {
        flags |= (jetzt & 2) ? MOUSE_RIGHT_BUTTON_DOWN
                             : MOUSE_RIGHT_BUTTON_UP;
    }

    if ((jetzt & 4) != (vorher & 4)) {
        flags |= (jetzt & 4) ? MOUSE_MIDDLE_BUTTON_DOWN
                             : MOUSE_MIDDLE_BUTTON_UP;
    }


    sc->sc_last_basis = jetzt;

    memset(&sc->sc_one, 0, sizeof(sc->sc_one));
    sc->sc_one.Flags       = MOUSE_MOVE_RELATIVE;
    sc->sc_one.ButtonFlags = flags;
    sc->sc_one.RawButtons  = jetzt;
    sc->sc_one.LastX       = dx;
    sc->sc_one.LastY       = dy;

    /*  Rad: ein Rastschritt sind 120 (WHEEL_DELTA).  NT 4.0 kennt
     *  WM_MOUSEWHEEL; Tasten 4 und 5 kennt es nicht (kamen mit 2000).  */

    if (rad != 0) {
        sc->sc_one.ButtonFlags |= MOUSE_WHEEL;
        sc->sc_one.ButtonData   = (USHORT)(int16_t)(rad * 120);
    }

#if defined(NTU_EINGABE_PROTOKOLL)      /* nur Diagnosebau: je Bericht! */

    /*  Diagnose: jeden Bericht roh, mit Laenge, und was daraus wurde.
     *  NUR im Diagnosebau -- nicht im Herzschlag-Bau: eine echte Maus
     *  meldet bis zu 1000-mal je Sekunde, und eine Zeile je Bericht auf
     *  COM1 bremst dann die Messung, die sie ermoeglichen soll.  */

    ntu_printf("ums: %u byte: %02x %02x %02x %02x %02x %02x %02x %02x"
               "  -> %s knoepfe %x vorher %x dx %d dy %d rad %d flags %04x\n", len,
               r[0], (len > 1) ? r[1] : 0, (len > 2) ? r[2] : 0,
               (len > 3) ? r[3] : 0, (len > 4) ? r[4] : 0, (len > 5) ? r[5] : 0,
               (len > 6) ? r[6] : 0, (len > 7) ? r[7] : 0,
               sc->sc_hid ? "hid" : "start", alle, vorher_alle, dx, dy, rad, sc->sc_one.ButtonFlags);
#endif
    /*  Leere Meldungen nicht weitergeben: keine Bewegung, kein Tasten-
     *  wechsel, kein Rad.  Echte Maeuse melden oft dauernd -- beim
     *  Bewegen bis 1000-mal je Sekunde, manche auch in Ruhe (wenn sie
     *  SET_IDLE 0 nicht beachten).  Jede leere Meldung landete sonst in
     *  der Warteschlange von mouclass, und ein Klick stand dahinter an.
     *  i8042prt gibt bei PS/2 auch nur Pakete mit Inhalt weiter.  */

    ntu_maus_protokoll_eintragen(r, len, alle, vorher_alle, sc->sc_one.ButtonFlags, dx, dy,
                                 sc->sc_one.ButtonFlags != 0 || dx != 0 || dy != 0);

    if (sc->sc_one.ButtonFlags != 0 || dx != 0 || dy != 0) {
        ntums_melden(sc);
    } else {
        InterlockedIncrement((LONG *)&ntu_maus_leer);
    }
    if (doppel) {
        ntums_doppelklick(sc);
    }
}

/*  Startprotokoll: Byte 0 Knoepfe, 1/2 Bewegung, 3 Rad (freiwillig).  */

static VOID
ntums_bericht (struct ntums_softc *sc, const uint8_t *r, int len)
{
    /*  Die Umwandlung ueber int8_t ist noetig: aus dem Puffer kommen die
     *  Byte vorzeichenlos, und eine Bewegung nach links waere sonst eine
     *  Bewegung um 250 nach rechts.  */

    ntums_werte(sc, (uint8_t)(r[0] & 0x07), (LONG)(int8_t)r[1],
                (LONG)(int8_t)r[2], (len > 3) ? (LONG)(int8_t)r[3] : 0, r, len);
}

/*  Volles Format: die Stellen stehen im Deskriptor.  Die Berichtsnummer
 *  (falls es eine gibt) steht vorn und gehoert NICHT zu den Stellen --
 *  so zaehlt FreeBSDs Parser.  */

static VOID
ntums_bericht_hid (struct ntums_softc *sc, const uint8_t *r, int len)
{
    const uint8_t *p = r;
    int            n = len, k;
    uint8_t        knoepfe = 0;

    if (sc->sc_iid != 0) {
        if (n < 1 || p[0] != sc->sc_iid) {
            return;                     /* ein anderer Bericht dieser Schnittstelle */
        }
        p += 1;
        n -= 1;
    }

    for (k = 0; k < sc->sc_n_knopf; k += 1) {
        if (hid_get_data(p, n, &sc->sc_loc_knopf[k]) != 0) {
            knoepfe |= (uint8_t)(1 << k);
        }
    }

    ntums_werte(sc, knoepfe,
                (LONG)hid_get_data(p, n, &sc->sc_loc_x),
                (LONG)hid_get_data(p, n, &sc->sc_loc_y),
                sc->sc_rad ? (LONG)hid_get_data(p, n, &sc->sc_loc_rad) : 0,
                r, len);
}

/*  Den Berichtsdeskriptor holen und darin X, Y, Rad, Knoepfe finden --
 *  wie FreeBSDs ums.  1 = volles Format nutzbar.  */

static int
ntums_deskriptor (struct ntums_softc *sc)
{
    void     *d = NULL;
    uint16_t  n = 0;
    uint32_t  fl;
    uint8_t   id;
    int       k, ok = 0;

    if (usbd_req_get_hid_desc(sc->sc_udev, NULL, &d, &n, M_TEMP,
                              sc->sc_iface_index) != 0 || d == NULL) {
        return 0;
    }

    if (hid_locate(d, n, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X), hid_input, 0,
                   &sc->sc_loc_x, &fl, &sc->sc_iid) && (fl & HIO_RELATIVE) &&
        hid_locate(d, n, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y), hid_input, 0,
                   &sc->sc_loc_y, &fl, &id) && id == sc->sc_iid) {

        sc->sc_rad = hid_locate(d, n, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_WHEEL),
                                hid_input, 0, &sc->sc_loc_rad, &fl, &id) &&
                     id == sc->sc_iid;

        for (k = 0; k < 8; k += 1) {
            if (!hid_locate(d, n, HID_USAGE2(HUP_BUTTON, k + 1), hid_input, 0,
                            &sc->sc_loc_knopf[k], &fl, &id) || id != sc->sc_iid) {
                break;
            }
        }

        sc->sc_n_knopf = k;
        ok = (k > 0);
    }

    free(d, M_TEMP);
    return ok;
}


static void
ntums_intr_callback (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntums_softc    *sc = usbd_xfer_softc(xfer);
    struct usb_page_cache *pc;
    uint8_t                buf[64];
    int                    len;

    usbd_xfer_status(xfer, &len, NULL, NULL, NULL);

    switch (USB_GET_STATE(xfer)) {

        case USB_ST_TRANSFERRED:

            if (len > (int)sizeof(buf)) {
#if defined(NTU_EINGABE_PROTOKOLL)      /* nur Diagnosebau: je Bericht! */
                /*  Laenger als ein Startprotokoll-Bericht: die Maus spricht
                 *  ihr eigenes Format.  Fuer die Diagnose festhalten, bevor
                 *  gekuerzt wird.  */
                ntu_printf("ums: bericht war %d byte lang, gekuerzt auf %u\n",
                           len, (unsigned)sizeof(buf));
#endif
                len = (int)sizeof(buf);
            }

            if (len >= 1) {
                memset(buf, 0, sizeof(buf));
                pc = usbd_xfer_get_frame(xfer, 0);
                usbd_copy_out(pc, 0, buf, len);

                if (sc->sc_hid) {
                    ntums_bericht_hid(sc, buf, len);
                } else if (len >= 3) {
                    ntums_bericht(sc, buf, len);
                }
            }

            /* FALLTHROUGH */

        case USB_ST_SETUP:
tr_setup:
            usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
            usbd_transfer_submit(xfer);
            break;

        default:

            if (error != USB_ERR_CANCELLED) {
                usbd_xfer_set_stall(xfer);
                goto tr_setup;
            }

            break;
    }
}

static int
ntums_probe (device_t dev)
{
    struct usb_attach_arg *uaa = device_get_ivars(dev);

    if (uaa->usb_mode != USB_MODE_HOST) {
        return ENXIO;
    }

    if (uaa->info.bInterfaceClass != UICLASS_HID) {
        return ENXIO;
    }

    if (uaa->info.bInterfaceSubClass == UISUBCLASS_BOOT &&
        uaa->info.bInterfaceProtocol == UIPROTO_MOUSE) {
        return BUS_PROBE_DEFAULT;
    }

    /*  Auch Maeuse ohne Boot-Kennzeichen (manche Funk- und Spielemaeuse):
     *  entscheidet der Berichtsdeskriptor, wie bei FreeBSDs ums.  Bisher
     *  landeten sie roh bei UsbDevN.  Tastaturen (Boot-Protokoll 1)
     *  gehoeren ntukbd; Joysticks und Gamepads beschreiben sich nicht als
     *  Maus und bleiben unberuehrt.  */

    if (uaa->info.bInterfaceProtocol != UIPROTO_BOOT_KEYBOARD) {

        void     *d = NULL;
        uint16_t  n = 0;
        int       maus = 0;

        if (usbd_req_get_hid_desc(uaa->device, NULL, &d, &n, M_TEMP,
                                  uaa->info.bIfaceIndex) == 0 && d != NULL) {

            /*  "Maus" UND relatives X.  Ein Grafiktablett beschreibt sich
             *  ebenfalls als Maus, aber mit absoluten Koordinaten -- als
             *  Mausbewegung gedeutet, spraenge der Zeiger umher.  Tabletts
             *  bleiben bei UsbDevN.  */

            struct hid_location l;
            uint32_t            fl = 0;
            uint8_t             id;

            maus = hid_is_mouse(d, n) &&
                   hid_locate(d, n, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X),
                              hid_input, 0, &l, &fl, &id) &&
                   (fl & HIO_RELATIVE);
            free(d, M_TEMP);
        }

        if (maus) {
            return BUS_PROBE_DEFAULT;
        }
    }

    return ENXIO;
}

static int
ntums_attach (device_t dev)
{
    struct ntums_softc    *sc  = device_get_softc(dev);
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    int err;

    sc->sc_dev         = dev;
    sc->sc_udev        = uaa->device;
    sc->sc_iface_index = uaa->info.bIfaceIndex;

    mtx_init(&sc->sc_mtx, "ntums", NULL, MTX_DEF);

    /*  Volles Format, wenn der Deskriptor X, Y und mindestens einen Knopf
     *  beschreibt.  Dann ausdruecklich ins Berichtsprotokoll (1) schalten:
     *  hat das BIOS die Maus fuer den Start ins Startprotokoll gesetzt,
     *  passten Bericht und Deskriptor sonst nicht zusammen.  Sonst wie
     *  bisher ins Startprotokoll (0).  */

    sc->sc_hid = ntums_deskriptor(sc);

    if (sc->sc_hid) {
        (void)usbd_req_set_protocol(sc->sc_udev, NULL, sc->sc_iface_index, 1);
        device_set_desc(dev, "USB-Maus (Berichtsdeskriptor)");
        ntu_printf("ums: Berichtsdeskriptor: %d Knoepfe%s%s, Bericht %u\n",
                   sc->sc_n_knopf, sc->sc_rad ? ", Rad" : "",
                   (sc->sc_loc_x.size > 8) ? ", 16-Bit-Bewegung" : "",
                   sc->sc_iid);
    } else {
        (void)usbd_req_set_protocol(sc->sc_udev, NULL, sc->sc_iface_index, 0);
        device_set_desc(dev, "USB-Maus (Startprotokoll)");
        ntu_printf("ums: Startprotokoll (kein brauchbarer Deskriptor)\n");
    }
    (void)usbd_req_set_idle(sc->sc_udev, NULL, sc->sc_iface_index, 0, 0);

    err = usbd_transfer_setup(uaa->device, &sc->sc_iface_index,
                              sc->sc_xfer, ntums_config,
                              NTUMS_N_TRANSFER, sc, &sc->sc_mtx);

    if (err != 0) {
        device_printf(dev, "usbd_transfer_setup: %d\n", err);
        mtx_destroy(&sc->sc_mtx);
        return ENXIO;
    }

    sc->sc_conn = ntu_mou_anmelden();

    /*  Fuer die Systemsteuerung "USB-Maus": wie viele Maeuse, wie viele
     *  Tasten hat die groesste (Boot-Protokoll: drei).  */

    InterlockedIncrement((LONG *)&ntu_maus_anzahl);
    {
        LONG n = (sc->sc_n_knopf > 0) ? sc->sc_n_knopf : 3;
        if (n > ntu_maus_knoepfe) {
            InterlockedExchange((LONG *)&ntu_maus_knoepfe, n);
        }
    }

    mtx_lock(&sc->sc_mtx);
    usbd_transfer_start(sc->sc_xfer[NTUMS_INTR_DT]);
    mtx_unlock(&sc->sc_mtx);

    return 0;
}

static int
ntums_detach (device_t dev)
{
    struct ntums_softc *sc = device_get_softc(dev);

    ntu_mou_abmelden();

    InterlockedDecrement((LONG *)&ntu_maus_anzahl);
    sc->sc_conn = 0;

    usbd_transfer_unsetup(sc->sc_xfer, NTUMS_N_TRANSFER);
    mtx_destroy(&sc->sc_mtx);
    return 0;
}

static device_method_t ntums_methods[] = {
    DEVMETHOD(device_probe,  ntums_probe),
    DEVMETHOD(device_attach, ntums_attach),
    DEVMETHOD(device_detach, ntums_detach),
    DEVMETHOD_END
};

static driver_t ntums_driver = {
    .name    = "ums",
    .methods = ntums_methods,
    .size    = sizeof(struct ntums_softc),
};

DRIVER_MODULE(ums, uhub, ntums_driver, 0, 0);
