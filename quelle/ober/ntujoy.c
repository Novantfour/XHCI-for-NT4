/*
 *  ober/ntujoy.c -- Gamepads und Joysticks.
 *
 *  Nimmt:
 *    - HID-Geraete, deren Berichtsdeskriptor "Joystick" oder "Gamepad"
 *      sagt;
 *    - Xbox-360-Controller (Klasse FF/5D/01, auch die vielen kompatiblen
 *      anderer Hersteller).  Die sind KEIN HID -- Microsoft hat ihnen ein
 *      eigenes Protokoll gegeben, ohne Berichtsdeskriptor.  FreeBSD hat
 *      dafuer einen nachgebauten Deskriptor (HID_XB360GP_REPORT_DESCR in
 *      dev/hid/hidrdesc.h); damit laufen sie durch denselben Parser wie
 *      jedes HID-Gamepad.  So macht es FreeBSDs uhid auch.
 *
 *  Liest die Berichte fortlaufend und haelt den aktuellen Zustand bereit
 *  (Achsen 0..65535, Knoepfe, Hut/Steuerkreuz).  \\.\UsbJoyN gibt ihn auf
 *  IOCTL_NTUJ_ZUSTAND heraus -- das ist alles, was usbjoy.dll braucht.
 *
 *  Beim Xbox-360-Controller: LED 1 an beim Einstecken (sonst blinkt der
 *  Ring endlos), und nur echte Eingabeberichte (00 14 ...) auswerten --
 *  er schickt dazwischen Statusmeldungen zu LED und Rumble.
 *
 *  Xbox-One-Controller (Klasse FF/47/D0) sprechen GIP, ein anderes
 *  Protokoll: sie schweigen, bis sie das Paket "einschalten" bekommen
 *  (05 20 .. 01 00); Eingaben kommen als Typ 20, die Guide-Taste als
 *  eigene Nachricht Typ 07, die quittiert werden will.  Trigger haben
 *  10 Bit.  Nach dem Linux-Treiber xpad (drivers/input/joystick/xpad.c).
 *  Auch hier ein nachgebauter Deskriptor, und dieselben Knopfnummern wie
 *  beim 360 -- Spiele sehen beide gleich.
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
#include <dev/hid/hidrdesc.h>
#include <ober/ntkbdmou.h>
#include <ober/ntujoy.h>

#define NTUJ_KNOEPFE  32
#define NTUJ_MAX      16

enum { NTUJ_EIN, NTUJ_AUS, NTUJ_N };

static const uint8_t ntuj_xb360_desc[] = { HID_XB360GP_REPORT_DESCR() };

/*  Xbox One, GIP-Eingabebericht (Typ 20), wie ihn xpad liest:
 *
 *    0..3   Kopf: Typ 20, Schalter, Folgenummer, Laenge
 *    4      Bit 2 Menue(Start) 3 Ansicht(Back) 4 A 5 B 6 X 7 Y
 *    5      Bit 0 hoch 1 runter 2 links 3 rechts 4 LB 5 RB 6 LS 7 RS
 *    6..9   LT, RT: 16 Bit, 0..1023
 *    10..17 LX LY RX RY: 16 Bit mit Vorzeichen, oben = positiv
 *
 *  Die Knopfnummern wie beim 360 (FreeBSDs Deskriptor): A B X Y = 1-4,
 *  LB RB = 5 6, Back Start = 7 8, LS RS = 9 10; Guide = 11 kommt aus der
 *  eigenen Nachricht.  */

static const uint8_t ntuj_xb1_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,          /* Gamepad, Application */
    0x75, 0x08, 0x95, 0x04, 0x81, 0x01,          /*  Kopf, 4 Byte, konstant */
    0x75, 0x01, 0x95, 0x02, 0x81, 0x01,          /*  Bit 0,1 unbenutzt */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x06,
    0x05, 0x09,
    0x09, 0x08, 0x09, 0x07,                      /*  Menue=8, Ansicht=7 */
    0x09, 0x01, 0x09, 0x02, 0x09, 0x03, 0x09, 0x04,  /* A B X Y */
    0x81, 0x02,
    0x05, 0x01, 0x95, 0x04,
    0x09, 0x90, 0x09, 0x91, 0x09, 0x93, 0x09, 0x92,  /* hoch runter links rechts */
    0x81, 0x02,
    0x05, 0x09, 0x95, 0x04,
    0x09, 0x05, 0x09, 0x06, 0x09, 0x09, 0x09, 0x0A,  /* LB RB LS RS */
    0x81, 0x02,
    0x05, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x03,    /*  Trigger 0..1023 */
    0x75, 0x10, 0x95, 0x02, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02,
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F,          /*  Sticks -32768..32767 */
    0x75, 0x10, 0x95, 0x04,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x81, 0x02,
    0xC0
};

#define NTUJ_XB360  1
#define NTUJ_XB1    2

struct ntuj_feld {
    struct hid_location loc;
    int32_t             min, max;
    int                 vz;                /* mit Vorzeichen lesen */
    uint8_t             id;
    int                 da;
};

struct ntuj_softc;

typedef struct ntuj_erweiterung {
    ntu_port            Port;              /* MUSS vorn stehen */
    KSPIN_LOCK          Sperre;
    struct ntuj_softc  *Sc;                /* 0 = abgezogen */
    ULONG               Nummer;
    volatile LONG       Nutzer;            /* laufende Aufrufe ausserhalb der Sperre */
} ntuj_erweiterung;

struct ntuj_softc {
    device_t            sc_dev;
    struct usb_device  *sc_udev;
    uint8_t             sc_iface_index;
    struct mtx          sc_mtx;
    struct usb_xfer    *sc_xfer[NTUJ_N];
    int                 sc_xbox;            /* 0, NTUJ_XB360, NTUJ_XB1 */
    struct callout      sc_wecker;          /* Xbox One: nachhaken, wenn nichts kommt */
    int                 sc_gip_eingaben;    /* Pakete 20/07 seit dem Einschalten */
    int                 sc_nachhaken;       /* wie oft schon erneut eingeschaltet */

    /*  Pakete an den Controller (LED, Einschalten, Quittungen): eine
     *  kleine Schlange, geleert vom OUT-Rueckruf.  */
    uint8_t             sc_aus[4][16];
    uint8_t             sc_aus_n[4];
    int                 sc_aus_kopf, sc_aus_anzahl;
    uint8_t             sc_gip_seq;
    int                 sc_guide;

    /* aus dem Deskriptor */
    int                 sc_n_achsen;
    struct ntuj_feld    sc_achse[NTUJ_ACHSEN];
    unsigned short      sc_achsen_art[NTUJ_ACHSEN];
    struct ntuj_feld    sc_knopf[NTUJ_KNOEPFE];
    int                 sc_n_knopf;        /* hoechste Nummer */
    struct ntuj_feld    sc_hut;
    struct ntuj_feld    sc_pad[4];         /* hoch, runter, rechts, links */
    int                 sc_hat_pad;
    uint8_t             sc_iid;            /* Berichtsnummer, 0 = keine */
    int                 sc_iid_da;

    /* Zustand */
    NTUJ_INFO           sc_info;
    NTUJ_ZUSTAND        sc_z;
    PDEVICE_OBJECT      sc_nt;
};

static LONG       ntuj_belegt[NTUJ_MAX];
static KSPIN_LOCK ntuj_sperre;
static int        ntuj_bereit;


/* ====================================================================
 *  Deskriptor zerlegen
 * ==================================================================== */

static struct ntuj_softc *ntuj_merke_sc;

static void
ntuj_merke (struct ntuj_feld *f, const struct hid_item *h)
{
    /*  Die Nummer des ERSTEN gefundenen Feldes -- gleich welcher Art --
     *  sagt, ob die Berichte eine Nummer vorn tragen.  */

    if (!ntuj_merke_sc->sc_iid_da) {
        ntuj_merke_sc->sc_iid    = (uint8_t)h->report_ID;
        ntuj_merke_sc->sc_iid_da = 1;
    }

    f->loc = h->loc;
    f->min = h->logical_minimum;
    f->max = h->logical_maximum;
    f->vz  = (h->logical_minimum < 0);
    f->id  = (uint8_t)h->report_ID;
    f->da  = 1;
}

static int
ntuj_zerlegen (struct ntuj_softc *sc, const void *d, uint16_t n)
{
    struct hid_data *s;
    struct hid_item  h;

    ntuj_merke_sc = sc;             /* nur waehrend attach, unter Giant */
    s = hid_start_parse(d, n, 1 << hid_input);

    while (hid_get_item(s, &h)) {

        uint16_t seite = (uint16_t)(h.usage >> 16);
        uint16_t verw  = (uint16_t)(h.usage & 0xFFFF);

        if (h.kind != hid_input || (h.flags & HIO_CONST) ||
            !(h.flags & HIO_VARIABLE)) {
            continue;
        }

        if (seite == HUP_GENERIC_DESKTOP) {

            if (verw >= HUG_X && verw <= HUG_WHEEL &&
                sc->sc_n_achsen < NTUJ_ACHSEN) {
                ntuj_merke(&sc->sc_achse[sc->sc_n_achsen], &h);
                sc->sc_achsen_art[sc->sc_n_achsen] = verw;
                sc->sc_n_achsen += 1;
            } else if (verw == HUG_HAT_SWITCH) {
                ntuj_merke(&sc->sc_hut, &h);
                sc->sc_hat_pad = 1;
            } else if (verw >= HUG_D_PAD_UP && verw <= HUG_D_PAD_UP + 3) {
                ntuj_merke(&sc->sc_pad[verw - HUG_D_PAD_UP], &h);
                sc->sc_hat_pad = 1;
            }

        } else if (seite == HUP_BUTTON && verw >= 1 && verw <= NTUJ_KNOEPFE) {
            ntuj_merke(&sc->sc_knopf[verw - 1], &h);
            if (verw > sc->sc_n_knopf) {
                sc->sc_n_knopf = verw;
            }
        }
    }

    hid_end_parse(s);
    return sc->sc_n_achsen > 0 || sc->sc_n_knopf > 0;
}


/* ====================================================================
 *  Pakete an den Controller
 *
 *  Aufgerufen unter sc_mtx (aus attach oder aus dem IN-Rueckruf); der
 *  OUT-Rueckruf schickt eines nach dem anderen.
 * ==================================================================== */

static void
ntuj_senden (struct ntuj_softc *sc, const uint8_t *p, int n)
{
    int platz;

    if (sc->sc_aus_anzahl >= 4 || n > 16 || sc->sc_xfer[1] == NULL) {
        return;
    }
    platz = (sc->sc_aus_kopf + sc->sc_aus_anzahl) % 4;
    memcpy(sc->sc_aus[platz], p, (size_t)n);
    sc->sc_aus_n[platz] = (uint8_t)n;
    sc->sc_aus_anzahl += 1;
    usbd_transfer_start(sc->sc_xfer[1]);
}

/*  Vibration, wie xpads xpad_play_effect.  Unter sc_mtx.  1 = geschickt.  */

static int
ntuj_ruetteln (struct ntuj_softc *sc, uint8_t stark, uint8_t schwach)
{
    if (sc->sc_xbox == NTUJ_XB360) {
        uint8_t r[8] = { 0x00, 0x08, 0x00, 0, 0, 0x00, 0x00, 0x00 };
        r[3] = stark;
        r[4] = schwach;
        ntuj_senden(sc, r, sizeof(r));
        return 1;
    }
    if (sc->sc_xbox == NTUJ_XB1) {
        /*  GIP-Befehl 0x09, alle Motoren (0x0F); Staerke 0..127,
         *  Dauer 0xFF (so lange wie moeglich).  */
        uint8_t r[13] = { 0x09, 0x00, 0, 0x09, 0x00, 0x0F, 0x00, 0x00,
                          0, 0, 0xFF, 0x00, 0x00 };
        r[2] = sc->sc_gip_seq++;
        r[8] = (uint8_t)(stark >> 1);
        r[9] = (uint8_t)(schwach >> 1);
        ntuj_senden(sc, r, sizeof(r));
        return 1;
    }
    return 0;
}

/*  GIP-Quittung fuer die Guide-Nachricht, wie xpadone_ack_mode_report.  */

static void
ntuj_gip_quittung (struct ntuj_softc *sc, uint8_t seq)
{
    uint8_t q[13] = { 0x01, 0x20, 0, 0x09, 0x00, 0x07, 0x20, 0x02,
                      0x00, 0x00, 0x00, 0x00, 0x00 };
    q[2] = seq;
    ntuj_senden(sc, q, sizeof(q));
}

/*  Xbox One / Series: Einschalt-Folge, GENAU wie xpads
 *  xboxone_init_packets (Linux):
 *    alle              Einschalten           05 20 nn 01 00
 *    045e:02ea, 0b00   S-Start (One S, Elite 2) 05 20 nn 0F 06
 *    045e:0b00         Zusatztasten (Elite 2)   4D 10 nn 02 07 00
 *    0e6f:*            PDP: LED an, Anmeldung
 *    0f0d:0067         Hori: Quittung
 *    24c6:541a/542a/543a  PowerA: Vibration an/aus
 *  Frueher gingen S-Start an JEDEN Controller und die Zusatztasten auch an
 *  02ea (One S, Model 1708) -- der schaltete sich beim Anwender nach dem
 *  Blinken ab.  Unter sc_mtx.  */

static void
ntuj_xb1_einschalten (struct ntuj_softc *sc, const char *warum)
{
    uint8_t     an[5]  = { 0x05, 0x20, 0x00, 0x01, 0x00 };
    uint8_t     ans[5] = { 0x05, 0x20, 0x00, 0x0F, 0x06 };
    uint16_t    vid = sc->sc_info.Vid, pid = sc->sc_info.Pid;
    const char *zusatz = "";

    an[2] = sc->sc_gip_seq++;
    ntuj_senden(sc, an, sizeof(an));

    if (vid == 0x045e && (pid == 0x02ea || pid == 0x0b00)) {
        ans[2] = sc->sc_gip_seq++;
        ntuj_senden(sc, ans, sizeof(ans));
        zusatz = ", S-Start";
        if (pid == 0x0b00) {
            uint8_t ext[6] = { 0x4d, 0x10, 0, 0x02, 0x07, 0x00 };
            ext[2] = sc->sc_gip_seq++;
            ntuj_senden(sc, ext, sizeof(ext));
            zusatz = ", S-Start, Zusatztasten";
        }
    } else if (vid == 0x0e6f) {                            /* PDP */
        uint8_t led[7] = { 0x0a, 0x20, 0, 0x03, 0x00, 0x01, 0x14 };
        uint8_t anm[6] = { 0x06, 0x20, 0, 0x02, 0x01, 0x00 };
        led[2] = sc->sc_gip_seq++;  ntuj_senden(sc, led, sizeof(led));
        anm[2] = sc->sc_gip_seq++;  ntuj_senden(sc, anm, sizeof(anm));
        zusatz = ", PDP-LED, PDP-Anmeldung";
    } else if (vid == 0x0f0d && pid == 0x0067) {           /* Hori */
        uint8_t ack[13] = { 0x01, 0x20, 0, 0x09, 0x00, 0x04, 0x20, 0x3a,
                            0x00, 0x00, 0x00, 0x80, 0x00 };
        ack[2] = sc->sc_gip_seq++;  ntuj_senden(sc, ack, sizeof(ack));
        zusatz = ", Hori-Quittung";
    } else if (vid == 0x24c6 && (pid == 0x541a || pid == 0x542a || pid == 0x543a)) {
        uint8_t anf[13] = { 0x09, 0x00, 0, 0x09, 0x00, 0x0F, 0x00, 0x00,
                            0x1D, 0x1D, 0xFF, 0x00, 0x00 };     /* PowerA */
        uint8_t end[13] = { 0x09, 0x00, 0, 0x09, 0x00, 0x0F, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00 };
        anf[2] = sc->sc_gip_seq++;  ntuj_senden(sc, anf, sizeof(anf));
        end[2] = sc->sc_gip_seq++;  ntuj_senden(sc, end, sizeof(end));
        zusatz = ", PowerA-Vibration";
    }
    sc->sc_gip_eingaben = 0;
    ntu_printf("ujoy: %04x:%04x Xbox One/Series (GIP) %s: Einschalten%s\n",
               vid, pid, warum, zusatz);
}

/*  Eine Sekunde nach dem Einschalten: kam nichts vom Controller, noch
 *  einmal einschalten -- ein echter Controller braucht nach dem
 *  Anstecken einen Moment; eine zu fruehe Nachricht geht ins Leere.
 *  Hoechstens dreimal.  Laeuft unter sc_mtx (callout_init_mtx).  */

static void
ntuj_xb1_wecker (void *arg)
{
    struct ntuj_softc *sc = arg;

    if (sc->sc_gip_eingaben > 0 || sc->sc_nachhaken >= 3) {
        if (sc->sc_gip_eingaben == 0) {
            ntu_printf("ujoy: Controller antwortet nicht -- nach %d Versuchen aufgegeben\n",
                       sc->sc_nachhaken + 1);
        }
        return;
    }
    sc->sc_nachhaken += 1;
    ntuj_xb1_einschalten(sc, "nachgehakt, keine Antwort");
    callout_reset(&sc->sc_wecker, hz, ntuj_xb1_wecker, sc);
}

static void ntuj_zustand_setzen(struct ntuj_softc *, const NTUJ_ZUSTAND *);

/*  Guide gedrueckt/losgelassen: nur das Bit im Zustand aendern.  */

static void
ntuj_guide_setzen (struct ntuj_softc *sc)
{
    NTUJ_ZUSTAND z = sc->sc_z;

    if (sc->sc_guide) z.Knoepfe |=  (1ul << 10);
    else              z.Knoepfe &= ~(1ul << 10);
    ntuj_zustand_setzen(sc, &z);
}


/* ====================================================================
 *  Bericht -> Zustand
 * ==================================================================== */

static int32_t
ntuj_lies (const struct ntuj_feld *f, const uint8_t *p, int n)
{
    return f->vz ? hid_get_data(p, n, (struct hid_location *)&f->loc)
                 : (int32_t)hid_get_udata(p, n, (struct hid_location *)&f->loc);
}

static unsigned long
ntuj_norm (const struct ntuj_feld *f, int32_t v)
{
    LONGLONG spanne = (LONGLONG)f->max - f->min;

    if (spanne <= 0) {
        return 32768;
    }
    if (v < f->min) v = f->min;
    if (v > f->max) v = f->max;

    return (unsigned long)(((LONGLONG)v - f->min) * 65535 / spanne);
}

static void
ntuj_bericht (struct ntuj_softc *sc, const uint8_t *r, int len)
{
    NTUJ_ZUSTAND    z;
    const uint8_t  *p = r;
    int             n = len, k;
    uint8_t         id = 0;

    /*  Xbox: nur Eingabeberichte.  Typ 00, Laenge 14h.  Alles andere sind
     *  Statusmeldungen (LED 01, Rumble 03, ...).  */

    if (sc->sc_xbox == NTUJ_XB360 && (len < 20 || r[0] != 0x00 || r[1] != 0x14)) {
        return;
    }

    if (sc->sc_xbox == NTUJ_XB1) {

        /*  Guide: eigene Nachricht Typ 07.  Verlangt sie eine Quittung
         *  (Schalter 10), sonst schickt der Controller sie immer wieder.  */

        /*  Ankuendigung (02): der Controller ist jetzt bereit.  Kam
         *  unsere Einschalt-Nachricht zu frueh, ging sie ins Leere --
         *  also jetzt noch einmal (wie es Windows' eigener Treiber nach
         *  der Ankuendigung tut).  */

        if (len >= 4 && r[0] == 0x02) {
            ntu_printf("ujoy: Controller meldet sich an (02)\n");
            ntuj_xb1_einschalten(sc, "nach Anmeldung");
            return;
        }

        if (r[0] == 0x20 || r[0] == 0x07) {
            sc->sc_gip_eingaben += 1;
        }

        if (len >= 5 && r[0] == 0x07) {
            if (r[1] & 0x10) {
                ntuj_gip_quittung(sc, r[2]);
            }
            sc->sc_guide = r[4] & 0x01;
            ntuj_guide_setzen(sc);
            return;
        }

        if (len < 18 || r[0] != 0x20) {
            return;                     /* Ankuendigung, Status, ... */
        }
    }

    /*  Berichtsnummer: steht vorn, gehoert nicht zu den Stellen.  Nur
     *  Felder mit dieser Nummer gelten.  */

    if (sc->sc_iid != 0) {
        if (n < 1) return;
        id = p[0];
        p += 1;
        n -= 1;
    }

    memset(&z, 0, sizeof(z));
    z.Fassung = NTUJ_FASSUNG;
    z.Pov     = NTUJ_POV_MITTE;

    for (k = 0; k < sc->sc_n_achsen; k += 1) {

        struct ntuj_feld *f = &sc->sc_achse[k];
        unsigned long     w;

        if (f->id != id) {
            w = sc->sc_z.Achse[k];          /* anderer Bericht: stehen lassen */
        } else {
            w = ntuj_norm(f, ntuj_lies(f, p, n));

            /*  Xbox: Sticks melden "oben = positiv", winmm will
             *  "oben = klein".  */

            if (sc->sc_xbox != 0 && (sc->sc_achsen_art[k] == NTUJ_Y ||
                                sc->sc_achsen_art[k] == NTUJ_RY)) {
                w = 65535 - w;
            }
        }
        z.Achse[k] = w;
    }

    for (k = 0; k < sc->sc_n_knopf; k += 1) {
        struct ntuj_feld *f = &sc->sc_knopf[k];
        if (f->da && f->id == id && ntuj_lies(f, p, n) != 0) {
            z.Knoepfe |= 1ul << k;
        }
    }

    if (sc->sc_xbox == NTUJ_XB1 && sc->sc_guide) {
        z.Knoepfe |= 1ul << 10;         /* Guide = Knopf 11 */
    }

    if (sc->sc_hut.da && sc->sc_hut.id == id) {

        /*  Hutschalter: meist 0..7 (oder 1..8) fuer acht Richtungen;
         *  ausserhalb = Mitte ("Nullzustand").  */

        int32_t v = ntuj_lies(&sc->sc_hut, p, n);

        if (v >= sc->sc_hut.min && v <= sc->sc_hut.max) {
            z.Pov = (unsigned long)((v - sc->sc_hut.min) * 36000 /
                                    (sc->sc_hut.max - sc->sc_hut.min + 1));
        }

    } else if (sc->sc_pad[0].da) {

        /*  Steuerkreuz als vier Knoepfe -> Richtung.  */

        int o = ntuj_lies(&sc->sc_pad[0], p, n) != 0;
        int u = sc->sc_pad[1].da && ntuj_lies(&sc->sc_pad[1], p, n) != 0;
        int r = sc->sc_pad[2].da && ntuj_lies(&sc->sc_pad[2], p, n) != 0;
        int l = sc->sc_pad[3].da && ntuj_lies(&sc->sc_pad[3], p, n) != 0;

        if (o && r)      z.Pov =  4500;
        else if (r && u) z.Pov = 13500;
        else if (u && l) z.Pov = 22500;
        else if (l && o) z.Pov = 31500;
        else if (o)      z.Pov =     0;
        else if (r)      z.Pov =  9000;
        else if (u)      z.Pov = 18000;
        else if (l)      z.Pov = 27000;
    }

    ntuj_zustand_setzen(sc, &z);
}

static void
ntuj_zustand_setzen (struct ntuj_softc *sc, const NTUJ_ZUSTAND *neu)
{
    ntuj_erweiterung *x;
    KIRQL             alt;

    if (sc->sc_nt == 0) {
        sc->sc_z = *neu;
        return;
    }

    x = (ntuj_erweiterung *)sc->sc_nt->DeviceExtension;

    KeAcquireSpinLock(&x->Sperre, &alt);
    {
        ULONG zaehler = sc->sc_z.Zaehler + 1;
        sc->sc_z         = *neu;
        sc->sc_z.Zaehler = zaehler;
    }
    KeReleaseSpinLock(&x->Sperre, alt);
}


/* ====================================================================
 *  Uebertragungen
 * ==================================================================== */

static void
ntuj_ein_rueckruf (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntuj_softc     *sc = usbd_xfer_softc(xfer);
    struct usb_page_cache *pc;
    uint8_t                buf[64];
    int                    len;

    usbd_xfer_status(xfer, &len, NULL, NULL, NULL);

    switch (USB_GET_STATE(xfer)) {

        case USB_ST_TRANSFERRED:

            if (len > (int)sizeof(buf)) {
                len = (int)sizeof(buf);
            }
            if (len > 0) {
                memset(buf, 0, sizeof(buf));
                pc = usbd_xfer_get_frame(xfer, 0);
                usbd_copy_out(pc, 0, buf, len);
                ntuj_bericht(sc, buf, len);
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

/*  Die Sendeschlange leeren: ein Paket je Durchlauf.  */

static void
ntuj_aus_rueckruf (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntuj_softc     *sc = usbd_xfer_softc(xfer);
    struct usb_page_cache *pc;
    int                    k;

    switch (USB_GET_STATE(xfer)) {

        case USB_ST_TRANSFERRED:
        case USB_ST_SETUP:
        default:
            if (sc->sc_aus_anzahl == 0) {
                break;
            }
            k = sc->sc_aus_kopf;
            pc = usbd_xfer_get_frame(xfer, 0);
            usbd_copy_in(pc, 0, sc->sc_aus[k], sc->sc_aus_n[k]);
            usbd_xfer_set_frame_len(xfer, 0, sc->sc_aus_n[k]);
            sc->sc_aus_kopf    = (k + 1) % 4;
            sc->sc_aus_anzahl -= 1;
            usbd_transfer_submit(xfer);
            break;
    }
}

static const struct usb_config ntuj_konfig[NTUJ_N] = {
    [NTUJ_EIN] = {
        .type      = UE_INTERRUPT,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_IN,
        .flags     = { .pipe_bof = 1, .short_xfer_ok = 1 },
        .bufsize   = 0,
        .callback  = &ntuj_ein_rueckruf,
    },
    [NTUJ_AUS] = {
        .type      = UE_INTERRUPT,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_OUT,
        .flags     = { .pipe_bof = 1 },
        .bufsize   = 64,
        .callback  = &ntuj_aus_rueckruf,
        .timeout   = 1000,
    },
};


/* ====================================================================
 *  Das NT-Geraet  \\Device\\UsbJoyN, \\DosDevices\\UsbJoyN
 * ==================================================================== */

static void
ntuj_namen (WCHAR *d, WCHAR *l, ULONG n)
{
    ntu_wsprintf(d, 32, L"\\Device\\UsbJoy%u", n);
    ntu_wsprintf(l, 32, L"\\DosDevices\\UsbJoy%u", n);
}

static int
ntuj_nt_anlegen (struct ntuj_softc *sc)
{
    WCHAR             dn[32], ln[32];
    UNICODE_STRING    ud, ul;
    PDEVICE_OBJECT    dev = 0;
    ntuj_erweiterung *x;
    NTSTATUS          st;
    ULONG             n;
    KIRQL             alt;

    if (!ntuj_bereit) {
        KeInitializeSpinLock(&ntuj_sperre);
        ntuj_bereit = 1;
    }

    for (n = 0; n < NTUJ_MAX; n += 1) {

        KeAcquireSpinLock(&ntuj_sperre, &alt);
        if (ntuj_belegt[n]) {
            KeReleaseSpinLock(&ntuj_sperre, alt);
            continue;
        }
        ntuj_belegt[n] = 1;
        KeReleaseSpinLock(&ntuj_sperre, alt);

        ntuj_namen(dn, ln, n);
        RtlInitUnicodeString(&ud, dn);

        st = IoCreateDevice(NtuDriverObject, sizeof(ntuj_erweiterung), &ud,
                            FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
        if (NT_SUCCESS(st)) {
            break;
        }

        /*  Hat ein Programm das Geraet eines eben abgezogenen Gamepads
         *  noch offen, lebt dessen Name weiter -- dann die naechste
         *  Nummer.  */

        ntuj_belegt[n] = 0;
        if (st != STATUS_OBJECT_NAME_COLLISION) {
            return 0;
        }
    }

    if (n == NTUJ_MAX) {
        return 0;
    }

    x = (ntuj_erweiterung *)dev->DeviceExtension;
    memset(x, 0, sizeof(*x));
    x->Port.Kind   = NTU_PORT_JOY;
    x->Port.Suffix = n;
    x->Nummer      = n;
    x->Sc          = sc;
    KeInitializeSpinLock(&x->Sperre);

    dev->Flags |= DO_BUFFERED_IO;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    RtlInitUnicodeString(&ul, ln);
    IoCreateSymbolicLink(&ul, &ud);

    sc->sc_nt = dev;
    return 1;
}

static void
ntuj_nt_abbauen (struct ntuj_softc *sc)
{
    ntuj_erweiterung *x;
    WCHAR             dn[32], ln[32];
    UNICODE_STRING    ul;
    KIRQL             alt;

    if (sc->sc_nt == 0) {
        return;
    }

    x = (ntuj_erweiterung *)sc->sc_nt->DeviceExtension;

    KeAcquireSpinLock(&x->Sperre, &alt);
    x->Sc = 0;
    KeReleaseSpinLock(&x->Sperre, alt);

    /*  Laufende Vibrationsaufrufe (IOCTL_NTUJ_RUETTELN) zu Ende gehen
     *  lassen -- sie benutzen sc und sc_mtx.  */
    while (x->Nutzer != 0) {
        LARGE_INTEGER t;
        t.QuadPart = -10 * 1000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }

    ntuj_namen(dn, ln, x->Nummer);
    RtlInitUnicodeString(&ul, ln);
    IoDeleteSymbolicLink(&ul);

    ntu_printf("ujoy: UsbJoy%u abgezogen\n", x->Nummer);

    ntuj_belegt[x->Nummer] = 0;
    IoDeleteDevice(sc->sc_nt);
    sc->sc_nt = 0;
}

NTSTATUS
ntuj_ioctl (PDEVICE_OBJECT dev, PIRP irp)
{
    ntuj_erweiterung  *x  = (ntuj_erweiterung *)dev->DeviceExtension;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    ULONG              aus = sl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID              p   = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS           st  = STATUS_SUCCESS;
    ULONG              info = 0;
    KIRQL              alt;

    /*  Vibration: senden braucht sc_mtx, und die darf man unter der
     *  Spinsperre nicht nehmen.  Also unter der Sperre nur den Zeiger
     *  und einen Nutzer-Zaehler holen; das Abziehen wartet auf 0.  */

    if (sl->Parameters.DeviceIoControl.IoControlCode == IOCTL_NTUJ_RUETTELN) {
        struct ntuj_softc *sc;
        NTUJ_RUETTELN     *r = (NTUJ_RUETTELN *)p;

        if (sl->Parameters.DeviceIoControl.InputBufferLength < 2) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            KeAcquireSpinLock(&x->Sperre, &alt);
            sc = x->Sc;
            if (sc != 0) InterlockedIncrement((LONG *)&x->Nutzer);
            KeReleaseSpinLock(&x->Sperre, alt);
            if (sc == 0) {
                st = STATUS_DEVICE_NOT_CONNECTED;
            } else {
                mtx_lock(&sc->sc_mtx);
                st = ntuj_ruetteln(sc, r->Stark, r->Schwach) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
                mtx_unlock(&sc->sc_mtx);
                InterlockedDecrement((LONG *)&x->Nutzer);
            }
        }
        irp->IoStatus.Status      = st;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }

    KeAcquireSpinLock(&x->Sperre, &alt);

    if (x->Sc == 0) {
        st = STATUS_DEVICE_NOT_CONNECTED;
    } else {
        switch (sl->Parameters.DeviceIoControl.IoControlCode) {

            case IOCTL_NTUJ_INFO:
                if (aus < sizeof(NTUJ_INFO)) {
                    st = STATUS_BUFFER_TOO_SMALL;
                } else {
                    memcpy(p, &x->Sc->sc_info, sizeof(NTUJ_INFO));
                    info = sizeof(NTUJ_INFO);
                }
                break;

            case IOCTL_NTUJ_ZUSTAND:
                if (aus < sizeof(NTUJ_ZUSTAND)) {
                    st = STATUS_BUFFER_TOO_SMALL;
                } else {
                    memcpy(p, &x->Sc->sc_z, sizeof(NTUJ_ZUSTAND));
                    info = sizeof(NTUJ_ZUSTAND);
                }
                break;

            default:
                st = STATUS_INVALID_DEVICE_REQUEST;
                break;
        }
    }

    KeReleaseSpinLock(&x->Sperre, alt);

    irp->IoStatus.Status      = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}


/* ====================================================================
 *  probe, attach, detach
 * ==================================================================== */

static int
ntuj_ist_xbox (const struct usb_attach_arg *uaa)
{
    if (uaa->info.bInterfaceClass != UICLASS_VENDOR) {
        return 0;
    }
    if (uaa->info.bInterfaceSubClass == 0x5D && uaa->info.bInterfaceProtocol == 0x01) {
        return NTUJ_XB360;
    }
    if (uaa->info.bInterfaceSubClass == 0x47 && uaa->info.bInterfaceProtocol == 0xD0 &&
        uaa->info.bIfaceNum == 0) {
        return NTUJ_XB1;                /* nur Schnittstelle 0: die Eingaben */
    }
    return 0;
}

static int
ntuj_probe (device_t dev)
{
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    void     *d = NULL;
    uint16_t  n = 0;
    int       ja = 0;

    if (uaa->usb_mode != USB_MODE_HOST) {
        return ENXIO;
    }

    if (ntuj_ist_xbox(uaa)) {
        return BUS_PROBE_DEFAULT;
    }

    if (uaa->info.bInterfaceClass != UICLASS_HID ||
        uaa->info.bInterfaceSubClass == UISUBCLASS_BOOT) {
        return ENXIO;               /* Boot-Tastaturen und -Maeuse: anderswo */
    }

    if (usbd_req_get_hid_desc(uaa->device, NULL, &d, &n, M_TEMP,
                              uaa->info.bIfaceIndex) == 0 && d != NULL) {
        ja = hid_is_collection(d, n, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_JOYSTICK)) ||
             hid_is_collection(d, n, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_GAME_PAD));
        free(d, M_TEMP);
    }

    return ja ? BUS_PROBE_DEFAULT : ENXIO;
}

static int
ntuj_attach (device_t dev)
{
    struct ntuj_softc     *sc  = device_get_softc(dev);
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    struct usb_device_descriptor *dd;
    void     *d = NULL;
    uint16_t  n = 0;
    int       ok, err, k;
    const char *pr;

    sc->sc_dev         = dev;
    sc->sc_udev        = uaa->device;
    sc->sc_iface_index = uaa->info.bIfaceIndex;
    sc->sc_xbox        = ntuj_ist_xbox(uaa);

    if (sc->sc_xbox == NTUJ_XB360) {
        ok = ntuj_zerlegen(sc, ntuj_xb360_desc, sizeof(ntuj_xb360_desc));
    } else if (sc->sc_xbox == NTUJ_XB1) {
        ok = ntuj_zerlegen(sc, ntuj_xb1_desc, sizeof(ntuj_xb1_desc));
        sc->sc_n_knopf = 11;            /* Guide kommt aus Typ 07 */
    } else {
        ok = 0;
        if (usbd_req_get_hid_desc(uaa->device, NULL, &d, &n, M_TEMP,
                                  sc->sc_iface_index) == 0 && d != NULL) {
            ok = ntuj_zerlegen(sc, d, n);
            free(d, M_TEMP);
        }
        (void)usbd_req_set_idle(sc->sc_udev, NULL, sc->sc_iface_index, 0, 0);
    }

    if (!ok) {
        device_printf(dev, "Deskriptor ohne Achsen und Knoepfe\n");
        return ENXIO;
    }

    device_set_desc(dev, (sc->sc_xbox == NTUJ_XB1)   ? "Xbox-One-Controller" :
                         (sc->sc_xbox == NTUJ_XB360) ? "Xbox-360-Controller" : "USB-Gamepad");
    mtx_init(&sc->sc_mtx, "ntujoy", NULL, MTX_DEF);
    callout_init_mtx(&sc->sc_wecker, &sc->sc_mtx, 0);

    /*  Info fuer usbjoy.dll.  */

    dd = usbd_get_device_descriptor(sc->sc_udev);
    sc->sc_info.Fassung       = NTUJ_FASSUNG;
    sc->sc_info.AnzahlAchsen  = (unsigned long)sc->sc_n_achsen;
    for (k = 0; k < sc->sc_n_achsen; k += 1) {
        sc->sc_info.AchsenArt[k] = sc->sc_achsen_art[k];
    }
    sc->sc_info.AnzahlKnoepfe = (unsigned long)sc->sc_n_knopf;
    sc->sc_info.HatPov        = (unsigned long)sc->sc_hat_pad;
    sc->sc_info.Vid           = UGETW(dd->idVendor);
    sc->sc_info.Pid           = UGETW(dd->idProduct);
    sc->sc_info.Xbox          = (unsigned long)sc->sc_xbox;

    pr = usb_get_product(sc->sc_udev);
    for (k = 0; pr != NULL && pr[k] != '\0' && k < 31; k += 1) {
        sc->sc_info.Name[k] = pr[k];
    }
    if (k == 0) {
        strcpy(sc->sc_info.Name, (sc->sc_xbox == NTUJ_XB1)   ? "Xbox One Controller" :
                                 (sc->sc_xbox == NTUJ_XB360) ? "Xbox 360 Controller" : "USB-Gamepad");
    }

    /*  Mitte, bis der erste Bericht kommt.  */

    sc->sc_z.Fassung = NTUJ_FASSUNG;
    sc->sc_z.Pov     = NTUJ_POV_MITTE;
    for (k = 0; k < NTUJ_ACHSEN; k += 1) {
        sc->sc_z.Achse[k] = 32768;
    }

    /*  Xbox hat einen OUT-Endpunkt (LED, Rumble); HID-Gamepads meist
     *  nicht -- dann nur den IN-Teil einrichten.  */

    err = usbd_transfer_setup(uaa->device, &sc->sc_iface_index, sc->sc_xfer,
                              ntuj_konfig, sc->sc_xbox ? NTUJ_N : 1, sc,
                              &sc->sc_mtx);
    if (err != 0) {
        device_printf(dev, "usbd_transfer_setup: %s\n", usbd_errstr(err));
        mtx_destroy(&sc->sc_mtx);
        return ENXIO;
    }

    if (!ntuj_nt_anlegen(sc)) {
        usbd_transfer_unsetup(sc->sc_xfer, NTUJ_N);
        mtx_destroy(&sc->sc_mtx);
        return ENXIO;
    }

    ntu_printf("ujoy: UsbJoy%u = %s  %04x:%04x  %d Achsen, %d Knoepfe%s%s\n",
               ((ntuj_erweiterung *)sc->sc_nt->DeviceExtension)->Nummer,
               sc->sc_info.Name, sc->sc_info.Vid, sc->sc_info.Pid,
               sc->sc_n_achsen, sc->sc_n_knopf,
               sc->sc_hat_pad ? ", Steuerkreuz" : "",
               (sc->sc_xbox == NTUJ_XB1)   ? ", Xbox-One-Protokoll (GIP)" :
               (sc->sc_xbox == NTUJ_XB360) ? ", Xbox-360-Protokoll" : "");

    mtx_lock(&sc->sc_mtx);
    usbd_transfer_start(sc->sc_xfer[NTUJ_EIN]);

    if (sc->sc_xbox == NTUJ_XB360) {

        /*  Spieler-LED nach der Nummer: UsbJoy0 = winmm-Joystick 1 =
         *  Muster 06, UsbJoy1 = 07, ... bis 09 (Spieler 4).  Vorher bekam
         *  jeder Controller 06 -- mit zwei 360ern leuchteten beide als
         *  Spieler 1 (im Pruefstand mit pademu gesehen).  */
        ULONG   nr     = ((ntuj_erweiterung *)sc->sc_nt->DeviceExtension)->Nummer;
        uint8_t led[3] = { 0x01, 0x03, 0x06 };

        if (nr < 4) {
            led[2] = (uint8_t)(0x06 + nr);
        }
        ntuj_senden(sc, led, sizeof(led));
        ntu_printf("ujoy: %04x:%04x Xbox 360, geschickt: Spieler-LED %u\n",
                   sc->sc_info.Vid, sc->sc_info.Pid, (unsigned)(led[2] - 0x05));

    } else if (sc->sc_xbox == NTUJ_XB1) {

        ntuj_xb1_einschalten(sc, "beim Anstecken");
        callout_reset(&sc->sc_wecker, hz, ntuj_xb1_wecker, sc);
    }

    mtx_unlock(&sc->sc_mtx);

    return 0;
}

static int
ntuj_detach (device_t dev)
{
    struct ntuj_softc *sc = device_get_softc(dev);

    /*  Erst das NT-Geraet von uns loesen (Bruecke kappen), dann die
     *  Uebertragungen.  Ein Programm, das UsbJoyN noch offen haelt,
     *  bekommt ab jetzt "nicht verbunden".  */

    callout_drain(&sc->sc_wecker);
    usbd_transfer_unsetup(sc->sc_xfer, NTUJ_N);
    ntuj_nt_abbauen(sc);
    mtx_destroy(&sc->sc_mtx);
    return 0;
}

static device_method_t ntuj_methods[] = {
    DEVMETHOD(device_probe,  ntuj_probe),
    DEVMETHOD(device_attach, ntuj_attach),
    DEVMETHOD(device_detach, ntuj_detach),
    DEVMETHOD_END
};

static driver_t ntuj_driver = {
    .name    = "ntujoy",
    .methods = ntuj_methods,
    .size    = sizeof(struct ntuj_softc),
};

DRIVER_MODULE(ntujoy, uhub, ntuj_driver, 0, 0);
