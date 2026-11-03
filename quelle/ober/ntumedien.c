/*
 *  ober/ntumedien.c -- Medien- und Systemtasten der USB-Tastatur.
 *
 *  Viele Tastaturen melden Play/Pause, Lautstaerke, Rechner, E-Mail ...
 *  und Ausschalten/Ruhezustand auf einer ZWEITEN HID-Schnittstelle
 *  ("Consumer Control", Seite 0x0C; "System Control", Seite 0x01,
 *  Nutzung 0x81..0x83).  Bisher landete sie roh bei UsbDevN.
 *
 *  Uebernommen wird eine HID-Schnittstelle, deren Berichtsdeskriptor
 *  solche Tasten als Eingabe beschreibt und die keine Maus und keine
 *  Starttastatur ist (die gehoeren ntums und ntukbd).
 *
 *  Beide Bauarten von Deskriptoren:
 *    Bitfeld  (HIO_VARIABLE): ein Bit je Taste,
 *    Liste    (Array): die Nummern der gedrueckten Tasten, Nutzung =
 *             Usage Minimum + Wert.
 *  Aus jedem Bericht wird die Menge der gedrueckten Sondertasten gebildet
 *  (Index in ntsonder.h), mit dem vorigen Bericht DERSELBEN Berichts-
 *  nummer verglichen, und jedes Druecken/Loslassen geht an
 *  ntu_sonder_taste() -- was die Taste tut, sagt dort ihre Belegung.
 *  Tasten, die nicht in der Tabelle stehen, werden einmal gemeldet.
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

#define NTUMED_FELDER  32
#define NTUMED_IDS      8

struct ntumed_feld {
    struct hid_location loc;
    uint8_t             id;          /* Berichtsnummer, 0 = keine */
    uint8_t             liste;       /* 1 = Array */
    uint32_t            nutzung;     /* Bitfeld: Seite<<16 | Nutzung */
    uint32_t            umin, umax;  /* Liste: Bereich */
    int32_t             lmin;        /* Liste: kleinster Wert */
};

enum { NTUMED_EIN, NTUMED_N };

struct ntumed_softc {
    device_t            sc_dev;
    struct usb_device  *sc_udev;
    struct mtx          sc_mtx;
    struct usb_xfer    *sc_xfer[NTUMED_N];
    uint8_t             sc_iface_index;
    int                 sc_n;
    struct ntumed_feld  sc_feld[NTUMED_FELDER];
    int                 sc_mit_id;          /* Berichte tragen eine Nummer */
    uint8_t             sc_last_id[NTUMED_IDS];
    uint32_t            sc_last[NTUMED_IDS];   /* gedrueckt, je Berichtsnummer */
    uint32_t            sc_unbekannt[8];
    int                 sc_n_unbekannt;
};

static usb_callback_t ntumed_intr;

static const struct usb_config ntumed_konfig[NTUMED_N] = {
    [NTUMED_EIN] = {
        .type      = UE_INTERRUPT,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_IN,
        .flags     = { .pipe_bof = 1, .short_xfer_ok = 1 },
        .bufsize   = 0,
        .callback  = &ntumed_intr,
    },
};

/*  Gehoert diese Nutzung zu uns?  */
static int
ntumed_unsere (uint32_t u)
{
    uint16_t seite = (uint16_t)(u >> 16), n = (uint16_t)u;
    return seite == 0x0C || (seite == 0x01 && n >= 0x81 && n <= 0x83);
}

/*  Deskriptor lesen: die Eingabefelder mit Medien-/Systemtasten.  */
static int
ntumed_felder (const void *d, uint16_t len, struct ntumed_feld *f, int max, int *mit_id)
{
    struct hid_data *h;
    struct hid_item  it;
    int              n = 0;

    *mit_id = 0;
    h = hid_start_parse(d, len, 1 << hid_input);
    if (h == NULL) {
        return 0;
    }
    while (hid_get_item(h, &it) && n < max) {
        if (it.report_ID != 0) {
            *mit_id = 1;
        }
        if (it.kind != hid_input || (it.flags & HIO_CONST)) {
            continue;
        }
        if (it.flags & HIO_VARIABLE) {
            if (!ntumed_unsere(it.usage)) continue;
            f[n].liste   = 0;
            f[n].nutzung = it.usage;
        } else {
            if (!ntumed_unsere(it.usage_minimum)) continue;
            f[n].liste = 1;
            f[n].umin  = it.usage_minimum;
            f[n].umax  = it.usage_maximum;
            f[n].lmin  = it.logical_minimum;
        }
        f[n].loc = it.loc;
        f[n].id  = it.report_ID;
        n += 1;
    }
    hid_end_parse(h);
    return n;
}

static void
ntumed_unbekannt (struct ntumed_softc *sc, uint32_t u)
{
    int k;
    for (k = 0; k < sc->sc_n_unbekannt; k += 1) {
        if (sc->sc_unbekannt[k] == u) return;
    }
    if (sc->sc_n_unbekannt < 8) {
        sc->sc_unbekannt[sc->sc_n_unbekannt++] = u;
        ntu_printf("umed: Taste %02x:%04x steht nicht in der Tabelle -- ohne Wirkung\n",
                   (unsigned)(u >> 16), (unsigned)(u & 0xFFFF));
    }
}

/*  Ein Bericht.  */
static void
ntumed_bericht (struct ntumed_softc *sc, const uint8_t *p, int len)
{
    uint8_t  id = 0;
    uint32_t jetzt = 0, vorher, wechsel;
    int      k, slot;

    if (sc->sc_mit_id) {
        if (len < 1) return;
        id = p[0];
        p += 1;
        len -= 1;
    }

    for (k = 0; k < sc->sc_n; k += 1) {
        struct ntumed_feld *f = &sc->sc_feld[k];
        if (f->id != id) continue;

        if (!f->liste) {
            if (hid_get_data(p, len, &f->loc) != 0) {
                int i = ntu_sonder_index(f->nutzung >> 16, f->nutzung & 0xFFFF);
                if (i >= 0) jetzt |= 1ul << i; else ntumed_unbekannt(sc, f->nutzung);
            }
        } else {
            struct hid_location l = f->loc;
            uint32_t            c, anz = (l.count > 0) ? l.count : 1;
            for (c = 0; c < anz; c += 1) {
                int32_t  v;
                uint32_t u;
                l.pos   = f->loc.pos + c * f->loc.size;
                l.count = 1;
                v = hid_get_udata(p, len, &l);
                if (v == 0 || v < f->lmin) continue;             /* 0 = keine Taste */
                u = f->umin + (uint32_t)(v - f->lmin);
                if (u > f->umax) continue;
                {
                    int i = ntu_sonder_index(u >> 16, u & 0xFFFF);
                    if (i >= 0) jetzt |= 1ul << i; else ntumed_unbekannt(sc, u);
                }
            }
        }
    }

    /*  Vorigen Stand derselben Berichtsnummer suchen (oder Platz nehmen).  */
    slot = 0;
    for (k = 0; k < NTUMED_IDS; k += 1) {
        if (sc->sc_last_id[k] == id) { slot = k; break; }
        if (sc->sc_last_id[k] == 0xFF) { slot = k; sc->sc_last_id[k] = id; sc->sc_last[k] = 0; break; }
    }
    vorher = sc->sc_last[slot];
    wechsel = vorher ^ jetzt;
    sc->sc_last[slot] = jetzt;

    for (k = 0; k < NTU_SONDER_ANZAHL; k += 1) {        /* erst loslassen */
        if ((wechsel & (1ul << k)) && !(jetzt & (1ul << k))) ntu_sonder_taste(k, 0);
    }
    for (k = 0; k < NTU_SONDER_ANZAHL; k += 1) {
        if ((wechsel & (1ul << k)) && (jetzt & (1ul << k))) ntu_sonder_taste(k, 1);
    }
}

static void
ntumed_intr (struct usb_xfer *x, usb_error_t fehler)
{
    struct ntumed_softc   *sc = usbd_xfer_softc(x);
    struct usb_page_cache *pc;
    uint8_t                puffer[64];
    int                    len;

    usbd_xfer_status(x, &len, NULL, NULL, NULL);

    switch (USB_GET_STATE(x)) {
        case USB_ST_TRANSFERRED:
            if (len > (int)sizeof(puffer)) len = sizeof(puffer);
            pc = usbd_xfer_get_frame(x, 0);
            usbd_copy_out(pc, 0, puffer, len);
            ntumed_bericht(sc, puffer, len);
            /* FALLTHROUGH */
        case USB_ST_SETUP:
        weiter:
            usbd_xfer_set_frame_len(x, 0, usbd_xfer_max_len(x));
            usbd_transfer_submit(x);
            break;
        default:
            if (fehler != USB_ERR_CANCELLED) {
                usbd_xfer_set_stall(x);
                goto weiter;
            }
            break;
    }
}

static int
ntumed_probe (device_t dev)
{
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    struct ntumed_feld     f[NTUMED_FELDER];
    void                  *d = NULL;
    uint16_t               n = 0;
    int                    anz = 0, mit_id;

    if (uaa->usb_mode != USB_MODE_HOST || uaa->info.bInterfaceClass != UICLASS_HID ||
        uaa->info.bInterfaceProtocol == UIPROTO_BOOT_KEYBOARD ||
        uaa->info.bInterfaceProtocol == UIPROTO_MOUSE) {
        return ENXIO;
    }
    if (usbd_req_get_hid_desc(uaa->device, NULL, &d, &n, M_TEMP, uaa->info.bIfaceIndex) != 0 ||
        d == NULL) {
        return ENXIO;
    }
    if (!hid_is_mouse(d, n)) {
        anz = ntumed_felder(d, n, f, NTUMED_FELDER, &mit_id);
    }
    free(d, M_TEMP);
    return (anz > 0) ? BUS_PROBE_DEFAULT : ENXIO;
}

static int
ntumed_attach (device_t dev)
{
    struct ntumed_softc   *sc  = device_get_softc(dev);
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    void                  *d = NULL;
    uint16_t               n = 0;
    int                    err, k;

    sc->sc_dev         = dev;
    sc->sc_udev        = uaa->device;
    sc->sc_iface_index = uaa->info.bIfaceIndex;
    for (k = 0; k < NTUMED_IDS; k += 1) sc->sc_last_id[k] = 0xFF;

    if (usbd_req_get_hid_desc(uaa->device, NULL, &d, &n, M_TEMP, uaa->info.bIfaceIndex) != 0 ||
        d == NULL) {
        return ENXIO;
    }
    sc->sc_n = ntumed_felder(d, n, sc->sc_feld, NTUMED_FELDER, &sc->sc_mit_id);
    free(d, M_TEMP);
    if (sc->sc_n == 0) {
        return ENXIO;
    }

    device_set_desc(dev, "USB-Tastatur: Medien- und Systemtasten");
    mtx_init(&sc->sc_mtx, "ntumed", NULL, MTX_DEF);

    usbd_req_set_idle(sc->sc_udev, NULL, sc->sc_iface_index, 0, 0);

    err = usbd_transfer_setup(sc->sc_udev, &sc->sc_iface_index, sc->sc_xfer,
                              ntumed_konfig, NTUMED_N, sc, &sc->sc_mtx);
    if (err != 0) {
        device_printf(dev, "usbd_transfer_setup: %s\n", usbd_errstr(err));
        mtx_destroy(&sc->sc_mtx);
        return ENXIO;
    }
    InterlockedIncrement((LONG *)&ntu_sonder_tastaturen);
    ntu_printf("umed: Medien-/Systemtasten, %d Felder%s\n", sc->sc_n,
               sc->sc_mit_id ? ", mit Berichtsnummern" : "");

    mtx_lock(&sc->sc_mtx);
    usbd_transfer_start(sc->sc_xfer[NTUMED_EIN]);
    mtx_unlock(&sc->sc_mtx);
    return 0;
}

static int
ntumed_detach (device_t dev)
{
    struct ntumed_softc *sc = device_get_softc(dev);
    int                  k, j;

    usbd_transfer_unsetup(sc->sc_xfer, NTUMED_N);

    /*  Noch gehaltene Tasten loslassen -- sonst bliebe z. B. eine auf
     *  "strg+c" belegte Taste bei Windows gedrueckt.  */
    for (j = 0; j < NTUMED_IDS; j += 1) {
        for (k = 0; k < NTU_SONDER_ANZAHL; k += 1) {
            if (sc->sc_last[j] & (1ul << k)) ntu_sonder_taste(k, 0);
        }
        sc->sc_last[j] = 0;
    }
    InterlockedDecrement((LONG *)&ntu_sonder_tastaturen);
    mtx_destroy(&sc->sc_mtx);
    return 0;
}

static device_method_t ntumed_methods[] = {
    DEVMETHOD(device_probe,  ntumed_probe),
    DEVMETHOD(device_attach, ntumed_attach),
    DEVMETHOD(device_detach, ntumed_detach),
    DEVMETHOD_END
};

static driver_t ntumed_driver = {
    .name    = "ntumedien",
    .methods = ntumed_methods,
    .size    = sizeof(struct ntumed_softc),
};

DRIVER_MODULE(ntumedien, uhub, ntumed_driver, 0, 0);
