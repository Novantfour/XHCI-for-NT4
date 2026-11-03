/*
 *  werkzeug16/wave16.c -- Wiedergabe aus Sicht eines Win16-Programms.
 *
 *  Win16-Programme laufen unter NT 4.0 in WOW; ihre waveOut-Aufrufe gehen
 *  ueber die 16-Bit-MMSYSTEM.DLL und winmm (joy32Message-Gegenstueck fuer
 *  Wave) an usbaudio.dll.  Beim Joystick hatte winmm genau auf diesem Weg
 *  einen eigenen Fehler (verschobene 16-Bit-Struktur) -- deshalb pruefen.
 *
 *      WAVE16            Geraete, Faehigkeiten, 2 s Sinus 1 kHz in 22 kHz
 *                        8 Bit Mono (typisch fuer Win16-Spiele), Position,
 *                        Lautstaerke; Ergebnis nach C:\TEMP\WAVE16.TXT
 *
 *  Bauen (Open Watcom):  wcl -bt=windows -l=windows wave16.c mmsystem.lib
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

static HFILE f;
static void zeile (const char *s) { _lwrite(f, s, strlen(s)); _lwrite(f, "\r\n", 2); }

#define RATE    22050
#define DAUER   2
#define NPUF    4

static unsigned char huge *daten;

static int sinus8 (long k)                    /* 1 kHz, 8 Bit, ohne Gleitkomma-Bibliothek */
{
    /* Tabelle einer Periode bei 22050 Hz: 22.05 Abtastungen -> Phase in 1/1000 */
    static const signed char t[32] = {
        0, 12, 24, 35, 45, 53, 59, 62, 63, 62, 59, 53, 45, 35, 24, 12,
        0,-12,-24,-35,-45,-53,-59,-62,-63,-62,-59,-53,-45,-35,-24,-12 };
    long ph = (k * 1000L * 32L / RATE) & 31;  /* 1000 Hz */
    return 128 + t[ph];
}

int PASCAL WinMain (HINSTANCE hi, HINSTANCE hv, LPSTR cmd, int show)
{
    WAVEOUTCAPS   c;
    PCMWAVEFORMAT wf;
    HWAVEOUT      wo;
    WAVEHDR       hd[NPUF];
    MMTIME        mt;
    UINT          n, r, k;
    DWORD         v, lang = (DWORD)RATE * DAUER, stueck = RATE / 5, pos = 0, start;
    char          z[200];
    int           frei;

    f = _lcreat("C:\\TEMP\\WAVE16.TXT", 0);
    if (f == HFILE_ERROR) return 1;

    n = waveOutGetNumDevs();
    sprintf(z, "Win16: waveOutGetNumDevs = %u, sizeof(WAVEOUTCAPS) = %u", n, sizeof(c));
    zeile(z);
    for (k = 0; k < n; k++) {
        memset(&c, 0, sizeof(c));
        r = waveOutGetDevCaps(k, &c, sizeof(c));
        sprintf(z, "Geraet %u: GetDevCaps -> %u  \"%s\"  Formate %08lx  Kanaele %u  Support %08lx",
                k, r, c.szPname, c.dwFormats, c.wChannels, c.dwSupport);
        zeile(z);
    }

    daten = (unsigned char huge *)GlobalLock(GlobalAlloc(GMEM_MOVEABLE, lang));
    for (v = 0; v < lang; v++) daten[v] = (unsigned char)sinus8((long)v);

    memset(&wf, 0, sizeof(wf));
    wf.wf.wFormatTag = WAVE_FORMAT_PCM; wf.wf.nChannels = 1; wf.wf.nSamplesPerSec = RATE;
    wf.wf.nAvgBytesPerSec = RATE; wf.wf.nBlockAlign = 1; wf.wBitsPerSample = 8;
    r = waveOutOpen(&wo, WAVE_MAPPER, (LPWAVEFORMAT)&wf, 0, 0, 0);
    sprintf(z, "waveOutOpen(22050 Hz, 8 Bit, Mono) -> %u", r);
    zeile(z);
    if (r != 0) { _lclose(f); return 0; }

    r = waveOutSetVolume(0, 0xFFFFFFFFUL);
    waveOutGetVolume(0, &v);
    sprintf(z, "waveOutSetVolume(100 %%) -> %u, GetVolume = %08lx", r, v);
    zeile(z);

    memset(hd, 0, sizeof(hd));
    start = GetTickCount();
    while (pos < lang) {                       /* Puffer kreisen lassen, abfragen statt Rueckruf */
        frei = -1;
        for (k = 0; k < NPUF; k++) {
            if (hd[k].dwFlags & WHDR_PREPARED) {
                if (hd[k].dwFlags & WHDR_DONE) {
                    waveOutUnprepareHeader(wo, &hd[k], sizeof(WAVEHDR));
                    hd[k].dwFlags = 0;
                }
            }
            if (!(hd[k].dwFlags & WHDR_PREPARED) && frei < 0) frei = k;
        }
        if (frei < 0) { Yield(); continue; }
        hd[frei].lpData = (LPSTR)(daten + pos);
        hd[frei].dwBufferLength = (lang - pos < stueck) ? lang - pos : stueck;
        pos += hd[frei].dwBufferLength;
        waveOutPrepareHeader(wo, &hd[frei], sizeof(WAVEHDR));
        r = waveOutWrite(wo, &hd[frei], sizeof(WAVEHDR));
        if (r != 0) { sprintf(z, "waveOutWrite -> %u", r); zeile(z); break; }
    }
    for (;;) {                                 /* bis alle fertig */
        int offen = 0;
        for (k = 0; k < NPUF; k++) if ((hd[k].dwFlags & WHDR_PREPARED) && !(hd[k].dwFlags & WHDR_DONE)) offen++;
        if (!offen || GetTickCount() - start > 10000) break;
        Yield();
    }
    mt.wType = TIME_BYTES;
    waveOutGetPosition(wo, &mt, sizeof(mt));
    sprintf(z, "fertig nach %lu ms, waveOutGetPosition = %lu Byte von %lu", GetTickCount() - start, mt.u.cb, lang);
    zeile(z);
    for (k = 0; k < NPUF; k++) if (hd[k].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hd[k], sizeof(WAVEHDR));
    r = waveOutClose(wo);
    sprintf(z, "waveOutClose -> %u", r);
    zeile(z);
    _lclose(f);
    return 0;
}
