/*
 *  werkzeug/usbtasten.c -- USBTASTEN.EXE: fuehrt aus, was der Treiber bei
 *  Sondertasten selbst nicht kann (Belegungen "lauter", "leiser",
 *  "stumm", "programm:<befehl>").
 *
 *  Sichtbar: Symbol im Infobereich der Taskleiste (Kurzinfo, Menue mit
 *  "Beenden").  Frueher lief das Programm unsichtbar -- "unsichtbar +
 *  Autostart + startet Programme auf Tastendruck" ist fuer Virenscanner
 *  das Muster eines Keyloggers.  Jetzt ist es ein gewoehnliches
 *  Hilfsprogramm mit Symbol, wie es Tastatur- und Maustreiber mitbringen.
 *
 *  Ein Faden wartet auf das Ereignis "UsbNtSondertasten", holt die
 *  Auftraege ab (IOCTL_NTU_SONDER_ABHOLEN) und fuehrt sie aus:
 *    lauter/leiser  Gesamtlautstaerke (Mixer), in Schritten von 1/16;
 *                   ohne Mixer waveOutSetVolume
 *    stumm          Ton aus/an (Mixer: Stumm)
 *    programm:X     CreateProcess(X), sonst ShellExecute (Dokumente, Adressen)
 *  Ist der Treiber noch nicht da, alle 5 s ein neuer Versuch.
 *
 *  Ohne C-Laufzeit (eigener Einstieg ruft WinMain): der MinGW-w64-Startcode
 *  haengt unter echtem NT 4.0 SP6a (gemessen).  kernel32, user32, winmm,
 *  shell32.
 */

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <winioctl.h>
#include "../ober/ntusbioctl.h"
#include "auswurf.h"

/*  Der Uebersetzer erzeugt memset-Aufrufe auch ohne Quelltext dafuer.  */
void *memset (void *z, int c, unsigned int n) { unsigned char *p = z; while (n--) *p++ = (unsigned char)c; return z; }

#define WM_SYMBOL      (WM_APP + 1)
#define ID_BEENDEN     100
#define ID_INFO        101
#define ID_AUSWURF     200            /* + 0..25: Laufwerk A..Z */
#define ID_KEIN_USB    230
/*  NT 4.0 kennt NOTIFYICONDATA nur in der ersten Fassung (88 Byte);
    mit groesserem cbSize zeigt die Shell von NT 4.0 kein Symbol.  */
#define NID_V1_GROESSE 88

static HINSTANCE gInst;
static HWND      gFenster;
static UINT      gTaskleisteNeu;

/* ---------------- Lautstaerke ---------------- */

static BOOL
regler (DWORD typ, HMIXER *m, MIXERCONTROLA *c)
{
    MIXERLINEA         l;
    MIXERLINECONTROLSA lc;

    if (mixerGetNumDevs() == 0 || mixerOpen(m, 0, 0, 0, MIXER_OBJECTF_MIXER) != MMSYSERR_NOERROR) {
        return FALSE;
    }
    ZeroMemory(&l, sizeof(l));
    l.cbStruct        = sizeof(l);
    l.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    if (mixerGetLineInfoA((HMIXEROBJ)*m, &l, MIXER_GETLINEINFOF_COMPONENTTYPE) != MMSYSERR_NOERROR) {
        mixerClose(*m);
        return FALSE;
    }
    ZeroMemory(&lc, sizeof(lc));
    ZeroMemory(c, sizeof(*c));
    lc.cbStruct      = sizeof(lc);
    lc.dwLineID      = l.dwLineID;
    lc.dwControlType = typ;
    lc.cControls     = 1;
    lc.cbmxctrl      = sizeof(*c);
    lc.pamxctrl      = c;
    if (mixerGetLineControlsA((HMIXEROBJ)*m, &lc, MIXER_GETLINECONTROLSF_ONEBYTYPE) != MMSYSERR_NOERROR) {
        mixerClose(*m);
        return FALSE;
    }
    return TRUE;
}

static void
lautstaerke (int richtung)
{
    HMIXER                        m;
    MIXERCONTROLA                 c;
    MIXERCONTROLDETAILS           d;
    MIXERCONTROLDETAILS_UNSIGNED  w;
    DWORD                         schritt, v;

    if (!regler(MIXERCONTROL_CONTROLTYPE_VOLUME, &m, &c)) {
        DWORD alt = 0;                                   /* ohne Mixer */
        waveOutGetVolume((HWAVEOUT)0, &alt);
        v = alt & 0xFFFF;
        v = richtung > 0 ? (v + 0x1000 > 0xFFFF ? 0xFFFF : v + 0x1000)
                         : (v < 0x1000 ? 0 : v - 0x1000);
        waveOutSetVolume((HWAVEOUT)0, v | (v << 16));
        return;
    }
    ZeroMemory(&d, sizeof(d));
    d.cbStruct       = sizeof(d);
    d.dwControlID    = c.dwControlID;
    d.cChannels      = 1;
    d.cbDetails      = sizeof(w);
    d.paDetails      = &w;
    if (mixerGetControlDetailsA((HMIXEROBJ)m, &d, MIXER_GETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR) {
        schritt = (c.Bounds.dwMaximum - c.Bounds.dwMinimum) / 16;
        v = w.dwValue;
        if (richtung > 0) v = (v + schritt > c.Bounds.dwMaximum) ? c.Bounds.dwMaximum : v + schritt;
        else              v = (v < c.Bounds.dwMinimum + schritt) ? c.Bounds.dwMinimum : v - schritt;
        w.dwValue = v;
        mixerSetControlDetails((HMIXEROBJ)m, &d, MIXER_SETCONTROLDETAILSF_VALUE);
    }
    mixerClose(m);
}

static void
stumm (void)
{
    HMIXER                        m;
    MIXERCONTROLA                 c;
    MIXERCONTROLDETAILS           d;
    MIXERCONTROLDETAILS_BOOLEAN   b;

    if (!regler(MIXERCONTROL_CONTROLTYPE_MUTE, &m, &c)) {
        return;
    }
    ZeroMemory(&d, sizeof(d));
    d.cbStruct    = sizeof(d);
    d.dwControlID = c.dwControlID;
    d.cChannels   = 1;
    d.cbDetails   = sizeof(b);
    d.paDetails   = &b;
    if (mixerGetControlDetailsA((HMIXEROBJ)m, &d, MIXER_GETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR) {
        b.fValue = !b.fValue;
        mixerSetControlDetails((HMIXEROBJ)m, &d, MIXER_SETCONTROLDETAILSF_VALUE);
    }
    mixerClose(m);
}

/* ---------------- Programme ---------------- */

static void
programm (const char *befehl)
{
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    char                z[NTU_SONDER_TEXT];

    lstrcpynA(z, befehl, sizeof(z));
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessA(NULL, z, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }
    /*  Dokument, Adresse, Verknuepfung: dem Explorer ueberlassen.  */
    ShellExecuteA(gFenster, "open", befehl, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------------- Sicher entfernen ---------------- */

/*  Eigener Faden je Meldefenster: weder das Abholen weiterer Auftraege
 *  (Lautstaerketasten) noch das Menue sollen warten, bis jemand OK
 *  drueckt.  */
static DWORD WINAPI
meldung_faden (LPVOID p)
{
    char *t = (char *)p;
    UINT  art = (t[0] == '!') ? MB_ICONEXCLAMATION : MB_ICONINFORMATION;

    MessageBoxA(NULL, t + 1, "USB-Ger\xe4t sicher entfernen",
                MB_OK | art | MB_SETFOREGROUND | MB_TOPMOST);
    HeapFree(GetProcessHeap(), 0, t);
    return 0;
}

/*  art: 'i' Hinweis, '!' Warnung.  */
static void
meldung_zeigen (char art, const char *text)
{
    int   n = lstrlenA(text);
    char *t = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n + 2);
    DWORD tid;

    if (t == NULL) {
        return;
    }
    t[0] = art;
    lstrcpyA(t + 1, text);
    CloseHandle(CreateThread(NULL, 0, meldung_faden, t, 0, &tid));
}

/*  Vom Treiber: "ausgeworfen:<Bu>|<Name>|<0 sicher, 1 eingebunden>|<CD>".
 *  Der Treiber schickt das erst, wenn alle Lese- und Schreibauftraege
 *  fertig sind, der Stick seinen Puffer ausgeschrieben hat und kein
 *  Befehl mehr an ihn geht.  */
static void
ausgeworfen (const char *p)
{
    char        bu = p[0], name[64], t[400];
    const char *q;
    int         i, warn = 0, cd = 0;

    q = p;
    while (*q && *q != '|') q++;
    if (*q == '|') q++;
    for (i = 0; *q && *q != '|' && i < (int)sizeof(name) - 1; q++) {
        /*  SCSI-Hersteller und -Produkt sind mit Leerzeichen aufgefuellt
         *  ("QEMU    QEMU HARDDISK"): mehrere zu einem.  */
        if (*q == ' ' && (i == 0 || name[i - 1] == ' ')) {
            continue;
        }
        name[i++] = *q;
    }
    name[i] = '\0';
    while (i > 0 && name[i - 1] == ' ') {
        name[--i] = '\0';                  /* SCSI-Namen sind aufgefuellt */
    }
    if (*q == '|') { q++; warn = (*q == '1'); while (*q && *q != '|') q++; }
    if (*q == '|') { q++; cd = (*q == '1'); }

    if (warn) {
        wsprintfA(t, "Das Laufwerk %c: (%s) ist angehalten, aber das Dateisystem "
                     "war nicht abgemeldet: ein Programm hatte es noch offen, oder es "
                     "wurde ohne Abmelden ausgeworfen.\r\n\r\n"
                     "Was das Programm noch nicht gespeichert hatte, kann fehlen. "
                     "Zum sicheren Entfernen das USB-Symbol im Infobereich anklicken "
                     "und \"USB-Laufwerk sicher entfernen\" w\xe4hlen.", bu, name);
        meldung_zeigen('!', t);
    } else if (cd) {
        wsprintfA(t, "Die Scheibe in %c: (%s) kann jetzt entnommen werden.", bu, name);
        meldung_zeigen('i', t);
    } else {
        wsprintfA(t, "Das USB-Ger\xe4t %c: (%s) kann jetzt herausgezogen werden.\r\n\r\n"
                     "Alle Daten sind geschrieben, das Dateisystem ist abgemeldet, "
                     "und es laufen keine Lese- oder Schreibvorg\xe4nge mehr.\r\n"
                     "Bis zum erneuten Anstecken greift NT nicht mehr darauf zu.",
                  bu, name);
        meldung_zeigen('i', t);
    }
}

/*  Faden: ein Laufwerk sicher entfernen.  Bei Erfolg kommt die
 *  Bestaetigung vom Treiber (ausgeworfen); hier nur Fehler zeigen.  */
static DWORD WINAPI
auswerfen_faden (LPVOID p)
{
    char lw = (char)(UINT_PTR)p;
    char m[400];

    if (usb_auswerfen(lw, m, sizeof(m), 0) != AUSWURF_OK) {
        meldung_zeigen('!', m);
    }
    return 0;
}

static void
ausfuehren (const char *t)
{
    if (CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, t, 12, "ausgeworfen:", 12) == 2)
                                                ausgeworfen(t + 12);
    else if (lstrcmpiA(t, "lauter") == 0)       lautstaerke(1);
    else if (lstrcmpiA(t, "leiser") == 0)       lautstaerke(-1);
    else if (lstrcmpiA(t, "stumm") == 0)        stumm();
    else if (CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, t, 9, "programm:", 9) == 2)
                                                programm(t + 9);
}

/*  Faden: Treiber oeffnen, auf Auftraege warten, ausfuehren.  */
static DWORD WINAPI
abholer (LPVOID unbenutzt)
{
    static const char *namen[] = { "\\\\.\\UsbNt", "\\\\.\\UhciNt", "\\\\.\\OhciNt",
                                   "\\\\.\\EhciNt", "\\\\.\\XhciNt" };
    HANDLE ereignis = NULL, g = INVALID_HANDLE_VALUE;
    int    i;

    (void)unbenutzt;
    for (;;) {
        NTU_SONDER_AUFTRAG a;
        DWORD              got;

        if (g == INVALID_HANDLE_VALUE) {
            for (i = 0; i < 5 && g == INVALID_HANDLE_VALUE; i += 1) {
                g = CreateFileA(namen[i], GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            }
            if (g == INVALID_HANDLE_VALUE) {
                Sleep(5000);                      /* Treiber noch nicht da */
                continue;
            }
        }
        /*  Das Ereignis legt der Treiber spaetestens beim ersten Abholen
         *  an (beim Systemstart gibt es \BaseNamedObjects noch nicht).  */
        if (ereignis == NULL) {
            ereignis = OpenEventA(SYNCHRONIZE, FALSE, NTU_SONDER_EREIGNIS);
        }
        if (ereignis != NULL) {
            WaitForSingleObject(ereignis, 2000);
        } else {
            Sleep(250);
        }
        while (DeviceIoControl(g, IOCTL_NTU_SONDER_ABHOLEN, NULL, 0, &a, sizeof(a), &got, NULL) && got) {
            a.Text[sizeof(a.Text) - 1] = '\0';
            ausfuehren(a.Text);
        }
        if (GetLastError() == ERROR_DEVICE_NOT_CONNECTED) {
            CloseHandle(g);
            g = INVALID_HANDLE_VALUE;
        }
    }
    return 0;                                   /* nie erreicht */
}

/* ---------------- Symbol im Infobereich ---------------- */

static void
symbol (DWORD was)
{
    NOTIFYICONDATAA n;

    ZeroMemory(&n, sizeof(n));
    n.cbSize           = NID_V1_GROESSE;
    n.hWnd             = gFenster;
    n.uID              = 1;
    n.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    n.uCallbackMessage = WM_SYMBOL;
    n.hIcon            = (HICON)LoadImageA(gInst, MAKEINTRESOURCEA(1), IMAGE_ICON, 16, 16, 0);
    lstrcpynA(n.szTip, "USB: Sondertasten, Laufwerk sicher entfernen", 64);
    Shell_NotifyIconA(was, &n);
}

static void
menue (void)
{
    HMENU m = CreatePopupMenu();
    POINT p;

    {
        HMENU u = CreatePopupMenu();
        char  lw, z[64], e[96];
        int   n = 0;

        for (lw = 'C'; lw <= 'Z'; lw += 1) {
            if (usb_laufwerk(lw, z, sizeof(z))) {
                wsprintfA(e, "Laufwerk &%c:", lw);
                AppendMenuA(u, MF_STRING, ID_AUSWURF + (lw - 'A'), e);
                n += 1;
            }
        }
        if (n == 0) {
            AppendMenuA(u, MF_STRING | MF_GRAYED, ID_KEIN_USB, "(kein USB-Laufwerk)");
        }
        AppendMenuA(m, MF_POPUP, (UINT_PTR)u, "USB-Laufwerk &sicher entfernen");
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuA(m, MF_STRING, ID_INFO,    "&Was macht das?");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_BEENDEN, "&Beenden");
    GetCursorPos(&p);
    SetForegroundWindow(gFenster);      /* sonst schliesst das Menue nicht */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, gFenster, NULL);
    PostMessageA(gFenster, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK
fensterfunktion (HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == gTaskleisteNeu && gTaskleisteNeu != 0) {
        symbol(NIM_ADD);                /* Explorer neu gestartet */
        return 0;
    }
    switch (msg) {
    case WM_SYMBOL:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            menue();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) >= ID_AUSWURF && LOWORD(wp) < ID_AUSWURF + 26) {
            DWORD tid;
            CloseHandle(CreateThread(NULL, 0, auswerfen_faden,
                                     (LPVOID)(UINT_PTR)('A' + LOWORD(wp) - ID_AUSWURF),
                                     0, &tid));
        } else if (LOWORD(wp) == ID_BEENDEN) {
            DestroyWindow(h);
        } else if (LOWORD(wp) == ID_INFO) {
            MessageBoxA(h,
                "USB-Laufwerk sicher entfernen: schreibt alles aus, meldet das\r\n"
                "Dateisystem ab, wartet, bis keine Lese- und Schreibauftraege mehr\r\n"
                "laufen, und sagt dann, dass der Stick gezogen werden kann.\r\n\r\n"
                "USBTASTEN fuehrt ausserdem Sondertasten-Belegungen aus, die der USB-Treiber\r\n"
                "nicht selbst erledigen kann:  lauter, leiser, stumm, programm:<Befehl>.\r\n\r\n"
                "Belegen: Systemsteuerung -> Tastatur -> Reiter \"Sondertasten\".\r\n"
                "Tastenkombinationen brauchen dieses Programm nicht.\r\n\r\n"
                "Beenden ist gefahrlos; beim naechsten Anmelden startet es wieder,\r\n"
                "solange es im Autostart eingetragen ist (USBINST aus entfernt es).",
                "USB-Sondertasten", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    case WM_DESTROY:
        symbol(NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI
WinMain (HINSTANCE inst, HINSTANCE vorher, LPSTR zeile, int zeigen)
{
    WNDCLASSA k;
    MSG       m;
    DWORD     tid;

    (void)vorher; (void)zeile; (void)zeigen;
    gInst = inst;

    CreateMutexA(NULL, TRUE, "UsbTastenHelfer");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;                                   /* laeuft schon */
    }

    ZeroMemory(&k, sizeof(k));
    k.lpfnWndProc   = fensterfunktion;
    k.hInstance     = inst;
    k.hIcon         = LoadIconA(inst, MAKEINTRESOURCEA(1));
    k.lpszClassName = "UsbTastenFenster";
    if (!RegisterClassA(&k)) {
        return 1;
    }
    /*  Unsichtbares Verwaltungsfenster fuer die Nachrichten des Symbols.  */
    gFenster = CreateWindowExA(0, "UsbTastenFenster", "USB-Sondertasten", WS_OVERLAPPED,
                               0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!gFenster) {
        return 1;
    }
    gTaskleisteNeu = RegisterWindowMessageA("TaskbarCreated");
    symbol(NIM_ADD);

    CloseHandle(CreateThread(NULL, 0, abholer, NULL, 0, &tid));

    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

/*  Einstieg ohne C-Laufzeit (-nostdlib, -e _Einstieg@0).  */
void WINAPI
Einstieg (void)
{
    ExitProcess((UINT)WinMain(GetModuleHandleA(NULL), NULL, GetCommandLineA(), SW_SHOWDEFAULT));
}
