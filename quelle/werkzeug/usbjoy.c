/*
 *  werkzeug/usbjoy.c -- Joystick-Treiber fuer winmm unter NT 4.0.
 *
 *  winmm.dll (joyGetPosEx, joyGetDevCaps, ...; darauf bauen die
 *  Systemsteuerung "Joystick" und DirectInput unter NT 4.0) laedt den
 *  Treiber, der unter
 *
 *      HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32
 *          joystick = usbjoy.dll
 *
 *  steht, und schickt ihm Nachrichten an DriverProc (mmddk.h).  Dieser
 *  Treiber weiss nichts von USB: er fragt beim Kerntreiber den AKTUELLEN
 *  Zustand ab (\\.\UsbJoyN, IOCTL_NTUJ_ZUSTAND) -- ein Kopieren, kein
 *  Warten.  winmm-Joystick 1 = \\.\UsbJoy0, 2 = \\.\UsbJoy1 -- mehr
 *  bedient NT 4.0s winmm nicht.
 *
 *  Achsen nach ihrer Art auf winmms sechs verteilt:
 *      X->X  Y->Y  Z->Z  Rz->R  Rx->U  Ry->V   (Schieber: Z oder R, was frei ist)
 *  Xbox 360: linker Stick X/Y, LT = Z, RT = R, rechter Stick U/V.
 *
 *  Protokoll: liegt C:\TEMP\usbjoy.log beim Laden, wird jede Nachricht
 *  dort angehaengt (zum Nachsehen, was winmm wirklich schickt).
 *
 *  Nur kernel32 und user32 (wsprintfA).  Kein CRT.
 */

#include <windows.h>
#include <mmsystem.h>
#include "../ober/ntujoy.h"

#define JDD_GETNUMDEVS       (DRV_RESERVED + 0x0001)
#define JDD_GETDEVCAPS       (DRV_RESERVED + 0x0002)
#define JDD_GETPOS           (DRV_RESERVED + 0x0101)
#define JDD_SETCALIBRATION   (DRV_RESERVED + 0x0102)
#define JDD_CONFIGCHANGED    (DRV_RESERVED + 0x0103)
#define JDD_GETPOSEX         (DRV_RESERVED + 0x0104)

#define USBJOY_MAX   16

/*  szRegKey in JOYCAPS: DirectInput 3 (NT 4.0) und joy.cpl setzen ihn in
 *      System\CurrentControlSet\Control\MediaResources\Joystick\%s\
 *          CurrentJoystickSettings
 *  ein und lesen dort Joystick<n>OEMName.  Er muss also GENAU der Name des
 *  Schluessels sein, unter dem USBINST die Einstellungen anlegt.  Vorher
 *  stand hier "USBJOY", die Einstellungen lagen aber unter "usbjoy.dll":
 *  DirectInput fand keinen OEM-Namen und listete den Controller nicht.
 *  (In DINPUT.DLL und joy.cpl von NT 4.0 SP6a nachgesehen.)  */

#define REGSCHLUESSEL  "usbjoy.dll"

static HANDLE geraet[USBJOY_MAX];
static HANDLE logdatei = INVALID_HANDLE_VALUE;
static LONG   geloggt;


/* ---------------------------------------------------------------- */

static void
logzeile (const char *fmt, DWORD a, DWORD b, DWORD c, DWORD d)
{
    char  z[200];
    DWORD n;

    if (logdatei == INVALID_HANDLE_VALUE || geloggt > 400) {
        return;
    }
    geloggt += 1;
    n = (DWORD)wsprintfA(z, fmt, a, b, c, d);
    WriteFile(logdatei, z, n, &n, NULL);
}

static void *
nullen (void *p, DWORD n)
{
    char *q = (char *)p;
    while (n--) *q++ = 0;
    return p;
}


/* ---------------------------------------------------------------- *
 *  Kerntreiber abfragen
 * ---------------------------------------------------------------- */

static HANDLE
oeffne (DWORD id)
{
    char name[20];

    if (id >= USBJOY_MAX) {
        return INVALID_HANDLE_VALUE;
    }

    if (geraet[id] != 0 && geraet[id] != INVALID_HANDLE_VALUE) {
        return geraet[id];
    }

    wsprintfA(name, "\\\\.\\UsbJoy%lu", id);
    geraet[id] = CreateFileA(name, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
    return geraet[id];
}

static void
schliesse (DWORD id)
{
    if (id < USBJOY_MAX && geraet[id] != 0 && geraet[id] != INVALID_HANDLE_VALUE) {
        CloseHandle(geraet[id]);
    }
    if (id < USBJOY_MAX) {
        geraet[id] = 0;
    }
}

/*  Einmal neu oeffnen, wenn es scheitert: der Controller kann
 *  abgezogen und wieder angesteckt worden sein -- dann gehoert der alte
 *  Griff zu einem Geraet, das es nicht mehr gibt.  */

static int
frage (DWORD id, DWORD code, void *aus, DWORD n)
{
    int    k;
    DWORD  got;
    HANDLE h;

    for (k = 0; k < 2; k += 1) {
        h = oeffne(id);
        if (h == INVALID_HANDLE_VALUE) {
            geraet[id] = 0;
            return 0;
        }
        if (DeviceIoControl(h, code, NULL, 0, aus, n, &got, NULL) && got == n) {
            return 1;
        }
        schliesse(id);
    }
    return 0;
}


/* ---------------------------------------------------------------- *
 *  Achsen verteilen
 * ---------------------------------------------------------------- */

/*  platz[0..5] = X Y Z R U V; Inhalt = Index in Info/Zustand, -1 = leer  */

static void
verteilen (const NTUJ_INFO *i, int platz[6])
{
    unsigned long k;
    int           schieber = -1;

    for (k = 0; k < 6; k += 1) platz[k] = -1;

    for (k = 0; k < i->AnzahlAchsen && k < NTUJ_ACHSEN; k += 1) {
        switch (i->AchsenArt[k]) {
            case NTUJ_X:  if (platz[0] < 0) platz[0] = (int)k; break;
            case NTUJ_Y:  if (platz[1] < 0) platz[1] = (int)k; break;
            case NTUJ_Z:  if (platz[2] < 0) platz[2] = (int)k; break;
            case NTUJ_RZ: if (platz[3] < 0) platz[3] = (int)k; break;
            case NTUJ_RX: if (platz[4] < 0) platz[4] = (int)k; break;
            case NTUJ_RY: if (platz[5] < 0) platz[5] = (int)k; break;
            case NTUJ_SLIDER: case NTUJ_DIAL: case NTUJ_WHEEL:
                if (schieber < 0) schieber = (int)k;
                break;
        }
    }

    if (schieber >= 0) {
        if (platz[2] < 0)      platz[2] = schieber;
        else if (platz[3] < 0) platz[3] = schieber;
    }
}


/* ---------------------------------------------------------------- *
 *  Win16-Spiele: Fehler in winmm ausgleichen
 *
 *  Win16-Programme rufen unter WOW das 16-Bit-joyGetDevCaps; das landet
 *  in winmm!joy32Message.  Das holt die 32-Bit-JOYCAPSA und baut daraus
 *  die 16-Bit-JOYCAPS (54 Byte) -- legt dabei aber szPname und ALLE
 *  folgenden Felder um 2 Byte zu weit hinten ab (lea -0x5a(%ebp) statt
 *  -0x5c; in winmm.dll 4.00 von NT 4.0 SP6a disassembliert).  Das
 *  Win16-Programm liest dann:
 *      wXmin  <- szPname[30..31]      wXmax <- wXmin
 *      wYmin  <- wXmax                wYmax <- wYmin
 *      wZmin  <- wYmax                wZmax <- wZmin
 *      wNumButtons <- wZmax           wPeriodMin <- wNumButtons
 *      wPeriodMax  <- wPeriodMin
 *  Gemessen: wNumButtons = 0xFFFF, als Vorzeichenzahl -1 -- SinkSub Pro:
 *  "Joystick must support at least 2 buttons".  Das trifft JEDEN
 *  Joystick-Treiber, nicht nur diesen.
 *
 *  Ausgleich: in einem WOW-Prozess die Felder um eins versetzt liefern,
 *  damit nach winmms Verschiebung das Richtige ankommt.  Nur wenn
 *  joy32Message die fehlerhafte Anweisung wirklich enthaelt -- eine
 *  berichtigte winmm wird nicht verbogen.  Die beiden Bytes vor dem Namen
 *  (Stapelreste in winmm) lassen sich nicht beeinflussen; der Name ist
 *  im Win16-Programm deshalb um zwei Zeichen verschoben, die Zahlen
 *  stimmen.  joyGetPos ist nicht betroffen.
 * ---------------------------------------------------------------- */

static int wow_versatz = -1;           /* -1 = noch nicht geprueft */

static int
wow_braucht_ausgleich (void)
{
    static const unsigned char muster[7] = { 0x8d, 0x7d, 0xa6, 0x66, 0x89, 0x4d, 0xa0 };
    const unsigned char *code;
    HMODULE w;
    int     k, j;

    if (wow_versatz >= 0) {
        return wow_versatz;
    }
    wow_versatz = 0;

    if (GetModuleHandleA("WOW32.DLL") == NULL) {
        return 0;                       /* kein Win16-Prozess */
    }
    w = GetModuleHandleA("WINMM.DLL");
    code = w ? (const unsigned char *)GetProcAddress(w, "joy32Message") : NULL;
    if (code == NULL || IsBadReadPtr(code, 0x100)) {
        return 0;
    }
    for (k = 0; k + 7 <= 0x100; k += 1) {
        for (j = 0; j < 7 && code[k + j] == muster[j]; j += 1) ;
        if (j == 7) {
            wow_versatz = 1;
            logzeile("winmm!joy32Message: 16-Bit-JOYCAPS-Versatz erkannt (+%lu), wird ausgeglichen\r\n",
                     (DWORD)k, 0, 0, 0);
            break;
        }
    }
    return wow_versatz;
}

/* ---------------------------------------------------------------- *
 *  Nachrichten
 * ---------------------------------------------------------------- */

static LRESULT
faehigkeiten (DWORD id, void *p, DWORD groesse)
{
    NTUJ_INFO i;
    int       platz[6], k, achsen = 0;
    UINT      caps = 0;

    if (!frage(id, IOCTL_NTUJ_INFO, &i, sizeof(i))) {
        return JOYERR_UNPLUGGED;
    }

    verteilen(&i, platz);
    for (k = 0; k < 6; k += 1) {
        if (platz[k] >= 0) achsen += 1;
    }
    if (platz[2] >= 0) caps |= JOYCAPS_HASZ;
    if (platz[3] >= 0) caps |= JOYCAPS_HASR;
    if (platz[4] >= 0) caps |= JOYCAPS_HASU;
    if (platz[5] >= 0) caps |= JOYCAPS_HASV;
    if (i.HatPov)      caps |= JOYCAPS_HASPOV | JOYCAPS_POVCTS;

    /*  ANSI oder Unicode -- winmm sagt es nur durch die Groesse.  */

    if (groesse >= sizeof(JOYCAPSW)) {

        JOYCAPSW *c = (JOYCAPSW *)p;

        nullen(c, sizeof(*c));
        c->wMid = 1; c->wPid = 1;
        for (k = 0; i.Name[k] != '\0' && k < MAXPNAMELEN - 1; k += 1) {
            c->szPname[k] = (WCHAR)(unsigned char)i.Name[k];
        }
        c->wXmax = c->wYmax = c->wZmax = c->wRmax = c->wUmax = c->wVmax = 65535;
        c->wNumButtons = (i.AnzahlKnoepfe > 32) ? 32 : (UINT)i.AnzahlKnoepfe;
        c->wMaxButtons = 32;
        c->wPeriodMin  = 10;
        c->wPeriodMax  = 1000;
        c->wMaxAxes    = 6;
        c->wNumAxes    = (UINT)achsen;
        c->wCaps       = caps;
        for (k = 0; REGSCHLUESSEL[k] != '\0'; k += 1) {
            c->szRegKey[k] = (WCHAR)REGSCHLUESSEL[k];
        }

        if (wow_braucht_ausgleich()) {
            /*  Siehe oben: um ein Feld versetzt, damit das Win16-
             *  Programm nach winmms Verschiebung das Richtige liest.
             *  szPname[30..31] wird zu wXmin -- also Null.  */
            c->szPname[30] = c->szPname[31] = 0;
            c->wXmin = 65535;                   /* -> wXmax      */
            c->wXmax = 0;                       /* -> wYmin      */
            c->wYmin = 65535;                   /* -> wYmax      */
            c->wYmax = 0;                       /* -> wZmin      */
            c->wZmin = 65535;                   /* -> wZmax      */
            c->wZmax = c->wNumButtons;          /* -> wNumButtons */
            c->wNumButtons = c->wPeriodMin;     /* -> wPeriodMin */
            c->wPeriodMin  = c->wPeriodMax;     /* -> wPeriodMax */
        }

    } else if (groesse >= sizeof(JOYCAPSA)) {

        JOYCAPSA *c = (JOYCAPSA *)p;

        nullen(c, sizeof(*c));
        c->wMid = 1; c->wPid = 1;
        for (k = 0; i.Name[k] != '\0' && k < MAXPNAMELEN - 1; k += 1) {
            c->szPname[k] = i.Name[k];
        }
        c->wXmax = c->wYmax = c->wZmax = c->wRmax = c->wUmax = c->wVmax = 65535;
        c->wNumButtons = (i.AnzahlKnoepfe > 32) ? 32 : (UINT)i.AnzahlKnoepfe;
        c->wMaxButtons = 32;
        c->wPeriodMin  = 10;
        c->wPeriodMax  = 1000;
        c->wMaxAxes    = 6;
        c->wNumAxes    = (UINT)achsen;
        c->wCaps       = caps;
        lstrcpyA(c->szRegKey, REGSCHLUESSEL);

    } else if (groesse >= 54) {

        /*  16-Bit-JOYCAPS (Windows 3.1, 54 Byte).  Win16-Programme rufen
         *  ueber WOW das 16-Bit-joyGetDevCaps auf; dessen Struktur endet
         *  nach wPeriodMax -- ohne wNumAxes, wCaps und szRegKey.  Wer
         *  hier JOYERR_PARMS liefert, laesst solche Spiele mit
         *  "Joystick must support at least 2 buttons" scheitern
         *  (gemessen mit SinkSub Pro).  Felder sind WORD, nicht UINT.
         *
         *    WORD wMid, wPid;  char szPname[32];
         *    WORD wXmin,wXmax,wYmin,wYmax,wZmin,wZmax;
         *    WORD wNumButtons, wPeriodMin, wPeriodMax;                   */

        unsigned char *b = (unsigned char *)p;
        WORD          *w;
        UINT           knoepfe = (i.AnzahlKnoepfe > 32) ? 32 : (UINT)i.AnzahlKnoepfe;

        nullen(p, 54);
        w = (WORD *)b;
        w[0] = 1;                       /* wMid */
        w[1] = 1;                       /* wPid */
        for (k = 0; i.Name[k] != '\0' && k < 31; k += 1) {
            b[4 + k] = (unsigned char)i.Name[k];
        }
        b[4 + k] = '\0';
        w = (WORD *)(b + 36);           /* nach szPname[32] */
        w[0] = 0;     w[1] = 65535;     /* wXmin, wXmax */
        w[2] = 0;     w[3] = 65535;     /* wYmin, wYmax */
        w[4] = 0;     w[5] = 65535;     /* wZmin, wZmax */
        w[6] = (WORD)knoepfe;           /* wNumButtons */
        w[7] = 10;                      /* wPeriodMin  */
        w[8] = 1000;                    /* wPeriodMax  */

    } else {
        return JOYERR_PARMS;
    }

    return JOYERR_NOERROR;
}

static LRESULT
position (DWORD id, JOYINFOEX *j)
{
    NTUJ_INFO    i;
    NTUJ_ZUSTAND z;
    int          platz[6], k;
    DWORD        wert[6];

    if (!frage(id, IOCTL_NTUJ_INFO, &i, sizeof(i)) ||
        !frage(id, IOCTL_NTUJ_ZUSTAND, &z, sizeof(z))) {
        return JOYERR_UNPLUGGED;
    }

    verteilen(&i, platz);
    for (k = 0; k < 6; k += 1) {
        wert[k] = (platz[k] >= 0) ? z.Achse[platz[k]] : 32768;
    }

    /*  Vor dem ersten Bericht (Zaehler 0) haelt der Kerntreiber alle Achsen
     *  in der Mitte -- fuer Xbox-Trigger (Z, Rz) ist die Ruhelage aber 0.
     *  Ohne das sah ein Spiel nach dem Einstecken kurz "Trigger halb
     *  gedrueckt" (MM_JOY1ZMOVE auf 32768, im Pruefstand gemessen); ein
     *  Xbox One schweigt bis zum Einschalten, dort dauerte es laenger.  */

    if (z.Zaehler == 0 && i.Xbox != 0) {
        for (k = 0; k < 6; k += 1) {
            if (platz[k] >= 0 && (i.AchsenArt[platz[k]] == NTUJ_Z ||
                                  i.AchsenArt[platz[k]] == NTUJ_RZ)) {
                wert[k] = 0;
            }
        }
    }

    j->dwXpos    = wert[0];
    j->dwYpos    = wert[1];
    j->dwZpos    = wert[2];
    j->dwRpos    = wert[3];
    j->dwUpos    = wert[4];
    j->dwVpos    = wert[5];
    j->dwButtons = z.Knoepfe;
    j->dwPOV     = i.HatPov ? z.Pov : JOY_POVCENTERED;

    /*  Nummer des niedrigsten gedrueckten Knopfs, 0 = keiner.  */

    j->dwButtonNumber = 0;
    for (k = 0; k < 32; k += 1) {
        if (z.Knoepfe & (1ul << k)) {
            j->dwButtonNumber = (DWORD)k + 1;
            break;
        }
    }

    return JOYERR_NOERROR;
}

LRESULT CALLBACK
DriverProc (DWORD dwId, HDRVR hdrv, UINT msg, LPARAM p1, LPARAM p2)
{
    /*  Welcher Joystick?  Oeffnet winmm den Treiber je Joystick, liefert
     *  DRV_OPEN in p2 dessen Nummer, und wir geben Nummer+1 als
     *  Treiberkennung zurueck; sie kommt in dwId wieder.  */

    DWORD id = (dwId > 0 && dwId <= USBJOY_MAX) ? dwId - 1 : 0;

    if (msg != JDD_GETPOSEX || geloggt < 40) {
        logzeile("DriverProc id=%lu msg=%04lx p1=%08lx p2=%08lx\r\n",
                 dwId, msg, (DWORD)p1, (DWORD)p2);
    }

    switch (msg) {

        case DRV_LOAD:
        case DRV_FREE:
        case DRV_ENABLE:
        case DRV_DISABLE:
        case DRV_INSTALL:
        case DRV_REMOVE:
            return 1;

        case DRV_OPEN:
            return (LRESULT)(((DWORD)p2 < USBJOY_MAX) ? (DWORD)p2 + 1 : 1);

        case DRV_CLOSE:
            schliesse(id);
            return 1;

        case DRV_QUERYCONFIGURE:
            return 0;

        case JDD_GETNUMDEVS:

            /*  NT 4.0s winmm bedient ZWEI Joysticks: es oeffnet den
             *  Treiber genau zweimal (DRV_OPEN mit 0 und 1) und ruft ihn fuer
             *  hoehere Nummern nie -- joyGetDevCaps meldet dort "kein Fehler"
             *  und laesst die Struktur leer.  Im Protokoll nachgesehen.  Also
             *  nicht mehr versprechen: Joystick 1 = UsbJoy0, 2 = UsbJoy1.  */

            return 2;

        case JDD_GETDEVCAPS:
            return faehigkeiten(id, (void *)p1, (DWORD)p2);

        case JDD_GETPOSEX:
            return position(id, (JOYINFOEX *)p1);

        case JDD_GETPOS: {

            /*  JOYINFO gibt es in zwei Breiten: 32 Bit mit vier UINT
             *  (16 Byte) und 16 Bit mit vier WORD (8 Byte).  Win16-
             *  Programme kommen ueber WOW mit der kleinen Fassung --
             *  wer blind die grosse schreibt, zerstoert fremden Speicher
             *  und liefert Unsinn.  p2 traegt die Groesse.
             *  Knoepfe: alle 32 durchreichen, nicht nur die ersten vier
             *  (& 0x0F liess Knopf 5 aufwaerts verschwinden).           */

            JOYINFOEX x;
            LRESULT   r;
            DWORD     gr = (DWORD)p2;

            nullen(&x, sizeof(x));
            x.dwSize  = sizeof(x);
            x.dwFlags = JOY_RETURNALL;
            r = position(id, &x);
            if (r != JOYERR_NOERROR) return r;

            if (gr == 0 || gr >= sizeof(JOYINFO)) {
                JOYINFO *j = (JOYINFO *)p1;
                j->wXpos    = (UINT)x.dwXpos;
                j->wYpos    = (UINT)x.dwYpos;
                j->wZpos    = (UINT)x.dwZpos;
                j->wButtons = (UINT)x.dwButtons;
            } else {
                WORD *w = (WORD *)p1;        /* 16-Bit-JOYINFO, 8 Byte */
                w[0] = (WORD)x.dwXpos;
                w[1] = (WORD)x.dwYpos;
                w[2] = (WORD)x.dwZpos;
                w[3] = (WORD)x.dwButtons;
            }
            return JOYERR_NOERROR;
        }

        case JDD_SETCALIBRATION:
        case JDD_CONFIGCHANGED:
            return JOYERR_NOERROR;
    }

    return DefDriverProc(dwId, hdrv, msg, p1, p2);
}

BOOL WINAPI
DllEntry (HINSTANCE h, DWORD grund, LPVOID r)
{
    if (grund == DLL_PROCESS_ATTACH) {
        logdatei = CreateFileA("C:\\TEMP\\usbjoy.log", GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (logdatei != INVALID_HANDLE_VALUE) {
            SetFilePointer(logdatei, 0, NULL, FILE_END);
            logzeile("usbjoy.dll geladen (Prozess %lu)\r\n",
                     GetCurrentProcessId(), 0, 0, 0);
        }
    } else if (grund == DLL_PROCESS_DETACH) {
        DWORD k;
        for (k = 0; k < USBJOY_MAX; k += 1) {
            schliesse(k);
        }
        if (logdatei != INVALID_HANDLE_VALUE) {
            CloseHandle(logdatei);
        }
    }
    return TRUE;
}
