/*
 *  werkzeug/usbmaus.c -- Systemsteuerung "USB-Maus" (usbmaus.cpl).
 *
 *  Acht Zeilen, eine je Maustaste:
 *    - ein Laempchen, das leuchtet, solange die Taste gedrueckt ist --
 *      auch 4..8, die Windows unter NT 4.0 gar nicht kennt; es fragt den
 *      Treiber direkt (IOCTL_NTU_MAUS_ZUSTAND, alle 30 ms);
 *    - die Belegung, zur Auswahl oder selbst getippt ("strg+umschalt+esc").
 *
 *  "Uebernehmen" schickt jede geaenderte Belegung an den Treiber
 *  (IOCTL_NTU_MAUS_SETZEN): sie wirkt sofort und steht danach unter
 *  Parameters\TasteN -- kein Neustart.  "Vorgabe" traegt die Vorgaben ein.
 *
 *  Schutz: ist danach keine Taste mehr die linke Maustaste, wird erst
 *  gefragt -- sonst sperrt man sich ohne zweite Maus aus.
 *
 *  Das Steuergeraet heisst je nach Bauform \\.\UsbNt, \\.\UhciNt, ...;
 *  es wird das erste genommen, das sich oeffnen laesst.
 *
 *  kernel32, user32, gdi32.  Kein CRT.
 */

#include <windows.h>
#include <cpl.h>
#include "../ober/ntusbioctl.h"

#define N_TASTEN   8
#define ID_WAHL    200             /* +0..7 */
#define ID_UEBER   300
#define ID_VORGABE 301
#define ID_ZU      302

#define ZEILE0     74
#define ZEILENH    30

static HINSTANCE inst;
static HWND      fenster;
static HANDLE    geraet = INVALID_HANDLE_VALUE;
static HFONT     schrift, fett;
static int       fertig;
static NTU_MAUS_ZUSTAND zustand;
static int       zustand_ok;
static char      gesetzt[N_TASTEN][48];     /* was der Treiber gerade hat */

static const char *vorgaben[N_TASTEN] = {
    "klick-links", "klick-rechts", "mitte", "alt+links",
    "alt+rechts", "aus", "aus", "aus"
};

/*  Auswahl: Wert, zwei Leerzeichen, Erklaerung in Klammern.  Genommen
 *  wird der Teil vor dem ersten Leerzeichen.  */
static const char *auswahl[] = {
    "klick-links  (Linke Taste)",
    "klick-rechts  (Rechte Taste)",
    "mitte  (Mittlere Taste)",
    "doppelklick  (Doppelklick)",
    "alt+links  (Zur\xfc" "ck)",
    "alt+rechts  (Vorw\xe4rts)",
    "strg+c  (Kopieren)",
    "strg+v  (Einf\xfcgen)",
    "strg+z  (R\xfc" "ckg\xe4ngig)",
    "f5  (Aktualisieren)",
    "aus  (keine Wirkung)"
};
#define N_AUSWAHL (sizeof(auswahl) / sizeof(auswahl[0]))

void *
memset (void *z, int c, size_t n)
{
    unsigned char *p = (unsigned char *)z;
    while (n--) *p++ = (unsigned char)c;
    return z;
}


/* ---------------------------------------------------------------- *
 *  Treiber
 * ---------------------------------------------------------------- */

static int
oeffnen (void)
{
    static const char *namen[] = { "\\\\.\\UsbNt", "\\\\.\\UhciNt", "\\\\.\\OhciNt",
                                   "\\\\.\\EhciNt", "\\\\.\\XhciNt" };
    int k;

    for (k = 0; k < 5 && geraet == INVALID_HANDLE_VALUE; k += 1) {
        geraet = CreateFileA(namen[k], GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
    }
    return geraet != INVALID_HANDLE_VALUE;
}

static void
abfragen (void)
{
    DWORD got;
    zustand_ok = geraet != INVALID_HANDLE_VALUE &&
                 DeviceIoControl(geraet, IOCTL_NTU_MAUS_ZUSTAND, NULL, 0, &zustand,
                                 sizeof(zustand), &got, NULL) && got == sizeof(zustand);
}

static int
belegung_holen (int taste, char *ziel)
{
    NTU_MAUS_BELEGUNG b;
    DWORD             got;

    memset(&b, 0, sizeof(b));
    b.Taste = (unsigned long)taste;
    if (!DeviceIoControl(geraet, IOCTL_NTU_MAUS_HOLEN, &b, sizeof(b), &b, sizeof(b),
                         &got, NULL)) {
        return 0;
    }
    lstrcpynA(ziel, b.Text, 48);
    return 1;
}

static int
belegung_setzen (int taste, const char *text)
{
    NTU_MAUS_BELEGUNG b;
    DWORD             got;

    memset(&b, 0, sizeof(b));
    b.Taste = (unsigned long)taste;
    lstrcpynA(b.Text, text, sizeof(b.Text));
    return DeviceIoControl(geraet, IOCTL_NTU_MAUS_SETZEN, &b, sizeof(b), NULL, 0,
                           &got, NULL);
}


/* ---------------------------------------------------------------- *
 *  Auswahlfelder
 * ---------------------------------------------------------------- */

/*  Den Wert aus dem Feld: bis zum ersten Leerzeichen, klein.  */

static void
wert_aus_feld (int k, char *w)
{
    char t[80];
    int  i;

    GetWindowTextA(GetDlgItem(fenster, ID_WAHL + k), t, sizeof(t));
    for (i = 0; t[i] == ' '; i += 1) {
    }
    lstrcpynA(w, t + i, 48);
    for (i = 0; w[i] != '\0'; i += 1) {
        if (w[i] == ' ' || w[i] == '(') { w[i] = '\0'; break; }
        if (w[i] >= 'A' && w[i] <= 'Z') w[i] = (char)(w[i] + 32);
    }
}

/*  Einen Wert ins Feld: passt er zu einem Eintrag, den ganzen Eintrag.  */

static void
wert_ins_feld (int k, const char *w)
{
    HWND c = GetDlgItem(fenster, ID_WAHL + k);
    int  i, n = lstrlenA(w);

    for (i = 0; i < (int)N_AUSWAHL; i += 1) {
        if (CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, auswahl[i], n, w, n) == 2 &&
            auswahl[i][n] == ' ') {
            SendMessageA(c, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
    SendMessageA(c, CB_SETCURSEL, (WPARAM)-1, 0);
    SetWindowTextA(c, w);
}

static void
alle_holen (void)
{
    int k;
    for (k = 0; k < N_TASTEN; k += 1) {
        if (!belegung_holen(k + 1, gesetzt[k])) {
            lstrcpyA(gesetzt[k], vorgaben[k]);
        }
        wert_ins_feld(k, gesetzt[k]);
    }
}

static void
uebernehmen (void)
{
    char w[N_TASTEN][48], meldung[300];
    int  k, links = 0, fehler = 0;

    for (k = 0; k < N_TASTEN; k += 1) {
        wert_aus_feld(k, w[k]);
        if (w[k][0] == '\0') lstrcpyA(w[k], "aus");
        if (lstrcmpiA(w[k], "klick-links") == 0) links = 1;
    }

    if (!links &&
        MessageBoxA(fenster, "Keine Taste ist dann mehr die linke Maustaste.\n\n"
                             "Ohne eine zweite Maus kommen Sie so nur noch mit der Tastatur "
                             "weiter.  Trotzdem \xfc" "bernehmen?",
                    "USB-Maus", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    for (k = 0; k < N_TASTEN; k += 1) {
        if (lstrcmpiA(w[k], gesetzt[k]) == 0) {
            continue;
        }
        if (belegung_setzen(k + 1, w[k])) {
            lstrcpyA(gesetzt[k], w[k]);
        } else {
            wsprintfA(meldung, "Taste %d: \"%s\" ist keine g\xfcltige Belegung.\n\n"
                               "M\xf6glich: klick-links, klick-rechts, mitte, doppelklick, aus, "
                               "oder Tasten mit + verbunden, z. B. strg+umschalt+esc, alt+f4, f5.",
                      k + 1, w[k]);
            MessageBoxA(fenster, meldung, "USB-Maus", MB_OK | MB_ICONEXCLAMATION);
            fehler = 1;
        }
    }
    if (!fehler) {
        SetWindowTextA(GetDlgItem(fenster, ID_UEBER), "\xdc" "bernommen");
    }
}


/* ---------------------------------------------------------------- *
 *  Zeichnen: Kopf, Laempchen
 * ---------------------------------------------------------------- */

static void
zeichne (HDC dc)
{
    char   z[160];
    int    k;
    HFONT  af;

    SetBkMode(dc, TRANSPARENT);
    af = (HFONT)SelectObject(dc, fett);
    TextOutA(dc, 14, 10, "Maustasten erkennen und belegen", 31);
    SelectObject(dc, schrift);

    if (geraet == INVALID_HANDLE_VALUE) {
        lstrcpyA(z, "Der USB-Treiber ist nicht geladen.");
    } else if (!zustand_ok) {
        lstrcpyA(z, "Der USB-Treiber kennt diese Anfrage nicht -- bitte neuere Fassung einrichten.");
    } else if (zustand.Maeuse == 0) {
        lstrcpyA(z, "Keine USB-Maus angesteckt.  Belegen geht trotzdem.");
    } else {
        wsprintfA(z, "%lu %s, bis zu %lu Tasten.  Dr\xfc" "cken Sie eine Taste -- "
                     "ihre Zeile leuchtet.", zustand.Maeuse,
                  zustand.Maeuse == 1 ? "USB-Maus" : "USB-M\xe4use", zustand.Knoepfe);
    }
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    TextOutA(dc, 14, 34, z, lstrlenA(z));

    for (k = 0; k < N_TASTEN; k += 1) {
        int    y  = ZEILE0 + k * ZEILENH;
        int    an = zustand_ok && (zustand.Roh & (1ul << k));
        int    da = !zustand_ok || zustand.Knoepfe == 0 || (unsigned long)(k + 1) <= zustand.Knoepfe;
        HBRUSH b  = CreateSolidBrush(an ? RGB(0, 190, 0) : RGB(255, 255, 255));
        HPEN   p  = CreatePen(PS_SOLID, 1, RGB(64, 64, 64));
        HBRUSH ab = (HBRUSH)SelectObject(dc, b);
        HPEN   ap = (HPEN)SelectObject(dc, p);

        Ellipse(dc, 16, y + 4, 32, y + 20);
        SelectObject(dc, ab); SelectObject(dc, ap);
        DeleteObject(b); DeleteObject(p);

        wsprintfA(z, "Taste %d", k + 1);
        SetTextColor(dc, da ? GetSysColor(COLOR_WINDOWTEXT) : GetSysColor(COLOR_GRAYTEXT));
        TextOutA(dc, 42, y + 5, z, lstrlenA(z));
    }

    SelectObject(dc, af);
}

static LRESULT CALLBACK
proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {

        case WM_TIMER: {
            unsigned long vorher = zustand.Roh, vz = zustand.Maeuse;
            int           vo = zustand_ok;
            RECT          r;
            abfragen();
            if (zustand.Roh != vorher || zustand.Maeuse != vz || zustand_ok != vo) {
                r.left = 0; r.top = 28; r.right = 110; r.bottom = ZEILE0 + N_TASTEN * ZEILENH;
                InvalidateRect(w, &r, TRUE);
                r.left = 0; r.top = 28; r.right = 560; r.bottom = 52;
                InvalidateRect(w, &r, TRUE);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(w, &ps);
            zeichne(dc);
            EndPaint(w, &ps);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == ID_UEBER) {
                uebernehmen();
            } else if (LOWORD(wp) == ID_VORGABE) {
                int k;
                for (k = 0; k < N_TASTEN; k += 1) wert_ins_feld(k, vorgaben[k]);
                SetWindowTextA(GetDlgItem(w, ID_UEBER), "\xdc" "&bernehmen");
            } else if (LOWORD(wp) == ID_ZU) {
                DestroyWindow(w);
            } else if (LOWORD(wp) >= ID_WAHL && LOWORD(wp) < ID_WAHL + N_TASTEN &&
                       (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == CBN_EDITCHANGE)) {
                SetWindowTextA(GetDlgItem(w, ID_UEBER), "\xdc" "&bernehmen");
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(w);
            return 0;

        case WM_DESTROY:
            KillTimer(w, 1);
            fertig = 1;
            return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static HWND
kind (const char *klasse, const char *text, DWORD stil, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, klasse, text, WS_CHILD | WS_VISIBLE | stil, x, y, w, h,
                             fenster, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)schrift, FALSE);
    return c;
}

static void
zeigen (HWND eltern)
{
    WNDCLASSA k;
    RECT      r = { 0, 0, 560, ZEILE0 + N_TASTEN * ZEILENH + 50 };
    DWORD     stil = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    MSG       msg;
    int       i, j;

    schrift = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    fett    = CreateFontA(16, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, 0,
                          VARIABLE_PITCH | FF_SWISS, "MS Sans Serif");

    memset(&k, 0, sizeof(k));
    k.lpfnWndProc   = proz;
    k.hInstance     = inst;
    k.hIcon         = LoadIconA(inst, MAKEINTRESOURCEA(1));
    k.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    k.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    k.lpszClassName = "UsbMausCpl";
    RegisterClassA(&k);

    AdjustWindowRect(&r, stil, FALSE);
    fenster = CreateWindowExA(0, "UsbMausCpl", "USB-Maus", stil, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, eltern, NULL, inst, NULL);

    for (i = 0; i < N_TASTEN; i += 1) {
        HWND c = kind("COMBOBOX", "", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
                      120, ZEILE0 + i * ZEILENH, 300, 220, ID_WAHL + i);
        for (j = 0; j < (int)N_AUSWAHL; j += 1) {
            SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)auswahl[j]);
        }
    }
    kind("STATIC", "Eigene Tasten: mit + verbinden, z. B. strg+umschalt+esc, alt+f4, f5.",
         SS_LEFT, 430, ZEILE0, 120, 90, -1);

    {
        int y = ZEILE0 + N_TASTEN * ZEILENH + 12;
        kind("BUTTON", "\xdc" "&bernehmen", BS_DEFPUSHBUTTON | WS_TABSTOP, 120, y, 110, 26, ID_UEBER);
        kind("BUTTON", "&Vorgabe",   BS_PUSHBUTTON | WS_TABSTOP, 240, y, 110, 26, ID_VORGABE);
        kind("BUTTON", "&Schlie\xdf" "en", BS_PUSHBUTTON | WS_TABSTOP, 430, y, 110, 26, ID_ZU);
    }

    oeffnen();
    abfragen();
    if (geraet != INVALID_HANDLE_VALUE && zustand_ok) {
        alle_holen();
    } else {
        for (i = 0; i < N_TASTEN; i += 1) wert_ins_feld(i, vorgaben[i]);
        EnableWindow(GetDlgItem(fenster, ID_UEBER), FALSE);
    }

    fertig = 0;
    SetTimer(fenster, 1, 30, NULL);
    ShowWindow(fenster, SW_SHOW);
    SetForegroundWindow(fenster);
    SetFocus(GetDlgItem(fenster, ID_WAHL));

    while (!fertig && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(fenster, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (geraet != INVALID_HANDLE_VALUE) {
        CloseHandle(geraet);
        geraet = INVALID_HANDLE_VALUE;
    }
    if (fett != NULL) {
        DeleteObject(fett);
    }
}


/* ---------------------------------------------------------------- *
 *  Systemsteuerung
 * ---------------------------------------------------------------- */

LONG APIENTRY
CPlApplet (HWND eltern, UINT msg, LPARAM p1, LPARAM p2)
{
    switch (msg) {
        case CPL_INIT:      return 1;
        case CPL_GETCOUNT:  return 1;
        case CPL_INQUIRE: {
            CPLINFO *i = (CPLINFO *)p2;
            i->idIcon = 1; i->idName = 1; i->idInfo = 2; i->lData = 0;
            return 0;
        }
        case CPL_NEWINQUIRE: return 1;
        case CPL_DBLCLK:     zeigen(eltern); return 0;
    }
    return 0;
}

BOOL WINAPI
DllEntry (HINSTANCE h, DWORD grund, LPVOID r)
{
    if (grund == DLL_PROCESS_ATTACH) {
        inst = h;
    }
    return TRUE;
}
