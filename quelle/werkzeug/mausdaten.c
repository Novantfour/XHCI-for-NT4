/*
 *  werkzeug/mausdaten.c -- MAUSDATEN.EXE: was die USB-Maus schickt, was der
 *  Treiber daraus an Windows gibt, und wann Windows den Klick meldet --
 *  alles in EINER Datei, zeitlich sortiert.
 *
 *  Bedienung: starten, dreimal langsam klicken, Esc.  Ergebnis:
 *  MAUSDATEN.TXT neben dem Programm.
 *
 *  Die Treiberseite kommt aus IOCTL_NTU_MAUS_PROTOKOLL (die letzten 64
 *  Berichte mit Zeit, Rohbytes, erkannten Tasten und den ButtonFlags an
 *  mouclass).  Die Windowsseite zeichnet das Fenster selbst auf.  Beide
 *  Zeiten beruhen auf dem Taktzaehler (GetTickCount / KeQueryTickCount).
 *
 *  kernel32, user32, gdi32.  Kein CRT.
 */

#include <windows.h>
#include "../ober/ntusbioctl.h"

void *
memset (void *z, int c, size_t n)
{
    unsigned char *p = (unsigned char *)z;
    while (n--) *p++ = (unsigned char)c;
    return z;
}

#define MAXW 200
static DWORD  w_t[MAXW], w_r[MAXW];
static int    w_art[MAXW];            /* 0 ab, 1 auf */
static int    n_w;
static DWORD  t_start;

/*  Millisekunden nach dem Systemtakt -- wie der Treiber (KeTickCount).
 *  Nicht QueryPerformanceCounter: auf Mehrprozessor-Kernen liest er den
 *  Zaehler des gerade laufenden Prozessors, und die laufen nicht gleich.  */
static DWORD
jetzt_ms (void)
{
    return GetTickCount();
}
static HFONT  gross;
static HANDLE geraet = INVALID_HANDLE_VALUE;
static NTU_MAUS_PROTOKOLL prot;

/*  Alle Treibereintraege in einer Liste: die Tastenwechsel aus T (die
 *  Bewegung nicht verdraengt) und aus E nur die ohne Tastenwechsel.  */
static NTU_MAUS_EINTRAG alle_e[96];
static int              n_alle;

static void
treiber_liste (void)
{
    ULONG i;
    int   a, b;
    n_alle = 0;
    for (i = 0; i < prot.TAnzahl && i < 32; i += 1) alle_e[n_alle++] = prot.T[i];
    for (i = 0; i < prot.Anzahl && i < 64; i += 1) {
        if (prot.E[i].Alle == prot.E[i].Vorher) alle_e[n_alle++] = prot.E[i];
    }
    for (a = 1; a < n_alle; a += 1) {                  /* nach Zeit sortieren */
        NTU_MAUS_EINTRAG x = alle_e[a];
        for (b = a - 1; b >= 0 && (LONG)(alle_e[b].Ms - x.Ms) > 0; b -= 1) alle_e[b + 1] = alle_e[b];
        alle_e[b + 1] = x;
    }
}

static void
flags_text (char *s, unsigned short f)
{
    s[0] = '\0';
    if (f & 0x0001) lstrcatA(s, "LINKS AB ");
    if (f & 0x0002) lstrcatA(s, "LINKS AUF ");
    if (f & 0x0004) lstrcatA(s, "RECHTS AB ");
    if (f & 0x0008) lstrcatA(s, "RECHTS AUF ");
    if (f & 0x0010) lstrcatA(s, "MITTE AB ");
    if (f & 0x0020) lstrcatA(s, "MITTE AUF ");
    if (f & 0x0400) lstrcatA(s, "RAD ");
    if (s[0] == '\0') lstrcpyA(s, "nur Bewegung");
}

static void
schreiben (void)
{
    char   pfad[MAX_PATH], *p, z[200], f[80];
    HANDLE d;
    DWORD  w;
    ULONG  i = 0;
    int    k = 0;

    GetModuleFileNameA(NULL, pfad, sizeof(pfad));
    for (p = pfad + lstrlenA(pfad); p > pfad && p[-1] != '\\'; p -= 1) {
    }
    lstrcpyA(p, "MAUSDATEN.TXT");
    d = CreateFileA(pfad, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (d == INVALID_HANDLE_VALUE) return;

#define ZEILE(x) WriteFile(d, (x), lstrlenA(x), &w, NULL), WriteFile(d, "\r\n", 2, &w, NULL)
    ZEILE("MAUSDATEN -- Zeiten in ms ab Programmstart");
    ZEILE("  Treiber: was die USB-Maus schickte (Rohbytes), erkannte Tasten, und was an Windows ging");
    ZEILE("  Windows: wann der Klick im Fenster ankam (in Klammern: wann Windows ihn erzeugte)");
    ZEILE("");

    memset(&prot, 0, sizeof(prot));
    if (geraet == INVALID_HANDLE_VALUE) {
        ZEILE("  (USB-Treiber nicht erreichbar)");
    } else {
        DWORD got;
        if (!DeviceIoControl(geraet, IOCTL_NTU_MAUS_PROTOKOLL, NULL, 0, &prot, sizeof(prot),
                             &got, NULL)) {
            wsprintfA(z, "  (Treiber kennt das Protokoll nicht, Fehler %lu -- alte USBNT.SYS?)",
                      GetLastError());
            ZEILE(z);
        }
    }

    treiber_liste();
    {
        int i = 0;
        while (i < n_alle || k < n_w) {
            int nehme_treiber;
            if (i < n_alle && (LONG)(alle_e[i].Ms - t_start) < -200) { i += 1; continue; }
            if (i >= n_alle) nehme_treiber = 0;
            else if (k >= n_w) nehme_treiber = 1;
            else nehme_treiber = (LONG)(alle_e[i].Ms - w_t[k]) <= 0;
            if (nehme_treiber) {
                NTU_MAUS_EINTRAG *e = &alle_e[i++];
                flags_text(f, e->Flags);
                wsprintfA(z, "  %6ld ms  Treiber: Bericht %02x %02x %02x %02x  Tasten %02x (vorher %02x)  "
                             "-> %s%s", (LONG)(e->Ms - t_start), e->Roh[0], e->Roh[1], e->Roh[2], e->Roh[3],
                          e->Alle, e->Vorher, e->Geliefert ? "an Windows: " : "",
                          e->Geliefert ? f : "leer, nicht weitergegeben");
            } else {
                wsprintfA(z, "  %6ld ms  Windows: %s kommt im Fenster an (erzeugt bei %ld ms)",
                          (LONG)(w_t[k] - t_start), w_art[k] ? "LINKS AUF" : "LINKS AB",
                          (LONG)(w_r[k] - t_start));
                k += 1;
            }
            ZEILE(z);
        }
    }
    ZEILE("");
    wsprintfA(z, "Berichte seit Treiberstart: %lu", prot.Gesamt);
    ZEILE(z);
    CloseHandle(d);
}

static LRESULT CALLBACK
proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            if (n_w < MAXW) {
                w_t[n_w] = jetzt_ms();
                /*  GetMessageTime zaehlt nach GetTickCount: als Abstand
                 *  "erzeugt vor der Ankunft" umrechnen.  */
                w_r[n_w] = (DWORD)GetMessageTime();
                w_art[n_w] = (m == WM_LBUTTONUP);
                n_w += 1;
                InvalidateRect(w, NULL, TRUE);
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                schreiben();
                DestroyWindow(w);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(w, &ps);
            HFONT       af;
            char        s[120];
            int         klicks = 0, k;
            for (k = 0; k < n_w; k += 1) if (!w_art[k]) klicks += 1;
            SetBkMode(dc, TRANSPARENT);
            af = (HFONT)SelectObject(dc, gross);
            lstrcpyA(s, "1.  DREIMAL klicken -- die Maus dabei NICHT bewegen.");
            TextOutA(dc, 40, 60, s, lstrlenA(s));
            lstrcpyA(s, "2.  5 Sekunden warten, weiter nicht bewegen.");
            TextOutA(dc, 40, 95, s, lstrlenA(s));
            lstrcpyA(s, "3.  Dann die Maus bewegen.");
            TextOutA(dc, 40, 130, s, lstrlenA(s));
            lstrcpyA(s, "4.  Esc druecken.  Die Datei MAUSDATEN.TXT schicken.");
            TextOutA(dc, 40, 165, s, lstrlenA(s));
            wsprintfA(s, "Klicks bei Windows angekommen: %d", klicks);
            TextOutA(dc, 40, 230, s, lstrlenA(s));
            if (geraet == INVALID_HANDLE_VALUE) {
                lstrcpyA(s, "Achtung: USB-Treiber nicht erreichbar.");
                TextOutA(dc, 40, 280, s, lstrlenA(s));
            }
            SelectObject(dc, af);
            EndPaint(w, &ps);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static int
hauptteil (void)
{
    static const char *namen[] = { "\\\\.\\UsbNt", "\\\\.\\UhciNt", "\\\\.\\OhciNt",
                                   "\\\\.\\EhciNt", "\\\\.\\XhciNt" };
    WNDCLASSA k;
    MSG       msg;
    HWND      f;
    int       i;

    for (i = 0; i < 5 && geraet == INVALID_HANDLE_VALUE; i += 1) {
        geraet = CreateFileA(namen[i], GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    t_start = jetzt_ms();
    gross = CreateFontA(26, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, 0,
                        VARIABLE_PITCH | FF_SWISS, "Arial");
    memset(&k, 0, sizeof(k));
    k.lpfnWndProc   = proz;
    k.hInstance     = GetModuleHandleA(NULL);
    k.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    k.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    k.lpszClassName = "MausDaten";
    RegisterClassA(&k);
    f = CreateWindowExA(WS_EX_TOPMOST, "MausDaten", "Mausdaten", WS_POPUP | WS_VISIBLE,
                        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                        NULL, NULL, k.hInstance, NULL);
    SetForegroundWindow(f);
    SetFocus(f);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

void WINAPI
Einstieg (void)
{
    ExitProcess((UINT)hauptteil());
}
