/*
 *  werkzeug/sondertest.c -- SONDERTEST.EXE: Sondertasten pruefen.
 *  Fenster, 10 s: jede Taste, die bei Windows ankommt (WM_KEYDOWN und
 *  WM_SYSKEYDOWN), dann Zustand und Auftraege vom Treiber.  Ergebnis in
 *  SONDERTEST.TXT.  kernel32, user32.  Kein CRT.
 */
#include <windows.h>
#define NTU_SONDER_TABELLE
#include "../ober/ntusbioctl.h"
#include "../ober/ntsonder.h"

void *memset (void *z, int c, size_t n) { unsigned char *p = z; while (n--) *p++ = (unsigned char)c; return z; }

static HANDLE datei = INVALID_HANDLE_VALUE;
static DWORD  t0;

static void
zeile (const char *s)
{
    DWORD w;
    WriteFile(datei, s, lstrlenA(s), &w, NULL);
    WriteFile(datei, "\r\n", 2, &w, NULL);
}

static LRESULT CALLBACK
proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    char z[80];
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        wsprintfA(z, "%6lu ms  Windows: %s VK %02X (Scan %02X%s)", GetTickCount() - t0,
                  m == WM_SYSKEYDOWN ? "SYSKEYDOWN" : "KEYDOWN   ", (unsigned)wp,
                  (unsigned)((lp >> 16) & 0xFF), (lp & (1 << 24)) ? ", E0" : "");
        zeile(z);
        return 0;
    }
    if (m == WM_SYSKEYUP && wp == VK_MENU) return 0;      /* kein Systemmenue */
    if (m == WM_TIMER) { DestroyWindow(w); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(w, m, wp, lp);
}

void WINAPI
Einstieg (void)
{
    static const char *namen[] = { "\\\\.\\UsbNt", "\\\\.\\UhciNt", "\\\\.\\OhciNt",
                                   "\\\\.\\EhciNt", "\\\\.\\XhciNt" };
    WNDCLASSA          k;
    MSG                msg;
    HWND               f;
    HANDLE             g = INVALID_HANDLE_VALUE;
    NTU_SONDER_ZUSTAND z;
    NTU_SONDER_AUFTRAG a;
    DWORD              got;
    char               s[300];
    int                i;

    datei = CreateFileA("C:\\TEMP\\SONDERTEST.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, 0, NULL);
    t0 = GetTickCount();
    memset(&k, 0, sizeof(k));
    k.lpfnWndProc = proz; k.hInstance = GetModuleHandleA(NULL);
    k.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); k.lpszClassName = "SonderTest";
    RegisterClassA(&k);
    f = CreateWindowExA(WS_EX_TOPMOST, "SonderTest", "Sondertest", WS_POPUP | WS_VISIBLE,
                        0, 0, 400, 200, NULL, NULL, k.hInstance, NULL);
    SetForegroundWindow(f); SetFocus(f);
    SetTimer(f, 1, 10000, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }

    for (i = 0; i < 5 && g == INVALID_HANDLE_VALUE; i += 1) {
        g = CreateFileA(namen[i], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    }
    if (g != INVALID_HANDLE_VALUE &&
        DeviceIoControl(g, IOCTL_NTU_SONDER_ZUSTAND, NULL, 0, &z, sizeof(z), &got, NULL)) {
        wsprintfA(s, "Treiber: %lu Tastaturen mit Medientasten, %lu Auftraege wartend", z.Tastaturen, z.Wartend);
        zeile(s);
        for (i = 0; i < NTU_SONDER_ANZAHL; i += 1) {
            if (z.Gesehen & (1ul << i)) {
                NTU_SONDER_BELEGUNG b;
                b.Index = (unsigned long)i; b.Text[0] = 0;
                DeviceIoControl(g, IOCTL_NTU_SONDER_HOLEN, &b, sizeof(b), &b, sizeof(b), &got, NULL);
                wsprintfA(s, "  gesehen: %-20s Belegung: %s", ntu_sonder_tabelle[i].name, b.Text);
                zeile(s);
            }
        }
        while (DeviceIoControl(g, IOCTL_NTU_SONDER_ABHOLEN, NULL, 0, &a, sizeof(a), &got, NULL) && got) {
            wsprintfA(s, "  Auftrag: %s (%s)", a.Text, ntu_sonder_tabelle[a.Index].name);
            zeile(s);
        }
    } else {
        zeile("Treiber: Sondertasten-Abfrage nicht moeglich");
    }
    CloseHandle(datei);
    ExitProcess(0);
}
