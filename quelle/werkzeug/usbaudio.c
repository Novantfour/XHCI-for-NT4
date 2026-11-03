/*
 *  werkzeug/usbaudio.c -- winmm-Wave-Treiber fuer USB-Audio (usbaudio.dll).
 *
 *  Eingetragen unter HKLM\...\Drivers32 als "wave" (oder wave1..wave9,
 *  AUDIOINST waehlt einen freien Namen).  winmm laedt die DLL und ruft
 *  DriverProc, wodMessage (Wiedergabe), widMessage (Aufnahme) und
 *  mxdMessage (Mixer: Lautstaerkeregelung, Lautsprecher-Symbol).
 *
 *  Weg der Daten:  Programm --waveOutWrite--> Warteschlange --Faden-->
 *  Formatwandlung --IOCTL_NTUA_SCHREIBEN--> Ring in USBAUDIO.SYS -->
 *  isochron an das Geraet.
 *
 *  Formatwandlung: jedes PCM-Format, das ein Programm verlangt (8/16 Bit,
 *  Mono/Stereo, 8000..48000 Hz), wird auf das Geraetformat gebracht --
 *  16 Bit Stereo mit der Rate, die das Geraet kann (dieselbe, wenn es sie
 *  kann, sonst 48 kHz).  Raten: lineare Interpolation, fortlaufend ueber
 *  Puffergrenzen (kein Knacken zwischen zwei WAVEHDR).
 *
 *  WOM_DONE kommt, sobald ein Puffer im Ring liegt (hoechstens ~85 ms vor
 *  dem Hoeren).  Schliessen wartet, bis der Ring leer gespielt ist, damit
 *  das Ende eines Klangs nicht abgeschnitten wird.  GETPOS rechnet die vom
 *  Treiber gespielten Byte auf das Format des Programms zurueck.
 *
 *  Lautstaerke (waveOutSetVolume): auf die Feature Unit des Geraets, in dB
 *  (20*log10), ueber ntugen an der AudioControl-Schnittstelle.
 *
 *  Nur kernel32, user32, advapi32, winmm; keine C-Laufzeit (ntbasis, NTB_DLL).
 */

#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include "ntbasis.h"
#include "uac.h"

#define MAX_GERAETE   4
#define RING_BYTE     16384                 /* ~85 ms bei 48 kHz Stereo 16 Bit */
#define AUS_ABT       1024                  /* Abtastungen je Schreibblock */

typedef struct {
    UAC_GERAET     g;
    UAC_ALT       *alt;                     /* Wiedergabe */
    unsigned long  rate48;
    int            au;                      /* UsbAudioN der Wiedergabe */
    UAC_ALT       *alt_ein;                 /* Aufnahme */
    int            au_ein;
    DWORD          lautstaerke;             /* zuletzt gesetzt, wie winmm */
    LONG           stroeme;                 /* offene Wiedergabe-/Aufnahmestroeme */
} GERAET;

static GERAET           geraete[MAX_GERAETE];
static int              anzahl = -1;        /* Wiedergabegeraete */
static int              anzahl_ein = -1;    /* Aufnahmegeraete */
static int              anzahl_alle = -1;   /* alle Audiogeraete = Mixer */
static int              wo_liste[MAX_GERAETE], wi_liste[MAX_GERAETE];
static CRITICAL_SECTION gsperre;

typedef struct INST {
    int               nr;
    HANDLE            h;                    /* \\.\UsbAudioN */
    WAVEFORMATEX      wf;
    DWORD_PTR         cb, cbinst;
    DWORD             flags;
    HWAVE             hw;
    unsigned long     drate;                /* Geraeterate */
    CRITICAL_SECTION  cs;
    WAVEHDR          *kopf, *ende;
    HANDLE            ev, faden, reset_fertig;
    volatile int      schluss, pause, reset;
    unsigned long     off;                  /* Fortschritt im vordersten WAVEHDR */
    double            t, schritt;           /* Interpolation */
    short             pl, pr;
    unsigned __int64  basis;                /* GespieltLo/Hi bei Open/Reset */
    short             aus[AUS_ABT * 2];
    unsigned long     naus;
} INST;

/* ---------------------------------------------------------------- *
 *  Hilfen
 * ---------------------------------------------------------------- */

static void
rueckmelden (INST *x, UINT msg, DWORD_PTR p1)
{
    DriverCallback(x->cb, HIWORD(x->flags), (HDRVR)x->hw, msg, x->cbinst, p1, 0);
}

/*  Eine Suche, zwei Nummerierungen: Wiedergabe (wodMessage) und
 *  Aufnahme (widMessage).  Ein Headset steht in beiden, ein Mikrofon
 *  nur in der zweiten.  */
static void
geraete_suchen (void)
{
    int n, k = 0, wo = 0, wi = 0;

    EnterCriticalSection(&gsperre);
    for (n = 0; n < 8 && k < MAX_GERAETE; n += 1) {
        GERAET *d = &geraete[k];
        DWORD   laut = d->lautstaerke;
        unsigned long r = 0;
        if (!uac_suchen(&d->g, n)) break;
        if (d->g.uac != 1) continue;
        d->alt = uac_wiedergabe(&d->g, 48000, &r);
        if (d->alt && (d->alt->ifnr >= UAC_MAX_IF || d->g.au_dev[d->alt->ifnr] < 0)) d->alt = 0;
        d->rate48 = r;
        d->au = d->alt ? d->g.au_dev[d->alt->ifnr] : -1;
        d->alt_ein = uac_aufnahme(&d->g, 48000, &r);
        if (d->alt_ein && (d->alt_ein->ifnr >= UAC_MAX_IF || d->g.au_dev[d->alt_ein->ifnr] < 0)) d->alt_ein = 0;
        d->au_ein = d->alt_ein ? d->g.au_dev[d->alt_ein->ifnr] : -1;
        if (d->alt == 0 && d->alt_ein == 0) continue;
        d->lautstaerke = laut ? laut : 0xFFFFFFFFul;
        if (d->alt) wo_liste[wo++] = k;
        if (d->alt_ein) wi_liste[wi++] = k;
        k += 1;
    }
    anzahl = wo;
    anzahl_ein = wi;
    anzahl_alle = k;
    LeaveCriticalSection(&gsperre);
}

static double hoch10(double x);          /* Definition weiter unten */

/*  Umrechnung Geraete-dB <-> Reglerstellung 0..65535.
 *
 *  Frueher physikalisch gerechnet (Amplitude, 10^(dB/20)).  Das sieht am
 *  Regler falsch aus:  der Bereich eines Headsets geht von -64 dB bis
 *  0 dB, und ein angenehmer Pegel von -19 dB landet damit bei 11 % --
 *  alles Brauchbare quetscht sich ins oberste Zehntel (Anwenderbericht:
 *  "die Skala haut nicht hin").
 *
 *  Mischpulte und die Windows-Lautstaerkeregelung legen den dB-Bereich
 *  stattdessen GLEICHMAESSIG auf den Weg des Reglers.  Mitte = Mitte des
 *  dB-Bereichs.  Genau das machen diese beiden Funktionen; mn/mx sind die
 *  Grenzen, die das Geraet selbst meldet.  */
static DWORD
db_nach_regler (short v, short mn, short mx)
{
    long spanne = (long)mx - (long)mn;
    long x;
    if (spanne <= 0) return 65535;
    if (v <= mn) return 0;
    if (v >= mx) return 65535;
    x = ((long)v - (long)mn) * 65535L / spanne;
    return (DWORD)x;
}

static short
regler_nach_db (DWORD w, short mn, short mx)
{
    long spanne = (long)mx - (long)mn;
    long v;
    if (w == 0) return mn;
    if (w >= 65535) return mx;
    v = (long)mn + (long)w * spanne / 65535L;
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    return (short)v;
}

/*  Alle Zugriffe aufs Steuergeraet (ntugen oeffnet exklusiv) laufen durch
 *  diese Sperre.  Sonst blockiert z. B. der Waechterfaden (Lautstaerke-
 *  regelung offen) genau dann das Hoerbarmachen, wenn ein Klang startet --
 *  Ergebnis waere: kein Ton.  */
static HANDLE g_steuer;
static void steuer_sperre(void)
{
    if (g_steuer == 0) {
        HANDLE n = CreateMutexA(NULL, FALSE, NULL);
        if (InterlockedCompareExchangePointer((PVOID *)&g_steuer, n, NULL) != NULL) CloseHandle(n);
    }
    WaitForSingleObject(g_steuer, 3000);
}
static void steuer_frei(void) { if (g_steuer) ReleaseMutex(g_steuer); }
static void   mx_melden(int geraet, DWORD id);   /* Mixer benachrichtigen */
#define MX_SPK_LAUT_ID   1               /* muss zu MX_SPK_LAUT passen */
#define MX_SPK_STUMM_ID  2

static double
ln (double x)                                /* x > 0 */
{
    double y, y2, s;
    int k = 0;
    while (x < 0.5) { x *= 2; k += 1; }
    while (x > 2.0) { x /= 2; k -= 1; }
    y = (x - 1) / (x + 1); y2 = y * y;
    s = 2 * y * (1 + y2 / 3 * (1 + y2 * 3 / 5 * (1 + y2 * 5 / 7 * (1 + y2 * 7 / 9 * (1 + y2 * 9 / 11)))));
    return s - k * 0.693147180559945;
}

/*  winmm-Lautstaerke (0..0xFFFF je Kanal) -> Feature Unit (1/256 dB) im
 *  Wiedergabezweig.  Ein Headset hat oft zwei Feature Units (Lautsprecher
 *  und Mikrofon); die erste im Deskriptor ist nicht unbedingt die richtige
 *  (HyperX Cloud Stinger Core: FU 3 = Mikrofon steht vor FU 2).  Deshalb
 *  ueber den Signalweg der Wiedergabe-Alternative (uac_fu_fuer).
 *  Schaltet stumm aus, ausser bei 0.  */
/*  Lautstaerke AUS dem Geraet lesen -> winmm (0..0xFFFF je Kanal).  Das
 *  Geraet ist die einzige Wahrheit; so laufen winmm-Regler und Mixer nicht
 *  auseinander.  dB (1/256) -> 0..0xFFFF: 0 dB und mehr = voll, Minimum = 0.  */
/*  Geraet hoerbar machen -- in EINEM Zugriff aufs Steuergeraet:
 *    Stumm aus; Lautstaerke lesen; nur wenn sie praktisch am Minimum steht
 *    (< 2 %), auf einen hoerbaren Wert setzen.
 *  So bleibt die bewusste Einstellung des Anwenders erhalten, Geraete die
 *  stumm/leise hochkommen werden aber hoerbar.  Ein einziges Oeffnen, damit
 *  nichts dazwischenkommt (Waechter, anderes Programm).
 *  Rueckgabe: 1 = Lautstaerke wurde angehoben (Mixer benachrichtigen).  */
static int
hoerbar_machen (GERAET *d)
{
    UAC_FU *f = d->alt ? uac_fu_fuer(&d->g, d->alt) : 0;
    HANDLE  h;
    short   mn, mx, rs, v;
    int     c, kan, angehoben = 0, ist_leise = 1;

    if (f == 0) f = uac_fu(&d->g, 2);
    if (f == 0) return 0;

    steuer_sperre();
    h = uac_ac_oeffnen(&d->g);
    if (h == INVALID_HANDLE_VALUE) { steuer_frei(); return 0; }

    if (f->ctl[0] & 1) uac_stumm_setzen(&d->g, f, h, 0);   /* Stumm aus */

    kan = (f->kanaele > 2 && (f->ctl[1] & 2) && (f->ctl[2] & 2)) ? 2 : 1;
    for (c = (kan == 2 ? 1 : 0); c <= (kan == 2 ? 2 : 0); c += 1) {
        if (!uac_laut_bereich(&d->g, f, h, c, &mn, &mx, &rs)) continue;
        if (!uac_laut_lesen(&d->g, f, h, c, &v)) continue;
        {   DWORD x = db_nach_regler(v, mn, mx);
            if (x >= 1300) ist_leise = 0;     /* ueber 2 %: Anwenderwunsch */
        }
    }
    if (ist_leise) {
        DWORD w = d->lautstaerke ? d->lautstaerke : 0xFFFFFFFFul;
        for (c = (kan == 2 ? 1 : 0); c <= (kan == 2 ? 2 : 0); c += 1) {
            unsigned short p = (c == 2) ? (unsigned short)(w >> 16) : (unsigned short)(w & 0xFFFF);
            int ziel;
            if (!uac_laut_bereich(&d->g, f, h, c, &mn, &mx, &rs)) continue;
            ziel = regler_nach_db(p, mn, mx);
            if (ziel < mn) ziel = mn;
            if (ziel > 0) ziel = 0;
            uac_laut_setzen(&d->g, f, h, c, (short)ziel);
            angehoben = 1;
        }
    }
    CloseHandle(h);
    steuer_frei();
    return angehoben;
}

static DWORD
lautstaerke_lesen (GERAET *d)
{
    UAC_FU *f = d->alt ? uac_fu_fuer(&d->g, d->alt) : 0;
    HANDLE  h;
    short   mn, mx, rs, v;
    unsigned short kan[2];
    int     c, hat = 0;

    if (f == 0) f = uac_fu(&d->g, 2);
    if (f == 0) return 0xFFFFFFFFul;            /* kein Regler: voll melden */
    steuer_sperre();
    h = uac_ac_oeffnen(&d->g);
    if (h == INVALID_HANDLE_VALUE) { steuer_frei(); return d->lautstaerke ? d->lautstaerke : 0xFFFFFFFFul; }
    kan[0] = kan[1] = 0;
    for (c = 1; c <= 2; c += 1) {
        if (!uac_laut_bereich(&d->g, f, h, c, &mn, &mx, &rs)) continue;
        if (uac_laut_lesen(&d->g, f, h, c, &v)) {
            kan[c - 1] = (unsigned short)db_nach_regler(v, mn, mx);
            hat = 1;
        }
    }
    if (!hat && uac_laut_bereich(&d->g, f, h, 0, &mn, &mx, &rs)) {   /* nur Master */
        if (uac_laut_lesen(&d->g, f, h, 0, &v)) {
            DWORD x = db_nach_regler(v, mn, mx);
            kan[0] = kan[1] = (unsigned short)x;
            hat = 1;
        }
    }
    CloseHandle(h);
    steuer_frei();
    if (!hat) return d->lautstaerke ? d->lautstaerke : 0xFFFFFFFFul;
    return (DWORD)kan[0] | ((DWORD)kan[1] << 16);
}

static void
lautstaerke_setzen (GERAET *d, DWORD v)
{
    UAC_FU *f = d->alt ? uac_fu_fuer(&d->g, d->alt) : 0;
    HANDLE h;
    unsigned short kan[2];
    short mn, mx, rs;
    int c, ziel, gesetzt = 0;

    if (f == 0) f = uac_fu(&d->g, 2);
    if (f == 0) return;
    steuer_sperre();
    h = uac_ac_oeffnen(&d->g);
    if (h == INVALID_HANDLE_VALUE) { steuer_frei(); return; }
    kan[0] = LOWORD(v); kan[1] = HIWORD(v);
    for (c = 1; c <= 2; c += 1) {            /* Kanal 1 = links, 2 = rechts */
        if (!uac_laut_bereich(&d->g, f, h, c, &mn, &mx, &rs)) continue;
        if (kan[c - 1] == 0) ziel = mn;
        else ziel = regler_nach_db(kan[c - 1], mn, mx);
        if (ziel < mn) ziel = mn;
        if (ziel > mx) ziel = mx;
        if (ziel > 0) ziel = 0;               /* 0xFFFF = 0 dB, nicht verstaerken */
        gesetzt |= uac_laut_setzen(&d->g, f, h, c, (short)ziel);
    }
    if (!gesetzt && uac_laut_bereich(&d->g, f, h, 0, &mn, &mx, &rs)) {   /* nur Master */
        unsigned long m = ((unsigned long)kan[0] + kan[1]) / 2;
        ziel = m ? regler_nach_db(m, mn, mx) : mn;
        if (ziel < mn) ziel = mn;
        if (ziel > mx) ziel = mx;
        if (ziel > 0) ziel = 0;
        uac_laut_setzen(&d->g, f, h, 0, (short)ziel);
    }
    uac_stumm_setzen(&d->g, f, h, v == 0);
    CloseHandle(h);
    steuer_frei();
}

/* ---------------------------------------------------------------- *
 *  Strom
 * ---------------------------------------------------------------- */

static int
schreiben (INST *x, const void *d, unsigned long n)
{
    NTUA_STAND z;
    DWORD got;
    unsigned long ab = 0;

    while (ab < n) {
        if (x->schluss || x->reset) return 0;
        if (!DeviceIoControl(x->h, IOCTL_NTUA_SCHREIBEN, (char *)d + ab, n - ab,
                             &z, sizeof(z), &got, NULL)) {
            return 0;                         /* Geraet abgezogen */
        }
        ab += z.Angenommen;
        if (z.Angenommen == 0) Sleep(5);     /* Ring voll */
    }
    return 1;
}

static void
ausgeben (INST *x, short l, short r)
{
    while (x->t < 1.0) {
        x->aus[2 * x->naus]     = (short)(x->pl + (l - x->pl) * x->t);
        x->aus[2 * x->naus + 1] = (short)(x->pr + (r - x->pr) * x->t);
        x->t += x->schritt;
        if (++x->naus == AUS_ABT) {
            schreiben(x, x->aus, AUS_ABT * 4);
            x->naus = 0;
        }
    }
    x->t -= 1.0;
    x->pl = l; x->pr = r;
}

/*  Einen Teil des vordersten Puffers wandeln.  1 = Puffer fertig.  */
static int
wandeln (INST *x, WAVEHDR *hd)
{
    const unsigned char *p = (const unsigned char *)hd->lpData;
    unsigned long ba = x->wf.nBlockAlign, ende = hd->dwBufferLength - hd->dwBufferLength % ba;
    unsigned long bis = x->off + 512 * ba;   /* in Happen: reset/pause bleiben flott */
    short l, r;

    if (bis > ende) bis = ende;
    while (x->off < bis && !x->reset && !x->schluss) {
        const unsigned char *s = p + x->off;
        if (x->wf.wBitsPerSample == 8) {
            l = (short)((s[0] - 128) << 8);
            r = (x->wf.nChannels == 2) ? (short)((s[1] - 128) << 8) : l;
        } else {
            l = *(const short *)s;
            r = (x->wf.nChannels == 2) ? *(const short *)(s + 2) : l;
        }
        ausgeben(x, l, r);
        x->off += ba;
    }
    return x->off >= ende;
}

static void
alle_zurueck (INST *x)
{
    WAVEHDR *hd;

    EnterCriticalSection(&x->cs);
    hd = x->kopf;
    x->kopf = x->ende = 0;
    LeaveCriticalSection(&x->cs);
    while (hd != 0) {
        WAVEHDR *n = hd->lpNext;
        hd->dwFlags &= ~WHDR_INQUEUE;
        hd->dwFlags |= WHDR_DONE;
        rueckmelden(x, WOM_DONE, (DWORD_PTR)hd);
        hd = n;
    }
}

static DWORD WINAPI
faden (LPVOID arg)
{
    INST *x = (INST *)arg;
    WAVEHDR *hd;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (!x->schluss) {
        if (x->reset) {
            NTUA_STAND z;
            DWORD got;
            alle_zurueck(x);
            x->off = 0; x->naus = 0;
            DeviceIoControl(x->h, IOCTL_NTUA_LEEREN, NULL, 0, &z, sizeof(z), &got, NULL);
            x->basis = ((unsigned __int64)z.GespieltHi << 32) | z.GespieltLo;
            x->reset = 0;
            SetEvent(x->reset_fertig);
            continue;
        }
        EnterCriticalSection(&x->cs);
        hd = x->pause ? 0 : x->kopf;
        LeaveCriticalSection(&x->cs);
        if (hd == 0) {
            WaitForSingleObject(x->ev, 50);
            continue;
        }
        if (wandeln(x, hd)) {
            EnterCriticalSection(&x->cs);
            if (x->kopf == hd) {              /* nicht inzwischen per Reset weg */
                x->kopf = hd->lpNext;
                if (x->kopf == 0) x->ende = 0;
                x->off = 0;
                hd->dwFlags &= ~WHDR_INQUEUE;
                hd->dwFlags |= WHDR_DONE;
                LeaveCriticalSection(&x->cs);
                if (x->kopf == 0 && x->naus != 0) {    /* Rest raus, sonst haengt das Ende */
                    schreiben(x, x->aus, x->naus * 4);
                    x->naus = 0;
                }
                rueckmelden(x, WOM_DONE, (DWORD_PTR)hd);
            } else {
                LeaveCriticalSection(&x->cs);
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- *
 *  wodMessage
 * ---------------------------------------------------------------- */

static int
format_ok (const WAVEFORMATEX *wf)
{
    return wf->wFormatTag == WAVE_FORMAT_PCM &&
           (wf->wBitsPerSample == 8 || wf->wBitsPerSample == 16) &&
           (wf->nChannels == 1 || wf->nChannels == 2) &&
           wf->nSamplesPerSec >= 4000 && wf->nSamplesPerSec <= 96000 &&
           wf->nBlockAlign == wf->nChannels * wf->wBitsPerSample / 8;
}

static DWORD
oeffnen (UINT nr, INST **aus, LPWAVEOPENDESC od, DWORD flags)
{
    GERAET     *d;
    INST       *x;
    NTUA_START  st;
    NTUA_STAND  z;
    DWORD       got, id;
    unsigned long r = 0;

    if (anzahl < 0) geraete_suchen();
    if ((int)nr >= anzahl) return MMSYSERR_BADDEVICEID;
    if (od == 0 || od->lpFormat == 0) return MMSYSERR_INVALPARAM;
    nr = (UINT)wo_liste[nr];
    if (!format_ok((const WAVEFORMATEX *)od->lpFormat)) return WAVERR_BADFORMAT;   /* PCM: PCMWAVEFORMAT */
    if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;

    d = &geraete[nr];
    x = (INST *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(INST));
    if (x == 0) return MMSYSERR_NOMEM;
    x->nr = (int)nr;
    memcpy(&x->wf, od->lpFormat, sizeof(PCMWAVEFORMAT));
    x->wf.cbSize = 0;
    x->cb = od->dwCallback; x->cbinst = od->dwInstance; x->flags = flags; x->hw = od->hWave;

    /*  Rate des Programms, wenn das Geraet sie kann -- sonst 48 kHz.  */
    d->alt = uac_wiedergabe(&d->g, x->wf.nSamplesPerSec, &r);
    if (d->alt == 0) { HeapFree(GetProcessHeap(), 0, x); return MMSYSERR_NODRIVER; }
    x->drate   = r;
    x->schritt = (double)x->wf.nSamplesPerSec / (double)r;
    x->t       = 1.0;                         /* erste Abtastung nur merken */

    /*  Hoerbar machen -- VOR dem Oeffnen des Stroms!  Danach haelt der Strom
     *  das Geraet exklusiv, und das Steuergeraet laesst sich nicht mehr
     *  oeffnen; dann bliebe alles stumm.  */
    if (hoerbar_machen(d)) mx_melden((int)(d - geraete), MX_SPK_LAUT_ID);

    x->h = uac_strom_oeffnen(d->au);
    if (x->h == INVALID_HANDLE_VALUE) { HeapFree(GetProcessHeap(), 0, x); return MMSYSERR_ALLOCATED; }
    InterlockedIncrement(&d->stroeme);      /* Waechter soll nicht stoeren */

    memset(&st, 0, sizeof(st));
    st.Alternative = d->alt->alt; st.Endpunkt = d->alt->ep;
    st.RateSetzen = (unsigned char)(d->alt->cs_attr & 1);
    st.Rate = r; st.BytesJeAbtastung = 4; st.RingByte = RING_BYTE;
    if (!DeviceIoControl(x->h, IOCTL_NTUA_START, &st, sizeof(st), &z, sizeof(z), &got, NULL)) {
        DWORD e = GetLastError();
        CloseHandle(x->h);
        InterlockedDecrement(&d->stroeme);
        HeapFree(GetProcessHeap(), 0, x);
        return e == ERROR_BUSY ? MMSYSERR_ALLOCATED : MMSYSERR_ERROR;
    }
    x->basis = 0;

    InitializeCriticalSection(&x->cs);
    x->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    x->reset_fertig = CreateEventA(NULL, FALSE, FALSE, NULL);
    x->faden = CreateThread(NULL, 0, faden, x, 0, &id);

    *aus = x;
    rueckmelden(x, WOM_OPEN, 0);
    return MMSYSERR_NOERROR;
}

static DWORD
schliessen (INST *x)
{
    NTUA_STAND z;
    DWORD got;
    int k;

    /*  Haengen noch Puffer, gaben wir frueher WAVERR_STILLPLAYING zurueck.
     *  Das ist zwar regelkonform, aber gefaehrlich: ein Programm, das den
     *  Fehler nicht behandelt (der Explorer bei Systemklaengen), laesst den
     *  Griff einfach offen -- und weil der Explorer nie endet, bleibt das
     *  Geraet bis zum Abmelden belegt.  Danach geht KEIN Klang mehr.
     *  Gemessen am HyperX: nach dem Anmeldeklang war UsbAudio1 dauerhaft
     *  belegt (START scheiterte mit 170/ERROR_BUSY).
     *  Deshalb jetzt: offene Puffer zurueckgeben und trotzdem schliessen.  */
    /*  Erst AUSLAUFEN lassen.  Der Abspielfaden gibt jeden Puffer selbst
     *  zurueck, sobald er gespielt ist; wir warten darauf (hoechstens
     *  3 s).  Frueher wurde hier sofort geleert -- dann fehlte das Ende,
     *  und kurze Klaenge (Menue-Popup, ding.wav) waren ganz weg:
     *  gemessen war ding.wav mit 0,52 s Inhalt schon nach 198 ms fertig.  */
    for (k = 0; k < 300 && x->kopf != 0; k += 1) Sleep(10);

    /*  Was dann noch haengt, geben wir zurueck und schliessen trotzdem.
     *  Sonst behaelt ein Programm, das WAVERR_STILLPLAYING nicht
     *  behandelt (der Explorer bei Systemklaengen), den Griff fuer immer
     *  -- und das Geraet bleibt bis zum Abmelden belegt.  */
    if (x->kopf != 0) {
        EnterCriticalSection(&x->cs);
        while (x->kopf != 0) {
            WAVEHDR *hd = x->kopf;
            x->kopf = hd->lpNext;
            if (x->kopf == 0) x->ende = 0;
            hd->dwFlags &= ~WHDR_INQUEUE;
            hd->dwFlags |= WHDR_DONE;
            LeaveCriticalSection(&x->cs);
            rueckmelden(x, WOM_DONE, (DWORD_PTR)hd);
            EnterCriticalSection(&x->cs);
        }
        LeaveCriticalSection(&x->cs);
    }

    /*  Ring leer spielen lassen (hoechstens 1 s), sonst fehlt das Ende.  */
    for (k = 0; k < 100; k += 1) {
        if (!DeviceIoControl(x->h, IOCTL_NTUA_STAND, NULL, 0, &z, sizeof(z), &got, NULL) ||
            z.Fuellstand == 0) break;
        Sleep(10);
    }
    x->schluss = 1;
    SetEvent(x->ev);
    WaitForSingleObject(x->faden, 2000);
    CloseHandle(x->faden);
    DeviceIoControl(x->h, IOCTL_NTUA_STOP, NULL, 0, &z, sizeof(z), &got, NULL);
    CloseHandle(x->h);
    InterlockedDecrement(&geraete[x->nr].stroeme);
    CloseHandle(x->ev);
    CloseHandle(x->reset_fertig);
    DeleteCriticalSection(&x->cs);
    rueckmelden(x, WOM_CLOSE, 0);
    HeapFree(GetProcessHeap(), 0, x);
    return MMSYSERR_NOERROR;
}

static DWORD
position (INST *x, LPMMTIME mt, DWORD groesse)
{
    NTUA_STAND z;
    DWORD got;
    unsigned __int64 g;
    double abt;

    if (mt == 0 || groesse < sizeof(MMTIME)) return MMSYSERR_INVALPARAM;
    if (!DeviceIoControl(x->h, IOCTL_NTUA_STAND, NULL, 0, &z, sizeof(z), &got, NULL)) {
        return MMSYSERR_NODRIVER;
    }
    g = (((unsigned __int64)z.GespieltHi << 32) | z.GespieltLo) - x->basis;
    abt = (double)(__int64)(g / 4) * (double)x->wf.nSamplesPerSec / (double)x->drate;
    if (mt->wType == TIME_SAMPLES) {
        mt->u.sample = (DWORD)abt;
    } else if (mt->wType == TIME_MS) {
        mt->u.ms = (DWORD)(abt * 1000.0 / x->wf.nSamplesPerSec);
    } else {
        mt->wType = TIME_BYTES;
        mt->u.cb = (DWORD)abt * x->wf.nBlockAlign;
    }
    return MMSYSERR_NOERROR;
}

static DWORD
faehigkeiten (UINT nr, void *p, DWORD groesse)
{
    GERAET *d;
    char    name[MAXPNAMELEN];
    int     k;

    if (anzahl < 0) geraete_suchen();
    if ((int)nr >= anzahl || p == 0) return MMSYSERR_BADDEVICEID;
    d = &geraete[wo_liste[nr]];
    memset(name, 0, sizeof(name));
    wsprintfA(name, "USB: %.25s", d->g.name[0] ? d->g.name : "Audio");

    /*  winmm unter NT fragt mit WAVEOUTCAPSW (wie beim Joystick die
     *  JOYCAPSW); an der Groesse erkennen, welche Fassung.  */
    if (groesse >= sizeof(WAVEOUTCAPSW)) {
        WAVEOUTCAPSW *c = (WAVEOUTCAPSW *)p;
        memset(c, 0, sizeof(*c));
        c->wMid = 1; c->wPid = (WORD)d->g.pid; c->vDriverVersion = 0x0100;
        for (k = 0; name[k] && k < MAXPNAMELEN - 1; k += 1) c->szPname[k] = (WCHAR)(unsigned char)name[k];
        c->dwFormats = 0xFFF;                 /* 11/22/44 kHz, 8/16 Bit, Mono/Stereo */
        c->wChannels = 2;
        c->dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME;
    } else if (groesse >= sizeof(WAVEOUTCAPSA)) {
        WAVEOUTCAPSA *c = (WAVEOUTCAPSA *)p;
        memset(c, 0, sizeof(*c));
        c->wMid = 1; c->wPid = (WORD)d->g.pid; c->vDriverVersion = 0x0100;
        memcpy(c->szPname, name, MAXPNAMELEN);
        c->dwFormats = 0xFFF; c->wChannels = 2;
        c->dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME;
    } else {
        return MMSYSERR_INVALPARAM;
    }
    return MMSYSERR_NOERROR;
}

DWORD APIENTRY
wodMessage (UINT nr, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2)
{
    INST *x = (INST *)user;

    switch (msg) {
    case DRVM_INIT:
        return MMSYSERR_NOERROR;
    case WODM_GETNUMDEVS:
        geraete_suchen();
        return (DWORD)anzahl;
    case WODM_GETDEVCAPS:
        return faehigkeiten(nr, (void *)p1, (DWORD)p2);
    case WODM_OPEN:
        return oeffnen(nr, (INST **)user, (LPWAVEOPENDESC)p1, (DWORD)p2);
    case WODM_CLOSE:
        return schliessen(x);
    case WODM_PREPARE:
    case WODM_UNPREPARE:
        return MMSYSERR_NOTSUPPORTED;         /* winmm erledigt das selbst */
    case WODM_WRITE: {
        WAVEHDR *hd = (WAVEHDR *)p1;
        if (hd == 0 || !(hd->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
        hd->dwFlags &= ~WHDR_DONE;
        hd->dwFlags |= WHDR_INQUEUE;
        hd->lpNext = 0;
        EnterCriticalSection(&x->cs);
        if (x->ende) x->ende->lpNext = hd; else x->kopf = hd;
        x->ende = hd;
        LeaveCriticalSection(&x->cs);
        SetEvent(x->ev);
        return MMSYSERR_NOERROR;
    }
    case WODM_PAUSE:
        x->pause = 1;
        return MMSYSERR_NOERROR;
    case WODM_RESTART:
        x->pause = 0;
        SetEvent(x->ev);
        return MMSYSERR_NOERROR;
    case WODM_RESET:
    case WODM_BREAKLOOP:
        if (msg == WODM_BREAKLOOP) return MMSYSERR_NOERROR;   /* Schleifen: einmal */
        x->reset = 1;
        SetEvent(x->ev);
        WaitForSingleObject(x->reset_fertig, 3000);
        x->pause = 0;
        return MMSYSERR_NOERROR;
    case WODM_GETPOS:
        return position(x, (LPMMTIME)p1, (DWORD)p2);
    case WODM_GETVOLUME:
        if ((int)nr >= anzahl || p1 == 0) return MMSYSERR_BADDEVICEID;
        *(DWORD *)p1 = lautstaerke_lesen(&geraete[wo_liste[nr]]);
        return MMSYSERR_NOERROR;
    case WODM_SETVOLUME:
        if ((int)nr >= anzahl) return MMSYSERR_BADDEVICEID;
        geraete[wo_liste[nr]].lautstaerke = (DWORD)p1;
        lautstaerke_setzen(&geraete[wo_liste[nr]], (DWORD)p1);
        /*  sndvol32 zeigt sonst weiter den alten Wert, wenn ein Programm
         *  die Lautstaerke setzt (Anwenderbericht).  */
        mx_melden(wo_liste[nr], MX_SPK_LAUT_ID);
        mx_melden(wo_liste[nr], MX_SPK_STUMM_ID);
        return MMSYSERR_NOERROR;
    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}

/* ---------------------------------------------------------------- *
 *  Aufnahme: widMessage
 *
 *  Faden: holt aus dem Ring (IOCTL_NTUA_LESEN), rechnet vom Geraetformat
 *  (16 Bit, Mono/Stereo, Geraeterate) auf das Format des Programms um und
 *  fuellt dessen Puffer; voller Puffer -> WIM_DATA.  Vor WIDM_START und
 *  ohne Puffer wird gelesen und verworfen, damit der Ring nicht ueberlaeuft.
 * ---------------------------------------------------------------- */

typedef struct EIN {
    int               nr;                   /* Geraet (fuer den Strom-Zaehler) */
    HANDLE            h;
    WAVEFORMATEX      wf;
    DWORD_PTR         cb, cbinst;
    DWORD             flags;
    HWAVE             hw;
    unsigned long     drate, dkan;          /* Geraet */
    CRITICAL_SECTION  cs;
    WAVEHDR          *kopf, *ende;
    HANDLE            faden, quittung;
    volatile int      schluss, laeuft, anhalten, reset;
    double            t, schritt;
    short             pl, pr;
    unsigned long     aufgenommen;          /* Byte im Programmformat seit Start */
    unsigned char     lese[sizeof(NTUA_STAND) + 8192];
} EIN;

static void
ein_melden (EIN *x, UINT msg, DWORD_PTR p1)
{
    DriverCallback(x->cb, HIWORD(x->flags), (HDRVR)x->hw, msg, x->cbinst, p1, 0);
}

/*  Vordersten Puffer abschliessen (auch halb voll) und zurueckgeben.  */
static void
ein_abgeben (EIN *x)
{
    WAVEHDR *hd;

    EnterCriticalSection(&x->cs);
    hd = x->kopf;
    if (hd != 0) {
        x->kopf = hd->lpNext;
        if (x->kopf == 0) x->ende = 0;
    }
    LeaveCriticalSection(&x->cs);
    if (hd != 0) {
        hd->dwFlags &= ~WHDR_INQUEUE;
        hd->dwFlags |= WHDR_DONE;
        ein_melden(x, WIM_DATA, (DWORD_PTR)hd);
    }
}

/*  Eine Abtastung im Programmformat in den vordersten Puffer.  */
static void
ein_ablegen (EIN *x, short l, short r)
{
    WAVEHDR       *hd = x->kopf;
    unsigned char *p;

    if (hd == 0) return;                      /* kein Puffer: verworfen */
    p = (unsigned char *)hd->lpData + hd->dwBytesRecorded;
    if (x->wf.wBitsPerSample == 8) {
        if (x->wf.nChannels == 2) { p[0] = (unsigned char)((l >> 8) + 128); p[1] = (unsigned char)((r >> 8) + 128); }
        else p[0] = (unsigned char)((((int)l + r) / 2 >> 8) + 128);
    } else {
        if (x->wf.nChannels == 2) { ((short *)p)[0] = l; ((short *)p)[1] = r; }
        else *(short *)p = (short)(((int)l + r) / 2);
    }
    hd->dwBytesRecorded += x->wf.nBlockAlign;
    x->aufgenommen += x->wf.nBlockAlign;
    if (hd->dwBytesRecorded + x->wf.nBlockAlign > hd->dwBufferLength) {
        ein_abgeben(x);
    }
}

static DWORD WINAPI
ein_faden (LPVOID arg)
{
    EIN        *x = (EIN *)arg;
    NTUA_STAND *z = (NTUA_STAND *)x->lese;
    DWORD       got;
    unsigned long k, n;
    short      *s;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (!x->schluss) {
        if (x->anhalten || x->reset) {        /* Stop/Reset: Quittung an den Aufrufer */
            if (x->reset) {
                while (x->kopf != 0) ein_abgeben(x);
            } else if (x->kopf != 0 && x->kopf->dwBytesRecorded != 0) {
                ein_abgeben(x);                /* der angefangene geht zurueck */
            }
            x->laeuft = 0;
            x->anhalten = x->reset = 0;
            SetEvent(x->quittung);
            continue;
        }
        if (!DeviceIoControl(x->h, IOCTL_NTUA_LESEN, NULL, 0, x->lese, sizeof(x->lese), &got, NULL)) {
            Sleep(50);                         /* Geraet weg */
            continue;
        }
        if (z->Angenommen == 0) { Sleep(10); continue; }
        if (!x->laeuft) continue;              /* vor WIDM_START: verwerfen */

        s = (short *)(x->lese + sizeof(NTUA_STAND));
        n = z->Angenommen / (2 * x->dkan);
        EnterCriticalSection(&x->cs);          /* gegen WIDM_ADDBUFFER */
        for (k = 0; k < n; k += 1) {
            short l = s[k * x->dkan], r = (x->dkan == 2) ? s[k * x->dkan + 1] : l;
            /*  Geraeterate -> Programmrate: Interpolation, fortlaufend.  */
            while (x->t < 1.0) {
                ein_ablegen(x, (short)(x->pl + (l - x->pl) * x->t), (short)(x->pr + (r - x->pr) * x->t));
                x->t += x->schritt;
            }
            x->t -= 1.0;
            x->pl = l; x->pr = r;
        }
        LeaveCriticalSection(&x->cs);
    }
    return 0;
}

static DWORD
ein_oeffnen (UINT nr, EIN **aus, LPWAVEOPENDESC od, DWORD flags)
{
    GERAET     *d;
    EIN        *x;
    NTUA_START  st;
    NTUA_STAND  z;
    DWORD       got, id;
    unsigned long r = 0;

    if (anzahl_ein < 0) geraete_suchen();
    if ((int)nr >= anzahl_ein) return MMSYSERR_BADDEVICEID;
    if (od == 0 || od->lpFormat == 0) return MMSYSERR_INVALPARAM;
    if (!format_ok((const WAVEFORMATEX *)od->lpFormat)) return WAVERR_BADFORMAT;
    if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;

    d = &geraete[wi_liste[nr]];
    x = (EIN *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(EIN));
    if (x == 0) return MMSYSERR_NOMEM;
    memcpy(&x->wf, od->lpFormat, sizeof(PCMWAVEFORMAT));
    x->wf.cbSize = 0;
    x->cb = od->dwCallback; x->cbinst = od->dwInstance; x->flags = flags; x->hw = od->hWave;

    d->alt_ein = uac_aufnahme(&d->g, x->wf.nSamplesPerSec, &r);
    if (d->alt_ein == 0) { HeapFree(GetProcessHeap(), 0, x); return MMSYSERR_NODRIVER; }
    x->drate   = r;
    x->dkan    = d->alt_ein->kanaele;
    x->schritt = (double)r / (double)x->wf.nSamplesPerSec;
    x->t       = 1.0;

    x->nr = (int)(d - geraete);
    x->h = uac_strom_oeffnen(d->au_ein);
    if (x->h != INVALID_HANDLE_VALUE) InterlockedIncrement(&d->stroeme);
    if (x->h == INVALID_HANDLE_VALUE) { HeapFree(GetProcessHeap(), 0, x); return MMSYSERR_ALLOCATED; }
    memset(&st, 0, sizeof(st));
    st.Alternative = d->alt_ein->alt; st.Endpunkt = d->alt_ein->ep;
    st.RateSetzen = (unsigned char)(d->alt_ein->cs_attr & 1);
    st.Rate = r; st.BytesJeAbtastung = 2 * x->dkan; st.RingByte = 32768;
    if (!DeviceIoControl(x->h, IOCTL_NTUA_START, &st, sizeof(st), &z, sizeof(z), &got, NULL)) {
        DWORD e = GetLastError();
        CloseHandle(x->h);
        InterlockedDecrement(&d->stroeme);
        HeapFree(GetProcessHeap(), 0, x);
        return e == ERROR_BUSY ? MMSYSERR_ALLOCATED : MMSYSERR_ERROR;
    }
    InitializeCriticalSection(&x->cs);
    x->quittung = CreateEventA(NULL, FALSE, FALSE, NULL);
    x->faden = CreateThread(NULL, 0, ein_faden, x, 0, &id);
    *aus = x;
    ein_melden(x, WIM_OPEN, 0);
    return MMSYSERR_NOERROR;
}

static void
ein_warten (EIN *x, volatile int *was)
{
    *was = 1;
    WaitForSingleObject(x->quittung, 3000);
}

DWORD APIENTRY
widMessage (UINT nr, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2)
{
    EIN *x = (EIN *)user;

    switch (msg) {
    case DRVM_INIT:
        return MMSYSERR_NOERROR;
    case WIDM_GETNUMDEVS:
        geraete_suchen();
        return (DWORD)anzahl_ein;
    case WIDM_GETDEVCAPS: {
        GERAET *d;
        char    name[MAXPNAMELEN];
        int     k;
        if (anzahl_ein < 0) geraete_suchen();
        if ((int)nr >= anzahl_ein || p1 == 0) return MMSYSERR_BADDEVICEID;
        d = &geraete[wi_liste[nr]];
        memset(name, 0, sizeof(name));
        wsprintfA(name, "USB: %.25s", d->g.name[0] ? d->g.name : "Audio");
        if (p2 >= sizeof(WAVEINCAPSW)) {
            WAVEINCAPSW *c = (WAVEINCAPSW *)p1;
            memset(c, 0, sizeof(*c));
            c->wMid = 1; c->wPid = (WORD)d->g.pid; c->vDriverVersion = 0x0100;
            for (k = 0; name[k] && k < MAXPNAMELEN - 1; k += 1) c->szPname[k] = (WCHAR)(unsigned char)name[k];
            c->dwFormats = 0xFFF; c->wChannels = 2;
        } else if (p2 >= sizeof(WAVEINCAPSA)) {
            WAVEINCAPSA *c = (WAVEINCAPSA *)p1;
            memset(c, 0, sizeof(*c));
            c->wMid = 1; c->wPid = (WORD)d->g.pid; c->vDriverVersion = 0x0100;
            memcpy(c->szPname, name, MAXPNAMELEN);
            c->dwFormats = 0xFFF; c->wChannels = 2;
        } else {
            return MMSYSERR_INVALPARAM;
        }
        return MMSYSERR_NOERROR;
    }
    case WIDM_OPEN:
        return ein_oeffnen(nr, (EIN **)user, (LPWAVEOPENDESC)p1, (DWORD)p2);
    case WIDM_PREPARE:
    case WIDM_UNPREPARE:
        return MMSYSERR_NOTSUPPORTED;
    case WIDM_ADDBUFFER: {
        WAVEHDR *hd = (WAVEHDR *)p1;
        if (hd == 0 || !(hd->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
        hd->dwFlags &= ~WHDR_DONE;
        hd->dwFlags |= WHDR_INQUEUE;
        hd->dwBytesRecorded = 0;
        hd->lpNext = 0;
        EnterCriticalSection(&x->cs);
        if (x->ende) x->ende->lpNext = hd; else x->kopf = hd;
        x->ende = hd;
        LeaveCriticalSection(&x->cs);
        return MMSYSERR_NOERROR;
    }
    case WIDM_START: {
        NTUA_STAND z;
        DWORD got;
        DeviceIoControl(x->h, IOCTL_NTUA_LEEREN, NULL, 0, &z, sizeof(z), &got, NULL);  /* Altes weg */
        x->laeuft = 1;
        return MMSYSERR_NOERROR;
    }
    case WIDM_STOP:
        if (x->laeuft) ein_warten(x, &x->anhalten);
        return MMSYSERR_NOERROR;
    case WIDM_RESET:
        ein_warten(x, &x->reset);
        x->aufgenommen = 0;
        return MMSYSERR_NOERROR;
    case WIDM_GETPOS: {
        LPMMTIME mt = (LPMMTIME)p1;
        if (mt == 0 || p2 < sizeof(MMTIME)) return MMSYSERR_INVALPARAM;
        if (mt->wType == TIME_SAMPLES) mt->u.sample = x->aufgenommen / x->wf.nBlockAlign;
        else if (mt->wType == TIME_MS) mt->u.ms = (DWORD)((double)x->aufgenommen / x->wf.nAvgBytesPerSec * 1000.0);
        else { mt->wType = TIME_BYTES; mt->u.cb = x->aufgenommen; }
        return MMSYSERR_NOERROR;
    }
    case WIDM_CLOSE: {
        NTUA_STAND z;
        DWORD got;
        if (x->kopf != 0) return WAVERR_STILLPLAYING;
        x->schluss = 1;
        WaitForSingleObject(x->faden, 2000);
        CloseHandle(x->faden);
        DeviceIoControl(x->h, IOCTL_NTUA_STOP, NULL, 0, &z, sizeof(z), &got, NULL);
        CloseHandle(x->h);
        InterlockedDecrement(&geraete[x->nr].stroeme);
        CloseHandle(x->quittung);
        DeleteCriticalSection(&x->cs);
        ein_melden(x, WIM_CLOSE, 0);
        HeapFree(GetProcessHeap(), 0, x);
        return MMSYSERR_NOERROR;
    }
    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}

/* ---------------------------------------------------------------- *
 *  Mixer: mxdMessage
 *
 *  Je Audiogeraet ein Mixer.  Aufbau (wie ihn sndvol32 erwartet):
 *    Ziel "Lautsprecher" (DST_SPEAKERS, an waveOut) -- Lautstaerke, Ton aus
 *        Quelle "Wave" (SRC_WAVEOUT), ohne eigene Regler
 *    Ziel "Aufnahme" (DST_WAVEIN, an waveIn)
 *        Quelle "Mikrofon" (SRC_MICROPHONE) -- Lautstaerke, Ton aus
 *  Die Regler sind die Feature Units des jeweiligen Zweigs (uac_fu_fuer),
 *  die Werte liegen im Geraet: jedes Programm sieht denselben Stand.
 *  Lautstaerke 0..65535 <-> 1/256 dB: 20*log10 hin, 10^(dB/20) zurueck.
 * ---------------------------------------------------------------- */

#define MX_SPK_LAUT   1                     /* Steuerelement-Kennungen */
#define MX_SPK_STUMM  2
#define MX_MIK_LAUT   3
#define MX_MIK_STUMM  4

typedef struct {
    DWORD id;                               /* dwLineID */
    int   ziel;                             /* Nummer des Ziels */
    int   quelle;                           /* -1 = das Ziel selbst */
    DWORD art, zieltyp;
    int   aufnahme;                         /* Zweig: 0 Lautsprecher, 1 Mikrofon */
    int   laut, stumm;                      /* Steuerelement-Kennung oder 0 */
    const char *name, *kurz;
} MX_LINIE;

static double
hoch10 (double x)                           /* 10^x, ohne C-Laufzeit */
{
    double y = x * 2.302585093, r, s = 1, t = 1;
    int k = 0, i;
    while (y > 0.693147) { y -= 0.693147180559945; k += 1; }
    while (y < -0.693147) { y += 0.693147180559945; k -= 1; }
    for (i = 1; i < 14; i += 1) { t *= y / i; s += t; }
    r = s;
    while (k > 0) { r *= 2; k -= 1; }
    while (k < 0) { r /= 2; k += 1; }
    return r;
}

static UAC_FU *
mx_fu (GERAET *d, int aufnahme)
{
    UAC_ALT *a = aufnahme ? d->alt_ein : d->alt;
    UAC_FU  *f = a ? uac_fu_fuer(&d->g, a) : 0;
    return (f == 0 && !aufnahme && a) ? uac_fu(&d->g, 2) : f;
}

/*  Kanaele fuer die Lautstaerke: 2 bei K1+K2 mit Regler, sonst Master.  */
static int
mx_kanaele (UAC_FU *f)
{
    return (f && f->kanaele > 2 && (f->ctl[1] & 2) && (f->ctl[2] & 2)) ? 2 : 1;
}

static int
mx_linien (GERAET *d, MX_LINIE *l)
{
    int n = 0, z = 0;
    UAC_FU *f;

    if (d->alt) {
        f = mx_fu(d, 0);
        memset(&l[n], 0, sizeof(l[n]));
        l[n].id = (DWORD)z; l[n].ziel = z; l[n].quelle = -1;
        l[n].art = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS; l[n].zieltyp = MIXERLINE_TARGETTYPE_WAVEOUT;
        l[n].laut  = (f && ((f->ctl[0] & 2) || mx_kanaele(f) == 2)) ? MX_SPK_LAUT : 0;
        l[n].stumm = (f && (f->ctl[0] & 1)) ? MX_SPK_STUMM : 0;
        l[n].name = "Lautsprecher"; l[n].kurz = "Lautsprecher";
        n += 1;
        memset(&l[n], 0, sizeof(l[n]));
        l[n].id = 0x10000ul | (DWORD)z; l[n].ziel = z; l[n].quelle = 0;
        l[n].art = MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT; l[n].zieltyp = MIXERLINE_TARGETTYPE_WAVEOUT;
        l[n].name = "Wave"; l[n].kurz = "Wave";
        n += 1; z += 1;
    }
    if (d->alt_ein) {
        f = mx_fu(d, 1);
        memset(&l[n], 0, sizeof(l[n]));
        l[n].id = (DWORD)z; l[n].ziel = z; l[n].quelle = -1; l[n].aufnahme = 1;
        l[n].art = MIXERLINE_COMPONENTTYPE_DST_WAVEIN; l[n].zieltyp = MIXERLINE_TARGETTYPE_WAVEIN;
        l[n].name = "Aufnahme"; l[n].kurz = "Aufnahme";
        n += 1;
        memset(&l[n], 0, sizeof(l[n]));
        l[n].id = 0x10000ul | (DWORD)z; l[n].ziel = z; l[n].quelle = 0; l[n].aufnahme = 1;
        l[n].art = MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE; l[n].zieltyp = MIXERLINE_TARGETTYPE_WAVEIN;
        l[n].laut  = (f && ((f->ctl[0] & 2) || mx_kanaele(f) == 2)) ? MX_MIK_LAUT : 0;
        l[n].stumm = (f && (f->ctl[0] & 1)) ? MX_MIK_STUMM : 0;
        l[n].name = "Mikrofon"; l[n].kurz = "Mikrofon";
        n += 1; z += 1;
    }
    return n;
}

static void
kopiere_name (void *ziel, int breit, const char *q, int max)
{
    int k;
    for (k = 0; q[k] && k < max - 1; k += 1) {
        if (breit) ((WCHAR *)ziel)[k] = (WCHAR)(unsigned char)q[k];
        else ((char *)ziel)[k] = q[k];
    }
    if (breit) ((WCHAR *)ziel)[k] = 0; else ((char *)ziel)[k] = 0;
}

static int
wave_nummer (int geraet, int aufnahme)
{
    int k, n = aufnahme ? anzahl_ein : anzahl;
    for (k = 0; k < n; k += 1) if ((aufnahme ? wi_liste[k] : wo_liste[k]) == geraet) return k;
    return 0;
}

static DWORD
mx_linie_fuellen (GERAET *d, int nr, MX_LINIE *l, int nl, MX_LINIE *x, LPMIXERLINEW m)
{
    int breit = m->cbStruct >= sizeof(MIXERLINEW);
    int k, nziel = 0, nctl = 0;

    for (k = 0; k < nl; k += 1) if (l[k].quelle < 0) nziel += 1;
    nctl = (x->laut ? 1 : 0) + (x->stumm ? 1 : 0);

    m->dwDestination   = (DWORD)x->ziel;
    m->dwSource        = x->quelle < 0 ? 0 : (DWORD)x->quelle;
    m->dwLineID        = x->id;
    m->fdwLine         = MIXERLINE_LINEF_ACTIVE | (x->quelle >= 0 ? MIXERLINE_LINEF_SOURCE : 0);
    m->dwUser          = 0;
    m->dwComponentType = x->art;
    /*  Kanaele: Lautsprecher 2 nur, wenn das Geraet links/rechts getrennt
     *  regeln kann -- sonst 1, damit sndvol32 keinen wirkungslosen
     *  Balance-Regler zeigt (HyperX: nur Master-Lautstaerke).  */
    m->cChannels       = x->aufnahme ? 1 : (x->quelle < 0 ? (DWORD)mx_kanaele(mx_fu(d, 0)) : 2);
    m->cConnections    = x->quelle < 0 ? 1 : 0;
    m->cControls       = (DWORD)nctl;
    if (breit) {
        kopiere_name(m->szShortName, 1, x->kurz, MIXER_SHORT_NAME_CHARS);
        kopiere_name(m->szName, 1, x->name, MIXER_LONG_NAME_CHARS);
        m->Target.dwType = x->zieltyp;
        m->Target.dwDeviceID = (DWORD)wave_nummer(nr, x->aufnahme);
        m->Target.wMid = 1; m->Target.wPid = (WORD)d->g.pid; m->Target.vDriverVersion = 0x0100;
        {
            char t[MAXPNAMELEN];
            memset(t, 0, sizeof(t));
            wsprintfA(t, "USB: %.25s", d->g.name[0] ? d->g.name : "Audio");
            kopiere_name(m->Target.szPname, 1, t, MAXPNAMELEN);
        }
    } else {
        LPMIXERLINEA a = (LPMIXERLINEA)m;
        char t[MAXPNAMELEN];
        kopiere_name(a->szShortName, 0, x->kurz, MIXER_SHORT_NAME_CHARS);
        kopiere_name(a->szName, 0, x->name, MIXER_LONG_NAME_CHARS);
        a->Target.dwType = x->zieltyp;
        a->Target.dwDeviceID = (DWORD)wave_nummer(nr, x->aufnahme);
        a->Target.wMid = 1; a->Target.wPid = (WORD)d->g.pid; a->Target.vDriverVersion = 0x0100;
        memset(t, 0, sizeof(t));
        wsprintfA(t, "USB: %.25s", d->g.name[0] ? d->g.name : "Audio");
        kopiere_name(a->Target.szPname, 0, t, MAXPNAMELEN);
    }
    (void)nziel;
    return MMSYSERR_NOERROR;
}

static void
mx_ctl_fuellen (GERAET *d, DWORD id, void *p, int breit)
{
    int stumm = (id == MX_SPK_STUMM || id == MX_MIK_STUMM);
    int aufn  = (id == MX_MIK_LAUT || id == MX_MIK_STUMM);
    UAC_FU *f = mx_fu(d, aufn);
    const char *nm = stumm ? (aufn ? "Mikrofon aus" : "Ton aus") : (aufn ? "Mikrofon" : "Lautstaerke");
    LPMIXERCONTROLW c = (LPMIXERCONTROLW)p;

    if (breit) memset(c, 0, sizeof(MIXERCONTROLW)); else memset(c, 0, sizeof(MIXERCONTROLA));
    c->cbStruct      = breit ? sizeof(MIXERCONTROLW) : sizeof(MIXERCONTROLA);
    c->dwControlID   = id;
    c->dwControlType = stumm ? MIXERCONTROL_CONTROLTYPE_MUTE : MIXERCONTROL_CONTROLTYPE_VOLUME;
    c->fdwControl    = (stumm || mx_kanaele(f) == 1) ? MIXERCONTROL_CONTROLF_UNIFORM : 0;
    c->cMultipleItems = 0;
    if (breit) {
        kopiere_name(c->szShortName, 1, nm, MIXER_SHORT_NAME_CHARS);
        kopiere_name(c->szName, 1, nm, MIXER_LONG_NAME_CHARS);
        c->Bounds.dwMinimum = 0; c->Bounds.dwMaximum = stumm ? 1 : 65535;
        c->Metrics.cSteps = stumm ? 0 : 64;
    } else {
        LPMIXERCONTROLA a = (LPMIXERCONTROLA)p;
        kopiere_name(a->szShortName, 0, nm, MIXER_SHORT_NAME_CHARS);
        kopiere_name(a->szName, 0, nm, MIXER_LONG_NAME_CHARS);
        a->Bounds.dwMinimum = 0; a->Bounds.dwMaximum = stumm ? 1 : 65535;
        a->Metrics.cSteps = stumm ? 0 : 64;
    }
}

/*  Wert lesen/setzen.  kan = 0: Master bzw. einheitlich, 1/2 = links/rechts.  */
static int
mx_wert (GERAET *d, DWORD id, int kan, DWORD *wert, int setzen)
{
    int stumm = (id == MX_SPK_STUMM || id == MX_MIK_STUMM);
    int aufn  = (id == MX_MIK_LAUT || id == MX_MIK_STUMM);
    UAC_FU *f = mx_fu(d, aufn);
    HANDLE  h;
    short   v, mn, mx, rs;
    int     ok = 0, fk, an = 0;

    if (f == 0) return 0;
    steuer_sperre();
    h = uac_ac_oeffnen(&d->g);
    if (h == INVALID_HANDLE_VALUE) { steuer_frei(); return 0; }
    if (stumm) {
        if (setzen) ok = uac_stumm_setzen(&d->g, f, h, *wert != 0);
        else if ((ok = uac_stumm_lesen(&d->g, f, h, &an)) != 0) *wert = an ? 1 : 0;
    } else {
        fk = (mx_kanaele(f) == 2) ? (kan ? kan : 1) : 0;    /* FU-Kanal */
        if (uac_laut_bereich(&d->g, f, h, fk, &mn, &mx, &rs)) {
            if (setzen) {
                int ziel = regler_nach_db(*wert, mn, mx);
                if (ziel < mn) ziel = mn;
                if (ziel > mx) ziel = mx;
                if (ziel > 0 && mx > 0) ziel = 0;                /* 65535 = 0 dB */
                ok = uac_laut_setzen(&d->g, f, h, fk, (short)ziel);
                if (ok && mx_kanaele(f) == 2 && kan == 0) uac_laut_setzen(&d->g, f, h, 2, (short)ziel);
            } else if (uac_laut_lesen(&d->g, f, h, fk, &v)) {
                *wert = db_nach_regler(v, mn, mx);
                ok = 1;
            }
        }
    }
    CloseHandle(h);
    steuer_frei();
    return ok;
}

/*  Offene Mixer (dieses Prozesses) fuer MM_MIXM_CONTROL_CHANGE.  */
typedef struct { int belegt; DWORD_PTR cb, inst; DWORD flags; HMIXER hmx; int geraet; } MX_OFFEN;
static MX_OFFEN mx_offen[8];

/*  Waechter: ein Regler am Geraet (Lautstaerkerad am Headset) aendert den
 *  Wert, ohne dass jemand danach fragt -- sndvol32 zeigt dann weiter den
 *  alten Stand (Anwenderbericht).  Dieser Faden liest den Wert, solange ein
 *  Mixer offen ist, alle 400 ms und meldet Aenderungen.  Gelesen wird nur,
 *  wenn kein Strom laeuft: waehrend der Wiedergabe kostet das Steuergeraet
 *  Zeit, und die Wiedergabe hat Vorrang.  */
static HANDLE   g_waechter;
static volatile LONG g_waechter_aus;

static DWORD WINAPI
waechter_faden (LPVOID arg)
{
    DWORD letzt[MAX_GERAETE], letzt_m[MAX_GERAETE];
    int k;
    (void)arg;
    for (k = 0; k < MAX_GERAETE; k += 1) { letzt[k] = 0xFFFFFFFFul; letzt_m[k] = 2; }
    while (!g_waechter_aus) {
        int offen = 0;
        for (k = 0; k < 8; k += 1) if (mx_offen[k].belegt) { offen = 1; break; }
        if (offen) {
            for (k = 0; k < anzahl_alle && k < MAX_GERAETE; k += 1) {
                GERAET *d = &geraete[k];
                DWORD v;
                UAC_FU *f;
                if (d->stroeme > 0) continue;        /* Strom laeuft: nicht stoeren */
                v = lautstaerke_lesen(d);
                if (v != letzt[k]) {
                    if (letzt[k] != 0xFFFFFFFFul) mx_melden(k, MX_SPK_LAUT_ID);
                    letzt[k] = v;
                }
                f = d->alt ? uac_fu_fuer(&d->g, d->alt) : 0;
                if (f && (f->ctl[0] & 1)) {
                    HANDLE h;
                    int an = 0;
                    steuer_sperre();
                    h = uac_ac_oeffnen(&d->g);
                    if (h != INVALID_HANDLE_VALUE) {
                        if (uac_stumm_lesen(&d->g, f, h, &an)) {
                            if ((DWORD)an != letzt_m[k]) {
                                if (letzt_m[k] != 2) mx_melden(k, MX_SPK_STUMM_ID);
                                letzt_m[k] = (DWORD)an;
                            }
                        }
                        CloseHandle(h);
                    }
                    steuer_frei();
                }
            }
        }
        Sleep(400);
    }
    return 0;
}

static void
mx_melden (int geraet, DWORD id)
{
    int k;
    for (k = 0; k < 8; k += 1) {
        if (mx_offen[k].belegt && mx_offen[k].geraet == geraet && mx_offen[k].cb) {
            DriverCallback(mx_offen[k].cb, HIWORD(mx_offen[k].flags), (HDRVR)mx_offen[k].hmx,
                           MM_MIXM_CONTROL_CHANGE, mx_offen[k].inst, id, 0);
        }
    }
}

DWORD APIENTRY
mxdMessage (UINT nr, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2)
{
    MX_LINIE l[4];
    GERAET  *d;
    int      nl, k;

    if (msg == MXDM_INIT) return MMSYSERR_NOERROR;
    if (msg == MXDM_GETNUMDEVS) { geraete_suchen(); return (DWORD)anzahl_alle; }
    if (anzahl_alle < 0) geraete_suchen();
    if ((int)nr >= anzahl_alle) return MMSYSERR_BADDEVICEID;
    d = &geraete[nr];
    nl = mx_linien(d, l);

    switch (msg) {

    case MXDM_GETDEVCAPS: {
        char t[MAXPNAMELEN];
        int nziel = 0;
        for (k = 0; k < nl; k += 1) if (l[k].quelle < 0) nziel += 1;
        memset(t, 0, sizeof(t));
        wsprintfA(t, "USB: %.25s", d->g.name[0] ? d->g.name : "Audio");
        if (p2 >= sizeof(MIXERCAPSW)) {
            MIXERCAPSW *c = (MIXERCAPSW *)p1;
            memset(c, 0, sizeof(*c));
            c->wMid = 1; c->wPid = (WORD)d->g.pid; c->vDriverVersion = 0x0100;
            kopiere_name(c->szPname, 1, t, MAXPNAMELEN);
            c->cDestinations = (DWORD)nziel;
        } else if (p2 >= sizeof(MIXERCAPSA)) {
            MIXERCAPSA *c = (MIXERCAPSA *)p1;
            memset(c, 0, sizeof(*c));
            c->wMid = 1; c->wPid = (WORD)d->g.pid; c->vDriverVersion = 0x0100;
            kopiere_name(c->szPname, 0, t, MAXPNAMELEN);
            c->cDestinations = (DWORD)nziel;
        } else {
            return MMSYSERR_INVALPARAM;
        }
        return MMSYSERR_NOERROR;
    }

    case MXDM_OPEN: {
        LPMIXEROPENDESC od = (LPMIXEROPENDESC)p1;
        for (k = 0; k < 8 && mx_offen[k].belegt; k += 1) ;
        if (k == 8) return MMSYSERR_NOMEM;
        mx_offen[k].belegt = 1; mx_offen[k].geraet = (int)nr;
        mx_offen[k].cb = od ? od->dwCallback : 0; mx_offen[k].inst = od ? od->dwInstance : 0;
        mx_offen[k].hmx = od ? od->hmx : 0; mx_offen[k].flags = (DWORD)p2;
        *(DWORD_PTR *)user = (DWORD_PTR)(k + 1);
        if (g_waechter == 0) {
            DWORD id;
            g_waechter_aus = 0;
            g_waechter = CreateThread(NULL, 0, waechter_faden, NULL, 0, &id);
        }
        return MMSYSERR_NOERROR;
    }

    case MXDM_CLOSE:
        if (user >= 1 && user <= 8) mx_offen[user - 1].belegt = 0;
        return MMSYSERR_NOERROR;

    case MXDM_GETLINEINFO: {
        LPMIXERLINEW m = (LPMIXERLINEW)p1;
        DWORD wie = (DWORD)p2 & MIXER_GETLINEINFOF_QUERYMASK;
        if (m == 0 || m->cbStruct < sizeof(MIXERLINEA)) return MMSYSERR_INVALPARAM;
        for (k = 0; k < nl; k += 1) {
            MX_LINIE *x = &l[k];
            if ((wie == MIXER_GETLINEINFOF_DESTINATION && x->quelle < 0 && (DWORD)x->ziel == m->dwDestination) ||
                (wie == MIXER_GETLINEINFOF_SOURCE && x->quelle >= 0 && (DWORD)x->ziel == m->dwDestination &&
                 (DWORD)x->quelle == m->dwSource) ||
                (wie == MIXER_GETLINEINFOF_LINEID && x->id == m->dwLineID) ||
                (wie == MIXER_GETLINEINFOF_COMPONENTTYPE && x->art == m->dwComponentType) ||
                (wie == MIXER_GETLINEINFOF_TARGETTYPE && x->quelle < 0 &&
                 x->zieltyp == (m->cbStruct >= sizeof(MIXERLINEW) ? m->Target.dwType : ((LPMIXERLINEA)m)->Target.dwType))) {
                return mx_linie_fuellen(d, (int)nr, l, nl, x, m);
            }
        }
        if (wie == MIXER_GETLINEINFOF_DESTINATION) return MIXERR_INVALLINE;
        if (wie == MIXER_GETLINEINFOF_SOURCE) return MIXERR_INVALLINE;
        return MIXERR_INVALLINE;
    }

    case MXDM_GETLINECONTROLS: {
        LPMIXERLINECONTROLSW lc = (LPMIXERLINECONTROLSW)p1;
        DWORD wie = (DWORD)p2 & MIXER_GETLINECONTROLSF_QUERYMASK;
        int   breit;
        if (lc == 0 || lc->pamxctrl == 0) return MMSYSERR_INVALPARAM;
        breit = lc->cbmxctrl >= sizeof(MIXERCONTROLW);
        if (lc->cbmxctrl < sizeof(MIXERCONTROLA)) return MMSYSERR_INVALPARAM;
        if (wie == MIXER_GETLINECONTROLSF_ONEBYID) {
            DWORD id = lc->dwControlID;
            for (k = 0; k < nl; k += 1) {
                if ((DWORD)l[k].laut == id || (DWORD)l[k].stumm == id) {
                    if (id == 0) break;
                    lc->dwLineID = l[k].id;
                    mx_ctl_fuellen(d, id, lc->pamxctrl, breit);
                    return MMSYSERR_NOERROR;
                }
            }
            return MIXERR_INVALCONTROL;
        }
        for (k = 0; k < nl; k += 1) {
            MX_LINIE *x = &l[k];
            if (x->id != lc->dwLineID) continue;
            if (wie == MIXER_GETLINECONTROLSF_ONEBYTYPE) {
                DWORD id = (lc->dwControlType == MIXERCONTROL_CONTROLTYPE_VOLUME) ? (DWORD)x->laut :
                           (lc->dwControlType == MIXERCONTROL_CONTROLTYPE_MUTE) ? (DWORD)x->stumm : 0;
                if (id == 0) return MIXERR_INVALCONTROL;
                mx_ctl_fuellen(d, id, lc->pamxctrl, breit);
                return MMSYSERR_NOERROR;
            } else {                                     /* ALL */
                char *z = (char *)lc->pamxctrl;
                DWORD n = 0;
                if (x->laut  && n < lc->cControls) { mx_ctl_fuellen(d, (DWORD)x->laut,  z + n * lc->cbmxctrl, breit); n += 1; }
                if (x->stumm && n < lc->cControls) { mx_ctl_fuellen(d, (DWORD)x->stumm, z + n * lc->cbmxctrl, breit); n += 1; }
                return MMSYSERR_NOERROR;
            }
        }
        return MIXERR_INVALLINE;
    }

    case MXDM_GETCONTROLDETAILS:
    case MXDM_SETCONTROLDETAILS: {
        LPMIXERCONTROLDETAILS cd = (LPMIXERCONTROLDETAILS)p1;
        DWORD id, *w;
        int   setzen = (msg == MXDM_SETCONTROLDETAILS), ok = 1;
        if (cd == 0 || cd->paDetails == 0 || cd->cbDetails < sizeof(DWORD)) return MMSYSERR_INVALPARAM;
        if (((DWORD)p2 & MIXER_GETCONTROLDETAILSF_QUERYMASK) != MIXER_GETCONTROLDETAILSF_VALUE) return MMSYSERR_NOTSUPPORTED;
        id = cd->dwControlID;
        if (id < MX_SPK_LAUT || id > MX_MIK_STUMM) return MIXERR_INVALCONTROL;
        w = (DWORD *)cd->paDetails;
        if (cd->cChannels <= 1 || id == MX_SPK_STUMM || id == MX_MIK_STUMM ||
            mx_kanaele(mx_fu(d, id >= MX_MIK_LAUT)) == 1) {
            ok = mx_wert(d, id, 0, &w[0], setzen);
            if (!setzen) for (k = 1; k < (int)cd->cChannels; k += 1) w[k] = w[0];
        } else {
            ok = mx_wert(d, id, 1, &w[0], setzen) && mx_wert(d, id, 2, &w[1], setzen);
        }
        if (!ok) return MMSYSERR_ERROR;
        if (setzen) {
            if (id == MX_SPK_LAUT) d->lautstaerke = (w[0] & 0xFFFF) | ((cd->cChannels > 1 ? w[1] : w[0]) << 16);
            mx_melden((int)nr, id);
        }
        return MMSYSERR_NOERROR;
    }

    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}

LRESULT APIENTRY
DriverProc (DWORD_PTR id, HDRVR hdrv, UINT msg, LPARAM p1, LPARAM p2)
{
    switch (msg) {
    case DRV_LOAD:
    case DRV_FREE:
    case DRV_OPEN:
    case DRV_CLOSE:
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1;
    case DRV_QUERYCONFIGURE:
        return 0;
    case DRV_INSTALL:
    case DRV_REMOVE:
        return DRVCNF_OK;
    default:
        return DefDriverProc(id, hdrv, msg, p1, p2);
    }
}

BOOL WINAPI
DllEntry (HINSTANCE h, DWORD grund, LPVOID r)
{
    (void)r;
    if (grund == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&gsperre);
    }
    return TRUE;
}
