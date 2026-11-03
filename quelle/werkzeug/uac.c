/*
 *  werkzeug/uac.c -- siehe uac.h.  Grundlage: USB Audio Class 1.0,
 *  Abschnitte 4 (Deskriptoren) und 5.2 (Anfragen); FreeBSD uaudio.c.
 *  Ohne C-Laufzeit (ntbasis): nur memset/memcpy/wsprintf.
 */

#include "uac.h"
#include "ntbasis.h"

#define CS_INTERFACE   0x24
#define CS_ENDPOINT    0x25
#define AC_HEADER      0x01
#define AC_IT          0x02
#define AC_OT          0x03
#define AC_MIXER       0x04
#define AC_SELECTOR    0x05
#define AC_FEATURE     0x06
#define AC_PROCESSING  0x07
#define AC_EXTENSION   0x08
#define AS_GENERAL     0x01
#define AS_FORMAT      0x02
#define SET_CUR        0x01
#define GET_MIN        0x82
#define GET_MAX        0x83
#define GET_RES        0x84
#define FU_MUTE        0x01
#define FU_VOLUME      0x02

static char          puffer[sizeof(NTUG_ERGEBNIS) + 8192];
static unsigned char konf[4096];

static HANDLE
dev_oeffnen (int n)
{
    char name[24];
    wsprintfA(name, "\\\\.\\UsbDev%d", n);
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}

HANDLE
uac_strom_oeffnen (int n)
{
    char name[24];
    wsprintfA(name, "\\\\.\\UsbAudio%d", n);
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

/*  ntugen oeffnet exklusiv: greifen Lautstaerkeregelung und ein Programm
 *  zugleich zu, scheitert einer kurz (Freigabeverletzung) -- kurz warten
 *  und noch einmal.  Die Griffe werden nur fuer eine Anfrage gehalten.  */
HANDLE
uac_ac_oeffnen (const UAC_GERAET *g)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    int k;
    for (k = 0; k < 20 && h == INVALID_HANDLE_VALUE; k += 1) {
        h = dev_oeffnen(g->ac_dev);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_ACCESS_DENIED) break;
            Sleep(10);
        }
    }
    return h;
}

static NTUG_ERGEBNIS *
rufe (HANDLE h, DWORD code, void *ein, DWORD einlen)
{
    DWORD got = 0;
    if (!DeviceIoControl(h, code, ein, einlen, puffer, sizeof(puffer), &got, NULL) ||
        got < sizeof(NTUG_ERGEBNIS)) {
        return 0;
    }
    return (NTUG_ERGEBNIS *)puffer;
}

static int
info_holen (HANDLE h, NTUG_INFO *i)
{
    NTUG_ERGEBNIS *e = rufe(h, IOCTL_NTUG_INFO, NULL, 0);
    if (e == 0 || e->Fehler != 0) return 0;
    memcpy(i, e + 1, sizeof(*i));
    return 1;
}

long
uac_steuer (HANDLE h, unsigned char rt, unsigned char rq, unsigned short wv,
            unsigned short wi, void *daten, unsigned short n)
{
    static struct { NTUG_STEUER s; unsigned char d[64]; } q;
    NTUG_ERGEBNIS *e;

    if (n > sizeof(q.d)) return -1;
    memset(&q, 0, sizeof(q));
    q.s.bmRequestType = rt; q.s.bRequest = rq;
    q.s.wValue = wv; q.s.wIndex = wi; q.s.wLength = n;
    q.s.Zeitlimit = 1000;                    /* manche Geraete schweigen */
    if ((rt & 0x80) == 0) memcpy(q.d, daten, n);
    e = rufe(h, IOCTL_NTUG_STEUER, &q, sizeof(q.s) + ((rt & 0x80) ? 0 : n));
    if (e == 0) return -1;
    if ((rt & 0x80) && e->Fehler == 0) memcpy(daten, e + 1, n);
    return e->Fehler;
}

static unsigned long
u24 (const unsigned char *p) { return p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16); }

static void
auswerten (UAC_GERAET *g, unsigned long konflen)
{
    unsigned long o = 0;
    int klasse = 0, unter = 0;
    UAC_ALT *a = 0;

    while (o + 2 <= konflen) {
        const unsigned char *p = konf + o;
        unsigned char l = p[0], t = p[1];

        if (l < 2 || o + l > konflen) break;

        if (t == 4 && l >= 9) {                        /* Schnittstelle */
            klasse = p[5]; unter = p[6];
            a = 0;
            if (klasse == 1 && unter == 2 && p[3] > 0 && g->nalt < UAC_MAX_ALT) {
                a = &g->alt[g->nalt++];
                memset(a, 0, sizeof(*a));
                a->ifnr = p[2]; a->alt = p[3];
            }
        } else if (t == 5 && l >= 7 && a != 0) {      /* Endpunkt, 7 oder 9 Byte */
            if ((p[3] & 3) == 1) {                     /* isochron */
                a->ep = p[2]; a->epattr = p[3];
                a->maxpaket = (unsigned short)(p[4] | (p[5] << 8));
                a->intervall = p[6];
            }
        } else if (t == CS_ENDPOINT && l >= 7 && a != 0) {
            a->cs_attr = p[3];
        } else if (t == CS_INTERFACE && klasse == 1 && unter == 1) {
            if (p[2] == AC_HEADER && l >= 5) {
                g->adc = (unsigned short)(p[3] | (p[4] << 8));
                g->uac = g->adc >= 0x0300 ? 3 : g->adc >= 0x0200 ? 2 : 1;
            }
            /*  Signalweg merken (UAC1 Abschnitt 4.3.2).  */
            if (p[2] >= AC_IT && p[2] <= AC_EXTENSION && l >= 5 && g->nent < UAC_MAX_ENT) {
                UAC_ENT *e = &g->ent[g->nent++];
                int q0 = 0, nq = 0, k;
                memset(e, 0, sizeof(*e));
                e->id = p[3]; e->art = p[2];
                switch (p[2]) {
                case AC_IT:        e->typ = (unsigned short)(p[4] | (p[5] << 8)); break;
                case AC_OT:        e->typ = (unsigned short)(p[4] | (p[5] << 8)); q0 = 7; nq = 1; break;
                case AC_FEATURE:   q0 = 4; nq = 1; break;
                case AC_MIXER:
                case AC_SELECTOR:  q0 = 5; nq = p[4]; break;
                case AC_PROCESSING:
                case AC_EXTENSION: q0 = 7; nq = (l > 6) ? p[6] : 0; break;
                }
                for (k = 0; k < nq && k < 8 && q0 + k < l; k += 1) e->quelle[e->nq++] = p[q0 + k];
            }
            if (p[2] == AC_FEATURE && l >= 7 && g->nfu < UAC_MAX_FU) {
                UAC_FU *f = &g->fu[g->nfu++];
                int k, n = p[5] ? (l - 7) / p[5] : 0;
                memset(f, 0, sizeof(*f));
                f->id = p[3]; f->quelle = p[4]; f->groesse = p[5];
                f->kanaele = (unsigned char)(n > 9 ? 9 : n);
                for (k = 0; k < f->kanaele; k += 1) {
                    const unsigned char *c = p + 6 + k * p[5];
                    f->ctl[k] = c[0] | (p[5] > 1 ? (c[1] << 8) : 0);
                }
            }
        } else if (t == CS_INTERFACE && a != 0) {
            if (p[2] == AS_GENERAL && l >= 7) {
                a->link = p[3]; a->format = (unsigned short)(p[5] | (p[6] << 8));
            } else if (p[2] == AS_FORMAT && l >= 8 && p[3] == 1) {
                int k;
                a->kanaele = p[4]; a->bytes = p[5]; a->bits = p[6]; a->ratentyp = p[7];
                if (p[7] == 0 && l >= 14) {
                    a->rate[0] = u24(p + 8); a->rate[1] = u24(p + 11);
                } else {
                    for (k = 0; k < p[7] && k < UAC_MAX_RATE && 8 + 3 * k + 3 <= l; k += 1) {
                        a->rate[k] = u24(p + 8 + 3 * k);
                    }
                }
            }
        }
        o += l;
    }
}

int
uac_suchen (UAC_GERAET *g, int nr)
{
    NTUG_INFO i;
    NTUG_DESK d;
    NTUG_ERGEBNIS *e;
    HANDLE h;
    unsigned long konflen = 0;
    int n, k, gesehen = 0;

    memset(g, 0, sizeof(*g));
    g->ac_dev = -1;
    for (k = 0; k < UAC_MAX_IF; k += 1) g->as_dev[k] = g->au_dev[k] = -1;

    /*  nr-te AudioControl-Schnittstelle (Klasse 1, Unterklasse 1).  */
    for (n = 0; n < 64 && g->ac_dev < 0; n += 1) {
        h = dev_oeffnen(n);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (info_holen(h, &i) && i.IfKlasse == 1 && i.IfUnter == 1 && gesehen++ == nr) {
            g->ac_dev = n; g->ac_if = i.IfNummer;
            g->vid = i.Hersteller; g->pid = i.Produkt;
            g->protokoll = i.IfProtokoll;
            memcpy(g->name, i.ProduktText, sizeof(g->name) - 1);
            /*  0x20 = UAC2, 0x30 = UAC3, sonst UAC1 (wie Linux; QEMUs
             *  usb-audio meldet 0x04).  bcdADC entscheidet spaeter.  */
            g->uac = i.IfProtokoll == 0x20 ? 2 : i.IfProtokoll == 0x30 ? 3 : 1;
            memset(&d, 0, sizeof(d));
            d.Typ = 2; d.Laenge = sizeof(konf);
            e = rufe(h, IOCTL_NTUG_DESKRIPTOR, &d, sizeof(d));
            if (e != 0 && e->Fehler == 0) {
                konflen = e->Laenge < sizeof(konf) ? e->Laenge : sizeof(konf);
                memcpy(konf, e + 1, konflen);
            }
        }
        CloseHandle(h);
    }
    if (g->ac_dev < 0) return 0;

    /*  AudioStreaming-Schnittstellen: an ntugen ...  */
    for (n = 0; n < 64; n += 1) {
        h = dev_oeffnen(n);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (info_holen(h, &i) && i.Hersteller == g->vid && i.Produkt == g->pid &&
            i.IfKlasse == 1 && i.IfUnter == 2 && i.IfNummer < UAC_MAX_IF) {
            g->as_dev[i.IfNummer] = n;
        }
        CloseHandle(h);
    }
    /*  ... und an USBAUDIO.SYS.  */
    for (n = 0; n < 8; n += 1) {
        NTUA_INFO ai;
        DWORD got = 0;
        h = uac_strom_oeffnen(n);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (DeviceIoControl(h, IOCTL_NTUA_INFO, NULL, 0, &ai, sizeof(ai), &got, NULL) &&
            ai.Hersteller == g->vid && ai.Produkt == g->pid && ai.IfNummer < UAC_MAX_IF) {
            g->au_dev[ai.IfNummer] = n;
        }
        CloseHandle(h);
    }

    auswerten(g, konflen);
    g->konflen = konflen < sizeof(g->konf) ? konflen : sizeof(g->konf);
    memcpy(g->konf, konf, g->konflen);
    return 1;
}

int
uac_kann_rate (const UAC_ALT *a, unsigned long rate)
{
    int c;
    if (a->ratentyp == 0) return a->rate[0] <= rate && rate <= a->rate[1];
    for (c = 0; c < a->ratentyp && c < UAC_MAX_RATE; c += 1) if (a->rate[c] == rate) return 1;
    return 0;
}

UAC_ALT *
uac_wiedergabe (UAC_GERAET *g, unsigned long wunsch, unsigned long *rate)
{
    UAC_ALT *erste = 0, *a48 = 0;
    int k;

    for (k = 0; k < g->nalt; k += 1) {
        UAC_ALT *a = &g->alt[k];
        if ((a->ep & 0x80) || a->ep == 0 || a->format != 1 || a->bytes != 2 || a->kanaele != 2) continue;
        if (wunsch && uac_kann_rate(a, wunsch)) { *rate = wunsch; return a; }
        if (a48 == 0 && uac_kann_rate(a, 48000)) a48 = a;
        if (erste == 0) erste = a;
    }
    if (a48 != 0) { *rate = 48000; return a48; }
    if (erste != 0) { *rate = erste->ratentyp == 0 ? erste->rate[1] : erste->rate[0]; }
    return erste;
}

UAC_ALT *
uac_aufnahme (UAC_GERAET *g, unsigned long wunsch, unsigned long *rate)
{
    UAC_ALT *erste = 0, *a48 = 0;
    int k;

    for (k = 0; k < g->nalt; k += 1) {
        UAC_ALT *a = &g->alt[k];
        if (!(a->ep & 0x80) || a->format != 1 || a->bytes != 2 ||
            (a->kanaele != 1 && a->kanaele != 2)) continue;
        if (wunsch && uac_kann_rate(a, wunsch)) { *rate = wunsch; return a; }
        if (a48 == 0 && uac_kann_rate(a, 48000)) a48 = a;
        if (erste == 0) erste = a;
    }
    if (a48 != 0) { *rate = 48000; return a48; }
    if (erste != 0) { *rate = erste->ratentyp == 0 ? erste->rate[1] : erste->rate[0]; }
    return erste;
}

UAC_FU *
uac_fu (UAC_GERAET *g, int bit)
{
    int k, c;
    for (k = 0; k < g->nfu; k += 1)
        for (c = 0; c < g->fu[k].kanaele; c += 1)
            if (g->fu[k].ctl[c] & bit) return &g->fu[k];
    return 0;
}

static UAC_ENT *
ent_suchen (UAC_GERAET *g, unsigned char id)
{
    int k;
    for (k = 0; k < g->nent; k += 1) if (g->ent[k].id == id) return &g->ent[k];
    return 0;
}

static UAC_FU *
fu_mit_id (UAC_GERAET *g, unsigned char id)
{
    int k;
    for (k = 0; k < g->nfu; k += 1) if (g->fu[k].id == id) return &g->fu[k];
    return 0;
}

UAC_FU *
uac_fu_fuer (UAC_GERAET *g, const UAC_ALT *a)
{
    unsigned char weg[UAC_MAX_ENT];
    int n = 0, k, i, j, tiefe;

    if (a == 0 || a->link == 0) return 0;

    if (!(a->ep & 0x80)) {
        /*  Wiedergabe: bTerminalLink ist das Eingangsterminal (USB-Strom).
         *  Vorwaerts: wer dieses Glied als Quelle hat -- bis zur ersten FU.  */
        weg[n++] = a->link;
        for (tiefe = 0; tiefe < 8 && n < UAC_MAX_ENT; tiefe += 1) {
            int vorher = n;
            for (k = 0; k < g->nent; k += 1) {
                UAC_ENT *e = &g->ent[k];
                for (i = 0; i < e->nq; i += 1) {
                    for (j = 0; j < vorher; j += 1) {
                        if (e->quelle[i] != weg[j]) continue;
                        if (e->art == AC_FEATURE && fu_mit_id(g, e->id)) return fu_mit_id(g, e->id);
                        {
                            int b, schon = 0;
                            for (b = 0; b < n; b += 1) if (weg[b] == e->id) schon = 1;
                            if (!schon) weg[n++] = e->id;
                        }
                        if (n >= UAC_MAX_ENT) return 0;
                        j = vorher; i = e->nq;     /* naechstes Glied */
                    }
                }
            }
            if (n == vorher) break;
        }
        return 0;
    }

    /*  Aufnahme: bTerminalLink ist das Ausgangsterminal (USB-Strom).
     *  Rueckwaerts ueber die Quellen -- bis zur ersten FU.  */
    weg[n++] = a->link;
    for (k = 0; k < n && k < UAC_MAX_ENT; k += 1) {
        UAC_ENT *e = ent_suchen(g, weg[k]);
        if (e == 0) continue;
        if (e->art == AC_FEATURE && k > 0 && fu_mit_id(g, e->id)) return fu_mit_id(g, e->id);
        for (i = 0; i < e->nq && n < UAC_MAX_ENT; i += 1) weg[n++] = e->quelle[i];
    }
    return 0;
}

static int
fu_hat (UAC_FU *f, int kanal, int bit)
{
    return f != 0 && kanal < f->kanaele && (f->ctl[kanal] & bit) != 0;
}

int
uac_laut_bereich (UAC_GERAET *g, UAC_FU *f, HANDLE h, int kanal, short *mn, short *mx, short *res)
{
    unsigned short wv = (unsigned short)((FU_VOLUME << 8) | kanal), wi;

    if (!fu_hat(f, kanal, 2)) return 0;
    wi = (unsigned short)((f->id << 8) | g->ac_if);
    *mn = *mx = *res = 0;
    if (uac_steuer(h, 0xA1, GET_MIN, wv, wi, mn, 2) != 0) return 0;
    if (uac_steuer(h, 0xA1, GET_MAX, wv, wi, mx, 2) != 0) return 0;
    uac_steuer(h, 0xA1, GET_RES, wv, wi, res, 2);
    return 1;
}

int
uac_laut_setzen (UAC_GERAET *g, UAC_FU *f, HANDLE h, int kanal, short wert)
{
    if (!fu_hat(f, kanal, 2)) return 0;
    return uac_steuer(h, 0x21, SET_CUR, (unsigned short)((FU_VOLUME << 8) | kanal),
                      (unsigned short)((f->id << 8) | g->ac_if), &wert, 2) == 0;
}

int
uac_laut_lesen (UAC_GERAET *g, UAC_FU *f, HANDLE h, int kanal, short *wert)
{
    if (!fu_hat(f, kanal, 2)) return 0;
    return uac_steuer(h, 0xA1, 0x81, (unsigned short)((FU_VOLUME << 8) | kanal),
                      (unsigned short)((f->id << 8) | g->ac_if), wert, 2) == 0;
}

int
uac_stumm_setzen (UAC_GERAET *g, UAC_FU *f, HANDLE h, int an)
{
    unsigned char b = (unsigned char)(an ? 1 : 0);
    int c, ok = 0;

    if (f == 0) return 0;
    for (c = 0; c < f->kanaele; c += 1) {
        if (f->ctl[c] & 1) {
            ok |= uac_steuer(h, 0x21, SET_CUR, (unsigned short)((FU_MUTE << 8) | c),
                             (unsigned short)((f->id << 8) | g->ac_if), &b, 1) == 0;
        }
    }
    return ok;
}

int
uac_stumm_lesen (UAC_GERAET *g, UAC_FU *f, HANDLE h, int *an)
{
    unsigned char b = 0;
    int c;

    if (f == 0) return 0;
    for (c = 0; c < f->kanaele; c += 1) {
        if ((f->ctl[c] & 1) &&
            uac_steuer(h, 0xA1, 0x81, (unsigned short)((FU_MUTE << 8) | c),
                       (unsigned short)((f->id << 8) | g->ac_if), &b, 1) == 0) {
            *an = b;
            return 1;
        }
    }
    return 0;
}
