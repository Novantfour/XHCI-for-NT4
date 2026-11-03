/*
 *  werkzeug/eingabetest.c -- was kommt bei Windows an?
 *
 *      eingabetest tasten  [ms]   Tastaturereignisse der Konsole zaehlen:
 *                                 Druecken, Wiederholungen, Loslassen
 *      eingabetest maus    [ms]   linken Mausknopf ueber GetAsyncKeyState
 *                                 (unter NT 4.0 aus diesem Prozess NICHT
 *                                 verlaesslich -- klemmt auch bei PS/2)
 *      eingabetest fenster [ms]   bildschirmfuellendes Fenster ganz vorn,
 *                                 zaehlt, was eine Anwendung bekommt:
 *                                 Knopf runter/hoch, Bewegung mit
 *                                 gehaltenem Knopf, Doppelklick, Tasten
 *
 *  Gemessen wird hinter USER, also genau das, was jede Anwendung sieht --
 *  nicht nur das, was der Treiber an kbdclass gibt.  Das Programm holt
 *  sein Konsolenfenster nach vorn, damit die Tastatureingabe dort landet.
 *
 *  Nur kernel32 und user32.
 */

#include <windows.h>
#include "ntbasis.h"

static void
nach_vorn (void)
{
    HWND w;

    SetConsoleTitleA("EINGABETEST");
    Sleep(200);
    w = FindWindowA(NULL, "EINGABETEST");

    if (w != NULL) {
        ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
    }
}

static void
tasten (DWORD dauer)
{
    HANDLE        ein;
    DWORD         anfang, n;
    INPUT_RECORD  r[32];
    unsigned long druck[256], wied[256], los[256];
    unsigned long k;

    memset(druck, 0, sizeof(druck));
    memset(wied, 0, sizeof(wied));
    memset(los, 0, sizeof(los));

    ein = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, 0, NULL);

    if (ein == INVALID_HANDLE_VALUE) {
        printf("  keine Konsole\n");
        return;
    }

    nach_vorn();
    FlushConsoleInputBuffer(ein);
    printf("\n  %lu ms lang Tastaturereignisse ...\n", dauer);

    anfang = GetTickCount();

    while (GetTickCount() - anfang < dauer) {

        if (WaitForSingleObject(ein, 50) != WAIT_OBJECT_0) {
            continue;
        }

        if (!ReadConsoleInputA(ein, r, 32, &n)) {
            break;
        }

        for (k = 0; k < n; k += 1) {

            KEY_EVENT_RECORD *e;
            unsigned          vk;

            if (r[k].EventType != KEY_EVENT) {
                continue;
            }

            e  = &r[k].Event.KeyEvent;
            vk = e->wVirtualKeyCode & 0xFF;

            /*  Eine Wiederholung erkennt USER daran, dass die Taste schon
             *  unten war.  In der Konsole erscheint sie als weiteres
             *  KeyDown, manchmal zusammengefasst ueber wRepeatCount.  */

            if (e->bKeyDown) {
                if (druck[vk] == los[vk]) {
                    druck[vk] += 1;
                    wied[vk]  += e->wRepeatCount - 1;
                } else {
                    wied[vk]  += e->wRepeatCount;
                }
            } else {
                los[vk] += 1;
            }
        }
    }

    CloseHandle(ein);

    printf("  Taste  Druecken  Wiederholungen  Loslassen\n");

    for (k = 0; k < 256; k += 1) {
        if (druck[k] || wied[k] || los[k]) {
            printf("  %02lx %c   %6lu    %10lu     %6lu\n", k,
                   (k >= 'A' && k <= 'Z') ? (char)k : ' ',
                   druck[k], wied[k], los[k]);
        }
    }

    printf("\n");
}

static void
maus (DWORD dauer)
{
    DWORD anfang = GetTickCount(), t;
    int   war = -1, ist;
    unsigned long wechsel = 0, proben = 0, unten = 0;

    printf("\n  %lu ms lang den linken Mausknopf beobachten ...\n", dauer);

    while ((t = GetTickCount() - anfang) < dauer) {

        ist = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
        proben += 1;
        unten  += (unsigned long)ist;

        if (ist != war) {
            if (war >= 0) {
                wechsel += 1;
            }
            printf("  %5lu ms  %s\n", t, ist ? "gedrueckt" : "los");
            war = ist;
        }

        Sleep(20);
    }

    printf("  %lu Wechsel, %lu von %lu Proben gedrueckt\n\n",
           wechsel, unten, proben);
}

/* ==================================================================== *
 *  Fensternachrichten -- was eine Anwendung wirklich bekommt
 * ==================================================================== */

static unsigned long f_runter, f_hoch, f_doppel, f_zug, f_frei;
static unsigned long f_rrunter, f_rhoch, f_taste, f_wied, f_los;
static DWORD         f_anfang;
static char          f_folge[512];
static int           f_folge_n;

static void
folge (char c)
{
    if (f_folge_n + 1 < (int)sizeof(f_folge) &&
        (f_folge_n == 0 || f_folge[f_folge_n - 1] != c || c != 'z')) {
        f_folge[f_folge_n++] = c;
        f_folge[f_folge_n]   = '\0';
    }
}

static char f_log[4096];
static int  f_log_n;

static void
protokoll (const char *name, WPARAM wp)
{
    char z[48];
    int  i = 0;
    unsigned long t = GetTickCount() - f_anfang;
    const char *q;

    /*  "  1234 NAME wp" von Hand zusammensetzen -- ntbasis hat kein
     *  sprintf mit mehreren Werten.  */

    char zahl[12]; int n = 0;
    do { zahl[n++] = (char)('0' + t % 10); t /= 10; } while (t);
    while (i < 6 - n) z[i++] = ' ';
    while (n) z[i++] = zahl[--n];
    z[i++] = ' ';
    for (q = name; *q && i < 40; q++) z[i++] = *q;
    z[i++] = ' ';
    z[i++] = "0123456789abcdef"[(wp >> 4) & 15];
    z[i++] = "0123456789abcdef"[wp & 15];
    z[i++] = '\n';
    z[i] = '\0';

    if (f_log_n + i + 1 < (int)sizeof(f_log)) {
        memcpy(f_log + f_log_n, z, (unsigned)i + 1);
        f_log_n += i;
    }
}

static LRESULT CALLBACK
fensterproz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case 0x0215: protokoll("CAPTURECHANGED", wp); break;
        case WM_CANCELMODE: protokoll("CANCELMODE", wp); break;
        case WM_ACTIVATE:   protokoll("ACTIVATE", wp); break;
        case WM_ACTIVATEAPP: protokoll("ACTIVATEAPP", wp); break;
        case WM_KILLFOCUS:  protokoll("KILLFOCUS", wp); break;
        case WM_SETFOCUS:   protokoll("SETFOCUS", wp); break;
    }

    switch (m) {
        case WM_LBUTTONDOWN:   f_runter += 1; folge('R'); protokoll("LBUTTONDOWN", wp); SetCapture(w); return 0;
        case WM_LBUTTONUP:     f_hoch   += 1; folge('H'); protokoll("LBUTTONUP", wp); ReleaseCapture(); return 0;
        case WM_LBUTTONDBLCLK: f_doppel += 1; folge('D'); protokoll("LBUTTONDBLCLK", wp); return 0;
        case WM_RBUTTONDOWN:   f_rrunter += 1; folge('r'); return 0;
        case WM_RBUTTONUP:     f_rhoch   += 1; folge('h'); return 0;
        case WM_MOUSEMOVE:
            if (wp & MK_LBUTTON) { f_zug += 1; folge('z'); }
            else                 { f_frei += 1; }
            protokoll("MOUSEMOVE", wp);
            return 0;
        case WM_KEYDOWN:
            if (lp & 0x40000000) f_wied += 1; else f_taste += 1;
            return 0;
        case WM_KEYUP:         f_los += 1; return 0;
        case WM_TIMER:         PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static void
fenster (DWORD dauer)
{
    WNDCLASSA k;
    HWND      w;
    MSG       msg;

    memset(&k, 0, sizeof(k));
    k.style         = CS_DBLCLKS;
    k.lpfnWndProc   = fensterproz;
    k.hInstance     = GetModuleHandleA(NULL);
    k.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    k.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    k.lpszClassName = "EINGABEFLAECHE";
    RegisterClassA(&k);

    w = CreateWindowExA(WS_EX_TOPMOST, "EINGABEFLAECHE", "EINGABEFLAECHE",
                        WS_POPUP | WS_VISIBLE, 0, 0,
                        GetSystemMetrics(SM_CXSCREEN),
                        GetSystemMetrics(SM_CYSCREEN),
                        NULL, NULL, k.hInstance, NULL);

    SetForegroundWindow(w);
    SetFocus(w);
    SetTimer(w, 1, dauer, NULL);
    f_anfang = GetTickCount();

    printf("\n  %lu ms lang Fensternachrichten ...\n", dauer);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DestroyWindow(w);

    printf("  linker Knopf    runter %lu   hoch %lu   Doppelklick %lu\n",
           f_runter, f_hoch, f_doppel);
    printf("  rechter Knopf   runter %lu   hoch %lu\n", f_rrunter, f_rhoch);
    printf("  Bewegung        mit gehaltenem Knopf %lu   ohne %lu\n",
           f_zug, f_frei);
    printf("  Tasten          gedrueckt %lu   Wiederholungen %lu   los %lu\n",
           f_taste, f_wied, f_los);
    printf("  Folge           %s\n", f_folge_n ? f_folge : "-");
    printf("                  (R runter, z Zug mit Knopf, H hoch, D doppelt)\n\n");
    printf("  Nachrichten (ms, Name, wParam):\n%s\n", f_log);
}


int
main (int argc, char **argv)
{
    DWORD dauer = (argc > 2) ? (DWORD)atoi(argv[2]) : 5000;

    if (argc > 1 && strcmp(argv[1], "tasten") == 0) {
        tasten(dauer);
    } else if (argc > 1 && (strcmp(argv[1], "maus") == 0 ||
                            strcmp(argv[1], "fenster") == 0)) {

        /*  Beide Messungen sind zurueckgezogen.  "maus" fragt
         *  GetAsyncKeyState ab und zeigte auch bei der PS/2-Maus einen
         *  klemmenden Knopf; "fenster" verschwand beim ersten Klick.
         *  Beide haetten auf echter Hardware falsche Befunde geliefert.
         *  Was traegt: ein Fenster an der Titelleiste ziehen -- das macht
         *  Windows' eigene Fensterverwaltung, und daran ist zu sehen, ob
         *  ein gehaltener Knopf gehalten bleibt.  */

        printf("\n  Diese Messung ist zurueckgezogen -- sie war unzuverlaessig.\n"
               "  Stattdessen: ein Fenster an der Titelleiste ziehen.  Folgt\n"
               "  es der Maus und bleibt beim Loslassen stehen, haelt der\n"
               "  Knopf.  Fuer die Rohdaten: der Diagnosetreiber (COM1).\n\n");
        (void)maus; (void)fenster;
        return 1;
    } else {
        printf("\n  eingabetest tasten [ms]\n\n");
        return 1;
    }

    return 0;
}
