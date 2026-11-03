/*
 *  werkzeug/usbjoycpl.c -- Systemsteuerung "Joystick" (usbjoy.cpl).
 *
 *  Ein Symbol fuer alles, mit drei Reitern:
 *    Gamepad    die Testanzeige unten (unveraendert, als Kindfenster);
 *    Vibration  beide Motoren eines Xbox-Controllers (IOCTL_NTUJ_RUETTELN);
 *    Gameport   oeffnet die urspruengliche Joystick-Seite von NT 4.0
 *               (joy.cpl) fuer alte Gameport-Joysticks.
 *  joy.cpl selbst laedt keine fremden Reiter; deshalb dieses Symbol, und
 *  joy.cpl wird in der Systemsteuerung ausgeblendet (JOYSTICK.REG:
 *  Control Panel\don't load).
 *
 *  Die Testanzeige:
 *
 *  Der Testdialog der Joystick-Systemsteuerung von NT 4.0 zeigt nur
 *  Taste 1 bis 4 und am Steuerkreuz nur vier Richtungen.  Dieses Element
 *  zeigt alles: beide Sticks, beide Trigger, das Steuerkreuz mit acht
 *  Richtungen und Winkel, und alle Tasten (bis 32) -- beim Xbox-Controller
 *  mit ihren Namen.
 *
 *  Die Werte kommen ueber joyGetPosEx, also genau so wie bei einem
 *  Spiel: was hier erscheint, bekommt jedes Spiel.  Nur ob es ein
 *  Xbox-Controller ist (fuer die Tastennamen), fragt es beim Kerntreiber
 *  (\\.\UsbJoyN, IOCTL_NTUJ_INFO) -- das weiss winmm nicht.
 *
 *  NT 4.0 zeigt jede .cpl-Datei aus system32 in der Systemsteuerung.
 *  Aufruf auch direkt:  control usbjoy.cpl
 *
 *  kernel32, user32, gdi32, winmm.  Kein CRT.
 */

#include <windows.h>
#include <commctrl.h>
#include <prsht.h>
#include <cpl.h>
#include <mmsystem.h>
#include "../ober/ntujoy.h"

#define BREITE   570
#define HOEHE    440
#define ID_J1    101
#define ID_J2    102

static HINSTANCE  inst;
static HWND       fenster;
static int        joy;               /* 0 oder 1 */
static JOYCAPSA   caps;
static JOYINFOEX  pos;
static MMRESULT   r_caps, r_pos;
static NTUJ_INFO  kinfo;
static int        kinfo_ok;
static int        fertig;

static const char *xbox_namen[11] = {
    "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LS", "RS", "Guide"
};

static void *
nullen (void *p, DWORD n)
{
    char *q = (char *)p;
    while (n--) *q++ = 0;
    return p;
}


/* ---------------------------------------------------------------- *
 *  Abfragen
 * ---------------------------------------------------------------- */

static void
kern_info (void)
{
    char   name[20];
    HANDLE h;
    DWORD  got;

    kinfo_ok = 0;
    wsprintfA(name, "\\\\.\\UsbJoy%d", joy);
    h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        kinfo_ok = DeviceIoControl(h, IOCTL_NTUJ_INFO, NULL, 0, &kinfo,
                                   sizeof(kinfo), &got, NULL) && got == sizeof(kinfo);
        CloseHandle(h);
    }
}

static void
abfragen (void)
{
    MMRESULT vorher = r_pos;

    nullen(&caps, sizeof(caps));
    r_caps = joyGetDevCapsA((UINT)joy, &caps, sizeof(caps));

    nullen(&pos, sizeof(pos));
    pos.dwSize  = sizeof(pos);
    pos.dwFlags = JOY_RETURNALL;
    r_pos = joyGetPosEx((UINT)joy, &pos);

    /*  Beim Anstecken (Fehler -> da) einmal neu nachsehen, was es ist.  */

    if (r_pos == JOYERR_NOERROR && (vorher != JOYERR_NOERROR || !kinfo_ok)) {
        kern_info();
    }
}


/* ---------------------------------------------------------------- *
 *  Zeichnen
 * ---------------------------------------------------------------- */

static void
text (HDC dc, int x, int y, const char *s, COLORREF farbe)
{
    SetTextColor(dc, farbe);
    TextOutA(dc, x, y, s, lstrlenA(s));
}

static void
text_mitte (HDC dc, int x, int y, int w, const char *s, COLORREF farbe)
{
    SIZE sz;
    GetTextExtentPoint32A(dc, s, lstrlenA(s), &sz);
    text(dc, x + (w - sz.cx) / 2, y, s, farbe);
}

static void
rahmen (HDC dc, int x, int y, int w, int h, COLORREF fuell)
{
    HBRUSH b = CreateSolidBrush(fuell), ab;
    HPEN   p = CreatePen(PS_SOLID, 1, RGB(64, 64, 64)), ap;

    ab = (HBRUSH)SelectObject(dc, b);
    ap = (HPEN)SelectObject(dc, p);
    Rectangle(dc, x, y, x + w, y + h);
    SelectObject(dc, ab);
    SelectObject(dc, ap);
    DeleteObject(b);
    DeleteObject(p);
}

static void
punkt (HDC dc, int cx, int cy, int r, COLORREF fuell)
{
    HBRUSH b = CreateSolidBrush(fuell), ab;
    HPEN   p = CreatePen(PS_SOLID, 1, RGB(64, 64, 64)), ap;

    ab = (HBRUSH)SelectObject(dc, b);
    ap = (HPEN)SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ab);
    SelectObject(dc, ap);
    DeleteObject(b);
    DeleteObject(p);
}

static void
linie (HDC dc, int x1, int y1, int x2, int y2, COLORREF farbe)
{
    HPEN p = CreatePen(PS_SOLID, 1, farbe), ap = (HPEN)SelectObject(dc, p);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    SelectObject(dc, ap);
    DeleteObject(p);
}

/*  Stick: Feld mit Fadenkreuz, Punkt an der Stellung.  */

static void
stick (HDC dc, int x, int y, int n, DWORD vx, DWORD vy, int da,
       const char *titel, const char *achsen)
{
    char z[40];
    int  px = x + 4 + (int)((vx * (DWORD)(n - 9)) / 65535);
    int  py = y + 4 + (int)((vy * (DWORD)(n - 9)) / 65535);

    rahmen(dc, x, y, n, n, RGB(255, 255, 255));
    linie(dc, x + n / 2, y + 1, x + n / 2, y + n - 1, RGB(200, 200, 200));
    linie(dc, x + 1, y + n / 2, x + n - 1, y + n / 2, RGB(200, 200, 200));
    if (da) {
        punkt(dc, px + 1, py + 1, 5, RGB(200, 0, 0));
    }
    text_mitte(dc, x, y + n + 4, n, titel, RGB(0, 0, 0));
    wsprintfA(z, "%s %5lu / %5lu", achsen, vx, vy);
    text_mitte(dc, x - 10, y + n + 20, n + 20, z, RGB(80, 80, 80));
}

/*  Trigger: Balken, voll gedrueckt = voll.  */

static void
balken (HDC dc, int x, int y, int w, int h, DWORD v, int da, const char *titel)
{
    char z[16];
    int  f = da ? (int)((v * (DWORD)(h - 2)) / 65535) : 0;

    rahmen(dc, x, y, w, h, RGB(255, 255, 255));
    if (f > 0) {
        rahmen(dc, x, y + h - f - 1, w, f + 1, RGB(0, 150, 0));
    }
    text_mitte(dc, x - 20, y + h + 4, w + 40, titel, RGB(0, 0, 0));
    wsprintfA(z, "%lu %%", da ? (v * 100) / 65535 : 0);
    text_mitte(dc, x - 20, y + h + 20, w + 40, z, RGB(80, 80, 80));
}

/*  Steuerkreuz: acht Richtungen und Mitte.  */

static void
steuerkreuz (HDC dc, int cx, int cy, int r, DWORD pov, int da, const char *name)
{
    /*  sin/cos der acht Richtungen, *1000; 0 Grad = oben.  */
    static const int sx[8] = { 0, 707, 1000, 707, 0, -707, -1000, -707 };
    static const int sy[8] = { -1000, -707, 0, 707, 1000, 707, 0, -707 };
    int  k, aktiv = -1;
    char z[24];

    if (da && pov != JOY_POVCENTERED && pov < 36000) {
        aktiv = (int)(((pov + 2250) % 36000) / 4500);
    }

    for (k = 0; k < 8; k += 1) {
        punkt(dc, cx + sx[k] * r / 1000, cy + sy[k] * r / 1000, 9,
              (k == aktiv) ? RGB(0, 150, 0) : RGB(255, 255, 255));
    }
    punkt(dc, cx, cy, 7, (da && aktiv < 0) ? RGB(160, 160, 160) : RGB(255, 255, 255));

    text_mitte(dc, cx - 60, cy + r + 16, 120, name, RGB(0, 0, 0));
    if (!da)             lstrcpyA(z, "-");
    else if (aktiv < 0)  lstrcpyA(z, "Mitte");
    else                 wsprintfA(z, "%lu Grad", pov / 100);
    text_mitte(dc, cx - 60, cy + r + 32, 120, z, RGB(80, 80, 80));
}

static void
tasten (HDC dc, int x0, int y0, int n, DWORD maske, int da, int xbox)
{
    int  k;
    char z[8];

    for (k = 0; k < n; k += 1) {

        int      x = x0 + (k % 8) * 66, y = y0 + (k / 8) * 44;
        int      an = da && (maske & (1ul << k));
        COLORREF schrift = an ? RGB(255, 255, 255) : RGB(0, 0, 0);

        rahmen(dc, x, y, 60, 38, an ? RGB(0, 150, 0) : RGB(255, 255, 255));
        wsprintfA(z, "%d", k + 1);
        text_mitte(dc, x, y + 3, 60, z, schrift);
        if (xbox && k < 11) {
            text_mitte(dc, x, y + 19, 60, xbox_namen[k], schrift);
        }
    }
}

/*  Welche Achse des Geraets liegt auf X, Y, Z, R, U, V?  Dieselbe
 *  Verteilung wie usbjoy.dll (verteilen), damit die Beschriftung zu dem
 *  passt, was Spiele sehen.  -1 = keine.  */
static void
verteilen (int platz[6])
{
    unsigned long k;
    int           schieber = -1;

    for (k = 0; k < 6; k += 1) platz[k] = -1;
    if (!kinfo_ok) return;
    for (k = 0; k < kinfo.AnzahlAchsen && k < NTUJ_ACHSEN; k += 1) {
        switch (kinfo.AchsenArt[k]) {
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

static const char *
achsname (int platz)
{
    if (platz < 0) return "--";
    switch (kinfo.AchsenArt[platz]) {
        case NTUJ_X:      return "X";
        case NTUJ_Y:      return "Y";
        case NTUJ_Z:      return "Z";
        case NTUJ_RX:     return "Rx";
        case NTUJ_RY:     return "Ry";
        case NTUJ_RZ:     return "Drehung";
        case NTUJ_SLIDER: return "Schub";
        case NTUJ_DIAL:   return "Regler";
        case NTUJ_WHEEL:  return "Rad";
    }
    return "Achse";
}

static void
zeichne (HDC dc, int w, int h)
{
    RECT     r = { 0, 0, w, h };
    HFONT    af = (HFONT)SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    int      da = (r_pos == JOYERR_NOERROR);
    int      xbox = kinfo_ok && kinfo.Xbox;
    int      n;
    char     z[160];

    FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);

    if (da) {
        wsprintfA(z, "%s%s  --  %u Achsen, %u Tasten", caps.szPname,
                  !xbox ? "" : (kinfo.Xbox == 2) ? " (Xbox One)" : " (Xbox 360)",
                  caps.wNumAxes, caps.wNumButtons);
        text(dc, 16, 14, z, RGB(0, 0, 0));
    } else {
        text(dc, 16, 14, "nicht angeschlossen", RGB(180, 0, 0));
    }

    /*  Gamepad (zwei Sticks, Trigger, Steuerkreuz) oder Joystick
     *  (Knueppel, Schub, Drehung, Coolie-Hat): beschriftet wird nach der
     *  Achse, die wirklich darunter liegt.  Ein Geraet ohne Rx/Ry gilt
     *  als Joystick.  */
    {
        int  platz[6];
        int  joystick;
        char zb[40], rb[40];

        verteilen(platz);
        joystick = kinfo_ok && !xbox && platz[4] < 0 && platz[5] < 0;
        if (xbox) {
            lstrcpyA(zb, "LT (Z)");
            lstrcpyA(rb, "RT (R)");
        } else {
            wsprintfA(zb, "%s", kinfo_ok ? achsname(platz[2]) : "Z");
            wsprintfA(rb, "%s", kinfo_ok ? achsname(platz[3]) : "R");
        }
        stick(dc, 16, 44, 130, pos.dwXpos, pos.dwYpos, da,
              joystick ? "Kn\xfc" "ppel" : "linker Stick", "X/Y");
        stick(dc, 166, 44, 130, pos.dwUpos, pos.dwVpos,
              da && !(kinfo_ok && platz[4] < 0 && platz[5] < 0),
              (kinfo_ok && platz[4] < 0 && platz[5] < 0) ? "nicht vorhanden" : "rechter Stick", "U/V");
        balken(dc, 330, 44, 26, 130, pos.dwZpos, da && !(kinfo_ok && platz[2] < 0), zb);
        balken(dc, 390, 44, 26, 130, pos.dwRpos, da && !(kinfo_ok && platz[3] < 0), rb);
        steuerkreuz(dc, 500, 108, 44, pos.dwPOV, da && (caps.wCaps & JOYCAPS_HASPOV),
                    joystick ? "Coolie-Hat" : "Steuerkreuz");
    }

    n = (int)caps.wNumButtons;
    if (!da || n <= 0) n = 11;
    if (n > 32)        n = 32;
    text(dc, 16, 236, "Tasten", RGB(0, 0, 0));
    tasten(dc, 16, 256, n, pos.dwButtons, da, xbox);

    SelectObject(dc, af);
}


/* ---------------------------------------------------------------- *
 *  Fenster
 * ---------------------------------------------------------------- */

static void
waehle (HWND w, int j)
{
    joy      = j;
    kinfo_ok = 0;
    r_pos    = JOYERR_UNPLUGGED;
    CheckRadioButton(w, ID_J1, ID_J2, j ? ID_J2 : ID_J1);
    abfragen();
    InvalidateRect(w, NULL, FALSE);
}

static LRESULT CALLBACK
fensterproz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {

        case WM_KEYDOWN:

            /*  1 / 2 schalten um, auch ohne Alt.  */

            if (wp == '1' || wp == VK_NUMPAD1) { waehle(w, 0); return 0; }
            if (wp == '2' || wp == VK_NUMPAD2) { waehle(w, 1); return 0; }
            break;

        case WM_TIMER:
            abfragen();
            InvalidateRect(w, NULL, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;                      /* alles in WM_PAINT: kein Flackern */

        case WM_PAINT: {
            PAINTSTRUCT ps;
            RECT        r;
            HDC         dc = BeginPaint(w, &ps), mem;
            HBITMAP     bm, abm;

            GetClientRect(w, &r);
            mem = CreateCompatibleDC(dc);
            bm  = CreateCompatibleBitmap(dc, r.right, r.bottom);
            abm = (HBITMAP)SelectObject(mem, bm);
            zeichne(mem, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, abm);
            DeleteObject(bm);
            DeleteDC(mem);
            EndPaint(w, &ps);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == ID_J1 || LOWORD(wp) == ID_J2) {
                waehle(w, LOWORD(wp) == ID_J2);
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

/*  Die Testanzeige als Kindfenster im Reiter "Gamepad".  */
static void
anzeige_anlegen (HWND seite)
{
    WNDCLASSA k;
    RECT      r;
    HWND      b;
    HFONT     schrift = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    nullen(&k, sizeof(k));
    k.lpfnWndProc   = fensterproz;
    k.hInstance     = inst;
    k.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    k.lpszClassName = "UsbJoyCpl";
    RegisterClassA(&k);

    GetClientRect(seite, &r);
    fenster = CreateWindowExA(0, "UsbJoyCpl", "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP,
                              0, 0, r.right, r.bottom, seite, NULL, inst, NULL);

    b = CreateWindowExA(0, "BUTTON", "Joystick &1",
                        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                        r.right - 200, 10, 90, 20, fenster, (HMENU)ID_J1, inst, NULL);
    SendMessageA(b, WM_SETFONT, (WPARAM)schrift, 0);
    SendMessageA(b, BM_SETCHECK, joy == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    b = CreateWindowExA(0, "BUTTON", "Joystick &2",
                        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                        r.right - 100, 10, 90, 20, fenster, (HMENU)ID_J2, inst, NULL);
    SendMessageA(b, WM_SETFONT, (WPARAM)schrift, 0);
    SendMessageA(b, BM_SETCHECK, joy == 1 ? BST_CHECKED : BST_UNCHECKED, 0);

    fertig = 0;
    r_pos  = JOYERR_UNPLUGGED;
    abfragen();
    SetTimer(fenster, 1, 30, NULL);
}

/* ---------------------------------------------------------------- *
 *  Reiter
 * ---------------------------------------------------------------- */

#define ID_STARK    201
#define ID_SCHWACH  202
#define ID_TESTEN   203
#define ID_AN       204
#define ID_AUS      205
#define ID_VINFO    206
#define ID_GAMEPORT 301

static HWND vseite;

static BOOL CALLBACK
seite_gamepad (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_INITDIALOG) {
        anzeige_anlegen(w);
        return TRUE;
    }
    if (m == WM_NOTIFY && ((NMHDR *)lp)->code == PSN_SETACTIVE) {
        abfragen();                       /* Joystick 1/2 kann im Reiter "Vibration" gewechselt sein */
        return FALSE;
    }
    return FALSE;
}

static int
ruetteln (int stark, int schwach)
{
    char          name[20];
    HANDLE        h;
    DWORD         got;
    NTUJ_RUETTELN r;
    int           ok;

    wsprintfA(name, "\\\\.\\UsbJoy%d", joy);
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    nullen(&r, sizeof(r));
    r.Stark = (unsigned char)stark;
    r.Schwach = (unsigned char)schwach;
    ok = DeviceIoControl(h, IOCTL_NTUJ_RUETTELN, &r, sizeof(r), NULL, 0, &got, NULL);
    if (!ok && GetLastError() == ERROR_NOT_SUPPORTED) ok = -2;
    CloseHandle(h);
    return ok;
}

static void
vib_info (void)
{
    char z[160];
    kern_info();
    if (!kinfo_ok) {
        wsprintfA(z, "Joystick %d: kein USB-Gamepad angesteckt.", joy + 1);
    } else if (kinfo.Xbox) {
        wsprintfA(z, "Joystick %d: %s (%04x:%04x) -- Vibration m\xf6glich.",
                  joy + 1, kinfo.Name, kinfo.Vid, kinfo.Pid);
    } else {
        wsprintfA(z, "Joystick %d: %s (%04x:%04x) -- f\xfcr dieses Gamepad kennt der Treiber "
                     "keine Vibration (nur Xbox 360 und Xbox One/Series).",
                  joy + 1, kinfo.Name, kinfo.Vid, kinfo.Pid);
    }
    SetDlgItemTextA(vseite, ID_VINFO, z);
}

static void
vkind (HWND seite, const char *klasse, const char *text, DWORD stil, int x, int y, int bw, int bh, int id)
{
    RECT r;
    HWND c;
    r.left = x; r.top = y; r.right = x + bw; r.bottom = y + bh;
    MapDialogRect(seite, &r);
    c = CreateWindowExA(0, klasse, text, WS_CHILD | WS_VISIBLE | stil, r.left, r.top,
                        r.right - r.left, r.bottom - r.top, seite, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(c, WM_SETFONT, SendMessageA(seite, WM_GETFONT, 0, 0), FALSE);
    if (lstrcmpA(klasse, TRACKBAR_CLASSA) == 0) {
        SendMessageA(c, TBM_SETRANGE, TRUE, MAKELONG(0, 255));
        SendMessageA(c, TBM_SETPAGESIZE, 0, 32);
        SendMessageA(c, TBM_SETPOS, TRUE, 128);
    }
}

static BOOL CALLBACK
seite_vibration (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_INITDIALOG:
            vseite = w;
            vkind(w, "STATIC", "", SS_LEFT, 10, 10, 360, 20, ID_VINFO);
            vkind(w, "BUTTON", "Joystick &1", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 10, 34, 60, 12, ID_J1);
            vkind(w, "BUTTON", "Joystick &2", BS_AUTORADIOBUTTON, 80, 34, 60, 12, ID_J2);
            CheckRadioButton(w, ID_J1, ID_J2, joy ? ID_J2 : ID_J1);
            vkind(w, "STATIC", "Gro\xdf" "er Motor (links, stark):", SS_LEFT, 10, 58, 200, 10, -1);
            vkind(w, TRACKBAR_CLASSA, "", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 10, 70, 220, 20, ID_STARK);
            vkind(w, "STATIC", "Kleiner Motor (rechts, schnell):", SS_LEFT, 10, 98, 200, 10, -1);
            vkind(w, TRACKBAR_CLASSA, "", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 10, 110, 220, 20, ID_SCHWACH);
            vkind(w, "BUTTON", "&Testen (1 Sekunde)", BS_PUSHBUTTON | WS_TABSTOP, 10, 142, 90, 14, ID_TESTEN);
            vkind(w, "BUTTON", "&Dauernd an", BS_PUSHBUTTON | WS_TABSTOP, 106, 142, 60, 14, ID_AN);
            vkind(w, "BUTTON", "A&us", BS_PUSHBUTTON | WS_TABSTOP, 172, 142, 40, 14, ID_AUS);
            vkind(w, "STATIC", "Xbox-One- und Series-Controller beenden die Vibration nach gut "
                               "2,5 Sekunden von selbst; \"Dauernd an\" erneuert sie.",
                  SS_LEFT, 10, 166, 360, 20, -1);
            vib_info();
            return TRUE;

        case WM_COMMAND: {
            int s = (int)SendDlgItemMessageA(w, ID_STARK, TBM_GETPOS, 0, 0);
            int k = (int)SendDlgItemMessageA(w, ID_SCHWACH, TBM_GETPOS, 0, 0);
            switch (LOWORD(wp)) {
                case ID_J1: case ID_J2:
                    ruetteln(0, 0);
                    KillTimer(w, 1); KillTimer(w, 2);
                    waehle(fenster, LOWORD(wp) == ID_J2);
                    vib_info();
                    break;
                case ID_TESTEN:
                    KillTimer(w, 2);
                    if (ruetteln(s, k) == -2) vib_info();
                    SetTimer(w, 1, 1000, NULL);
                    break;
                case ID_AN:
                    KillTimer(w, 1);
                    if (ruetteln(s, k) == -2) vib_info();
                    SetTimer(w, 2, 2000, NULL);              /* erneuern (Xbox One) */
                    break;
                case ID_AUS:
                    KillTimer(w, 1); KillTimer(w, 2);
                    ruetteln(0, 0);
                    break;
            }
            return TRUE;
        }

        case WM_TIMER:
            if (wp == 1) {
                KillTimer(w, 1);
                ruetteln(0, 0);
            } else {
                ruetteln((int)SendDlgItemMessageA(w, ID_STARK, TBM_GETPOS, 0, 0),
                         (int)SendDlgItemMessageA(w, ID_SCHWACH, TBM_GETPOS, 0, 0));
            }
            return TRUE;

        case WM_NOTIFY:
            if (((NMHDR *)lp)->code == PSN_KILLACTIVE) {     /* Reiter verlassen: aus */
                KillTimer(w, 1); KillTimer(w, 2);
                ruetteln(0, 0);
            } else if (((NMHDR *)lp)->code == PSN_SETACTIVE) {
                CheckRadioButton(w, ID_J1, ID_J2, joy ? ID_J2 : ID_J1);
                vib_info();
            }
            return FALSE;

        case WM_DESTROY:
            KillTimer(w, 1); KillTimer(w, 2);
            ruetteln(0, 0);
            return FALSE;
    }
    return FALSE;
}

static BOOL CALLBACK
seite_gameport (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_INITDIALOG) {
        vkind(w, "STATIC", "Alte Joysticks am Gameport (Soundkarte, 15-polig) richtet die "
                           "urspr\xfcngliche Joystick-Seite von Windows NT ein: Typ w\xe4hlen, "
                           "kalibrieren, testen.", SS_LEFT, 10, 10, 360, 30, -1);
        vkind(w, "BUTTON", "&Gameport-Joystick einrichten...", BS_PUSHBUTTON | WS_TABSTOP,
              10, 46, 130, 14, ID_GAMEPORT);
        vkind(w, "STATIC", "USB-Gamepads brauchen das nicht: sie sind sofort da (Reiter "
                           "\"Gamepad\") und erscheinen jedem Spiel als Joystick 1 und 2.",
              SS_LEFT, 10, 70, 360, 30, -1);
        return TRUE;
    }
    if (m == WM_COMMAND && LOWORD(wp) == ID_GAMEPORT) {
        /*  joy.cpl direkt ueber ihre Systemsteuerungs-Schnittstelle
         *  (CPlApplet) oeffnen, wie es die Systemsteuerung selbst tut.
         *  "rundll32 shell32.dll,Control_RunDLL joy.cpl" oeffnete unter
         *  NT 4.0 nichts, solange joy.cpl unter "don't load" steht.  */
        typedef LONG (APIENTRY *CPLFN)(HWND, UINT, LPARAM, LPARAM);
        HMODULE  m = LoadLibraryA("joy.cpl");
        CPLFN    f = m ? (CPLFN)GetProcAddress(m, "CPlApplet") : 0;
        CPLINFO  ci;

        if (f == 0) {
            MessageBoxA(w, "Die Joystick-Seite von Windows (joy.cpl) fehlt.",
                        "Joystick", MB_OK | MB_ICONEXCLAMATION);
        } else if (!f(w, CPL_INIT, 0, 0)) {
            /*  joy.cpl lehnt ab, wenn kein Joystick-Treiber eingetragen ist
             *  (Drivers32\joystick) -- geprueft: mit usbjoy.dll dort oeffnet
             *  sie sich.  */
            MessageBoxA(w, "Die Joystick-Seite von Windows \xf6" "ffnet sich nur, wenn ein "
                           "Joystick-Treiber eingerichtet ist.\n\n"
                           "USBINST richtet usbjoy.dll ein (Treiber f\xfcr USB-Gamepads); "
                           "f\xfcr einen Gameport-Joystick den Treiber der Soundkarte "
                           "einrichten (Systemsteuerung -> Multimedia -> Ger\xe4te).",
                        "Joystick", MB_OK | MB_ICONINFORMATION);
        } else {
            nullen(&ci, sizeof(ci));
            f(w, CPL_GETCOUNT, 0, 0);
            f(w, CPL_INQUIRE, 0, (LPARAM)&ci);
            f(GetParent(w), CPL_DBLCLK, 0, ci.lData);
            f(w, CPL_STOP, 0, ci.lData);
            f(w, CPL_EXIT, 0, 0);
        }
        if (m) FreeLibrary(m);
        return TRUE;
    }
    return FALSE;
}

/*  Leere Dialogvorlagen im Speicher; die Seiten legen ihre Knoepfe selbst
 *  an.  372 x 222 Einheiten (rund 560 x 360 Bildpunkte): so passt das
 *  ganze Fenster samt OK/Abbrechen auf 640 x 480 -- 380 x 272 ragte dort
 *  unten und rechts hinaus.  */
static WORD vorlagen[3][40];

static LPCDLGTEMPLATE
vorlage (int i)
{
    DLGTEMPLATE *t = (DLGTEMPLATE *)vorlagen[i];
    WORD        *p;
    const char  *f = "MS Sans Serif";

    nullen(vorlagen[i], sizeof(vorlagen[i]));
    t->style = WS_CHILD | WS_VISIBLE | DS_SETFONT | DS_3DLOOK | WS_CLIPCHILDREN;
    t->cx = 372; t->cy = 222;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 8;
    while (*f) *p++ = (WORD)(unsigned char)*f++;
    *p++ = 0;
    return (LPCDLGTEMPLATE)t;
}

static void
zeigen (HWND eltern)
{
    static const char *titel[3] = { "Test", "Vibration", "Gameport" };
    static DLGPROC     prozs[3] = { (DLGPROC)seite_gamepad, (DLGPROC)seite_vibration,
                                    (DLGPROC)seite_gameport };
    PROPSHEETPAGEA     s[3];
    PROPSHEETHEADERA   k;
    int                i;

    InitCommonControls();
    joy = 0;
    for (i = 0; i < 3; i += 1) {
        nullen(&s[i], sizeof(s[i]));
        s[i].dwSize      = sizeof(s[i]);
        s[i].dwFlags     = PSP_DLGINDIRECT | PSP_USETITLE;
        s[i].hInstance   = inst;
        s[i].pResource   = vorlage(i);
        s[i].pszTitle    = titel[i];
        s[i].pfnDlgProc  = prozs[i];
    }
    nullen(&k, sizeof(k));
    k.dwSize     = sizeof(k);
    k.dwFlags    = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_USEHICON;
    k.hwndParent = eltern;
    k.hInstance  = inst;
    k.hIcon      = LoadIconA(inst, MAKEINTRESOURCEA(1));
    k.pszCaption = "Eigenschaften von Joystick";
    k.nPages     = 3;
    k.ppsp       = s;
    PropertySheetA(&k);
}


/* ---------------------------------------------------------------- *
 *  Systemsteuerung
 * ---------------------------------------------------------------- */

LONG APIENTRY
CPlApplet (HWND eltern, UINT msg, LPARAM p1, LPARAM p2)
{
    switch (msg) {

        case CPL_INIT:
            return 1;

        case CPL_GETCOUNT:
            return 1;

        case CPL_INQUIRE: {
            CPLINFO *i = (CPLINFO *)p2;
            i->idIcon = 1;
            i->idName = 1;
            i->idInfo = 2;
            i->lData  = 0;
            return 0;
        }

        case CPL_NEWINQUIRE:
            return 1;                      /* nicht behandelt: CPL_INQUIRE gilt */

        case CPL_DBLCLK:
            zeigen(eltern);
            return 0;
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
