/*
 *  werkzeug/usbsetup.c -- grafische Einrichtung fuer FreeBSD-USB unter NT 4.0.
 *
 *  Ein Fenster vor USBINST.EXE: es ruft USBINST mit den gewaehlten
 *  Einstellungen auf und zeigt dessen Ausgabe.  Die Einrichtung selbst
 *  bleibt in USBINST -- eine geprueft Logik, nicht zwei, die
 *  auseinanderlaufen.  USBINST.EXE und die Treiber muessen neben
 *  USBSETUP.EXE liegen (wie im Installer-Ordner).
 *
 *    Einrichten     usbinst ein <art> -seriell N, dann usbinst taste 4/5,
 *                   dann Parameters\XhciSchritt setzen oder entfernen,
 *                   dann Frage nach Neustart (usbinst -neustart)
 *    Ausprobieren   usbinst probe <art> -seriell N  (nichts fest eintragen)
 *    Entfernen      usbinst aus, dann Frage nach Neustart
 *                   (aus stellt auch Abgeschaltetes wieder her)
 *    beim Start     usbinst (Zustand), usbinst pruefen -- die Funde
 *                   (fremde Maus-/USB-Treiber, PS/2) zeigt vor dem
 *                   Einrichten ein Pruef-Fenster mit Haekchen; angehakte
 *                   werden vorher mit "usbinst abschalten" abgeschaltet
 *
 *  USBINST laeuft ohne Fenster (DETACHED_PROCESS); seine Standardausgabe
 *  geht ueber eine Pipe in ein Protokollfeld -- in einem eigenen Faden,
 *  damit das Fenster waehrend der Einrichtung nicht einfriert.  Solange
 *  etwas laeuft, sind die Knoepfe gesperrt.
 *
 *  Nur kernel32, user32, gdi32, advapi32.  Kein CRT.
 */

#include <windows.h>
#include "sprache.h"      /* Deutsch/Englisch/Franzoesisch nach Region */

#define ID_ART       100
#define ID_AUS       110
#define ID_COM1      111
#define ID_COM2      112
#define ID_TASTE4    120
#define ID_TASTE5    121
#define ID_VBOX      130
#define ID_EIN       140
#define ID_PROBE     141
#define ID_ENTF      142
#define ID_ENDE      143
#define ID_LOG       150

#define WM_TEXT      (WM_APP + 1)       /* lParam: Text (HeapAlloc), anhaengen */
#define WM_FERTIG    (WM_APP + 2)       /* wParam: 1 = gelungen, lParam: Aktion */

#define A_ZUSTAND    0
#define A_EIN        1
#define A_PROBE      2
#define A_ENTF       3
#define A_NEUSTART   4

static HINSTANCE inst;

/*  Mitschrift der letzten USBINST-Ausgabe (fuer "pruefen").  */
static char      mitschrift[12000];
static int       mitschrift_n;

/*  Funde aus "usbinst pruefen".  */
#define MAX_FUNDE 12
static struct {
    char kenn[128];
    char art[16];
    char text[220];
    int  an;
} funde[MAX_FUNDE];
static int n_funde;
static HWND      fenster, log_feld;
static char      ordner[MAX_PATH];         /* mit abschliessendem \ */
static HFONT     schrift, fett, fest;

static const char *arten[5]   = { "usbnt", "uhci", "ohci", "ehci", "xhci" };
static const char *dienste[5] = { "usbnt", "uhcint", "ohcint", "ehcint", "xhcint" };
static const char *art_text[5] = {
    "USBNT -- alle Controller (empfohlen)",
    "UHCINT -- nur UHCI (USB 1.1, Intel/VIA)",
    "OHCINT -- nur OHCI (USB 1.1, andere)",
    "EHCINT -- nur EHCI (USB 2.0)",
    "XHCINT -- nur xHCI (USB 3.0)"
};

/*  Maustasten: angezeigter Text, Belegung fuer USBINST.  */
static const char *tasten_text[7] = {
    "Zur\xfc" "ck (Alt+Links)", "Vorw\xe4rts (Alt+Rechts)", "Mittlere Maustaste",
    "Doppelklick", "Kopieren (Strg+C)", "Einf\xfcgen (Strg+V)", "Aus"
};
static const char *tasten_wert[7] = {
    "alt+links", "alt+rechts", "mitte", "doppelklick", "strg+c", "strg+v", "aus"
};

/*  Ohne CRT: ZeroMemory und Strukturinitialisierung rufen memset.  */
void *
memset (void *z, int c, size_t n)
{
    unsigned char *p = (unsigned char *)z;
    while (n--) *p++ = (unsigned char)c;
    return z;
}

/*  Auftrag an den Arbeitsfaden.  */
static struct {
    int  aktion;
    int  art;
    int  seriell;
    int  taste4, taste5;
    int  vbox;
} auftrag;


/* ---------------------------------------------------------------- *
 *  Protokoll
 * ---------------------------------------------------------------- */

static void
text_senden (const char *s)
{
    int   n = lstrlenA(s);
    char *p = (char *)HeapAlloc(GetProcessHeap(), 0, (DWORD)n + 1);

    if (p != NULL) {
        lstrcpyA(p, s);
        PostMessageA(fenster, WM_TEXT, 0, (LPARAM)p);
    }
}

static void
anhaengen (const char *s)
{
    int n = GetWindowTextLengthA(log_feld);
    SendMessageA(log_feld, EM_SETSEL, (WPARAM)n, (LPARAM)n);
    SendMessageA(log_feld, EM_REPLACESEL, FALSE, (LPARAM)s);
}


/* ---------------------------------------------------------------- *
 *  USBINST aufrufen
 * ---------------------------------------------------------------- */

/*  Fuehrt  USBINST.EXE <argumente>  aus, schickt die Ausgabe ins
 *  Protokoll (\n -> \r\n) und liefert den Rueckgabewert (0 = gelungen,
 *  -1 = liess sich nicht starten).  */

static int
usbinst (const char *argumente)
{
    char                zeile[MAX_PATH + 200];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    HANDLE              lesen, schreiben;
    char                puffer[512], aus[1100];
    DWORD               n, code = 1;
    char                vorher = 0;

    wsprintfA(zeile, "\"%sUSBINST.EXE\" %s", ordner, argumente);
    mitschrift_n = 0;
    mitschrift[0] = '\0';

    wsprintfA(aus, "\r\n> usbinst %s\r\n", argumente);
    text_senden(aus);

    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;
    if (!CreatePipe(&lesen, &schreiben, &sa, 0)) {
        text_senden("  (Pipe laesst sich nicht anlegen)\r\n");
        return -1;
    }
    SetHandleInformation(lesen, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = NULL;
    si.hStdOutput  = schreiben;
    si.hStdError   = schreiben;

    if (!CreateProcessA(NULL, zeile, NULL, NULL, TRUE, DETACHED_PROCESS,
                        NULL, ordner, &si, &pi)) {
        wsprintfA(aus, "  USBINST.EXE startet nicht (Fehler %lu) -- liegt es neben "
                       "USBSETUP.EXE?\r\n", GetLastError());
        text_senden(aus);
        CloseHandle(lesen);
        CloseHandle(schreiben);
        return -1;
    }
    CloseHandle(schreiben);           /* sonst endet ReadFile nie */

    while (ReadFile(lesen, puffer, sizeof(puffer), &n, NULL) && n > 0) {
        DWORD i, k = 0;
        for (i = 0; i < n && mitschrift_n < (int)sizeof(mitschrift) - 1; i += 1) {
            mitschrift[mitschrift_n++] = puffer[i];
        }
        mitschrift[mitschrift_n] = '\0';
        for (i = 0; i < n; i += 1) {
            if (puffer[i] == '\n' && vorher != '\r') {
                aus[k++] = '\r';
            }
            aus[k++] = puffer[i];
            vorher = puffer[i];
        }
        aus[k] = '\0';
        text_senden(aus);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(lesen);
    return (int)code;
}

/*  FUND|<kenn>|<art>|<text>-Zeilen aus der Mitschrift lesen.  */
static void
funde_lesen (void)
{
    char *p = mitschrift;
    n_funde = 0;
    while (*p != '\0' && n_funde < MAX_FUNDE) {
        char *ende = p, *f[4];
        int   k;
        while (*ende != '\0' && *ende != '\n') ende += 1;
        if (ende - p > 5 && p[0] == 'F' && p[1] == 'U' && p[2] == 'N' && p[3] == 'D' && p[4] == '|') {
            char zeile[600];
            int  n = (int)(ende - p);
            if (n > (int)sizeof(zeile) - 1) n = sizeof(zeile) - 1;
            memcpy(zeile, p, n);
            zeile[n] = '\0';
            if (n > 0 && zeile[n - 1] == '\r') zeile[n - 1] = '\0';
            /*  FUND|kenn|art|text: nach "FUND|" DREI Felder, also zwei
             *  Trennzeichen (die erste Fassung verlangte drei und
             *  verwarf jede Zeile -- das Pruef-Fenster erschien nie).  */
            f[0] = zeile + 5;
            for (k = 1; k < 3; k += 1) {
                char *q = f[k - 1];
                while (*q != '\0' && *q != '|') q += 1;
                if (*q == '\0') break;
                *q = '\0';
                f[k] = q + 1;
            }
            if (k == 3) {
                lstrcpynA(funde[n_funde].kenn, f[0], sizeof(funde[0].kenn));
                lstrcpynA(funde[n_funde].art,  f[1], sizeof(funde[0].art));
                lstrcpynA(funde[n_funde].text, f[2], sizeof(funde[0].text));
                funde[n_funde].an = lstrcmpA(f[1], "ps2") != 0;   /* PS/2 nicht vorwaehlen */
                n_funde += 1;
            }
        }
        p = (*ende != '\0') ? ende + 1 : ende;
    }
}

void *memcpy (void *z, const void *q, size_t n)
{
    unsigned char *a = (unsigned char *)z;
    const unsigned char *b = (const unsigned char *)q;
    while (n--) *a++ = *b++;
    return z;
}

/*  Parameters\XhciSchritt setzen (0) oder entfernen.  */

static void
xhci_schritt (int art, int setzen)
{
    char  schl[200], aus[200];
    HKEY  k;
    DWORD null = 0;

    wsprintfA(schl, "SYSTEM\\CurrentControlSet\\Services\\%s\\Parameters", dienste[art]);
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, schl, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, NULL) != ERROR_SUCCESS) {
        text_senden("  Parameters nicht beschreibbar\r\n");
        return;
    }
    if (setzen) {
        RegSetValueExA(k, "XhciSchritt", 0, REG_DWORD, (const BYTE *)&null, sizeof(null));
        wsprintfA(aus, "\r\n  %s: XhciSchritt = 0 (Intel-Schrittmodus aus, f\xfcr VirtualBox USB 3.0)\r\n",
                  dienste[art]);
    } else if (RegDeleteValueA(k, "XhciSchritt") == ERROR_SUCCESS) {
        wsprintfA(aus, "\r\n  %s: XhciSchritt entfernt (wieder Vorgabe)\r\n", dienste[art]);
    } else {
        aus[0] = '\0';
    }
    RegCloseKey(k);
    if (aus[0] != '\0') {
        text_senden(aus);
    }
}

static DWORD WINAPI
arbeiter (LPVOID p)
{
    char a[200];
    int  r = 0, gelungen = 0;

    switch (auftrag.aktion) {

        case A_ZUSTAND:
            r = usbinst("");
            gelungen = (r == 0);
            usbinst("pruefen");
            funde_lesen();
            break;

        case A_EIN: {
            int k;
            for (k = 0; k < n_funde; k += 1) {
                if (funde[k].an) {
                    wsprintfA(a, "abschalten %s", funde[k].kenn);
                    usbinst(a);
                }
            }
        }
            wsprintfA(a, "ein %s -seriell %d", arten[auftrag.art], auftrag.seriell);
            r = usbinst(a);
            gelungen = (r == 0);
            if (gelungen) {
                wsprintfA(a, "taste 4 %s", tasten_wert[auftrag.taste4]);
                usbinst(a);
                wsprintfA(a, "taste 5 %s", tasten_wert[auftrag.taste5]);
                usbinst(a);
                xhci_schritt(auftrag.art, auftrag.vbox);
            }
            break;

        case A_PROBE:
            wsprintfA(a, "probe %s -seriell %d", arten[auftrag.art], auftrag.seriell);
            gelungen = (usbinst(a) == 0);
            break;

        case A_ENTF:
            gelungen = (usbinst("aus") == 0);
            break;

        case A_NEUSTART:
            gelungen = (usbinst("-neustart") == 0);
            break;
    }

    PostMessageA(fenster, WM_FERTIG, (WPARAM)gelungen, (LPARAM)auftrag.aktion);
    return 0;
}


/* ---------------------------------------------------------------- *
 *  Fenster
 * ---------------------------------------------------------------- */

/*  Pruef-Fenster: die Funde mit Haekchen.  1 = weiter, 0 = abbrechen.  */
static int pruef_ergebnis;
static HWND pruef_fenster;

static LRESULT CALLBACK
pruef_proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        int k;
        if (LOWORD(wp) == IDOK) {
            for (k = 0; k < n_funde; k += 1) {
                funde[k].an = IsDlgButtonChecked(w, 500 + k) == BST_CHECKED;
            }
        }
        pruef_ergebnis = (LOWORD(wp) == IDOK);
        DestroyWindow(w);
        return 0;
    }
    if (m == WM_CLOSE) { pruef_ergebnis = 0; DestroyWindow(w); return 0; }
    if (m == WM_DESTROY) { pruef_fenster = NULL; return 0; }
    return DefWindowProcA(w, m, wp, lp);
}

static HWND
pkind (HWND e, const char *klasse, const char *text, DWORD stil, int x, int y, int bw, int bh, int id)
{
    HWND c = CreateWindowExA(0, klasse, text, WS_CHILD | WS_VISIBLE | stil, x, y, bw, bh,
                             e, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)schrift, FALSE);
    return c;
}

static int
pruefen_zeigen (void)
{
    static const char *art_name[][2] = {
        { "maus", "Maustreiber" }, { "autostart", "Autostart" },
        { "usb", "USB-Treiber" }, { "ps2", "PS/2" }, { 0, 0 } };
    WNDCLASSA k;
    MSG       msg;
    RECT      r;
    int       i, y, h;

    memset(&k, 0, sizeof(k));
    k.lpfnWndProc   = pruef_proz;
    k.hInstance     = inst;
    k.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    k.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    k.lpszClassName = "UsbSetupPruefen";
    RegisterClassA(&k);

    h = 110 + n_funde * 44 + 50;
    if (h > 470) h = 470;
    GetWindowRect(fenster, &r);
    pruef_fenster = CreateWindowExA(WS_EX_DLGMODALFRAME, "UsbSetupPruefen",
                                    "Vor dem Einrichten gefunden",
                                    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                    r.left + 20, r.top + 20, 560, h, fenster, NULL, inst, NULL);
    pkind(pruef_fenster, "STATIC",
          "Diese Treiber und Programme vertragen sich nicht gut mit dem USB-Treiber.\n"
          "Angehakte werden abgeschaltet -- ihr alter Zustand wird gesichert, und\n"
          "\"Entfernen\" stellt ihn wieder her.  Wirkt ab dem n\xe4" "chsten Start.",
          SS_LEFT, 12, 10, 530, 48, -1);
    y = 64;
    for (i = 0; i < n_funde && y < h - 90; i += 1) {
        char z[300];
        const char *an = funde[i].art;
        int j;
        for (j = 0; art_name[j][0] != 0; j += 1) {
            if (lstrcmpA(art_name[j][0], funde[i].art) == 0) { an = art_name[j][1]; break; }
        }
        wsprintfA(z, "%s: %s", an, funde[i].text);
        pkind(pruef_fenster, "BUTTON", z, BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP,
              16, y, 520, 34, 500 + i);
        CheckDlgButton(pruef_fenster, 500 + i, funde[i].an ? BST_CHECKED : BST_UNCHECKED);
        if (lstrcmpA(funde[i].art, "ps2") == 0) {
            pkind(pruef_fenster, "STATIC", "    Nur abschalten, wenn weder PS/2-Tastatur noch "
                  "PS/2-Maus gebraucht wird.", SS_LEFT, 32, y + 32, 500, 14, -1);
            y += 14;
        }
        y += 40;
    }
    pkind(pruef_fenster, "BUTTON", "&Weiter", BS_DEFPUSHBUTTON | WS_TABSTOP, 300, h - 70, 110, 26, IDOK);
    pkind(pruef_fenster, "BUTTON", S(T_I_ABBRECHEN), BS_PUSHBUTTON | WS_TABSTOP, 420, h - 70, 110, 26, IDCANCEL);

    EnableWindow(fenster, FALSE);
    pruef_ergebnis = 0;
    while (pruef_fenster != NULL && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(pruef_fenster, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    EnableWindow(fenster, TRUE);
    SetForegroundWindow(fenster);
    return pruef_ergebnis;
}

static void
knoepfe (BOOL an)
{
    static const int ids[] = { ID_EIN, ID_PROBE, ID_ENTF, ID_ENDE, ID_ART,
                               ID_AUS, ID_COM1, ID_COM2, ID_TASTE4, ID_TASTE5, ID_VBOX };
    int k;
    for (k = 0; k < (int)(sizeof(ids) / sizeof(ids[0])); k += 1) {
        EnableWindow(GetDlgItem(fenster, ids[k]), an);
    }
}

static void
starten (int aktion)
{
    HANDLE h;
    DWORD  id;

    auftrag.aktion = aktion;
    auftrag.art    = (int)SendDlgItemMessageA(fenster, ID_ART, CB_GETCURSEL, 0, 0);
    auftrag.seriell = IsDlgButtonChecked(fenster, ID_COM2) ? 2
                    : IsDlgButtonChecked(fenster, ID_COM1) ? 1 : 0;
    auftrag.taste4 = (int)SendDlgItemMessageA(fenster, ID_TASTE4, CB_GETCURSEL, 0, 0);
    auftrag.taste5 = (int)SendDlgItemMessageA(fenster, ID_TASTE5, CB_GETCURSEL, 0, 0);
    auftrag.vbox   = IsDlgButtonChecked(fenster, ID_VBOX) == BST_CHECKED;
    if (auftrag.art < 0) auftrag.art = 0;

    knoepfe(FALSE);
    SetCursor(LoadCursorA(NULL, IDC_WAIT));
    h = CreateThread(NULL, 0, arbeiter, NULL, 0, &id);
    if (h == NULL) {
        anhaengen("\r\n  Arbeitsfaden startet nicht.\r\n");
        knoepfe(TRUE);
    } else {
        CloseHandle(h);
    }
}

static HWND
kind (const char *klasse, const char *text, DWORD stil, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, klasse, text, WS_CHILD | WS_VISIBLE | stil,
                             x, y, w, h, fenster, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)schrift, FALSE);
    return c;
}

static void
aufbauen (void)
{
    HWND c;
    int  k;

    c = kind("STATIC", "FreeBSD-USB f\xfcr Windows NT 4.0", SS_LEFT, 12, 10, 400, 20, -1);
    SendMessageA(c, WM_SETFONT, (WPARAM)fett, FALSE);
    kind("STATIC", "Tastatur, Maus, Speicher und Drucker an USB -- "
                   "UHCI, OHCI, EHCI und xHCI.", SS_LEFT, 12, 30, 520, 16, -1);

    /*  Fassung aus der Versionsressource von USBNT.SYS daneben.  */
    {
        char   d[MAX_PATH], z[80];
        DWORD  leer, n;
        void  *v;
        char  *wert;
        UINT   l;
        wsprintfA(d, "%sUSBNT.SYS", ordner);
        n = GetFileVersionInfoSizeA(d, &leer);
        lstrcpyA(z, "");
        if (n > 0 && (v = HeapAlloc(GetProcessHeap(), 0, n)) != NULL) {
            if (GetFileVersionInfoA(d, 0, n, v) &&
                VerQueryValueA(v, "\\StringFileInfo\\040704E4\\FileVersion", (void **)&wert, &l)) {
                wsprintfA(z, "Fassung %s", wert);
            }
            HeapFree(GetProcessHeap(), 0, v);
        }
        kind("STATIC", z, SS_RIGHT, 380, 12, 150, 16, -1);
    }

    kind("BUTTON", "Treiber", BS_GROUPBOX, 10, 54, 520, 50, -1);
    c = kind("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 22, 72, 330, 160, ID_ART);
    for (k = 0; k < 5; k += 1) {
        SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)art_text[k]);
    }
    SendMessageA(c, CB_SETCURSEL, 0, 0);
    kind("STATIC", "Auf Brettern mit EHCI\nimmer USBNT nehmen.",
         SS_LEFT, 362, 70, 160, 30, -1);

    kind("BUTTON", "Einstellungen", BS_GROUPBOX, 10, 110, 520, 124, -1);
    kind("STATIC", "Diagnoseausgabe:", SS_LEFT, 22, 130, 110, 16, -1);
    kind("BUTTON", "Aus",  BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 140, 128, 50, 18, ID_AUS);
    kind("BUTTON", "COM1", BS_AUTORADIOBUTTON, 200, 128, 60, 18, ID_COM1);
    kind("BUTTON", "COM2", BS_AUTORADIOBUTTON, 270, 128, 60, 18, ID_COM2);
    /*  Vorgabe AUS: jede Zeile auf COM1 kostet auf echter Hardware Zeit
     *  (Diagnose ist fuer die Fehlersuche, nicht fuer den Alltag).  */
    CheckRadioButton(fenster, ID_AUS, ID_COM2, ID_AUS);

    kind("STATIC", "Maustaste 4:", SS_LEFT, 22, 158, 110, 16, -1);
    c = kind("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, 140, 154, 200, 160, ID_TASTE4);
    for (k = 0; k < 7; k += 1) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)tasten_text[k]);
    SendMessageA(c, CB_SETCURSEL, 0, 0);
    kind("STATIC", "Maustaste 5:", SS_LEFT, 22, 184, 110, 16, -1);
    c = kind("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 140, 180, 200, 160, ID_TASTE5);
    for (k = 0; k < 7; k += 1) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)tasten_text[k]);
    SendMessageA(c, CB_SETCURSEL, 1, 0);
    kind("STATIC", "(Seitentasten; NT 4.0 kennt sie nicht -- der Treiber \xfc" "bersetzt.)",
         SS_LEFT, 350, 158, 175, 44, -1);

    kind("BUTTON", "VirtualBox mit USB-Controller \"3.0\": Intel-Schrittmodus aus",
         BS_AUTOCHECKBOX | WS_TABSTOP, 22, 208, 420, 18, ID_VBOX);

    kind("BUTTON", "&Einrichten",   BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 10, 244, 120, 26, ID_EIN);
    kind("BUTTON", "&Ausprobieren", BS_PUSHBUTTON | WS_TABSTOP, 140, 244, 120, 26, ID_PROBE);
    kind("BUTTON", "Ent&fernen",    BS_PUSHBUTTON | WS_TABSTOP, 270, 244, 120, 26, ID_ENTF);
    kind("BUTTON", "&Beenden",      BS_PUSHBUTTON | WS_TABSTOP, 410, 244, 120, 26, ID_ENDE);

    log_feld = kind("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL |
                    WS_BORDER | WS_TABSTOP, 10, 280, 520, 124, ID_LOG);
    SendMessageA(log_feld, WM_SETFONT, (WPARAM)fest, FALSE);
    SendMessageA(log_feld, EM_LIMITTEXT, 0, 0);
}

static LRESULT CALLBACK
proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {

        case WM_TEXT:
            anhaengen((const char *)lp);
            HeapFree(GetProcessHeap(), 0, (LPVOID)lp);
            return 0;

        case WM_FERTIG:
            knoepfe(TRUE);
            SetFocus(GetDlgItem(w, ID_EIN));
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            if ((lp == A_EIN || lp == A_ENTF) && wp) {
                if (MessageBoxA(w, lp == A_EIN
                        ? "Die Einrichtung ist abgeschlossen.\n\nDer Treiber startet mit dem "
                          "System. Jetzt neu starten?"
                        : "Der Treiber ist entfernt.\n\nEr ist bis zum Neustart noch geladen. "
                          "Jetzt neu starten?",
                        "USB-Einrichtung", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    starten(A_NEUSTART);
                }
            } else if (lp == A_EIN || lp == A_PROBE || lp == A_ENTF) {
                if (!wp) {
                    MessageBoxA(w, "Das hat nicht geklappt -- Einzelheiten im Protokoll.",
                                "USB-Einrichtung", MB_OK | MB_ICONEXCLAMATION);
                }
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_EIN:
                    if (n_funde > 0 && !pruefen_zeigen()) {
                        anhaengen("\r\n  Einrichten abgebrochen.\r\n");
                        return 0;
                    }
                    starten(A_EIN);
                    return 0;
                case ID_PROBE: starten(A_PROBE); return 0;
                case ID_ENTF:
                    if (MessageBoxA(w, "Den USB-Treiber wirklich entfernen?\n\n"
                                       "Ohne ihn gehen USB-Tastatur und -Maus nicht mehr.",
                                    "USB-Einrichtung", MB_YESNO | MB_ICONWARNING) == IDYES) {
                        starten(A_ENTF);
                    }
                    return 0;
                case ID_ENDE:
                    DestroyWindow(w);
                    return 0;
            }
            break;

        case WM_CLOSE:
            if (IsWindowEnabled(GetDlgItem(w, ID_ENDE))) {
                DestroyWindow(w);
            }
            return 0;                   /* waehrend der Arbeit nicht schliessen */

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static int
hauptteil (void)
{
    WNDCLASSA k;
    RECT      r = { 0, 0, 540, 412 };     /* passt auch auf 640 x 480 */
    DWORD     stil = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    MSG       msg;
    char     *s;
    SC_HANDLE scm;

    inst = GetModuleHandleA(NULL);
    GetModuleFileNameA(NULL, ordner, sizeof(ordner));
    for (s = ordner + lstrlenA(ordner); s > ordner && s[-1] != '\\'; s -= 1) {
    }
    *s = '\0';

    /*  Ohne Administratorrechte kann nichts eingerichtet werden -- gleich
     *  sagen, statt spaeter an einem Dienstfehler zu scheitern.  */

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == NULL) {
        MessageBoxA(NULL, "Zum Einrichten sind Administratorrechte n\xf6tig.\n\n"
                          "Bitte als Administrator anmelden.", "USB-Einrichtung",
                    MB_OK | MB_ICONSTOP);
        return 1;
    }
    CloseServiceHandle(scm);

    schrift = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    fett    = CreateFontA(16, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, 0,
                          VARIABLE_PITCH | FF_SWISS, "MS Sans Serif");
    fest    = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0,
                          FIXED_PITCH | FF_MODERN, "Courier New");

    ZeroMemory(&k, sizeof(k));
    k.lpfnWndProc   = proz;
    k.hInstance     = inst;
    k.hIcon         = LoadIconA(inst, MAKEINTRESOURCEA(1));
    k.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    k.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    k.lpszClassName = "UsbSetup";
    RegisterClassA(&k);

    AdjustWindowRect(&r, stil, FALSE);
    {
        int w = r.right - r.left, h = r.bottom - r.top;
        int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2 - 12;   /* Taskleiste */
        fenster = CreateWindowExA(0, "UsbSetup", "USB-Einrichtung", stil,
                                  x < 0 ? 0 : x, y < 0 ? 0 : y, w, h,
                                  NULL, NULL, inst, NULL);
    }
    aufbauen();
    ShowWindow(fenster, SW_SHOW);
    UpdateWindow(fenster);
    SetFocus(GetDlgItem(fenster, ID_EIN));   /* Enter = Einrichten */

    starten(A_ZUSTAND);             /* Zustand gleich zeigen */

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(fenster, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return (int)msg.wParam;
}

void WINAPI
Einstieg (void)
{
    ExitProcess((UINT)hauptteil());
}
