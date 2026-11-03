/*
 *  werkzeug/mauslatenz.c -- MAUSLATENZ.EXE: wie lange braucht ein Klick
 *  vom USB-Treiber bis ins Fenster?
 *
 *  Ein Fenster ueber den ganzen Schirm -- jeder Klick landet hier.  Je
 *  Klick drei Zeitpunkte:
 *
 *    T  Treiber: ein eigener Faden fragt den Treiber staendig nach dem
 *       Tastenzustand (IOCTL_NTU_MAUS_ZUSTAND, wie die Systemsteuerung
 *       "USB-Maus") und merkt sich GetTickCount bei jedem Druck auf Taste 1.
 *       Gepaart wird der Reihe nach: k-ter Druck im Treiber zu k-tem Klick.
 *    R  Windows: GetMessageTime() der WM_LBUTTONDOWN -- wann der
 *       Eingabefaden von Windows (RIT) die Nachricht erzeugt hat.
 *    F  Fenster: GetTickCount beim Eintreffen der Nachricht.
 *
 *  Angezeigt:  T->R  (Treiber bis Windows)   R->F  (Windows bis Fenster)
 *
 *  Sind beide klein und dauert eine Reaktion trotzdem Sekunden, liegt es
 *  nicht an der Eingabe, sondern am Programm, das reagiert (etwa weil es
 *  Laufwerke abfragt).  Aufloesung: der Windows-Takt, 10 bis 16 ms.
 *
 *  Ergebnisse auch in MAUSLATENZ.TXT neben dem Programm.  Beenden: Esc.
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

static HANDLE        geraet = INVALID_HANDLE_VALUE;
/*  Paarung der Reihe nach: der k-te Tastendruck im Treiber gehoert zum
 *  k-ten WM_LBUTTONDOWN.  Die erste Fassung nahm "den letzten" -- Windows
 *  ist aber schneller als das Abfragen (Takt 10-15 ms), und so wurde die
 *  Zeit eines Klicks mit dem NAECHSTEN verrechnet: gemessen wurde der
 *  Klickabstand, nicht die Verzoegerung.  */
#define RING 256
static volatile DWORD t_ring[RING];   /* GetTickCount je Tastendruck im Treiber */
static volatile LONG  gepaart[RING];  /* 1: einem Windows-Klick zugeordnet */
static LONG           verloren;       /* Treiber-Klicks, die nie ankamen */
static HANDLE         roh = INVALID_HANDLE_VALUE;   /* MAUSROH.TXT */
static CRITICAL_SECTION roh_cs;

static void
roh_schreiben (const char *s)
{
    DWORD w;
    if (roh == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&roh_cs);
    WriteFile(roh, s, lstrlenA(s), &w, NULL);
    LeaveCriticalSection(&roh_cs);
}
static volatile LONG  n_treiber;      /* Tastendruecke im Treiber gesehen */
static LONG           n_windows;      /* WM_LBUTTONDOWN bekommen */
static volatile LONG  ende;
static HWND          fenster;
static char          zeilen[12][100];
static int           n_zeilen, n_klicks;
static HANDLE        datei = INVALID_HANDLE_VALUE;

static DWORD WINAPI
beobachter (LPVOID p)
{
    NTU_MAUS_ZUSTAND z;
    DWORD            got;
    unsigned long    vorher = 0;

    /*  Etwas hoeher als normal, damit das Abfragen nicht selbst
     *  verzoegert -- aber unter dem Eingabefaden von Windows, den es
     *  nicht verdraengen darf.  */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    while (!ende) {
        if (DeviceIoControl(geraet, IOCTL_NTU_MAUS_ZUSTAND, NULL, 0, &z, sizeof(z),
                            &got, NULL) && got == sizeof(z)) {
            if ((z.Roh & 1) && !(vorher & 1)) {
                char  zz[64];
                DWORD t = GetTickCount();
                t_ring[n_treiber % RING] = t;
                gepaart[n_treiber % RING] = 0;
                wsprintfA(zz, "T %lu  Treiber-Klick %ld\r\n", t, n_treiber + 1);
                roh_schreiben(zz);
                InterlockedIncrement((LONG *)&n_treiber);
            }
            vorher = z.Roh;
        }
        Sleep(1);                     /* ein Takt; genauer geht es nicht */
    }
    return 0;
}

static void
zeile (const char *s)
{
    DWORD w;
    int   k;

    if (n_zeilen == 12) {
        for (k = 1; k < 12; k += 1) lstrcpyA(zeilen[k - 1], zeilen[k]);
        n_zeilen = 11;
    }
    lstrcpynA(zeilen[n_zeilen++], s, 100);
    if (datei != INVALID_HANDLE_VALUE) {
        WriteFile(datei, s, lstrlenA(s), &w, NULL);
        WriteFile(datei, "\r\n", 2, &w, NULL);
    }
    InvalidateRect(fenster, NULL, TRUE);
}

static LRESULT CALLBACK
proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {

        case WM_LBUTTONDOWN: {
            DWORD f = GetTickCount();
            DWORD r = (DWORD)GetMessageTime();
            char  s[100];

            n_klicks += 1;
            n_windows += 1;
            if (geraet == INVALID_HANDLE_VALUE) {
                wsprintfA(s, "Klick %d:  Windows -> Fenster %lu ms  (Treiber nicht erreichbar)",
                          n_klicks, f - r);
            } else {
                /*  Hat das Abfragen diesen Tastendruck noch nicht gesehen,
                 *  kurz darauf warten -- Windows ist oft schneller.  */
                int  w;
                for (w = 0; w < 200 && n_treiber < n_windows; w += 1) {
                    Sleep(1);
                }
                {
                    char  zz[100];
                    LONG  k, naechst = -1, aelteste = -1, n = n_treiber;
                    wsprintfA(zz, "W %lu  Windows-Klick %d (Nachrichtenzeit %lu)\r\n", f, n_klicks, r);
                    roh_schreiben(zz);

                    /*  Zum zeitlich naechsten ungepaarten Treiber-Klick davor
                     *  (hoechstens 15 s zurueck), und zum aeltesten ungepaarten
                     *  ("der Reihe nach").  */
                    for (k = n - 1; k >= 0 && k >= n - RING; k -= 1) {
                        DWORD t = t_ring[k % RING];
                        if ((LONG)(r - t) > 15000) break;
                        if (gepaart[k % RING]) continue;
                        if ((LONG)(r - t) >= -50) {
                            if (naechst < 0) naechst = k;
                            aelteste = k;
                        }
                    }
                    /*  Aeltere ungepaarte als der hier genommene zaehlen als
                     *  verloren -- Windows hat sie nie gemeldet.  */
                    if (naechst >= 0) {
                        LONG v = 0;
                        for (k = aelteste; k < naechst; k += 1) {
                            if (!gepaart[k % RING]) { gepaart[k % RING] = 1; v += 1; }
                        }
                        gepaart[naechst % RING] = 1;
                        verloren += v;
                        wsprintfA(s, "Klick %d:  Treiber -> Windows %ld ms (zum naechsten)   "
                                     "der Reihe nach %ld ms   verloren bisher %ld",
                                  n_klicks, (LONG)(r - t_ring[naechst % RING]),
                                  (LONG)(r - t_ring[aelteste % RING]), verloren);
                    } else {
                        wsprintfA(s, "Klick %d:  kein Treiber-Klick davor -- andere Maus?  "
                                     "verloren bisher %ld", n_klicks, verloren);
                    }
                    n_windows = n_treiber;
                }
            }
            zeile(s);
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(w);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(w, &ps);
            int         k;
            const char *kopf1 = "MAUSLATENZ -- mit der USB-Maus irgendwo klicken.  Esc beendet.";
            const char *kopf2 = "Je Klick: Abstand zum naechsten Treiber-Klick, zum aeltesten "
                                "offenen (der Reihe nach), und wie viele nie ankamen.";
            SetBkMode(dc, TRANSPARENT);
            TextOutA(dc, 20, 20, kopf1, lstrlenA(kopf1));
            TextOutA(dc, 20, 40, kopf2, lstrlenA(kopf2));
            for (k = 0; k < n_zeilen; k += 1) {
                TextOutA(dc, 20, 80 + k * 20, zeilen[k], lstrlenA(zeilen[k]));
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

    for (i = 0; i < 5 && geraet == INVALID_HANDLE_VALUE; i += 1) {
        geraet = CreateFileA(namen[i], GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }

    GetModuleFileNameA(NULL, pfad, sizeof(pfad));
    for (s = pfad + lstrlenA(pfad); s > pfad && s[-1] != '\\'; s -= 1) {
    }
    lstrcpyA(s, "MAUSLATENZ.TXT");
    datei = CreateFileA(pfad, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    InitializeCriticalSection(&roh_cs);
    lstrcpyA(s, "MAUSROH.TXT");
    roh = CreateFileA(pfad, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);

    memset(&k, 0, sizeof(k));
    k.lpfnWndProc   = proz;
    k.hInstance     = GetModuleHandleA(NULL);
    k.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    k.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    k.lpszClassName = "MausLatenz";
    RegisterClassA(&k);

    fenster = CreateWindowExA(WS_EX_TOPMOST, "MausLatenz", "Mauslatenz", WS_POPUP | WS_VISIBLE,
                              0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                              NULL, NULL, k.hInstance, NULL);
    SetForegroundWindow(fenster);
    SetFocus(fenster);

    if (geraet != INVALID_HANDLE_VALUE) {
        HANDLE h = CreateThread(NULL, 0, beobachter, NULL, 0, &id);
        if (h != NULL) CloseHandle(h);
    } else {
        zeile("USB-Treiber nicht erreichbar -- gemessen wird nur Windows -> Fenster.");
    }

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (datei != INVALID_HANDLE_VALUE) CloseHandle(datei);
    if (roh != INVALID_HANDLE_VALUE) CloseHandle(roh);
    return 0;
}

void WINAPI
Einstieg (void)
{
    ExitProcess((UINT)hauptteil());
}
