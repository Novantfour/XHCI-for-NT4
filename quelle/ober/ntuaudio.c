/*
 *  ober/ntuaudio.c -- USB-Audio: isochroner Strom fuer USBAUDIO.SYS.
 *
 *  Uebernimmt AudioStreaming-Schnittstellen (Klasse 01/02) und legt je
 *  Schnittstelle \Device\UsbAudioN an.  Auf NTUA_START wird die
 *  Alternative gewaehlt, ggf. die Abtastrate gesetzt und der Strom
 *  gestartet: ZWEI isochrone Uebertragungen zu je NTUA_RAHMEN Rahmen,
 *  beide sofort abgeschickt.  Jeder Rueckruf befuellt seine Uebertragung
 *  aus dem Ring neu und schickt sie gleich wieder ab -- waehrend die
 *  andere laeuft.  So steht immer eine im Zeitplan, und es entsteht keine
 *  Luecke.  Das ist FreeBSDs uaudio-Muster (UAUDIO_NCHANBUFS = 2).
 *
 *  Gemessen vorher, ueber ntugen (synchron, ein Aufruf nach dem anderen):
 *  je Aufruf 2-3 ms Luecke, weil der Startrahmen des naechsten Aufrufs
 *  schon vorbei ist und der Stapel ihn in die Zukunft verlegt.
 *
 *  Rahmenlaengen: Bruchteil-Akkumulator (Rate / Rahmen je Sekunde), so
 *  dass z. B. 44,1 kHz bei Full Speed 9 x 44 + 1 x 45 Abtastungen je
 *  10 ms ergibt.  Fehlen Daten im Ring, geht Stille hinaus und der
 *  Rahmen zaehlt als Unterlauf -- erst, nachdem einmal Daten kamen.
 *
 *  Aufnahme (Endpunkt mit 0x80): dieselben zwei Uebertragungen, jeder
 *  Rahmen mit der Hoechstlaenge angefordert; was das Geraet je Rahmen
 *  wirklich liefert, geht in den Ring (Rahmen k liegt bei k * mfl, wie in
 *  FreeBSDs uaudio).  Der Benutzermodus holt mit NTUA_LESEN ab; ist der
 *  Ring voll, wird verworfen und als Ueberlauf gezaehlt.
 *
 *  Rueckmelde-Endpunkte (asynchrone Senken) und UAC2-Takt kommen spaeter;
 *  QEMUs usb-audio, CM108 und PCM2902 brauchen sie nicht.
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
#include <ober/ntkbdmou.h>
#include <ober/ntuaudio.h>
#include <nt/ntpaged.h>

#define NTUA_MAX      8
#define NTUA_N        2                /* Uebertragungen im Wechsel */
#define NTUA_RAHMEN   8                /* Rahmen je Uebertragung (ms bei FS) */
#define NTUA_RING     65536

/*  Mehrere Tonquellen gleichzeitig (Anwenderbericht: "es geht nur eine
 *  Tonquelle, entweder Player oder Systemklang").  Vorbild ist der
 *  EMU10K1-Treiber fuer NT 4.0 (wavekern.c): dort fuehrt der Treiber ein
 *  Feld gKlient[] -- jeder Oeffner bekommt einen eigenen Ring -- und ein
 *  Softwaremischer addiert im DPC alle aktiven Stroeme zu einer Summe.
 *  Mischen im Benutzermodus hilft nicht: die winmm-DLL wird je Prozess
 *  geladen, zwei Programme koennten sich gar nicht absprechen.
 *
 *  Die Lautstaerke wirkt (wie dort) auf die Summe, nicht je Strom --
 *  sie sitzt ohnehin im Geraet.  */
#define NTUA_KLIENTEN  8

int  ntu_amdpll_betrifft(void *hc);
VOID ntu_amdpll_isoc_an(VOID);
VOID ntu_amdpll_isoc_aus(VOID);

/*  Ein Oeffner des Wiedergabegeraets.  */
struct ntua_klient {
    PFILE_OBJECT    k_besitzer;        /* 0 = Platz frei */
    int             k_laeuft;          /* START gesehen */
    uint8_t        *k_ring;
    uint32_t        k_ringgr, k_kopf, k_fuell;
    uint32_t        k_gesp_lo, k_gesp_hi;
    uint32_t        k_unterlaeufe, k_ueberlaeufe;
    int             k_daten_da;
};

struct ntua_softc {
    device_t            sc_dev;
    struct usb_device  *sc_udev;
    uint8_t             sc_iface_index;
    struct mtx          sc_mtx;            /* Ring und Zaehler; Rueckruf */
    KMUTEX              sc_steuer;         /* Start/Stopp/Abziehen */
    struct usb_xfer    *sc_xfer[NTUA_N];
    PDEVICE_OBJECT      sc_nt;
    PFILE_OBJECT        sc_besitzer;
    NTUA_INFO           sc_info;

    int                 sc_laeuft;
    int                 sc_daten_da;       /* einmal Daten gesehen */
    int                 sc_amdpll;
    uint8_t            *sc_ring;       /* nur Aufnahme (ein Leser) */
    uint32_t            sc_ringgr, sc_kopf, sc_fuell;
    struct ntua_klient  sc_kl[NTUA_KLIENTEN];   /* Wiedergabe: je Oeffner einer */
    int32_t             sc_mix[2048];           /* Summe eines Rahmens (Samples) */
    uint32_t            sc_leerlauf;            /* Rahmen ohne Klient (Nachlauf) */
    uint32_t            sc_bja, sc_rate, sc_fps, sc_akk;
    uint32_t            sc_gesp_lo, sc_gesp_hi;
    uint32_t            sc_unterlaeufe, sc_rahmen, sc_fehler;
    uint32_t            sc_ueberlaeufe;
    int                 sc_ein;            /* 1 = Aufnahme */
    uint8_t             sc_lese[8192];     /* NTUA_LESEN: Zwischenpuffer, unter sc_steuer */
    uint8_t             sc_mixaus[4096];   /* fertige Summe eines Rahmens (Rueckruf) */
};

static struct ntua_klient *ntua_klient_zu(struct ntua_softc *, PFILE_OBJECT, int);
static int                 ntua_jemand_spielt(struct ntua_softc *);

typedef struct ntua_erweiterung {
    KSPIN_LOCK          Sperre;
    struct ntua_softc  *Sc;                /* 0 = abgezogen */
    ULONG               Nummer;
    volatile LONG       Nutzer;
} ntua_erweiterung;

static KSPIN_LOCK ntua_sperre;
static int        ntua_bereit;
static UCHAR      ntua_belegt[NTUA_MAX];

/* ---------------------------------------------------------------- *
 *  Strom
 * ---------------------------------------------------------------- */

static void
ntua_rueckruf (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntua_softc     *sc = usbd_xfer_softc(xfer);
    struct usb_page_cache *pc;
    uint32_t nf, mfl, k, off = 0, n, len, teil, a;

    switch (USB_GET_STATE(xfer)) {

    case USB_ST_TRANSFERRED:
    case USB_ST_SETUP:
    weiter:
        if (!sc->sc_laeuft) {
            break;
        }
        /*  Nachlauf.  Spielt gerade kein Klient, hoeren wir NICHT sofort
         *  auf, sondern senden noch rund zwei Sekunden Stille.  Grund:
         *  ein Funk-Headset braucht nach dem Anlaufen des Stroms Zeit,
         *  bis es wirklich wiedergibt -- der Anfang eines kurzen Klangs
         *  geht sonst verloren, und sehr kurze Signale sind ganz weg.
         *  Mit laufendem Strom ist der naechste Klang sofort da; genau
         *  deshalb ging es "mit Musik immer, allein nicht".
         *  (Dasselbe Muster wie die Anlaufschwelle in wavekern.c des
         *  EMU10K1-Treibers, nur von der anderen Seite gedacht.)  */
        if (!sc->sc_ein) {
            if (ntua_jemand_spielt(sc)) {
                sc->sc_leerlauf = 0;
            } else if (sc->sc_leerlauf >= sc->sc_fps * 2) {
                sc->sc_laeuft = 0;               /* genug gewartet */
                break;
            }
        }
        nf  = usbd_xfer_max_frames(xfer);
        mfl = usbd_xfer_max_framelen(xfer);
        pc  = usbd_xfer_get_frame(xfer, 0);

        for (k = 0; k < nf; k += 1) {
            sc->sc_akk += sc->sc_rate;
            n = sc->sc_akk / sc->sc_fps;
            sc->sc_akk -= n * sc->sc_fps;
            len = n * sc->sc_bja;
            if (len > mfl) {
                len = mfl - mfl % sc->sc_bja;
            }

            /*  Mischen: alle Klienten, die spielen, werden addiert.
             *  teil = laengster Beitrag; so weit ist der Rahmen gefuellt,
             *  der Rest wird Stille.  Gerechnet wird in 16-Bit-Samples,
             *  mit Begrenzung gegen Uebersteuerung (kein Umschlagen).  */
            teil = 0;
            {
                uint32_t si, anz = len / 2;          /* Samples im Rahmen */
                int      kk, erster = 1;

                if (anz > (uint32_t)(sizeof(sc->sc_mix) / sizeof(sc->sc_mix[0])))
                    anz = sizeof(sc->sc_mix) / sizeof(sc->sc_mix[0]);

                for (kk = 0; kk < NTUA_KLIENTEN; kk += 1) {
                    struct ntua_klient *kl = &sc->sc_kl[kk];
                    uint32_t beitrag;

                    if (!kl->k_laeuft || kl->k_ring == NULL) continue;
                    beitrag = (len < kl->k_fuell) ? len : kl->k_fuell;
                    if (beitrag == 0) {
                        if (kl->k_daten_da) kl->k_unterlaeufe += 1;
                        continue;
                    }
                    beitrag -= beitrag % 2;          /* ganze Samples */
                    if (erster) {
                        for (si = 0; si < anz; si += 1) sc->sc_mix[si] = 0;
                        erster = 0;
                    }
                    for (si = 0; si < beitrag / 2 && si < anz; si += 1) {
                        uint32_t bp = (kl->k_kopf + si * 2) % kl->k_ringgr;
                        int16_t  w  = (int16_t)((uint16_t)kl->k_ring[bp] |
                                      ((uint16_t)kl->k_ring[(bp + 1) % kl->k_ringgr] << 8));
                        sc->sc_mix[si] += w;
                    }
                    kl->k_kopf  = (kl->k_kopf + beitrag) % kl->k_ringgr;
                    kl->k_fuell -= beitrag;
                    kl->k_gesp_lo += beitrag;
                    if (kl->k_gesp_lo < beitrag) kl->k_gesp_hi += 1;
                    if (beitrag > teil) teil = beitrag;
                }

                if (!erster && teil != 0) {
                    uint8_t *aus = sc->sc_mixaus;
                    for (si = 0; si < teil / 2 && si < anz; si += 1) {
                        int32_t v = sc->sc_mix[si];
                        if (v >  32767) v =  32767;
                        if (v < -32768) v = -32768;
                        aus[si * 2]     = (uint8_t)( (uint16_t)(int16_t)v       & 0xFF);
                        aus[si * 2 + 1] = (uint8_t)(((uint16_t)(int16_t)v >> 8) & 0xFF);
                    }
                    usbd_copy_in(pc, off, aus, teil);
                    sc->sc_gesp_lo += teil;
                    if (sc->sc_gesp_lo < teil) sc->sc_gesp_hi += 1;
                }
            }
            if (teil < len) {
                usbd_frame_zero(pc, off + teil, len - teil);
                if (sc->sc_daten_da && ntua_jemand_spielt(sc)) sc->sc_unterlaeufe += 1;
            }
            if (teil == 0 && !sc->sc_ein) sc->sc_leerlauf += 1;
            usbd_xfer_set_frame_len(xfer, k, len);
            off += len;
            sc->sc_rahmen += 1;
        }
        usbd_xfer_set_frames(xfer, nf);
        usbd_transfer_submit(xfer);
        break;

    default:
        sc->sc_fehler = (uint32_t)error;
        if (error != USB_ERR_CANCELLED) {
            goto weiter;
        }
        break;
    }
}

static void
ntua_rueckruf_ein (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntua_softc     *sc = usbd_xfer_softc(xfer);
    struct usb_page_cache *pc;
    uint32_t nf, mfl, k, len, platz, w, a;

    switch (USB_GET_STATE(xfer)) {

    case USB_ST_TRANSFERRED:
        nf  = usbd_xfer_max_frames(xfer);
        mfl = usbd_xfer_max_framelen(xfer);
        pc  = usbd_xfer_get_frame(xfer, 0);
        for (k = 0; k < nf; k += 1) {
            len = usbd_xfer_frame_len(xfer, k);
            len -= len % sc->sc_bja;
            platz = sc->sc_ringgr - sc->sc_fuell;
            if (len > platz) {
                sc->sc_ueberlaeufe += len - platz;
                len = platz;
            }
            if (len != 0) {
                w = (sc->sc_kopf + sc->sc_fuell) % sc->sc_ringgr;
                a = sc->sc_ringgr - w;
                if (a > len) a = len;
                usbd_copy_out(pc, k * mfl, sc->sc_ring + w, a);
                if (len > a) {
                    usbd_copy_out(pc, k * mfl + a, sc->sc_ring, len - a);
                }
                sc->sc_fuell += len;
                sc->sc_gesp_lo += len;
                if (sc->sc_gesp_lo < len) sc->sc_gesp_hi += 1;
            }
            sc->sc_rahmen += 1;
        }
        /* FALLTHROUGH */
    case USB_ST_SETUP:
    weiter:
        if (!sc->sc_laeuft) {
            break;
        }
        /*  Nachlauf.  Spielt gerade kein Klient, hoeren wir NICHT sofort
         *  auf, sondern senden noch rund zwei Sekunden Stille.  Grund:
         *  ein Funk-Headset braucht nach dem Anlaufen des Stroms Zeit,
         *  bis es wirklich wiedergibt -- der Anfang eines kurzen Klangs
         *  geht sonst verloren, und sehr kurze Signale sind ganz weg.
         *  Mit laufendem Strom ist der naechste Klang sofort da; genau
         *  deshalb ging es "mit Musik immer, allein nicht".
         *  (Dasselbe Muster wie die Anlaufschwelle in wavekern.c des
         *  EMU10K1-Treibers, nur von der anderen Seite gedacht.)  */
        if (!sc->sc_ein) {
            if (ntua_jemand_spielt(sc)) {
                sc->sc_leerlauf = 0;
            } else if (sc->sc_leerlauf >= sc->sc_fps * 2) {
                sc->sc_laeuft = 0;               /* genug gewartet */
                break;
            }
        }
        nf  = usbd_xfer_max_frames(xfer);
        mfl = usbd_xfer_max_framelen(xfer);
        for (k = 0; k < nf; k += 1) {
            usbd_xfer_set_frame_len(xfer, k, mfl);
        }
        usbd_xfer_set_frames(xfer, nf);
        usbd_transfer_submit(xfer);
        break;

    default:
        sc->sc_fehler = (uint32_t)error;
        if (error != USB_ERR_CANCELLED) {
            goto weiter;
        }
        break;
    }
}

static uint32_t
ntua_lesen (struct ntua_softc *sc, uint8_t *d, uint32_t n)
{
    uint32_t a;

    mtx_lock(&sc->sc_mtx);
    if (!sc->sc_laeuft || sc->sc_ring == NULL || !sc->sc_ein) {
        mtx_unlock(&sc->sc_mtx);
        return 0;
    }
    if (n > sc->sc_fuell) n = sc->sc_fuell;
    n -= n % sc->sc_bja;
    a = sc->sc_ringgr - sc->sc_kopf;
    if (a > n) a = n;
    memcpy(d, sc->sc_ring + sc->sc_kopf, a);
    if (n > a) memcpy(d + a, sc->sc_ring, n - a);
    sc->sc_kopf = (sc->sc_kopf + n) % sc->sc_ringgr;
    sc->sc_fuell -= n;
    mtx_unlock(&sc->sc_mtx);
    return n;
}

/*  Unter sc_steuer.  abziehen = 1: das Geraet ist weg, keine Anfragen.  */

static void
ntua_stoppen (struct ntua_softc *sc, int abziehen)
{
    if (sc->sc_xfer[0] != NULL) {
        mtx_lock(&sc->sc_mtx);
        sc->sc_laeuft = 0;
        mtx_unlock(&sc->sc_mtx);
        usbd_transfer_unsetup(sc->sc_xfer, NTUA_N);
        sc->sc_xfer[0] = sc->sc_xfer[1] = NULL;
        if (!abziehen) {
            (void)usbd_set_alt_interface_index(sc->sc_udev, sc->sc_iface_index, 0);
        }
    }
    if (sc->sc_amdpll) {
        ntu_amdpll_isoc_aus();
        sc->sc_amdpll = 0;
    }
    if (sc->sc_ring != NULL) {
        free(sc->sc_ring, M_DEVBUF);
        sc->sc_ring = NULL;
    }
    sc->sc_besitzer = NULL;
}

static int
ntua_starten (struct ntua_softc *sc, const NTUA_START *s)
{
    struct usb_config cfg[NTUA_N];
    int err, k;

    ntua_stoppen(sc, 0);

    if (s->BytesJeAbtastung == 0 || s->BytesJeAbtastung > 32 || s->Rate == 0) {
        return EINVAL;
    }
    sc->sc_ein = (s->Endpunkt & 0x80) != 0;

    err = usbd_set_alt_interface_index(sc->sc_udev, sc->sc_iface_index, s->Alternative);
    if (err != 0) {
        device_printf(sc->sc_dev, "Alternative %u: %s\n", s->Alternative, usbd_errstr(err));
        return err;
    }

    if (s->RateSetzen) {
        struct usb_device_request req;
        uint8_t d[3];

        req.bmRequestType = UT_WRITE_CLASS_ENDPOINT;
        req.bRequest      = 0x01;                     /* SET_CUR */
        USETW2(req.wValue, 0x01, 0);                  /* SAMPLING_FREQ_CONTROL */
        USETW(req.wIndex, s->Endpunkt);
        USETW(req.wLength, 3);
        d[0] = (uint8_t)s->Rate; d[1] = (uint8_t)(s->Rate >> 8); d[2] = (uint8_t)(s->Rate >> 16);
        err = usbd_do_request(sc->sc_udev, NULL, &req, d);
        if (err != 0) {
            /*  Wie FreeBSD: bei adaptiven Senken darf das scheitern.  */
            device_printf(sc->sc_dev, "Rate %lu: %s (weiter)\n",
                          (unsigned long)s->Rate, usbd_errstr(err));
        }
    }

    sc->sc_ringgr = s->RingByte ? s->RingByte : NTUA_RING;
    if (sc->sc_ringgr < 4096) sc->sc_ringgr = 4096;
    if (sc->sc_ringgr > 1024 * 1024) sc->sc_ringgr = 1024 * 1024;
    sc->sc_ringgr -= sc->sc_ringgr % s->BytesJeAbtastung;
    sc->sc_ring = malloc(sc->sc_ringgr, M_DEVBUF, M_WAITOK | M_ZERO);
    if (sc->sc_ring == NULL) {
        return ENOMEM;
    }

    memset(cfg, 0, sizeof(cfg));
    for (k = 0; k < NTUA_N; k += 1) {
        cfg[k].type      = UE_ISOCHRONOUS;
        cfg[k].endpoint  = UE_GET_ADDR(s->Endpunkt);
        cfg[k].direction = sc->sc_ein ? UE_DIR_IN : UE_DIR_OUT;
        cfg[k].frames    = NTUA_RAHMEN;
        cfg[k].bufsize   = 0;                         /* wMaxPacketSize * Rahmen */
        cfg[k].flags.short_xfer_ok = 1;
        cfg[k].callback  = sc->sc_ein ? &ntua_rueckruf_ein : &ntua_rueckruf;
    }
    err = usbd_transfer_setup(sc->sc_udev, &sc->sc_iface_index, sc->sc_xfer, cfg,
                              NTUA_N, sc, &sc->sc_mtx);
    if (err != 0) {
        device_printf(sc->sc_dev, "Uebertragungen: %s\n", usbd_errstr(err));
        free(sc->sc_ring, M_DEVBUF);
        sc->sc_ring = NULL;
        return err;
    }

    if (ntu_amdpll_betrifft(sc->sc_udev->bus->parent)) {
        ntu_amdpll_isoc_an();
        sc->sc_amdpll = 1;
    }

    mtx_lock(&sc->sc_mtx);
    sc->sc_bja  = s->BytesJeAbtastung;
    sc->sc_rate = s->Rate;
    sc->sc_fps  = usbd_get_isoc_fps(sc->sc_udev);     /* 1000 FS, 8000 HS */
    if (sc->sc_fps == 0) sc->sc_fps = 1000;
    sc->sc_akk = sc->sc_kopf = sc->sc_fuell = 0;
    sc->sc_gesp_lo = sc->sc_gesp_hi = 0;
    sc->sc_unterlaeufe = sc->sc_rahmen = sc->sc_fehler = 0;
    sc->sc_ueberlaeufe = 0;
    sc->sc_daten_da = 0;
    sc->sc_laeuft = 1;
    usbd_transfer_start(sc->sc_xfer[0]);
    usbd_transfer_start(sc->sc_xfer[1]);
    mtx_unlock(&sc->sc_mtx);

    device_printf(sc->sc_dev, "Strom (%s): Alt %u, EP %02x, %lu Hz, %lu Byte/Abtastung, %lu Rahmen/s\n",
                  sc->sc_ein ? "Aufnahme" : "Wiedergabe", s->Alternative, s->Endpunkt, (unsigned long)s->Rate,
                  (unsigned long)s->BytesJeAbtastung, (unsigned long)sc->sc_fps);
    return 0;
}


/*  Zustand melden.  Bei Wiedergabe gelten die Werte des ANFRAGENDEN
 *  Klienten -- seit dem Mischer hat jeder seinen eigenen Ring.  Wuerde
 *  hier weiter der alte gemeinsame Ring stehen, meldete der Treiber
 *  immer "leer": die DLL schliesst dann sofort, und das Ende des Klangs
 *  fehlt (kurze Systemklaenge waren ganz weg).  */
static void
ntua_stand_kl (struct ntua_softc *sc, struct ntua_klient *kl, NTUA_STAND *st)
{
    memset(st, 0, sizeof(*st));
    mtx_lock(&sc->sc_mtx);
    st->Laeuft          = (unsigned long)(kl ? kl->k_laeuft : sc->sc_laeuft);
    if (kl != NULL) {
        st->Fuellstand  = kl->k_fuell;
        st->Platz       = kl->k_ring ? kl->k_ringgr - kl->k_fuell : 0;
        st->GespieltLo  = kl->k_gesp_lo;
        st->GespieltHi  = kl->k_gesp_hi;
        st->Unterlaeufe = kl->k_unterlaeufe;
    } else {
        st->Fuellstand  = sc->sc_fuell;
        st->Platz       = sc->sc_ring ? sc->sc_ringgr - sc->sc_fuell : 0;
        st->GespieltLo  = sc->sc_gesp_lo;
        st->GespieltHi  = sc->sc_gesp_hi;
        st->Unterlaeufe = sc->sc_unterlaeufe;
    }
    st->Rahmen          = sc->sc_rahmen;
    st->Fehler          = sc->sc_fehler;
    st->RahmenJeSekunde = sc->sc_fps;
    st->Ueberlaeufe     = sc->sc_ueberlaeufe;
    st->Aufnahme        = (unsigned long)sc->sc_ein;
    mtx_unlock(&sc->sc_mtx);
}

static void
ntua_stand (struct ntua_softc *sc, NTUA_STAND *st)
{
    memset(st, 0, sizeof(*st));
    mtx_lock(&sc->sc_mtx);
    st->Laeuft          = (unsigned long)sc->sc_laeuft;
    st->Fuellstand      = sc->sc_fuell;
    st->Platz           = sc->sc_ring ? sc->sc_ringgr - sc->sc_fuell : 0;
    st->GespieltLo      = sc->sc_gesp_lo;
    st->GespieltHi      = sc->sc_gesp_hi;
    st->Unterlaeufe     = sc->sc_unterlaeufe;
    st->Rahmen          = sc->sc_rahmen;
    st->Fehler          = sc->sc_fehler;
    st->RahmenJeSekunde = sc->sc_fps;
    st->Ueberlaeufe     = sc->sc_ueberlaeufe;
    st->Aufnahme        = (unsigned long)sc->sc_ein;
    mtx_unlock(&sc->sc_mtx);
}

/*  Den Klienten zu einem Griff finden; mit anlegen=1 einen freien
 *  Platz belegen.  Unter sc_steuer aufrufen.  */
static struct ntua_klient *
ntua_klient_zu (struct ntua_softc *sc, PFILE_OBJECT fo, int anlegen)
{
    int i, frei = -1;

    for (i = 0; i < NTUA_KLIENTEN; i += 1) {
        if (sc->sc_kl[i].k_besitzer == fo) return &sc->sc_kl[i];
        if (sc->sc_kl[i].k_besitzer == NULL && frei < 0) frei = i;
    }
    if (!anlegen || frei < 0) return NULL;
    memset(&sc->sc_kl[frei], 0, sizeof(sc->sc_kl[frei]));
    sc->sc_kl[frei].k_besitzer = fo;
    return &sc->sc_kl[frei];
}

/*  Spielt noch irgendein Klient?  */
static int
ntua_jemand_spielt (struct ntua_softc *sc)
{
    int i;
    for (i = 0; i < NTUA_KLIENTEN; i += 1)
        if (sc->sc_kl[i].k_laeuft) return 1;
    return 0;
}

/*  Einen Klienten freigeben (Ring zurueck).  Unter sc_steuer.  */
static void
ntua_klient_weg (struct ntua_softc *sc, struct ntua_klient *kl)
{
    uint8_t *r;

    if (kl == NULL) return;
    mtx_lock(&sc->sc_mtx);
    kl->k_laeuft = 0;
    r = kl->k_ring;
    kl->k_ring = NULL;
    kl->k_fuell = kl->k_kopf = 0;
    mtx_unlock(&sc->sc_mtx);
    if (r != NULL) free(r, M_DEVBUF);
    kl->k_besitzer = NULL;
}

static uint32_t
ntua_schreiben (struct ntua_softc *sc, struct ntua_klient *kl,
                const uint8_t *d, uint32_t n)
{
    uint32_t platz, w, a;

    mtx_lock(&sc->sc_mtx);
    if (kl == NULL || !kl->k_laeuft || kl->k_ring == NULL || sc->sc_ein) {
        mtx_unlock(&sc->sc_mtx);
        return 0;
    }
    platz = kl->k_ringgr - kl->k_fuell;
    if (n > platz) n = platz;
    n -= n % sc->sc_bja;                    /* nur ganze Abtastungen */
    w = (kl->k_kopf + kl->k_fuell) % kl->k_ringgr;
    a = kl->k_ringgr - w;
    if (a > n) a = n;
    memcpy(kl->k_ring + w, d, a);
    if (n > a) memcpy(kl->k_ring, d + a, n - a);
    kl->k_fuell += n;
    if (n != 0) { kl->k_daten_da = 1; sc->sc_daten_da = 1; }
    mtx_unlock(&sc->sc_mtx);
    return n;
}

/* ---------------------------------------------------------------- *
 *  NT-Geraet \Device\UsbAudioN
 * ---------------------------------------------------------------- */

static void
ntua_namen (WCHAR *dn, WCHAR *ln, ULONG n)
{
    static const WCHAR d[] = L"\\Device\\UsbAudio";
    static const WCHAR l[] = L"\\DosDevices\\UsbAudio";
    int k;

    for (k = 0; d[k]; k += 1) dn[k] = d[k];
    dn[k] = (WCHAR)(L'0' + n); dn[k + 1] = 0;
    for (k = 0; l[k]; k += 1) ln[k] = l[k];
    ln[k] = (WCHAR)(L'0' + n); ln[k + 1] = 0;
}

static int
ntua_nt_anlegen (struct ntua_softc *sc)
{
    WCHAR             dn[32], ln[32];
    UNICODE_STRING    ud, ul;
    PDEVICE_OBJECT    dev = 0;
    ntua_erweiterung *x;
    NTSTATUS          st;
    ULONG             n;
    KIRQL             alt;

    if (!ntua_bereit) {
        KeInitializeSpinLock(&ntua_sperre);
        ntua_bereit = 1;
    }
    for (n = 0; n < NTUA_MAX; n += 1) {
        KeAcquireSpinLock(&ntua_sperre, &alt);
        if (ntua_belegt[n]) {
            KeReleaseSpinLock(&ntua_sperre, alt);
            continue;
        }
        ntua_belegt[n] = 1;
        KeReleaseSpinLock(&ntua_sperre, alt);

        ntua_namen(dn, ln, n);
        RtlInitUnicodeString(&ud, dn);
        st = IoCreateDevice(NtuDriverObject, sizeof(ntua_erweiterung), &ud,
                            FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
        if (NT_SUCCESS(st)) {
            break;
        }
        ntua_belegt[n] = 0;                 /* Name noch offen: naechste Nummer */
        if (st != STATUS_OBJECT_NAME_COLLISION) {
            return 0;
        }
    }
    if (n == NTUA_MAX) {
        return 0;
    }

    x = (ntua_erweiterung *)dev->DeviceExtension;
    memset(x, 0, sizeof(*x));
    x->Nummer = n;
    x->Sc     = sc;
    KeInitializeSpinLock(&x->Sperre);
    dev->Flags |= DO_BUFFERED_IO;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    RtlInitUnicodeString(&ul, ln);
    IoCreateSymbolicLink(&ul, &ud);
    sc->sc_nt = dev;
    ntu_printf("uaudio: UsbAudio%u = %s  %04x:%04x  Schnittstelle %u\n", n,
               sc->sc_info.ProduktText, sc->sc_info.Hersteller, sc->sc_info.Produkt,
               sc->sc_info.IfNummer);
    return 1;
}

static void
ntua_nt_abbauen (struct ntua_softc *sc)
{
    ntua_erweiterung *x;
    WCHAR             dn[32], ln[32];
    UNICODE_STRING    ul;
    KIRQL             alt;

    if (sc->sc_nt == 0) {
        return;
    }
    x = (ntua_erweiterung *)sc->sc_nt->DeviceExtension;

    KeAcquireSpinLock(&x->Sperre, &alt);
    x->Sc = 0;
    KeReleaseSpinLock(&x->Sperre, alt);

    while (x->Nutzer != 0) {                /* laufende Aufrufe zu Ende */
        LARGE_INTEGER t;
        t.QuadPart = -10 * 1000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }

    ntua_namen(dn, ln, x->Nummer);
    RtlInitUnicodeString(&ul, ln);
    IoDeleteSymbolicLink(&ul);
    ntu_printf("uaudio: UsbAudio%u abgezogen\n", x->Nummer);
    ntua_belegt[x->Nummer] = 0;
    IoDeleteDevice(sc->sc_nt);
    sc->sc_nt = 0;
}

static struct ntua_softc *
ntua_holen (PDEVICE_OBJECT dev)
{
    ntua_erweiterung  *x = (ntua_erweiterung *)dev->DeviceExtension;
    struct ntua_softc *sc;
    KIRQL              alt;

    KeAcquireSpinLock(&x->Sperre, &alt);
    sc = x->Sc;
    if (sc != 0) InterlockedIncrement((LONG *)&x->Nutzer);
    KeReleaseSpinLock(&x->Sperre, alt);
    return sc;
}

static void
ntua_loslassen (PDEVICE_OBJECT dev)
{
    InterlockedDecrement((LONG *)&((ntua_erweiterung *)dev->DeviceExtension)->Nutzer);
}

/*  IRP_MJ_DEVICE_CONTROL (aus audio/ntaudio.c).  */

NTSTATUS
ntua_ioctl (PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sl  = IoGetCurrentIrpStackLocation(irp);
    ULONG              ein = sl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG              aus = sl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID              p   = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS           st  = STATUS_SUCCESS;
    ULONG              info = 0;
    struct ntua_softc *sc  = ntua_holen(dev);

    if (sc == 0) {
        st = STATUS_DEVICE_NOT_CONNECTED;
        goto fertig;
    }

    switch (sl->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_NTUA_INFO:
        if (aus < sizeof(NTUA_INFO)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        memcpy(p, &sc->sc_info, sizeof(NTUA_INFO));
        info = sizeof(NTUA_INFO);
        break;

    case IOCTL_NTUA_START: {
        NTUA_START s;
        int        err;
        if (ein < sizeof(s) || aus < sizeof(NTUA_STAND)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        memcpy(&s, p, sizeof(s));
        KeWaitForSingleObject(&sc->sc_steuer, Executive, KernelMode, FALSE, NULL);
        if (sc->sc_ein) {
            /*  Aufnahme bleibt wie bisher: genau ein Leser.  */
            if (sc->sc_besitzer != NULL && sc->sc_besitzer != sl->FileObject) {
                st = STATUS_DEVICE_BUSY;
            } else {
                err = ntua_starten(sc, &s);
                if (err == 0) sc->sc_besitzer = sl->FileObject;
                else { ntua_stoppen(sc, 0); st = STATUS_UNSUCCESSFUL; }
            }
        } else {
            /*  Wiedergabe: jeder Oeffner bekommt einen eigenen Ring; der
             *  USB-Strom laeuft, sobald der erste spielt (wavekern-Muster).  */
            struct ntua_klient *kl = ntua_klient_zu(sc, sl->FileObject, 1);
            if (kl == NULL) {
                st = STATUS_INSUFFICIENT_RESOURCES;   /* alle Plaetze belegt */
            } else {
                int lief = sc->sc_laeuft;
                err = 0;
                if (!lief) {
                    err = ntua_starten(sc, &s);       /* legt Format/Strom an */
                    if (err == 0) sc->sc_besitzer = sl->FileObject;
                }
                if (err == 0) {
                    if (kl->k_ring == NULL) {
                        kl->k_ring = malloc(NTUA_RING, M_DEVBUF, M_WAITOK | M_ZERO);
                        kl->k_ringgr = NTUA_RING;
                    }
                    if (kl->k_ring == NULL) {
                        st = STATUS_INSUFFICIENT_RESOURCES;
                        ntua_klient_weg(sc, kl);
                    } else {
                        mtx_lock(&sc->sc_mtx);
                        kl->k_kopf = kl->k_fuell = 0;
                        kl->k_daten_da = 0;
                        kl->k_laeuft = 1;
                        mtx_unlock(&sc->sc_mtx);
                    }
                } else {
                    ntua_klient_weg(sc, kl);
                    if (!lief) ntua_stoppen(sc, 0);
                    st = STATUS_UNSUCCESSFUL;
                }
            }
        }
        KeReleaseMutex(&sc->sc_steuer, FALSE);
        ntua_stand_kl(sc, sc->sc_ein ? NULL : ntua_klient_zu(sc, sl->FileObject, 0),
                      (NTUA_STAND *)p);
        info = sizeof(NTUA_STAND);
        break;
    }

    case IOCTL_NTUA_SCHREIBEN: {
        uint32_t n;
        if (aus < sizeof(NTUA_STAND)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        {
            struct ntua_klient *kl = ntua_klient_zu(sc, sl->FileObject, 0);
            n = ntua_schreiben(sc, kl, (const uint8_t *)p, ein);
            ntua_stand_kl(sc, kl, (NTUA_STAND *)p);  /* ueberschreibt die Eingabe */
        }
        ((NTUA_STAND *)p)->Angenommen = n;
        info = sizeof(NTUA_STAND);
        break;
    }

    case IOCTL_NTUA_LESEN: {
        uint8_t         *zw = sc->sc_lese;  /* Kopf und Daten teilen sich den SystemBuffer */
        uint32_t         n;
        if (aus < sizeof(NTUA_STAND)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        n = aus - sizeof(NTUA_STAND);
        if (n > sizeof(sc->sc_lese)) n = sizeof(sc->sc_lese);
        KeWaitForSingleObject(&sc->sc_steuer, Executive, KernelMode, FALSE, NULL);
        n = ntua_lesen(sc, zw, n);
        ntua_stand(sc, (NTUA_STAND *)p);
        memcpy((uint8_t *)p + sizeof(NTUA_STAND), zw, n);
        KeReleaseMutex(&sc->sc_steuer, FALSE);
        ((NTUA_STAND *)p)->Angenommen = n;
        info = sizeof(NTUA_STAND) + n;
        break;
    }

    case IOCTL_NTUA_STAND:
        if (aus < sizeof(NTUA_STAND)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        ntua_stand_kl(sc, sc->sc_ein ? NULL : ntua_klient_zu(sc, sl->FileObject, 0),
                      (NTUA_STAND *)p);
        info = sizeof(NTUA_STAND);
        break;

    case IOCTL_NTUA_LEEREN: {
        struct ntua_klient *kl = sc->sc_ein ? NULL : ntua_klient_zu(sc, sl->FileObject, 0);
        mtx_lock(&sc->sc_mtx);
        if (kl != NULL) { kl->k_fuell = 0; kl->k_daten_da = 0; }
        else            { sc->sc_fuell = 0; }
        sc->sc_daten_da = 0;                /* Stille danach ist kein Unterlauf */
        mtx_unlock(&sc->sc_mtx);
        if (aus >= sizeof(NTUA_STAND)) {
            ntua_stand(sc, (NTUA_STAND *)p);
            info = sizeof(NTUA_STAND);
        }
        break;
    }

    case IOCTL_NTUA_STOP:
        KeWaitForSingleObject(&sc->sc_steuer, Executive, KernelMode, FALSE, NULL);
        if (sc->sc_ein) {
            if (sc->sc_besitzer == NULL || sc->sc_besitzer == sl->FileObject)
                ntua_stoppen(sc, 0);
        } else {
            /*  Strom bewusst NICHT sofort stoppen: der Rueckruf laesst
             *  ihn nach dem Nachlauf von selbst auslaufen, damit der
             *  naechste kurze Klang sofort hoerbar ist.  */
            ntua_klient_weg(sc, ntua_klient_zu(sc, sl->FileObject, 0));
        }
        KeReleaseMutex(&sc->sc_steuer, FALSE);
        if (aus >= sizeof(NTUA_STAND)) {
            ntua_stand(sc, (NTUA_STAND *)p);
            info = sizeof(NTUA_STAND);
        }
        break;

    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    ntua_loslassen(dev);

fertig:
    irp->IoStatus.Status      = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/*  IRP_MJ_CLEANUP: der Griff, der spielt, wird geschlossen -> Stopp.  */

NTSTATUS
ntua_aufraeumen (PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    struct ntua_softc *sc = ntua_holen(dev);

    if (sc != 0) {
        KeWaitForSingleObject(&sc->sc_steuer, Executive, KernelMode, FALSE, NULL);
        if (sc->sc_ein) {
            if (sc->sc_besitzer == sl->FileObject) ntua_stoppen(sc, 0);
        } else {
            ntua_klient_weg(sc, ntua_klient_zu(sc, sl->FileObject, 0));
        }
        KeReleaseMutex(&sc->sc_steuer, FALSE);
        ntua_loslassen(dev);
    }
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- *
 *  newbus
 * ---------------------------------------------------------------- */

static int ntua_probe (device_t);
NTU_PAGED(ntua_probe)
static int
ntua_probe (device_t dev)
{
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    NTU_PAGED_CODE();

    if (uaa->usb_mode != USB_MODE_HOST) {
        return ENXIO;
    }
    if (uaa->info.bInterfaceClass == UICLASS_AUDIO &&
        uaa->info.bInterfaceSubClass == UISUBCLASS_AUDIOSTREAM) {
        return BUS_PROBE_DEFAULT;
    }
    return ENXIO;
}

static int ntua_attach (device_t);
NTU_PAGED(ntua_attach)
static int
ntua_attach (device_t dev)
{
    struct ntua_softc     *sc  = device_get_softc(dev);
    NTU_PAGED_CODE();
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    struct usb_device_descriptor *dd;
    const char *pr;
    int k;

    sc->sc_dev         = dev;
    sc->sc_udev        = uaa->device;
    sc->sc_iface_index = uaa->info.bIfaceIndex;

    device_set_desc(dev, "USB-Audio");
    mtx_init(&sc->sc_mtx, "ntuaudio", NULL, MTX_DEF);
    KeInitializeMutex(&sc->sc_steuer, 0);

    dd = usbd_get_device_descriptor(sc->sc_udev);
    sc->sc_info.Fassung         = NTUA_FASSUNG;
    sc->sc_info.Hersteller      = UGETW(dd->idVendor);
    sc->sc_info.Produkt         = UGETW(dd->idProduct);
    sc->sc_info.IfNummer        = uaa->info.bIfaceNum;
    sc->sc_info.Geschwindigkeit = (unsigned char)usbd_get_speed(sc->sc_udev);
    pr = usb_get_product(sc->sc_udev);
    for (k = 0; pr != NULL && pr[k] != '\0' && k < 63; k += 1) {
        sc->sc_info.ProduktText[k] = pr[k];
    }

    if (!ntua_nt_anlegen(sc)) {
        mtx_destroy(&sc->sc_mtx);
        return ENXIO;
    }
    return 0;
}

static int ntua_detach (device_t);
NTU_PAGED(ntua_detach)
static int
ntua_detach (device_t dev)
{
    struct ntua_softc *sc = device_get_softc(dev);
    NTU_PAGED_CODE();

    /*  Erst die Bruecke kappen (keine neuen Aufrufe, laufende abwarten),
     *  dann den Strom ohne Anfragen an das verschwundene Geraet abbauen.  */

    ntua_nt_abbauen(sc);
    KeWaitForSingleObject(&sc->sc_steuer, Executive, KernelMode, FALSE, NULL);
    ntua_stoppen(sc, 1);
    KeReleaseMutex(&sc->sc_steuer, FALSE);
    mtx_destroy(&sc->sc_mtx);
    return 0;
}

static device_method_t ntua_methods[] = {
    DEVMETHOD(device_probe,  ntua_probe),
    DEVMETHOD(device_attach, ntua_attach),
    DEVMETHOD(device_detach, ntua_detach),
    DEVMETHOD_END
};

static driver_t ntua_driver = {
    .name    = "ntuaudio",
    .methods = ntua_methods,
    .size    = sizeof(struct ntua_softc),
};

DRIVER_MODULE(ntuaudio, uhub, ntua_driver, 0, 0);
