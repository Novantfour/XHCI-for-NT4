/*
 *  werkzeug/usbton.c -- USB-Audio (UAC1) pruefen.
 *
 *      usbton                    Audiogeraete finden, Deskriptoren auswerten
 *      usbton stumm 0|1 [mikro]  Stummschaltung (Lautsprecher- bzw. Mikrofonzweig)
 *      usbton laut DB [mikro]    Lautstaerke in dB (z. B. -10, 0)
 *      usbton lautlesen          aktuelle Werte beider Zweige
 *      usbton roh                Konfigurationsdeskriptor als Hex
 *      usbton ton [HZ] [S]       Sinus direkt ueber USBAUDIO.SYS (\\.\UsbAudioN)
 *      usbton tonroh [HZ] [S] [R]  dasselbe ueber ntugen (\\.\UsbDevN), R Rahmen
 *                                je Aufruf -- nur zum Vergleich: 2-3 ms Luecke
 *                                je Aufruf; geht nur ohne USBAUDIO.SYS
 *      usbton winmm [HZ] [S] [RATE] [BITS] [KANAELE] [GERAET]
 *                                Sinus ueber winmm (waveOut), wie jedes
 *                                Programm -- prueft usbaudio.dll samt
 *                                Formatwandlung
 *      usbton wav DATEI [GERAET] WAV-Datei ueber winmm abspielen
 *      usbton aufnahme S DATEI   S Sekunden ueber USBAUDIO.SYS aufnehmen,
 *                                als WAV speichern
 *      usbton mixer [laut PROZENT | stumm 0|1]
 *                                Mixer ueber winmm (wie sndvol32): Ziele,
 *                                Quellen, Regler; setzt Lautsprecher-Werte
 *      usbton wmaufnahme S RATE BITS KANAELE DATEI
 *                                dasselbe ueber winmm (waveIn), wie ein Programm
 *
 *  Geraeteauswertung: uac.c.  Nur kernel32, user32, advapi32, winmm.
 */

#include <windows.h>
#include <mmsystem.h>
#include "ntbasis.h"
#include "uac.h"

static UAC_GERAET g;
static unsigned char anfrage[sizeof(NTUG_ISO) + NTUG_MAX_ISO_RAHMEN * 4 + 32768];
static char          puffer[sizeof(NTUG_ERGEBNIS) + NTUG_MAX_DATEN + 1024];

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

static double
sinus (double x)                              /* x in [-pi, pi] */
{
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}

static void
liste (void)
{
    static const char *sync[4] = { "keine", "asynchron", "adaptiv", "synchron" };
    int k, c;

    printf("  Audiogeraet %04x:%04x \"%s\" -- AC-Schnittstelle %u an UsbDev%d (Protokoll %02x)\n",
           g.vid, g.pid, g.name, g.ac_if, g.ac_dev, g.protokoll);
    printf("  bcdADC %x.%02x -> UAC %d%s\n", g.adc >> 8, g.adc & 0xFF, g.uac,
           g.uac == 1 ? "" : "  (nur UAC 1.0 wird angesteuert)");
    for (k = 0; k < g.nent; k += 1) {
        static const char *art[9] = { "", "", "Eingang", "Ausgang", "Mixer", "Selector",
                                       "Feature Unit", "Processing", "Extension" };
        UAC_ENT *e = &g.ent[k];
        int q;
        printf("  Glied %u: %s", e->id, e->art < 9 ? art[e->art] : "?");
        if (e->art == 2 || e->art == 3) printf(" Typ %04x%s", e->typ,
               e->typ == 0x0101 ? " (USB-Strom)" : (e->typ >> 8) == 2 ? " (Mikrofon)" :
               (e->typ >> 8) == 3 ? " (Lautsprecher/Kopfhoerer)" : (e->typ >> 8) == 4 ? " (Headset)" : "");
        if (e->nq) { printf(" <-"); for (q = 0; q < e->nq; q += 1) printf(" %u", e->quelle[q]); }
        printf("\n");
    }
    for (k = 0; k < g.nfu; k += 1) {
        UAC_FU *f = &g.fu[k];
        printf("  Feature Unit %u (Quelle %u):", f->id, f->quelle);
        for (c = 0; c < f->kanaele; c += 1) {
            if (c == 0) printf(" Master["); else printf(" K%d[", c);
            printf("%s%s]", (f->ctl[c] & 1) ? "Stumm " : "", (f->ctl[c] & 2) ? "Laut" : "");
        }
        printf("\n");
    }
    for (k = 0; k < g.nalt; k += 1) {
        UAC_ALT *a = &g.alt[k];
        printf("  Schnittstelle %u Alt %u: Format %u, %u Kanaele, %u Bit in %u Byte",
               a->ifnr, a->alt, a->format, a->kanaele, a->bits, a->bytes);
        if (a->ifnr < UAC_MAX_IF && g.au_dev[a->ifnr] >= 0) printf("  -> UsbAudio%d\n", g.au_dev[a->ifnr]);
        else if (a->ifnr < UAC_MAX_IF && g.as_dev[a->ifnr] >= 0) printf("  -> UsbDev%d (ohne USBAUDIO)\n", g.as_dev[a->ifnr]);
        else printf("\n");
        printf("     Endpunkt %02x %s, %s, max. %u Byte, Raten", a->ep,
               (a->ep & 0x80) ? "IN (Aufnahme)" : "OUT (Wiedergabe)",
               sync[(a->epattr >> 2) & 3], a->maxpaket);
        if (a->ratentyp == 0) printf(" %lu..%lu Hz", a->rate[0], a->rate[1]);
        else for (c = 0; c < a->ratentyp && c < UAC_MAX_RATE; c += 1) printf(" %lu", a->rate[c]);
        printf("%s\n", (a->cs_attr & 1) ? ", Rate einstellbar" : ", Rate fest");
        {
            UAC_FU *f = uac_fu_fuer(&g, a);
            printf("     Signalweg ab Terminal %u, Regler: ", a->link);
            if (f) printf("Feature Unit %u\n", f->id); else printf("keiner gefunden\n");
        }
    }
}

/*  Feature Unit des Lautsprecher- bzw. Mikrofonzweigs.  */
static UAC_FU *
zweig_fu (int mikro)
{
    unsigned long r = 0;
    UAC_ALT *a = mikro ? uac_aufnahme(&g, 0, &r) : uac_wiedergabe(&g, 0, &r);
    UAC_FU  *f = uac_fu_fuer(&g, a);
    return f ? f : uac_fu(&g, 2);
}

static void
stumm (int an, int mikro)
{
    UAC_FU *f = zweig_fu(mikro);
    HANDLE h = uac_ac_oeffnen(&g);
    printf("  %s, Feature Unit %u: stumm %s: %s\n", mikro ? "Mikrofon" : "Lautsprecher", f ? f->id : 0,
           an ? "an" : "aus", uac_stumm_setzen(&g, f, h, an) ? "ok" : "Fehler/keine");
    CloseHandle(h);
}

static void
laut (int db, int mikro)
{
    UAC_FU *f = zweig_fu(mikro);
    HANDLE h = uac_ac_oeffnen(&g);
    short mn, mx, rs, v;
    int c;

    printf("  %s, Feature Unit %u:\n", mikro ? "Mikrofon" : "Lautsprecher", f ? f->id : 0);
    for (c = 0; c < 9; c += 1) {
        if (!uac_laut_bereich(&g, f, h, c, &mn, &mx, &rs)) continue;
        v = (short)(db * 256);
        if (v < mn) v = mn;
        if (v > mx) v = mx;
        printf("  %s: Bereich %d..%d /256 dB (Schritt %d), setze %d: %s\n", c ? "Kanal" : "Master",
               mn, mx, rs, v, uac_laut_setzen(&g, f, h, c, v) ? "ok" : "Fehler");
    }
    CloseHandle(h);
}

/*  Vor dem Abspielen hoerbar machen: Stumm aus, Lautstaerke 0 dB (oder
 *  das Hoechste, wenn das Geraet nicht bis 0 dB geht).  */
static void
hoerbar (UAC_FU *f, HANDLE h)
{
    short mn, mx, rs;
    int c;
    if (f == 0) return;
    uac_stumm_setzen(&g, f, h, 0);
    for (c = 0; c < 9; c += 1) {
        if (uac_laut_bereich(&g, f, h, c, &mn, &mx, &rs)) uac_laut_setzen(&g, f, h, c, (short)(mx < 0 ? mx : 0));
    }
}

static void
lautlesen (void)
{
    int z;
    for (z = 0; z < 2; z += 1) {
        UAC_FU *f = zweig_fu(z);
        HANDLE h = uac_ac_oeffnen(&g);
        short v, mn, mx, rs;
        int c, an;
        if (f == 0) continue;
        printf("  %s, Feature Unit %u:", z ? "Mikrofon" : "Lautsprecher", f->id);
        if (uac_stumm_lesen(&g, f, h, &an)) printf(" stumm=%d", an);
        for (c = 0; c < 9; c += 1) {
            if (uac_laut_lesen(&g, f, h, c, &v) && uac_laut_bereich(&g, f, h, c, &mn, &mx, &rs))
                printf("  %s %d (%d..%d)", c ? "K" : "Master", v, mn, mx);
        }
        printf("  [/256 dB]\n");
        CloseHandle(h);
    }
}

static void
roh (void)
{
    unsigned long k;
    printf("  Konfigurationsdeskriptor, %lu Byte:\n", g.konflen);
    for (k = 0; k < g.konflen; k += 1) {
        if (k % 16 == 0) printf("  %04lx:", k);
        printf(" %02x", g.konf[k]);
        if (k % 16 == 15 || k + 1 == g.konflen) printf("\n");
    }
}

static void
tonroh (int hz, int sekunden, int je)
{
    unsigned long r0 = 0;
    UAC_ALT *a = uac_wiedergabe(&g, 0, &r0);
    HANDLE h;
    NTUG_ERGEBNIS *e;
    NTUG_ISO *q;
    unsigned long rate, *laengen, akk = 0, gesamt = 0, fehler = 0, aufrufe = 0;
    LARGE_INTEGER f, t0, t1, ta, tb;
    double phase = 0, schritt, pi = 3.14159265358979, dmin = 1e9, dmax = 0;
    unsigned char alt;
    short *s;
    long r;

    if (a == 0) { printf("  Keine passende Alternative (PCM, 16 Bit, Stereo, OUT).\n"); return; }
    rate = r0;
    if (je < 1) je = 1;
    if (je > NTUG_MAX_ISO_RAHMEN) je = NTUG_MAX_ISO_RAHMEN;

    h = INVALID_HANDLE_VALUE;
    if (a->ifnr < UAC_MAX_IF && g.as_dev[a->ifnr] >= 0) {
        char nm[24];
        wsprintfA(nm, "\\\\.\\UsbDev%d", g.as_dev[a->ifnr]);
        h = CreateFileA(nm, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        printf("  Die Streaming-Schnittstelle gehoert USBAUDIO.SYS (oder fehlt) -- tonroh geht nur ohne.\n");
        return;
    }

    alt = a->alt;
    e = rufe(h, IOCTL_NTUG_ALTERNATIVE, &alt, 1);
    printf("  Schnittstelle %u, Alternative %u: %s\n", a->ifnr, a->alt,
           e && e->Fehler == 0 ? "ok" : "Fehler");
    if (a->cs_attr & 1) {
        unsigned char rr[3];
        rr[0] = (unsigned char)rate; rr[1] = (unsigned char)(rate >> 8); rr[2] = (unsigned char)(rate >> 16);
        r = uac_steuer(h, 0x22, 0x01, 0x0100, a->ep, rr, 3);
        printf("  Abtastrate %lu Hz setzen: %s\n", rate, r == 0 ? "ok" : "Fehler (wird ignoriert)");
    }
    printf("  Ton %d Hz, %d s, %lu Hz Stereo 16 Bit, %d Rahmen je Aufruf ...\n", hz, sekunden, rate, je);

    QueryPerformanceFrequency(&f);
    schritt = 2 * pi * hz / rate;
    QueryPerformanceCounter(&t0);

    while (gesamt < (unsigned long)sekunden * 1000) {
        unsigned long k, n, bytes = 0;

        q = (NTUG_ISO *)anfrage;
        memset(q, 0, sizeof(*q));
        q->Endpunkt = a->ep; q->Rahmen = je; q->Zeitlimit = 2000;
        laengen = (unsigned long *)(q + 1);
        s = (short *)(laengen + je);

        for (k = 0; k < (unsigned long)je; k += 1) {
            akk += rate;                     /* Bruchteil-Akkumulator: 44,1 kHz -> 44/45 */
            n = akk / 1000; akk -= n * 1000;
            laengen[k] = n * 4;
            while (n-- > 0) {
                short v = (short)(sinus(phase) * 8000);
                *s++ = v; *s++ = v;
                phase += schritt;
                if (phase > pi) phase -= 2 * pi;
            }
            bytes += laengen[k];
        }

        QueryPerformanceCounter(&ta);
        e = rufe(h, IOCTL_NTUG_ISO, anfrage, sizeof(*q) + je * 4 + bytes);
        QueryPerformanceCounter(&tb);
        aufrufe += 1;
        if (e == 0 || (e->Fehler != 0 && e->Fehler != NTUG_FEHLER_KURZ)) {
            fehler += 1;
            if (fehler <= 3) printf("  Aufruf %lu: Fehler %ld\n", aufrufe, e ? e->Fehler : -1L);
        }
        {
            double ms = (double)(tb.QuadPart - ta.QuadPart) * 1000.0 / (double)f.QuadPart;
            if (ms < dmin) dmin = ms;
            if (ms > dmax) dmax = ms;
        }
        gesamt += je;
    }
    QueryPerformanceCounter(&t1);

    alt = 0;
    rufe(h, IOCTL_NTUG_ALTERNATIVE, &alt, 1);
    CloseHandle(h);

    {
        long ist = (long)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
        printf("  %lu Rahmen in %lu Aufrufen, %lu Fehler\n", gesamt, aufrufe, fehler);
        printf("  Laufzeit %ld ms, Soll %lu ms -> %ld ms Luecken (%ld je Aufruf)\n",
               ist, gesamt, ist - (long)gesamt, (ist - (long)gesamt) / (long)aufrufe);
        printf("  je Aufruf %ld..%ld ms (Soll %d)\n", (long)dmin, (long)(dmax + 0.5), je);
    }
}

/*  Ueber USBAUDIO.SYS: Strom starten, Ring nachfuellen, am Ende leerlaufen
 *  lassen.  Massstab sind die Unterlaeufe des Treibers.  */

static void
ton (int hz, int sekunden)
{
    static short   blk[4096];               /* 2048 Abtastungen Stereo */
    unsigned long  r0 = 0;
    UAC_ALT       *a = uac_wiedergabe(&g, 0, &r0);
    HANDLE         h;
    NTUA_START     st;
    NTUA_STAND     z;
    DWORD          got;
    char           name[24];
    unsigned long  rate = 48000, soll, gesendet = 0, im_blk = 0, ab = 0, warte = 0;
    unsigned long  echt = 0;                 /* Unterlaeufe bis zum letzten Byte */
    double         phase = 0, schritt, pi = 3.14159265358979;
    LARGE_INTEGER  f, t0, t1;
    int            c;

    if (a == 0) { printf("  Keine passende Alternative (PCM, 16 Bit, Stereo, OUT).\n"); return; }
    if (a->ifnr >= UAC_MAX_IF || g.au_dev[a->ifnr] < 0) {
        printf("  Schnittstelle %u gehoert nicht USBAUDIO.SYS -- eingerichtet?  (usbton tonroh geht ohne)\n", a->ifnr);
        return;
    }
    rate = r0;
    wsprintfA(name, "\\\\.\\UsbAudio%d", g.au_dev[a->ifnr]);
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("  %s nicht zu oeffnen (%lu)\n", name + 4, GetLastError()); return; }

    memset(&st, 0, sizeof(st));
    st.Alternative = a->alt; st.Endpunkt = a->ep; st.RateSetzen = (unsigned char)(a->cs_attr & 1);
    st.Rate = rate; st.BytesJeAbtastung = 4; st.RingByte = 32768;
    {
        UAC_FU *fu = uac_fu_fuer(&g, a);
        HANDLE  ac = uac_ac_oeffnen(&g);
        if (fu && ac != INVALID_HANDLE_VALUE) {
            hoerbar(fu, ac);
            printf("  Feature Unit %u: stumm aus, 0 dB\n", fu->id);
        }
        if (ac != INVALID_HANDLE_VALUE) CloseHandle(ac);
    }
    if (!DeviceIoControl(h, IOCTL_NTUA_START, &st, sizeof(st), &z, sizeof(z), &got, NULL)) {
        printf("  START gescheitert (%lu)\n", GetLastError());
        CloseHandle(h);
        return;
    }
    printf("  %s: Alternative %u, EP %02x, %lu Hz, %lu Rahmen/s; Ton %d Hz, %d s ...\n",
           name + 4, a->alt, a->ep, rate, z.RahmenJeSekunde, hz, sekunden);

    soll = rate * 4 * (unsigned long)sekunden;
    schritt = 2 * pi * hz / rate;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    while (gesendet < soll) {
        if (ab == im_blk) {                  /* neuen Block erzeugen */
            unsigned long k, n = sizeof(blk) / 4;
            if (n * 4 > soll - gesendet) n = (soll - gesendet) / 4;
            for (k = 0; k < n; k += 1) {
                short v = (short)(sinus(phase) * 8000);
                blk[2 * k] = v; blk[2 * k + 1] = v;
                phase += schritt;
                if (phase > pi) phase -= 2 * pi;
            }
            im_blk = n * 4; ab = 0;
        }
        if (!DeviceIoControl(h, IOCTL_NTUA_SCHREIBEN, (char *)blk + ab, im_blk - ab,
                             &z, sizeof(z), &got, NULL)) {
            printf("  SCHREIBEN gescheitert (%lu)\n", GetLastError());
            break;
        }
        ab += z.Angenommen; gesendet += z.Angenommen;
        echt = z.Unterlaeufe;                /* danach: gewolltes Auslaufen */
        if (z.Angenommen == 0) { Sleep(5); warte += 1; }   /* Ring voll */
    }

    do {                                     /* leerlaufen lassen */
        Sleep(10);
        DeviceIoControl(h, IOCTL_NTUA_STAND, NULL, 0, &z, sizeof(z), &got, NULL);
    } while (z.Fuellstand != 0 && z.Laeuft);
    QueryPerformanceCounter(&t1);

    DeviceIoControl(h, IOCTL_NTUA_STOP, NULL, 0, &z, sizeof(z), &got, NULL);
    CloseHandle(h);

    printf("  %lu Byte geschrieben, %lu gespielt, %lu Rahmen, letzter USB-Fehler %lu\n",
           gesendet, z.GespieltLo, z.Rahmen, z.Fehler);
    printf("  Unterlaeufe waehrend des Tons: %lu   (beim Auslaufen danach: %lu, gewollt)\n",
           echt, z.Unterlaeufe - echt);
    printf("  Laufzeit %ld ms fuer %d000 ms Ton (%lu mal Ring voll)\n",
           (long)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart), sekunden, warte);
}

/*  Aufnahme direkt ueber USBAUDIO.SYS.  */

static void
aufnahme (int sek, const char *datei)
{
    static unsigned char d[48000 * 4 * 10];
    static unsigned char aus[sizeof(NTUA_STAND) + 8192];
    unsigned long  rate = 0, soll, n = 0, kopf[11];
    UAC_ALT       *a = uac_aufnahme(&g, 0, &rate);
    NTUA_START     st;
    NTUA_STAND     z, *zz = (NTUA_STAND *)aus;
    DWORD          got, w;
    HANDLE         h, f;
    int            leer = 0;

    if (a == 0) { printf("  Keine Aufnahme-Alternative (PCM, 16 Bit, IN).\n"); return; }
    if (a->ifnr >= UAC_MAX_IF || g.au_dev[a->ifnr] < 0) { printf("  Schnittstelle %u gehoert nicht USBAUDIO.SYS.\n", a->ifnr); return; }
    if (sek > 10) sek = 10;
    h = uac_strom_oeffnen(g.au_dev[a->ifnr]);
    if (h == INVALID_HANDLE_VALUE) { printf("  UsbAudio%d nicht zu oeffnen\n", g.au_dev[a->ifnr]); return; }

    memset(&st, 0, sizeof(st));
    st.Alternative = a->alt; st.Endpunkt = a->ep; st.RateSetzen = (unsigned char)(a->cs_attr & 1);
    st.Rate = rate; st.BytesJeAbtastung = (unsigned long)a->kanaele * 2; st.RingByte = 32768;
    if (!DeviceIoControl(h, IOCTL_NTUA_START, &st, sizeof(st), &z, sizeof(z), &got, NULL)) {
        printf("  START gescheitert (%lu)\n", GetLastError()); CloseHandle(h); return;
    }
    printf("  UsbAudio%d: Aufnahme Alt %u, EP %02x, %lu Hz, %u Kanaele, %d s ...\n",
           g.au_dev[a->ifnr], a->alt, a->ep, rate, a->kanaele, sek);
    soll = rate * st.BytesJeAbtastung * (unsigned long)sek;
    while (n < soll && leer < 300) {
        if (!DeviceIoControl(h, IOCTL_NTUA_LESEN, NULL, 0, aus, sizeof(aus), &got, NULL)) {
            printf("  LESEN gescheitert (%lu)\n", GetLastError()); break;
        }
        if (zz->Angenommen == 0) { Sleep(10); leer += 1; continue; }
        leer = 0;
        if (zz->Angenommen > soll - n) zz->Angenommen = soll - n;
        memcpy(d + n, aus + sizeof(NTUA_STAND), zz->Angenommen);
        n += zz->Angenommen;
    }
    DeviceIoControl(h, IOCTL_NTUA_STOP, NULL, 0, &z, sizeof(z), &got, NULL);
    CloseHandle(h);
    printf("  %lu Byte aufgenommen, %lu Rahmen, Ueberlaeufe %lu Byte, letzter USB-Fehler %lu\n",
           n, z.Rahmen, z.Ueberlaeufe, z.Fehler);

    /*  WAV schreiben.  */
    kopf[0] = 0x46464952; kopf[1] = 36 + n; kopf[2] = 0x45564157;      /* RIFF ... WAVE */
    kopf[3] = 0x20746d66; kopf[4] = 16;                                  /* fmt */
    kopf[5] = 1 | ((unsigned long)a->kanaele << 16);
    kopf[6] = rate; kopf[7] = rate * st.BytesJeAbtastung;
    kopf[8] = st.BytesJeAbtastung | (16ul << 16);
    kopf[9] = 0x61746164; kopf[10] = n;                                  /* data */
    f = CreateFileA(datei, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("  %s nicht anzulegen\n", datei); return; }
    WriteFile(f, kopf, sizeof(kopf), &w, NULL);
    WriteFile(f, d, n, &w, NULL);
    CloseHandle(f);
    printf("  gespeichert: %s\n", datei);
}

/* ---------------------------------------------------------------- *
 *  Ueber winmm (waveOut) -- der Weg jedes Programms
 * ---------------------------------------------------------------- */

#define WM_PUFFER  4

static unsigned long wm_abbruch_ms;           /* > 0: nach so vielen ms waveOutReset */
static unsigned long wm_pause_ms;             /* > 0: nach so vielen ms 1 s Pause */

static int
wm_spielen (UINT geraet, WAVEFORMATEX *wf, const unsigned char *daten, unsigned long laenge,
            const char *was)
{
    HWAVEOUT    wo;
    WAVEHDR     hd[WM_PUFFER];
    HANDLE      ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    MMRESULT    r;
    MMTIME      mt;
    unsigned long stueck, pos = 0, fertig = 0;
    int         k, offen = 0;
    LARGE_INTEGER f, t0, t1;
    WAVEOUTCAPSA caps;

    if (waveOutGetDevCapsA(geraet, &caps, sizeof(caps)) == 0) {
        printf("  winmm-Geraet %u: \"%s\"\n", geraet, caps.szPname);
    }
    r = waveOutOpen(&wo, geraet, wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (r != 0) {
        printf("  waveOutOpen: Fehler %u\n", r);
        return 0;
    }
    printf("  %s: %lu Hz, %u Bit, %u Kanaele, %lu Byte\n", was, wf->nSamplesPerSec,
           wf->wBitsPerSample, wf->nChannels, laenge);
    stueck = wf->nAvgBytesPerSec / 10;              /* 100 ms je Puffer */
    stueck -= stueck % wf->nBlockAlign;
    memset(hd, 0, sizeof(hd));

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    while (fertig < laenge) {
        for (k = 0; k < WM_PUFFER; k += 1) {
            if ((hd[k].dwFlags & WHDR_PREPARED) && !(hd[k].dwFlags & WHDR_DONE)) continue;
            if (hd[k].dwFlags & WHDR_DONE) {
                fertig += hd[k].dwBufferLength;
                waveOutUnprepareHeader(wo, &hd[k], sizeof(WAVEHDR));
                hd[k].dwFlags = 0;
                offen -= 1;
            }
            if (pos < laenge) {
                hd[k].lpData = (LPSTR)(daten + pos);
                hd[k].dwBufferLength = (laenge - pos < stueck) ? laenge - pos : stueck;
                pos += hd[k].dwBufferLength;
                waveOutPrepareHeader(wo, &hd[k], sizeof(WAVEHDR));
                r = waveOutWrite(wo, &hd[k], sizeof(WAVEHDR));
                if (r != 0) { printf("  waveOutWrite: Fehler %u\n", r); fertig = laenge; break; }
                offen += 1;
            }
        }
        if (fertig < laenge) WaitForSingleObject(ev, 1000);
        if (wm_pause_ms) {
            QueryPerformanceCounter(&t1);
            if ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart >= (double)wm_pause_ms) {
                MMTIME m1, m2;
                MMRESULT rp, rr;
                m1.wType = TIME_BYTES; m2.wType = TIME_BYTES;
                rp = waveOutPause(wo);
                Sleep(300);
                waveOutGetPosition(wo, &m1, sizeof(m1));
                Sleep(700);
                waveOutGetPosition(wo, &m2, sizeof(m2));
                rr = waveOutRestart(wo);
                printf("  waveOutPause %s, Position in der Pause %lu -> %lu Byte, waveOutRestart %s\n",
                       rp == 0 ? "ok" : "Fehler", m1.u.cb, m2.u.cb, rr == 0 ? "ok" : "Fehler");
                wm_pause_ms = 0;
            }
        }
        if (wm_abbruch_ms) {
            QueryPerformanceCounter(&t1);
            if ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart >= (double)wm_abbruch_ms) {
                int zurueck = 0;
                r = waveOutReset(wo);
                for (k = 0; k < WM_PUFFER; k += 1) {
                    if (hd[k].dwFlags & WHDR_PREPARED) {
                        if (hd[k].dwFlags & WHDR_DONE) zurueck += 1;
                        waveOutUnprepareHeader(wo, &hd[k], sizeof(WAVEHDR));
                        hd[k].dwFlags = 0;
                    }
                }
                printf("  waveOutReset nach %lu ms: %s, %d Puffer zurueck (offen waren %d)\n",
                       wm_abbruch_ms, r == 0 ? "ok" : "Fehler", zurueck, offen);
                break;
            }
        }
    }
    QueryPerformanceCounter(&t1);
    mt.wType = TIME_BYTES;
    waveOutGetPosition(wo, &mt, sizeof(mt));
    printf("  fertig: %lu ms, waveOutGetPosition %lu Byte\n",
           (unsigned long)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart),
           mt.u.cb);
    r = waveOutClose(wo);
    if (r != 0) printf("  waveOutClose: Fehler %u\n", r);
    CloseHandle(ev);
    return 1;
}

static void
wav_schreiben (const char *datei, const WAVEFORMATEX *wf, const void *d, unsigned long n)
{
    unsigned long kopf[11];
    HANDLE f;
    DWORD w;

    kopf[0] = 0x46464952; kopf[1] = 36 + n; kopf[2] = 0x45564157;
    kopf[3] = 0x20746d66; kopf[4] = 16;
    kopf[5] = 1 | ((unsigned long)wf->nChannels << 16);
    kopf[6] = wf->nSamplesPerSec; kopf[7] = wf->nAvgBytesPerSec;
    kopf[8] = wf->nBlockAlign | ((unsigned long)wf->wBitsPerSample << 16);
    kopf[9] = 0x61746164; kopf[10] = n;
    f = CreateFileA(datei, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("  %s nicht anzulegen\n", datei); return; }
    WriteFile(f, kopf, sizeof(kopf), &w, NULL);
    WriteFile(f, d, n, &w, NULL);
    CloseHandle(f);
    printf("  gespeichert: %s (%lu Byte)\n", datei, n);
}

/*  Aufnahme ueber winmm: 4 Puffer zu 100 ms kreisen lassen.  */
static void
wm_aufnahme (int sek, unsigned long rate, int bits, int kan, const char *datei)
{
    static unsigned char d[48000 * 4 * 10];
    static unsigned char puf[4][48000 * 4 / 10];
    WAVEFORMATEX wf;
    HWAVEIN      wi;
    WAVEHDR      hd[4];
    HANDLE       ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    MMRESULT     r;
    MMTIME       mt;
    WAVEINCAPSA  caps;
    unsigned long soll, n = 0, stueck;
    int          k, runden = 0;

    if (sek > 10) sek = 10;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = (WORD)kan; wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = (WORD)bits; wf.nBlockAlign = (WORD)(kan * bits / 8);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
    soll = wf.nAvgBytesPerSec * (unsigned long)sek;
    stueck = wf.nAvgBytesPerSec / 10; stueck -= stueck % wf.nBlockAlign;

    printf("  waveInGetNumDevs = %u\n", waveInGetNumDevs());
    if (waveInGetDevCapsA(0, &caps, sizeof(caps)) == 0) printf("  Geraet 0: \"%s\", Formate %08lx\n", caps.szPname, caps.dwFormats);
    r = waveInOpen(&wi, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (r != 0) { printf("  waveInOpen: Fehler %u\n", r); return; }
    printf("  Aufnahme %lu Hz, %d Bit, %d Kanaele, %d s ...\n", rate, bits, kan, sek);
    memset(hd, 0, sizeof(hd));
    for (k = 0; k < 4; k += 1) {
        hd[k].lpData = (LPSTR)puf[k]; hd[k].dwBufferLength = stueck;
        waveInPrepareHeader(wi, &hd[k], sizeof(WAVEHDR));
        waveInAddBuffer(wi, &hd[k], sizeof(WAVEHDR));
    }
    waveInStart(wi);
    while (n < soll && runden < 600) {
        WaitForSingleObject(ev, 100);
        runden += 1;
        for (k = 0; k < 4; k += 1) {
            if (!(hd[k].dwFlags & WHDR_DONE)) continue;
            if (hd[k].dwBytesRecorded > soll - n) hd[k].dwBytesRecorded = soll - n;
            memcpy(d + n, hd[k].lpData, hd[k].dwBytesRecorded);
            n += hd[k].dwBytesRecorded;
            hd[k].dwFlags &= ~WHDR_DONE;
            if (n < soll) waveInAddBuffer(wi, &hd[k], sizeof(WAVEHDR));
        }
    }
    mt.wType = TIME_BYTES;
    waveInGetPosition(wi, &mt, sizeof(mt));
    r = waveInReset(wi);
    printf("  %lu Byte, waveInGetPosition %lu, waveInReset %u\n", n, mt.u.cb, r);
    for (k = 0; k < 4; k += 1) waveInUnprepareHeader(wi, &hd[k], sizeof(WAVEHDR));
    r = waveInClose(wi);
    printf("  waveInClose %u\n", r);
    CloseHandle(ev);
    wav_schreiben(datei, &wf, d, n);
}

/*  Mixer wie sndvol32: alles ueber die winmm-Mixer-Schnittstelle.  */
static void
mixer (const char *was, int wert)
{
    UINT n = mixerGetNumDevs(), m;
    printf("  mixerGetNumDevs = %u\n", n);
    for (m = 0; m < n; m += 1) {
        MIXERCAPSA c;
        HMIXER hm;
        DWORD z, q;
        if (mixerGetDevCapsA(m, &c, sizeof(c)) != 0) continue;
        printf("  Mixer %u: \"%s\", %lu Ziele\n", m, c.szPname, c.cDestinations);
        if (mixerOpen(&hm, m, 0, 0, MIXER_OBJECTF_MIXER) != 0) { printf("    mixerOpen gescheitert\n"); continue; }
        for (z = 0; z < c.cDestinations; z += 1) {
            MIXERLINEA ml;
            memset(&ml, 0, sizeof(ml)); ml.cbStruct = sizeof(ml); ml.dwDestination = z;
            if (mixerGetLineInfoA((HMIXEROBJ)hm, &ml, MIXER_GETLINEINFOF_DESTINATION) != 0) continue;
            for (q = 0; q <= ml.cConnections; q += 1) {
                MIXERLINEA l = ml;
                MIXERLINECONTROLSA lc;
                MIXERCONTROLA ctl[4];
                DWORD k;
                if (q > 0) {
                    memset(&l, 0, sizeof(l)); l.cbStruct = sizeof(l); l.dwDestination = z; l.dwSource = q - 1;
                    if (mixerGetLineInfoA((HMIXEROBJ)hm, &l, MIXER_GETLINEINFOF_SOURCE) != 0) continue;
                }
                printf("    %s \"%s\" (Linie %08lx, Typ %04lx, %lu Kanaele, Ziel waveOut/In %lu), %lu Regler\n",
                       q ? "  Quelle" : "Ziel", l.szName, l.dwLineID, l.dwComponentType, l.cChannels,
                       l.Target.dwDeviceID, l.cControls);
                if (l.cControls == 0 || l.cControls > 4) continue;
                memset(&lc, 0, sizeof(lc)); lc.cbStruct = sizeof(lc); lc.dwLineID = l.dwLineID;
                lc.cControls = l.cControls; lc.cbmxctrl = sizeof(MIXERCONTROLA); lc.pamxctrl = ctl;
                if (mixerGetLineControlsA((HMIXEROBJ)hm, &lc, MIXER_GETLINECONTROLSF_ALL) != 0) continue;
                for (k = 0; k < l.cControls; k += 1) {
                    MIXERCONTROLDETAILS cd;
                    DWORD v[2] = { 0, 0 };
                    int ist_stumm = ctl[k].dwControlType == MIXERCONTROL_CONTROLTYPE_MUTE;
                    DWORD kan = (ctl[k].fdwControl & MIXERCONTROL_CONTROLF_UNIFORM) ? 1 : l.cChannels;
                    memset(&cd, 0, sizeof(cd)); cd.cbStruct = sizeof(cd); cd.dwControlID = ctl[k].dwControlID;
                    cd.cChannels = kan; cd.cbDetails = sizeof(DWORD); cd.paDetails = v;
                    if (z == 0 && q == 0 && was != 0 &&
                        ((ist_stumm && strcmp(was, "stumm") == 0) || (!ist_stumm && strcmp(was, "laut") == 0))) {
                        v[0] = v[1] = ist_stumm ? (DWORD)wert : (DWORD)wert * 65535ul / 100;
                        printf("        setze %s auf %lu: %u\n", ctl[k].szName, v[0],
                               mixerSetControlDetails((HMIXEROBJ)hm, &cd, MIXER_SETCONTROLDETAILSF_VALUE));
                        v[0] = v[1] = 0;
                    }
                    mixerGetControlDetails((HMIXEROBJ)hm, &cd, MIXER_GETCONTROLDETAILSF_VALUE);
                    printf("        Regler %lu \"%s\" %s%s: %lu", ctl[k].dwControlID, ctl[k].szName,
                           ist_stumm ? "(Ton aus)" : "(Lautstaerke 0..65535)",
                           kan == 1 ? ", einheitlich" : "", v[0]);
                    if (kan > 1) printf(" / %lu", v[1]);
                    printf("\n");
                }
            }
        }
        mixerClose(hm);
    }
}

static void
wm_ton (int hz, int sek, unsigned long rate, int bits, int kan, UINT geraet)
{
    static unsigned char d[48000 * 4 * 6];
    WAVEFORMATEX wf;
    unsigned long n, k, laenge;
    double phase = 0, schritt = 2 * 3.14159265358979 * hz / rate, pi = 3.14159265358979;
    int c;

    if (sek > 6) sek = 6;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = (WORD)kan; wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = (WORD)bits; wf.nBlockAlign = (WORD)(kan * bits / 8);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
    n = rate * (unsigned long)sek;
    laenge = n * wf.nBlockAlign;
    if (laenge > sizeof(d)) { n = sizeof(d) / wf.nBlockAlign; laenge = n * wf.nBlockAlign; }
    for (k = 0; k < n; k += 1) {
        double v = sinus(phase);
        for (c = 0; c < kan; c += 1) {
            if (bits == 8) d[k * kan + c] = (unsigned char)(128 + v * 60);
            else ((short *)d)[k * kan + c] = (short)(v * 8000);
        }
        phase += schritt;
        if (phase > pi) phase -= 2 * pi;
    }
    wm_spielen(geraet, &wf, d, laenge, "Sinus");
}

static int
gleich (const unsigned char *a, const char *b, int n)
{
    while (n-- > 0) if (*a++ != (unsigned char)*b++) return 0;
    return 1;
}

static void
wm_wav (const char *datei, UINT geraet)
{
    static unsigned char d[4 * 1024 * 1024];
    HANDLE h = CreateFileA(datei, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0, i = 12, l;
    WAVEFORMATEX wf;
    int ok = 0;

    if (h == INVALID_HANDLE_VALUE) { printf("  %s nicht zu oeffnen\n", datei); return; }
    ReadFile(h, d, sizeof(d), &n, NULL);
    CloseHandle(h);
    if (n < 44 || !gleich(d, "RIFF", 4) || !gleich(d + 8, "WAVE", 4)) {
        printf("  keine WAV-Datei\n"); return;
    }
    memset(&wf, 0, sizeof(wf));
    while (i + 8 <= n) {
        l = *(DWORD *)(d + i + 4);
        if (gleich(d + i, "fmt ", 4)) { memcpy(&wf, d + i + 8, 16); ok = 1; }
        if (gleich(d + i, "data", 4) && ok) {
            if (l > n - i - 8) l = n - i - 8;
            wm_spielen(geraet, &wf, d + i + 8, l - l % (wf.nBlockAlign ? wf.nBlockAlign : 1), datei);
            return;
        }
        i += 8 + l + (l & 1);
    }
    printf("  WAV ohne fmt/data\n");
}

int
main (int argc, char **argv)
{
    printf("\nusbton -- USB-Audio (UAC1) fuer FreeBSD-USB auf Windows NT 4.0\n\n");

    if (argc > 1 && strcmp(argv[1], "wmlaut") == 0) {
        /*  waveOut-Lautstaerke lesen, optional setzen -- zeigt, ob winmm und
         *  Mixer denselben Geraetewert sehen.  */
        HWAVEOUT ho;
        WAVEFORMATEX wf;
        DWORD v = 0;
        UINT n = waveOutGetNumDevs(), g = 0;
        memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 48000;
        wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 192000;
        if (n == 0) { printf("  kein waveOut-Geraet\n"); return 0; }
        if (waveOutOpen(&ho, g, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            printf("  waveOutOpen fehlgeschlagen\n"); return 0; }
        if (argc > 2) {
            DWORD p = (DWORD)atoi(argv[2]) * 65535ul / 100;
            DWORD w = (p & 0xFFFF) | (p << 16);
            printf("  waveOutSetVolume %lu%% (0x%08lx): %u\n", (unsigned long)atoi(argv[2]), w,
                   waveOutSetVolume(ho, w));
        }
        waveOutGetVolume(ho, &v);
        printf("  waveOutGetVolume: 0x%08lx  (links %lu%%, rechts %lu%%)\n",
               v, (v & 0xFFFF) * 100ul / 65535, (v >> 16) * 100ul / 65535);
        waveOutClose(ho);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "winmm") == 0) {
        wm_ton(argc > 2 ? atoi(argv[2]) : 440, argc > 3 ? atoi(argv[3]) : 3,
               argc > 4 ? (unsigned long)atoi(argv[4]) : 44100, argc > 5 ? atoi(argv[5]) : 16,
               argc > 6 ? atoi(argv[6]) : 2, argc > 7 ? (UINT)atoi(argv[7]) : WAVE_MAPPER);
        printf("\n");
        return 0;
    }
    if (argc > 6 && strcmp(argv[1], "wmaufnahme") == 0) {
        wm_aufnahme(atoi(argv[2]), (unsigned long)atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), argv[6]);
        printf("\n");
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "fassung") == 0)) {
        /*  Welche Fassungen laufen hier wirklich?  Ohne das raet man beim
         *  Suchen von Fehlern an der falschen Datei herum.  */
        static const char *dateien[] = {
            "usbaudio.dll", "drivers\\usbaudio.sys", "drivers\\usbnt.sys",
            "drivers\\uhcint.sys", "drivers\\ohcint.sys",
            "drivers\\ehcint.sys", "drivers\\xhcint.sys", 0
        };
        char sys[MAX_PATH], pfad[MAX_PATH];
        static char puffer[8192];       /* fest statt malloc (keine C-Laufzeit) */
        int i;
        GetSystemDirectoryA(sys, sizeof(sys));
        printf("  usbton selbst:  %s\n", FASSUNG_TEXT);
        for (i = 0; dateien[i]; i += 1) {
            DWORD d, groesse;
            void *puf;
            VS_FIXEDFILEINFO *fi = 0;
            UINT len = 0;
            wsprintfA(pfad, "%s\\%s", sys, dateien[i]);
            groesse = GetFileVersionInfoSizeA(pfad, &d);
            if (groesse == 0) { printf("  %-22s (nicht vorhanden)\n", dateien[i]); continue; }
            if (groesse > sizeof(puffer)) groesse = sizeof(puffer);
            puf = puffer;
            if (GetFileVersionInfoA(pfad, 0, groesse, puf)
                    && VerQueryValueA(puf, "\\", (void **)&fi, &len) && fi) {
                printf("  %-22s %u.%u.%u.%u\n", dateien[i],
                       HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                       HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            } else {
                printf("  %-22s (keine Versionsangabe)\n", dateien[i]);
            }
        }
        printf("\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "mixer") == 0) {
        mixer(argc > 3 ? argv[2] : 0, argc > 3 ? atoi(argv[3]) : 0);
        printf("\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "wmpause") == 0) {
        wm_pause_ms = 1000;
        wm_ton(1000, 4, 44100, 16, 2, WAVE_MAPPER);
        printf("\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "wmreset") == 0) {
        wm_abbruch_ms = 1000;
        wm_ton(1000, 4, 44100, 16, 2, WAVE_MAPPER);
        printf("\n");
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "wmlaut") == 0) {
        DWORD v = (DWORD)atoi(argv[2]) * 0xFFFF / 100, z = 0;
        MMRESULT r1 = waveOutSetVolume((HWAVEOUT)0, v | (v << 16));
        MMRESULT r2 = waveOutGetVolume((HWAVEOUT)0, &z);
        printf("  waveOutSetVolume(%s %%): %u, zurueckgelesen %08lx (%u)\n", argv[2], r1, z, r2);
        printf("\n");
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "wav") == 0) {
        wm_wav(argv[2], argc > 3 ? (UINT)atoi(argv[3]) : WAVE_MAPPER);
        printf("\n");
        return 0;
    }

    if (!uac_suchen(&g, 0)) {
        printf("  Kein USB-Audiogeraet gefunden (Schnittstelle Klasse 01/01).\n\n");
        return 1;
    }
    if (argc < 2 || strcmp(argv[1], "liste") == 0) {
        liste();
    } else if (strcmp(argv[1], "stumm") == 0 && argc > 2) {
        stumm(atoi(argv[2]), argc > 3 && strcmp(argv[3], "mikro") == 0);
    } else if (strcmp(argv[1], "aufnahme") == 0 && argc > 3) {
        aufnahme(atoi(argv[2]), argv[3]);
    } else if (strcmp(argv[1], "lautlesen") == 0) {
        lautlesen();
    } else if (strcmp(argv[1], "roh") == 0) {
        roh();
    } else if (strcmp(argv[1], "laut") == 0 && argc > 2) {
        laut(atoi(argv[2]), argc > 3 && strcmp(argv[3], "mikro") == 0);
    } else if (strcmp(argv[1], "ton") == 0) {
        if (g.uac != 1) { printf("  Nur UAC 1.0.\n"); return 1; }
        ton(argc > 2 ? atoi(argv[2]) : 440, argc > 3 ? atoi(argv[3]) : 5);
    } else if (strcmp(argv[1], "tonroh") == 0) {
        if (g.uac != 1) { printf("  Nur UAC 1.0.\n"); return 1; }
        tonroh(argc > 2 ? atoi(argv[2]) : 440, argc > 3 ? atoi(argv[3]) : 5,
               argc > 4 ? atoi(argv[4]) : 10);
    } else {
        printf("  usbton [liste | roh | lautlesen | stumm 0|1 [mikro] | laut DB [mikro] | ton [HZ] [S] | tonroh [HZ] [S] [R]\n"
               "         | winmm [HZ] [S] [RATE] [BITS] [KANAELE] [GERAET] | wav DATEI [GERAET]]\n");
    }
    printf("\n");
    return 0;
}
