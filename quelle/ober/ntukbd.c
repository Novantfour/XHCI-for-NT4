/*
 *  ober/ntukbd.c -- die Oberkante fuer Tastaturen.
 *
 *  Neu geschrieben, nicht uebernommen.  FreeBSDs ukbd.c haengt an
 *  kbdreg und evdev -- zwei weitere Untersysteme, deren Ertrag unter NT
 *  null ist, weil NT seine eigene Tastaturkette hat.  Was hier steht,
 *  redet nach unten mit dem unveraenderten USB-Stapel und nach oben mit
 *  NT.  Genau die Aufteilung, die der Skill vorsieht.
 *
 *  Nach oben ist das ein **Anschlusstreiber**, kein Filter:
 *
 *      \Device\KeyboardPort1        von uns angelegt
 *      IOCTL_INTERNAL_KEYBOARD_CONNECT   kbdclass meldet sich an und
 *                                        hinterlegt seinen Rueckruf
 *      ClassService(...)            wir liefern KEYBOARD_INPUT_DATA
 *
 *  Der Rueckruf muss auf DISPATCH_LEVEL laufen.  Unser Faden laeuft auf
 *  PASSIVE_LEVEL, also wird vorher angehoben.  Das ist die einzige
 *  Stelle im ganzen Bau, an der wir das tun, und sie ist kurz: der
 *  Rueckruf schreibt in einen Ring und kehrt zurueck.
 *
 *  ACHTUNG, gegen die echte NT-4.0-kbdclass zu pruefen: sie bedient von
 *  sich aus nur KeyboardPort0.  Damit sie unseren Anschluss findet,
 *  braucht es
 *
 *      HKLM\SYSTEM\CurrentControlSet\Services\Kbdclass\Parameters
 *          ConnectMultiplePorts  = 1
 *          MaximumPortsServiced  = 2
 *
 *  Das ist der Stand meiner Kenntnis und ausdruecklich nicht am Geraet
 *  belegt.  Waehrend der Inbetriebnahme bleibt die PS/2-Tastatur
 *  angesteckt -- schon deshalb, weil die BIOS-Nachbildung in dem
 *  Augenblick aufhoert, in dem der Hostbaustein uebernommen wird.
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


/* ====================================================================
 *  Die Umsetzung HID -> AT-Abtastcode, Satz 1
 *
 *  NT spricht mit kbdclass in Satz-1-Codes, genau wie i8042prt sie vom
 *  Tastaturbaustein bekommt.  Die USB-Tastatur liefert HID-Verwendungen.
 *  Dazwischen steht diese Tabelle.
 *
 *  Bit 0x100 heisst: dem Code geht E0 voraus.  NT traegt das als
 *  KEY_E0 in den Flags, nicht als eigenes Ereignis -- das ist der
 *  Unterschied zum Drahtprotokoll und die haeufigste Verwechslung.
 *
 *  Nachgeschlagen, nicht erinnert: HID Usage Tables 1.12, Abschnitt 10
 *  (Keyboard/Keypad Page 0x07), gegen die Satz-1-Tabelle im NT-DDK.
 * ==================================================================== */

#define E0 0x100

static const USHORT ntu_hid2set1[0x68] = {
    0,      0,      0,      0,                      /* 00-03 */
    0x1E,   0x30,   0x2E,   0x20,   0x12,   0x21,   /* a b c d e f */
    0x22,   0x23,   0x17,   0x24,   0x25,   0x26,   /* g h i j k l */
    0x32,   0x31,   0x18,   0x19,   0x10,   0x13,   /* m n o p q r */
    0x1F,   0x14,   0x16,   0x2F,   0x11,   0x2D,   /* s t u v w x */
    0x15,   0x2C,                                   /* y z */
    0x02,   0x03,   0x04,   0x05,   0x06,           /* 1 2 3 4 5 */
    0x07,   0x08,   0x09,   0x0A,   0x0B,           /* 6 7 8 9 0 */
    0x1C,   0x01,   0x0E,   0x0F,   0x39,           /* Enter Esc BS Tab Space */
    0x0C,   0x0D,   0x1A,   0x1B,   0x2B,           /* - = [ ] \ */
    0x2B,                                           /* non-US # */
    0x27,   0x28,   0x29,   0x33,   0x34,   0x35,   /* ; ' ` , . / */
    0x3A,                                           /* CapsLock */
    0x3B,   0x3C,   0x3D,   0x3E,   0x3F,   0x40,   /* F1-F6 */
    0x41,   0x42,   0x43,   0x44,   0x57,   0x58,   /* F7-F12 */
    E0|0x37,                                        /* PrintScreen */
    0x46,                                           /* ScrollLock */
    0,                                              /* Pause: Sonderfall */
    E0|0x52, E0|0x47, E0|0x49, E0|0x53,             /* Ins Home PgUp Del */
    E0|0x4F, E0|0x51, E0|0x4D, E0|0x4B,             /* End PgDn Right Left */
    E0|0x50, E0|0x48,                               /* Down Up */
    0x45,                                           /* NumLock */
    E0|0x35, 0x37,   0x4A,   0x4E,   E0|0x1C,       /* KP / * - + Enter */
    0x4F,   0x50,   0x51,   0x4B,   0x4C,           /* KP 1 2 3 4 5 */
    0x4D,   0x47,   0x48,   0x49,   0x52,   0x53,   /* KP 6 7 8 9 0 . */
    0x56,                                           /* non-US backslash */
    E0|0x5D,                                        /* Application */
    0,                                              /* Power */
    0x59                                            /* KP = */
};

static const USHORT ntu_mod2set1[8] = {
    0x1D,       /* LCtrl  */
    0x2A,       /* LShift */
    0x38,       /* LAlt   */
    E0|0x5B,    /* LGui   */
    E0|0x1D,    /* RCtrl  */
    0x36,       /* RShift */
    E0|0x38,    /* RAlt   */
    E0|0x5C     /* RGui   */
};


/* ====================================================================
 *  Zustand
 * ==================================================================== */

enum { NTUKBD_INTR_DT, NTUKBD_CTRL_LED, NTUKBD_N_TRANSFER };

struct ntukbd_softc {
    device_t            sc_dev;
    struct usb_device  *sc_udev;
    struct mtx          sc_mtx;
    struct usb_xfer    *sc_xfer[NTUKBD_N_TRANSFER];
    uint8_t             sc_iface_index;

    uint8_t             sc_last[8];     /* vorheriger Bericht */
    PDEVICE_OBJECT      sc_pdo;         /* \Device\KeyboardPortN */
    NTU_KBD_CONNECT    *sc_conn;        /* der feste Anschluss */
    KEYBOARD_INPUT_DATA sc_ring[32];

    /*  Wiederholung -- siehe "Tastenwiederholung" weiter unten.  Die
     *  Sperre schuetzt sc_wd_code UND jeden Aufruf von ClassService, aus
     *  beiden Richtungen.  */
    KSPIN_LOCK          sc_wd_sperre;
    KTIMER              sc_wd_timer;
    KDPC                sc_wd_dpc;
    USHORT              sc_wd_code;     /* 0 = nichts wiederholen */
    KEYBOARD_INPUT_DATA sc_wd_eintrag;

    /*  Leuchten: SET_REPORT auf dem Steuerkanal, wie FreeBSDs ukbd.  */
    uint8_t             sc_iface_no;
    uint8_t             sc_led;         /* HID: Bit 0 Num, 1 Caps, 2 Scroll */
    int                 sc_led_neu;
    volatile LONG       sc_ref;         /* Arbeitsauftrag benutzt sie gerade */
};

static usb_callback_t ntukbd_intr_callback;
static usb_callback_t ntukbd_led_callback;

static const struct usb_config ntukbd_config[NTUKBD_N_TRANSFER] = {
    [NTUKBD_INTR_DT] = {
        .type      = UE_INTERRUPT,
        .endpoint  = UE_ADDR_ANY,
        .direction = UE_DIR_IN,
        .flags     = { .pipe_bof = 1, .short_xfer_ok = 1 },
        .bufsize   = 0,                 /* wMaxPacketSize */
        .callback  = &ntukbd_intr_callback,
    },
    [NTUKBD_CTRL_LED] = {
        .type      = UE_CONTROL,
        .endpoint  = 0x00,
        .direction = UE_DIR_ANY,
        .bufsize   = sizeof(struct usb_device_request) + 8,
        .callback  = &ntukbd_led_callback,
        .timeout   = 1000,
    },
};


/* ====================================================================
 *  Leuchten (Num, Caps, Scroll)
 *
 *  kbdclass schickt IOCTL_KEYBOARD_SET_INDICATORS -- auf welcher Ebene,
 *  ist nicht zugesagt.  Ein USB-Steuertransfer braucht aber die KMUTEX-
 *  Sperre (unter DISPATCH_LEVEL).  Also merkt sich der IOCTL-Pfad nur den
 *  Zustand und reiht einen Arbeitsauftrag ein (ExQueueWorkItem geht auf
 *  jeder Ebene); der schickt bei PASSIVE_LEVEL jeder USB-Tastatur ein
 *  SET_REPORT.  Neu angesteckte Tastaturen bekommen den aktuellen Zustand
 *  gleich beim Anstecken.
 * ==================================================================== */

#define NTU_KBD_MAX 8

static struct ntukbd_softc *ntu_kbd_liste[NTU_KBD_MAX];
static KSPIN_LOCK           ntu_kbd_liste_sperre;
static LONG                 ntu_kbd_liste_bereit;
static WORK_QUEUE_ITEM      ntu_led_arbeit;
static volatile LONG        ntu_led_eingereiht;
static volatile uint8_t     ntu_led_hid;
static USHORT               ntu_led_nt;          /* fuer QUERY_INDICATORS */

static void
ntu_kbd_liste_init (void)
{
    if (InterlockedExchange(&ntu_kbd_liste_bereit, 1) == 0) {
        KeInitializeSpinLock(&ntu_kbd_liste_sperre);
    }
}

static void
ntukbd_led_callback (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntukbd_softc      *sc = usbd_xfer_softc(xfer);
    struct usb_device_request req;
    struct usb_page_cache    *pc;
    uint8_t                   led;

    switch (USB_GET_STATE(xfer)) {

        /*  Nach JEDER fertigen Uebertragung nachsehen, ob inzwischen ein
         *  neuer Zustand ansteht -- wie FreeBSDs ukbd (TRANSFERRED faellt in
         *  SETUP durch).  Laeuft ein SET_REPORT noch, tut
         *  usbd_transfer_start nichts; die erste Fassung sah nur beim Start
         *  nach, und der letzte Zustand eines schnellen Wechsels blieb
         *  liegen (auf zwei Prozessoren: 11x Caps, Tastatur zeigte 00).  */

        case USB_ST_TRANSFERRED:
        case USB_ST_SETUP:
            if (!sc->sc_led_neu) {
                break;
            }
            sc->sc_led_neu = 0;
            led = sc->sc_led;

            req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
            req.bRequest      = UR_SET_REPORT;
            USETW2(req.wValue, UHID_OUTPUT_REPORT, 0);
            USETW(req.wIndex, sc->sc_iface_no);
            USETW(req.wLength, 1);

            pc = usbd_xfer_get_frame(xfer, 0);
            usbd_copy_in(pc, 0, &req, sizeof(req));
            pc = usbd_xfer_get_frame(xfer, 1);
            usbd_copy_in(pc, 0, &led, 1);

            usbd_xfer_set_frame_len(xfer, 0, sizeof(req));
            usbd_xfer_set_frame_len(xfer, 1, 1);
            usbd_xfer_set_frames(xfer, 2);
            usbd_transfer_submit(xfer);
            break;

        default:
            /*  Tastaturen ohne Leuchten lehnen ab -- egal.  */
            break;
    }
}

/*  Einer Tastatur den aktuellen Zustand schicken.  PASSIVE_LEVEL.  */

static void
ntukbd_led_senden (struct ntukbd_softc *sc)
{
    mtx_lock(&sc->sc_mtx);
    sc->sc_led     = ntu_led_hid;
    sc->sc_led_neu = 1;
    usbd_transfer_start(sc->sc_xfer[NTUKBD_CTRL_LED]);
    mtx_unlock(&sc->sc_mtx);
}

static VOID
ntu_led_arbeiter (PVOID ctx)
{
    struct ntukbd_softc *jetzt[NTU_KBD_MAX];
    KIRQL                alt;
    int                  k;

    UNREFERENCED_PARAMETER(ctx);

    /*  Vor dem Arbeiten freigeben: eine Aenderung, die waehrenddessen
     *  kommt, reiht einen neuen Auftrag ein.  */

    InterlockedExchange(&ntu_led_eingereiht, 0);

    KeAcquireSpinLock(&ntu_kbd_liste_sperre, &alt);
    for (k = 0; k < NTU_KBD_MAX; k += 1) {
        jetzt[k] = ntu_kbd_liste[k];
        if (jetzt[k] != 0) {
            InterlockedIncrement(&jetzt[k]->sc_ref);
        }
    }
    KeReleaseSpinLock(&ntu_kbd_liste_sperre, alt);

    for (k = 0; k < NTU_KBD_MAX; k += 1) {
        if (jetzt[k] != 0) {
#ifdef NTU_EINGABE_PROTOKOLL
            ntu_printf("ukbd: Leuchten %02x an Tastatur %d\n", ntu_led_hid, k);
#endif
            ntukbd_led_senden(jetzt[k]);
            InterlockedDecrement(&jetzt[k]->sc_ref);
        }
    }
}

/*  Aus IOCTL_KEYBOARD_SET_INDICATORS (ntkbdport.c).  Jede Ebene.  */

void
ntu_kbd_leuchten (USHORT nt)
{
    uint8_t hid = 0;

#ifdef NTU_EINGABE_PROTOKOLL
    ntu_printf("ukbd: Leuchten -> %x (NT-Bits)\n", nt);
#endif
    ntu_kbd_liste_init();
    ntu_led_nt = nt;
    if (nt & KEYBOARD_NUM_LOCK_ON)    hid |= 0x01;
    if (nt & KEYBOARD_CAPS_LOCK_ON)   hid |= 0x02;
    if (nt & KEYBOARD_SCROLL_LOCK_ON) hid |= 0x04;
    ntu_led_hid = hid;

    if (InterlockedExchange(&ntu_led_eingereiht, 1) == 0) {
        ExInitializeWorkItem(&ntu_led_arbeit, ntu_led_arbeiter, NULL);
        ExQueueWorkItem(&ntu_led_arbeit, DelayedWorkQueue);
    }
}

USHORT
ntu_kbd_leuchten_jetzt (void)
{
    return ntu_led_nt;
}

/*  Eine Feststelltaste auf einer USB-Tastatur: Zustand umschalten und an
 *  alle USB-Tastaturen schicken.  Kommt doch ein SET_INDICATORS von NT
 *  (ohne PS/2 ist unser Anschluss Nummer 0), gilt dessen Zustand.  */

static void
ntu_kbd_umschalten (uint8_t usage)
{
    USHORT nt = ntu_led_nt;

    if (usage == 0x39) nt ^= KEYBOARD_CAPS_LOCK_ON;
    if (usage == 0x53) nt ^= KEYBOARD_NUM_LOCK_ON;
    if (usage == 0x47) nt ^= KEYBOARD_SCROLL_LOCK_ON;
    ntu_kbd_leuchten(nt);
}


/* ====================================================================
 *  Nach oben melden
 *
 *  kbdclass will die Daten auf DISPATCH_LEVEL.  Wir kommen von
 *  PASSIVE_LEVEL, heben an, melden, senken.  Kurz halten: der Rueckruf
 *  schreibt nur in kbdclass' eigenen Ring.
 * ==================================================================== */

/*  wd_neu: 0 = Wiederholung beenden, sonst der Code, der ab jetzt
 *  wiederholt wird; 0xFFFF = Wiederholung unveraendert lassen.
 *
 *  Beides -- Wiederholungszustand setzen und an kbdclass melden --
 *  geschieht unter sc_wd_sperre.  Der DPC der Wiederholung nimmt dieselbe
 *  Sperre.  So kann auf einem Mehrkernrechner keine letzte Wiederholung
 *  NACH dem Loslassen bei kbdclass ankommen; die Taste bliebe sonst fuer
 *  Windows gedrueckt.  */

#define NTUKBD_WD_UNVERAENDERT  0xFFFF

static VOID
ntukbd_melden (struct ntukbd_softc *sc, ULONG n, USHORT wd_neu)
{
    KIRQL old;
    ULONG verbraucht = 0;

    KeAcquireSpinLock(&sc->sc_wd_sperre, &old);

    if (wd_neu != NTUKBD_WD_UNVERAENDERT) {

        sc->sc_wd_code = wd_neu;

        if (wd_neu == 0) {
            KeCancelTimer(&sc->sc_wd_timer);
        } else {
            LARGE_INTEGER bis;
            ULONG         rate = ntu_typematik.Rate ? ntu_typematik.Rate : 30;

            /*  Erst nach der Verzoegerung, dann im Takt.  KeSetTimerEx
             *  setzt einen laufenden Zeitgeber neu -- eine neue Taste
             *  beginnt also wieder mit der vollen Verzoegerung.  */

            bis.QuadPart = -(LONGLONG)ntu_typematik.Delay * 10000;
            KeSetTimerEx(&sc->sc_wd_timer, bis, (LONG)(1000 / rate),
                         &sc->sc_wd_dpc);
        }
    }

    if (sc->sc_conn == 0 || sc->sc_conn->ClassService == 0 || n == 0) {
        KeReleaseSpinLock(&sc->sc_wd_sperre, old);
        return;
    }

    sc->sc_conn->ClassService(sc->sc_conn->ClassDeviceObject,
                             &sc->sc_ring[0],
                             &sc->sc_ring[n],
                             &verbraucht);

    KeReleaseSpinLock(&sc->sc_wd_sperre, old);

    InterlockedExchangeAdd((LONG *)&ntu_zaehler.AnschlaegeGesamt, (LONG)(n));
    InterlockedExchangeAdd((LONG *)&ntu_zaehler.AnschlaegeVerworf, (LONG)((n - verbraucht)));

    if (verbraucht != n) {

        /*  kbdclass hat weniger genommen, als wir gegeben haben -- sein
         *  Ring ist voll.  Nicht wiederholen: Tastendruecke, die ein
         *  volles kbdclass nicht mehr aufnimmt, sind ohnehin verloren,
         *  und ein Wiederholungsversuch bringt sie in falscher
         *  Reihenfolge nach.  Gezaehlt gehoert es aber.  */

        ntu_printf("ukbd: %u von %u Anschlaegen verworfen\n",
                   n - verbraucht, n);
    }
}

static ULONG
ntukbd_eintragen (struct ntukbd_softc *sc, ULONG n, USHORT code, int los)
{
    KEYBOARD_INPUT_DATA *k;

    if (code == 0 || n >= 32) {
        return n;
    }

    k = &sc->sc_ring[n];
    k->UnitId           = 0;
    k->MakeCode         = (USHORT)(code & 0xFF);
    k->Flags            = (USHORT)((los ? KEY_BREAK : KEY_MAKE)
                                   | ((code & E0) ? KEY_E0 : 0));
    k->Reserved         = 0;
    k->ExtraInformation = 0;

    return n + 1;
}


/* ====================================================================
 *  Tastenwiederholung
 *
 *  Eine USB-Tastatur wiederholt nie; eine PS/2-Tastatur tut es selbst,
 *  und NT verlaesst sich darauf.  Also wiederholt dieser Treiber, und zwar
 *  so, wie eine PS/2-Tastatur es taete:
 *
 *    - nur die ZULETZT gedrueckte Taste;
 *    - kommt eine weitere dazu, wiederholt ab dann diese, nach der
 *      vollen Verzoegerung;
 *    - wird die wiederholende losgelassen, ist Schluss -- auch wenn noch
 *      eine andere gehalten ist;
 *    - Umschalter (Strg, Umschalt, Alt, Gui) wiederholen nicht;
 *    - Feststelltasten (Umschalt-, Num-, Rollen-) und Druck/Pause auch
 *      nicht.  Windows schaltet sie beim Druecken um, und eine
 *      Wiederholung liesse sie im Takt flackern.
 *
 *  Wiederholt wird der Drueckcode allein, ohne Loslassen dazwischen --
 *  genau das, was eine PS/2-Tastatur sendet; USER erkennt daran die
 *  Wiederholung.
 * ==================================================================== */

static VOID
ntukbd_wd_dpc (PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    struct ntukbd_softc *sc = (struct ntukbd_softc *)ctx;
    KEYBOARD_INPUT_DATA *k  = &sc->sc_wd_eintrag;
    ULONG                verbraucht = 0;

    KeAcquireSpinLockAtDpcLevel(&sc->sc_wd_sperre);

    if (sc->sc_wd_code != 0 && sc->sc_conn != 0 &&
        sc->sc_conn->ClassService != 0) {

        k->UnitId           = 0;
        k->MakeCode         = (USHORT)(sc->sc_wd_code & 0xFF);
        k->Flags            = (USHORT)(KEY_MAKE |
                                       ((sc->sc_wd_code & E0) ? KEY_E0 : 0));
        k->Reserved         = 0;
        k->ExtraInformation = 0;

        sc->sc_conn->ClassService(sc->sc_conn->ClassDeviceObject,
                                 k, k + 1, &verbraucht);

        InterlockedIncrement((LONG *)&ntu_zaehler.AnschlaegeGesamt);
        InterlockedExchangeAdd((LONG *)&ntu_zaehler.AnschlaegeVerworf, (LONG)((1 - verbraucht)));
    }

    KeReleaseSpinLockFromDpcLevel(&sc->sc_wd_sperre);
}

static int
ntukbd_wiederholbar (uint8_t u)
{
    switch (u) {
        case 0x39:              /* Umschalt-Feststell */
        case 0x46:              /* Druck              */
        case 0x47:              /* Rollen             */
        case 0x48:              /* Pause              */
        case 0x53:              /* Num                */
            return 0;
        default:
            return u > 3 && u < 0x68 && ntu_hid2set1[u] != 0;
    }
}


/* ====================================================================
 *  Der Bericht
 *
 *  Das Startprotokoll liefert acht Byte:
 *
 *      [0]  Umschalter, ein Bit je Strg/Umschalt/Alt/Gui links+rechts
 *      [1]  reserviert
 *      [2..7]  bis zu sechs gleichzeitig gedrueckte Verwendungen
 *
 *  Die Liste ist eine **Menge**, keine Folge: sie sagt, was gerade
 *  gedrueckt ist, nicht was sich geaendert hat.  Die Aenderung muss aus
 *  dem Vergleich mit dem vorigen Bericht kommen -- wer das uebersieht,
 *  bekommt bei jeder Wiederholung neue Anschlaege statt keiner.
 *
 *  Steht in [2] der Wert 0x01, ist die Tastatur ueberlastet (mehr als
 *  sechs Tasten).  Dann sagt der Bericht nichts Brauchbares und wird
 *  verworfen -- nicht etwa als "alle Tasten los" ausgelegt.
 * ==================================================================== */

static int
ntukbd_enthalten (const uint8_t *liste, uint8_t u)
{
    int i;

    for (i = 2; i < 8; i += 1) {
        if (liste[i] == u) {
            return 1;
        }
    }

    return 0;
}

static VOID
ntukbd_bericht (struct ntukbd_softc *sc, const uint8_t *r)
{
    ULONG  n  = 0;
    int    i;
    USHORT wd = NTUKBD_WD_UNVERAENDERT;

#if defined(NTU_EINGABE_PROTOKOLL)      /* nur Diagnosebau: je Bericht! */
    ntu_printf("ukbd: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
#endif

    if (r[2] == 0x01) {
        return;                         /* Ueberlauf, siehe oben */
    }

    /*  Umschalter zuerst: Strg muss vor dem Buchstaben ankommen, sonst
     *  sieht die Anwendung ein nacktes C statt Strg-C.  */

    for (i = 0; i < 8; i += 1) {

        uint8_t jetzt  = (uint8_t)((r[0]          >> i) & 1);
        uint8_t vorher = (uint8_t)((sc->sc_last[0] >> i) & 1);

        if (jetzt != vorher) {
            n = ntukbd_eintragen(sc, n, ntu_mod2set1[i], !jetzt);
        }
    }

    /*  Losgelassen vor gedrueckt, aus demselben Grund.  */

    for (i = 2; i < 8; i += 1) {

        uint8_t u = sc->sc_last[i];

        if (u > 3 && u < 0x68 && !ntukbd_enthalten(r, u)) {
            n = ntukbd_eintragen(sc, n, ntu_hid2set1[u], 1);
        }

        /*  F13..F24: Sondertasten -- was sie tun, sagt ihre Belegung
         *  (Vorgabe: die Taste selbst, VK_F13..VK_F24).  */
        if (u >= 0x68 && u <= 0x73 && !ntukbd_enthalten(r, u)) {
            ntu_sonder_taste(ntu_sonder_index(0x07, u), 0);
        }
    }

    for (i = 2; i < 8; i += 1) {

        uint8_t u = r[i];

        if (u >= 0x68 && u <= 0x73 && !ntukbd_enthalten(sc->sc_last, u)) {
            ntu_sonder_taste(ntu_sonder_index(0x07, u), 1);
        }

        if (u > 3 && u < 0x68 && !ntukbd_enthalten(sc->sc_last, u)) {
            n = ntukbd_eintragen(sc, n, ntu_hid2set1[u], 0);

            /*  Feststelltasten: den Leuchten-Zustand selbst umschalten.
             *  kbdclass schickt IOCTL_KEYBOARD_SET_INDICATORS nur an
             *  Anschluss 0 -- mit PS/2 im Rechner ist das i8042prt, und
             *  USB-Tastaturen bekaemen die Leuchten nie.  */

            if (u == 0x39 || u == 0x53 || u == 0x47) {
                ntu_kbd_umschalten(u);
            }

            /*  Die zuletzt neu gedrueckte Taste wird die wiederholende --
             *  oder, wenn sie nicht wiederholt, beendet sie die
             *  Wiederholung der vorigen, wie auf PS/2.  */

            wd = ntukbd_wiederholbar(u) ? ntu_hid2set1[u] : 0;
        }
    }

    /*  Nichts neu gedrueckt, aber die wiederholende losgelassen?  */

    if (wd == NTUKBD_WD_UNVERAENDERT && sc->sc_wd_code != 0) {

        int noch = 0;

        for (i = 2; i < 8; i += 1) {
            if (r[i] > 3 && r[i] < 0x68 && ntu_hid2set1[r[i]] == sc->sc_wd_code) {
                noch = 1;
            }
        }

        if (!noch) {
            wd = 0;
        }
    }

    memcpy(sc->sc_last, r, 8);
    ntukbd_melden(sc, n, wd);
}

static void
ntukbd_intr_callback (struct usb_xfer *xfer, usb_error_t error)
{
    struct ntukbd_softc *sc = usbd_xfer_softc(xfer);
    struct usb_page_cache *pc;
    uint8_t  buf[8];
    int      len;

    usbd_xfer_status(xfer, &len, NULL, NULL, NULL);

    switch (USB_GET_STATE(xfer)) {

        case USB_ST_TRANSFERRED:

            if (len >= 8) {
                len = 8;
            }

            if (len > 0) {
                memset(buf, 0, sizeof(buf));
                pc = usbd_xfer_get_frame(xfer, 0);
                usbd_copy_out(pc, 0, buf, len);
                ntukbd_bericht(sc, buf);
            }

            /* FALLTHROUGH */

        case USB_ST_SETUP:
tr_setup:
            usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
            usbd_transfer_submit(xfer);
            break;

        default:

            if (error != USB_ERR_CANCELLED) {

                /*  Anhalten zuruecknehmen und weiter.  Eine Tastatur,
                 *  die einmal stehenbleibt, bleibt sonst stehen, und
                 *  der Anwender sieht einen Haenger.  */

                usbd_xfer_set_stall(xfer);
                goto tr_setup;
            }

            break;
    }
}


/* ====================================================================
 *  probe und attach
 * ==================================================================== */

static int
ntukbd_probe (device_t dev)
{
    struct usb_attach_arg *uaa = device_get_ivars(dev);

    if (uaa->usb_mode != USB_MODE_HOST) {
        return ENXIO;
    }

    if (uaa->info.bInterfaceClass != UICLASS_HID) {
        return ENXIO;
    }

    /*  Nur das Startprotokoll.  Eine Tastatur ohne Startprotokoll
     *  braucht einen HID-Berichtsdeskriptor-Leser, und den gibt es hier
     *  nicht -- ein ehrliches ENXIO statt einer halben Auswertung.  */

    if (uaa->info.bInterfaceSubClass == UISUBCLASS_BOOT &&
        uaa->info.bInterfaceProtocol == UIPROTO_BOOT_KEYBOARD) {
        return BUS_PROBE_DEFAULT;
    }

    return ENXIO;
}

static int
ntukbd_attach (device_t dev)
{
    struct ntukbd_softc *sc  = device_get_softc(dev);
    struct usb_attach_arg *uaa = device_get_ivars(dev);
    int err;

    sc->sc_dev         = dev;
    sc->sc_udev        = uaa->device;
    sc->sc_iface_index = uaa->info.bIfaceIndex;
    sc->sc_iface_no    = uaa->info.bIfaceNum;

    device_set_desc(dev, "USB-Tastatur (Startprotokoll)");
    mtx_init(&sc->sc_mtx, "ntukbd", NULL, MTX_DEF);

    /*  Startprotokoll erzwingen.  Ohne das liefern viele Tastaturen den
     *  Bericht ihres eigenen Deskriptors, und der ist nicht acht Byte
     *  lang.  Der Rueckgabewert wird bewusst nicht geprueft: Tastaturen,
     *  die nur das Startprotokoll koennen, lehnen die Anforderung ab und
     *  tun trotzdem das Richtige.  */

    (void)usbd_req_set_protocol(sc->sc_udev, NULL, sc->sc_iface_index, 0);

    /*  Berichte nur bei Aenderung (Idle 0).  Die erste Fassung begruendete
     *  das mit "NT wiederholt selbst" -- das war falsch und der Grund,
     *  warum gehaltene Tasten nur einmal kamen: NT wiederholt NICHT, es
     *  verlaesst sich auf die Typematic der PS/2-Tastatur.  Idle 0 bleibt
     *  trotzdem richtig, denn die Wiederholung erzeugt jetzt dieser
     *  Treiber selbst (ntukbd_wd_dpc), im Takt, den NT erwartet; eine
     *  vom Geraet wiederholte Meldung braechte nichts, weil der Vergleich
     *  mit dem vorigen Bericht sie ohnehin verschluckt.  */

    (void)usbd_req_set_idle(sc->sc_udev, NULL, sc->sc_iface_index, 0, 0);

    KeInitializeSpinLock(&sc->sc_wd_sperre);
    KeInitializeTimer(&sc->sc_wd_timer);
    KeInitializeDpc(&sc->sc_wd_dpc, ntukbd_wd_dpc, sc);
    sc->sc_wd_code = 0;

    err = usbd_transfer_setup(uaa->device, &sc->sc_iface_index,
                              sc->sc_xfer, ntukbd_config,
                              NTUKBD_N_TRANSFER, sc, &sc->sc_mtx);

    if (err != 0) {
        device_printf(dev, "usbd_transfer_setup: %d\n", err);
        mtx_destroy(&sc->sc_mtx);
        return ENXIO;
    }

    sc->sc_conn = ntu_kbd_anmelden();

    mtx_lock(&sc->sc_mtx);
    usbd_transfer_start(sc->sc_xfer[NTUKBD_INTR_DT]);
    mtx_unlock(&sc->sc_mtx);

    /*  In die Liste fuer die Leuchten, und gleich den aktuellen Zustand
     *  -- sonst stuende Num-Lock auf einer frisch gesteckten Tastatur
     *  falsch, bis jemand eine Feststelltaste drueckt.  */

    {
        KIRQL alt;
        int   k;
        ntu_kbd_liste_init();
        KeAcquireSpinLock(&ntu_kbd_liste_sperre, &alt);
        for (k = 0; k < NTU_KBD_MAX && ntu_kbd_liste[k] != 0; k += 1) {
        }
        if (k < NTU_KBD_MAX) {
            ntu_kbd_liste[k] = sc;
        }
        KeReleaseSpinLock(&ntu_kbd_liste_sperre, alt);
    }
    ntukbd_led_senden(sc);

    return 0;
}

static int
ntukbd_detach (device_t dev)
{
    struct ntukbd_softc *sc = device_get_softc(dev);

    KIRQL old;
    int   k;

    /*  Aus der Leuchten-Liste, und warten, bis ein laufender
     *  Arbeitsauftrag diese Tastatur nicht mehr anfasst.  */

    ntu_kbd_liste_init();
    KeAcquireSpinLock(&ntu_kbd_liste_sperre, &old);
    for (k = 0; k < NTU_KBD_MAX; k += 1) {
        if (ntu_kbd_liste[k] == sc) {
            ntu_kbd_liste[k] = 0;
        }
    }
    KeReleaseSpinLock(&ntu_kbd_liste_sperre, old);
    while (sc->sc_ref != 0) {
        LARGE_INTEGER t;
        t.QuadPart = -10 * 1000 * 10;   /* 10 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }

    /*  Zuerst die Wiederholung stillegen, und zwar vollstaendig, bevor
     *  der Speicher dieser Tastatur freigegeben wird:
     *
     *    1. unter der Sperre: nichts mehr wiederholen, Zeitgeber anhalten,
     *       Anschluss kappen;
     *    2. einen schon eingereihten DPC wieder herausnehmen;
     *    3. die Sperre noch einmal nehmen und freigeben -- laeuft der DPC
     *       gerade auf einem anderen Kern, ist er danach fertig; er
     *       beruehrt nach dem Freigeben der Sperre nichts mehr.
     *
     *  Dann erst abmelden und die Uebertragung abraeumen.  */

    KeAcquireSpinLock(&sc->sc_wd_sperre, &old);
    sc->sc_wd_code = 0;
    KeCancelTimer(&sc->sc_wd_timer);
    sc->sc_conn = 0;
    KeReleaseSpinLock(&sc->sc_wd_sperre, old);

    KeRemoveQueueDpc(&sc->sc_wd_dpc);

    KeAcquireSpinLock(&sc->sc_wd_sperre, &old);
    KeReleaseSpinLock(&sc->sc_wd_sperre, old);

    /*  Reihenfolge wie bisher: erst abmelden, dann die Uebertragung
     *  abraeumen.  */

    ntu_kbd_abmelden();

    usbd_transfer_unsetup(sc->sc_xfer, NTUKBD_N_TRANSFER);
    mtx_destroy(&sc->sc_mtx);
    return 0;
}

static device_method_t ntukbd_methods[] = {
    DEVMETHOD(device_probe,  ntukbd_probe),
    DEVMETHOD(device_attach, ntukbd_attach),
    DEVMETHOD(device_detach, ntukbd_detach),
    DEVMETHOD_END
};

static driver_t ntukbd_driver = {
    .name    = "ukbd",
    .methods = ntukbd_methods,
    .size    = sizeof(struct ntukbd_softc),
};

DRIVER_MODULE(ukbd, uhub, ntukbd_driver, 0, 0);
