/*
 *  werkzeug/klicktest.c -- KLICKTEST.EXE: haengen Mausklicks, und was
 *  gibt sie frei?
 *
 *  Gefuehrt in drei Schritten, je drei Versuche, Anweisung gross auf dem
 *  Schirm:
 *    1  Einmal klicken, dann 5 Sekunden nichts tun.
 *    2  Einmal klicken, dann gleich die Maus bewegen.
 *    3  Einmal klicken, nach etwa 2 Sekunden noch einmal.
 *
 *  Ein Faden fragt den Treiber jede Millisekunde (so fein, wie NT es
 *  zulaesst) nach dem Rohzustand (IOCTL_NTU_MAUS_ZUSTAND): Taste 1
 *  gedrueckt/losgelassen, und Berichte ohne Tastenwechsel = Bewegung.
 *  Das Fenster zaehlt WM_LBUTTONDOWN/WM_LBUTTONUP.  Je Versuch wird fuer
 *  Druecken und Loslassen beurteilt:
 *     sofort (< 100 ms)
 *     erst mit der Bewegung   -- kam innerhalb 100 ms nach der ersten
 *                               Bewegung, und die war spaeter
 *     erst mit dem naechsten Klick
 *     kam nach N ms            -- keins davon
 *     kam in 5 s nicht an
 *
 *  Ergebnisse in KLICKTEST.TXT (Urteile und alle Zeitpunkte).
 *  Esc beendet.  kernel32, user32, gdi32.  Kein CRT.
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

/*  Ereignisse, beide Seiten, mit GetTickCount.  */
enum { E_T_AB, E_T_AUF, E_T_BEW, E_W_AB, E_W_AUF };
static const char *e_name[] = { "Treiber ab ", "Treiber auf", "Treiber Bewegung",
                                "Windows ab ", "Windows auf" };
#define MAXE 4000
static volatile DWORD e_t[MAXE];
static volatile int   e_art[MAXE];
static volatile LONG  n_e;
static CRITICAL_SECTION cs;

static HANDLE geraet = INVALID_HANDLE_VALUE, datei = INVALID_HANDLE_VALUE;
static HWND   fenster;
static HFONT  gross;
static volatile LONG ende;

static int    schritt = 0, versuch = 0;        /* 0..2, 0..2 */
static DWORD  v_beginn;                        /* Treiber-ab des Versuchs */
static LONG   v_index;                         /* Ereignisindex davon */
static char   meldung[20][120];
static int    n_meldung;

static const char *anweisung[3] = {
    "Einmal klicken -- dann 5 Sekunden NICHTS tun (Maus liegen lassen).",
    "Einmal klicken -- dann SOFORT die Maus bewegen.",
    "Einmal klicken -- nach etwa 2 Sekunden NOCH EINMAL klicken."
};

static void
ereignis (int art)
{
    LONG i;
    EnterCriticalSection(&cs);
    i = n_e;
    if (i < MAXE) {
        e_t[i] = GetTickCount();
        e_art[i] = art;
        n_e = i + 1;
    }
    LeaveCriticalSection(&cs);
}

static void
schreiben (const char *s)
{
    DWORD w;
    if (datei != INVALID_HANDLE_VALUE) {
        WriteFile(datei, s, lstrlenA(s), &w, NULL);
        WriteFile(datei, "\r\n", 2, &w, NULL);
    }
}

static DWORD WINAPI
beobachter (LPVOID p)
{
    NTU_MAUS_ZUSTAND z;
    DWORD            got;
    unsigned long    roh_vorher = 0, ber_vorher = 0;
    int              erst = 1;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (!ende) {
        if (DeviceIoControl(geraet, IOCTL_NTU_MAUS_ZUSTAND, NULL, 0, &z, sizeof(z),
                            &got, NULL) && got == sizeof(z)) {
            if (!erst) {
                if ((z.Roh & 1) && !(roh_vorher & 1)) ereignis(E_T_AB);
                else if (!(z.Roh & 1) && (roh_vorher & 1)) ereignis(E_T_AUF);
                else if (z.Berichte != ber_vorher && z.Roh == roh_vorher) ereignis(E_T_BEW);
            }
            erst = 0;
            roh_vorher = z.Roh;
            ber_vorher = z.Berichte;
        }
        Sleep(1);
    }
    return 0;
}

/*  Erstes Ereignis der Art ab Index i (und nicht vor Zeit t), sonst -1.  */
static LONG
suche (LONG i, int art, DWORD t)
{
    for (; i < n_e; i += 1) {
        if (e_art[i] == art && (LONG)(e_t[i] - t) >= -50) return i;
    }
    return -1;
}

/*  Urteil fuer eine Windows-Meldung (Art w), die zum Treiber-Ereignis zur
 *  Zeit t gehoert.  "bew" und "naechst" sind die Zeiten der ersten
 *  Bewegung und des naechsten Treiber-ab danach (0 = keine).  */
static void
urteil (char *s, const char *was, LONG iw, DWORD t, DWORD bew, DWORD naechst)
{
    if (iw < 0) {
        wsprintfA(s, "%s: kam in 5 s NICHT an", was);
        return;
    }
    {
        DWORD w = e_t[iw];
        LONG  d = (LONG)(w - t);
        if (d < 100) {
            wsprintfA(s, "%s: sofort (%ld ms)", was, d);
        } else if (bew && (LONG)(w - bew) >= -20 && (LONG)(w - bew) < 100) {
            wsprintfA(s, "%s: nach %ld ms -- erst mit der BEWEGUNG", was, d);
        } else if (naechst && (LONG)(w - naechst) >= -20 && (LONG)(w - naechst) < 150) {
            wsprintfA(s, "%s: nach %ld ms -- erst mit dem NAECHSTEN KLICK", was, d);
        } else {
            wsprintfA(s, "%s: nach %ld ms", was, d);
        }
    }
}

static void
melde (const char *s)
{
    int k;
    if (n_meldung == 20) {
        for (k = 1; k < 20; k += 1) lstrcpyA(meldung[k - 1], meldung[k]);
        n_meldung = 19;
    }
    lstrcpynA(meldung[n_meldung++], s, 120);
    schreiben(s);
}

/*  Einen Versuch auswerten, 5,5 s nach seinem Beginn.  */
static void
auswerten (void)
{
    LONG  i_auf, i_wab, i_wauf, i_bew, i_next, k;
    DWORD t_ab = v_beginn, t_auf, bew = 0, naechst = 0;
    char  a[120], b[120], s[260];

    i_auf  = suche(v_index + 1, E_T_AUF, t_ab);
    t_auf  = (i_auf >= 0) ? e_t[i_auf] : t_ab;
    i_bew  = suche(i_auf >= 0 ? i_auf + 1 : v_index + 1, E_T_BEW, t_auf);
    i_next = suche(v_index + 1, E_T_AB, t_ab + 1);
    if (i_bew >= 0)  bew = e_t[i_bew];
    if (i_next >= 0) naechst = e_t[i_next];

    /*  Windows ist oft SCHNELLER als das Abfragen (Takt 10-15 ms): seine
     *  Meldung steht dann im Protokoll VOR dem Treiberereignis.  Also ein
     *  Stueck davor anfangen und nur nach Zeit (ab 50 ms vorher) suchen --
     *  die erste Fassung suchte erst ab dem Treiberereignis und meldete
     *  in QEMU faelschlich "kam nicht an".  */
    i_wab  = suche(v_index > 20 ? v_index - 20 : 0, E_W_AB, t_ab);
    i_wauf = suche(i_auf > 20 ? i_auf - 20 : 0, E_W_AUF, t_auf);
    if (i_wab >= 0 && (LONG)(e_t[i_wab] - t_ab) > 5500) i_wab = -1;
    if (i_wauf >= 0 && (LONG)(e_t[i_wauf] - t_auf) > 5500) i_wauf = -1;

    urteil(a, "Druecken ", i_wab, t_ab, bew, naechst);
    urteil(b, "Loslassen", i_wauf, t_auf, bew, naechst);
    wsprintfA(s, "Schritt %d, Versuch %d:  %s   |   %s", schritt + 1, versuch + 1, a, b);
    melde(s);

    /*  Die Zeitpunkte dieses Versuchs in die Datei.  */
    for (k = v_index; k < n_e && (LONG)(e_t[k] - t_ab) < 5500; k += 1) {
        wsprintfA(s, "      %6ld ms  %s", (LONG)(e_t[k] - t_ab), e_name[e_art[k]]);
        schreiben(s);
    }
}

static LRESULT CALLBACK
proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_LBUTTONDOWN: ereignis(E_W_AB);  return 0;
        case WM_LBUTTONUP:   ereignis(E_W_AUF); return 0;

        case WM_TIMER:
            if (schritt >= 3) return 0;
            if (v_beginn == 0) {
                /*  Auf den Treiber-Druck warten, der den Versuch beginnt.  */
                LONG i = suche(v_index, E_T_AB, 0);
                if (i >= 0) { v_index = i; v_beginn = e_t[i]; InvalidateRect(w, NULL, TRUE); }
            } else if ((LONG)(GetTickCount() - v_beginn) > 5500) {
                auswerten();
                /*  Naechster Versuch beginnt mit dem naechsten Druck nach
                 *  diesem Versuch -- in Schritt 3 ist der zweite Klick
                 *  Teil DIESES Versuchs, also danach.  */
                {
                    LONG k = v_index + 1;
                    while (k < n_e && (LONG)(e_t[k] - v_beginn) <= 5500) k += 1;
                    v_index = k;
                }
                v_beginn = 0;
                if (++versuch == 3) { versuch = 0; schritt += 1; }
                if (schritt == 3) melde("Fertig.  Ergebnisse in KLICKTEST.TXT -- Esc beendet.");
                InvalidateRect(w, NULL, TRUE);
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(w);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(w, &ps);
            HFONT       af;
            char        s[200];
            int         k;
            SetBkMode(dc, TRANSPARENT);
            af = (HFONT)SelectObject(dc, gross);
            if (geraet == INVALID_HANDLE_VALUE) {
                lstrcpyA(s, "USB-Treiber nicht erreichbar -- ist USBNT eingerichtet?");
            } else if (schritt < 3) {
                wsprintfA(s, "Schritt %d von 3, Versuch %d von 3:", schritt + 1, versuch + 1);
            } else {
                lstrcpyA(s, "Fertig -- Esc beendet.");
            }
            TextOutA(dc, 30, 30, s, lstrlenA(s));
            if (schritt < 3 && geraet != INVALID_HANDLE_VALUE) {
                TextOutA(dc, 30, 70, anweisung[schritt], lstrlenA(anweisung[schritt]));
                lstrcpyA(s, v_beginn ? "... Klick gesehen -- bitte warten, bis der naechste Versuch kommt"
                                     : "Jetzt klicken (mit der USB-Maus).");
                TextOutA(dc, 30, 110, s, lstrlenA(s));
            }
            SelectObject(dc, af);
            for (k = 0; k < n_meldung; k += 1) {
                TextOutA(dc, 30, 170 + k * 18, meldung[k], lstrlenA(meldung[k]));
            }
            EndPaint(w, &ps);
            return 0;
        }

        case WM_DESTROY:
            ende = 1;
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
    DWORD     id;
    char      pfad[MAX_PATH], *s;
    int       i;

    InitializeCriticalSection(&cs);
    for (i = 0; i < 5 && geraet == INVALID_HANDLE_VALUE; i += 1) {
        geraet = CreateFileA(namen[i], GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    GetModuleFileNameA(NULL, pfad, sizeof(pfad));
    for (s = pfad + lstrlenA(pfad); s > pfad && s[-1] != '\\'; s -= 1) {
    }
    lstrcpyA(s, "KLICKTEST.TXT");
    datei = CreateFileA(pfad, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    schreiben("KLICKTEST -- Urteile je Versuch, darunter die Zeitpunkte relativ zum Druecken im Treiber.");

    gross = CreateFontA(24, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, 0,
                        VARIABLE_PITCH | FF_SWISS, "Arial");
    memset(&k, 0, sizeof(k));
    k.lpfnWndProc   = proz;
    k.hInstance     = GetModuleHandleA(NULL);
    k.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    k.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    k.lpszClassName = "KlickTest";
    RegisterClassA(&k);
    fenster = CreateWindowExA(WS_EX_TOPMOST, "KlickTest", "Klicktest", WS_POPUP | WS_VISIBLE,
                              0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                              NULL, NULL, k.hInstance, NULL);
    SetForegroundWindow(fenster);
    SetFocus(fenster);

    if (geraet != INVALID_HANDLE_VALUE) {
        HANDLE h = CreateThread(NULL, 0, beobachter, NULL, 0, &id);
        if (h != NULL) CloseHandle(h);
    }
    SetTimer(fenster, 1, 50, NULL);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (datei != INVALID_HANDLE_VALUE) CloseHandle(datei);
    return 0;
}

void WINAPI
Einstieg (void)
{
    ExitProcess((UINT)hauptteil());
}
