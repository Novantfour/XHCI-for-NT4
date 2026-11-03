/*
 *  werkzeug/usbtastx.c -- USBTASTX.DLL: Reiter S(T_R_SONDERTASTEN) in der
 *  Tastatur-Systemsteuerung (main.cpl, "Tastatur").
 *
 *  Wie USBMAUSX.DLL eine Eigenschaftsseiten-Erweiterung, hier unter
 *      Controls Folder\Keyboard\shellex\PropertySheetHandlers.
 *
 *  Die Seite: eine Liste aller Sondertasten (F13..F24, Medien, System;
 *  Tabelle in ntsonder.h) mit ihrer Belegung.  Wird eine Taste gedrueckt,
 *  leuchtet ihre Zeile gruen und die Liste springt zu ihr (Abfrage alle
 *  50 ms, IOCTL_NTU_SONDER_ZUSTAND).  Darunter die Belegung der gewaehlten
 *  Taste: zur Auswahl, getippt, oder ein Programm ("Durchsuchen...").
 *  Uebernehmen/OK der Systemsteuerung: jede geaenderte Belegung an den
 *  Treiber (IOCTL_NTU_SONDER_SETZEN), sofort wirksam, in der Registry.
 *
 *  kernel32, user32, gdi32, advapi32, comctl32, comdlg32.  Kein CRT.
 */

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <prsht.h>
#define NTU_SONDER_TABELLE
#include "../ober/ntusbioctl.h"
#include "../ober/ntsonder.h"
#include "sprache.h"

void *
memset (void *z, int c, size_t n)
{
    unsigned char *p = (unsigned char *)z;
    while (n--) *p++ = (unsigned char)c;
    return z;
}

/* {6E1B7A13-3C44-4F5A-9B21-7D0C5A1E4B31} */
static const GUID CLSID_UsbMausTab =
    { 0x6e1b7a13, 0x3c44, 0x4f5a, { 0x9b, 0x21, 0x7d, 0x0c, 0x5a, 0x1e, 0x4b, 0x31 } };
#define CLSID_TEXT "{6E1B7A13-3C44-4F5A-9B21-7D0C5A1E4B31}"

static const GUID IID_Unbekannt = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_Fabrik    = { 0x00000001, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_SeitenErw = { 0x000214e9, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static int
gleich (const GUID *a, const GUID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    int i;
    for (i = 0; i < 16; i += 1) if (x[i] != y[i]) return 0;
    return 1;
}

static HINSTANCE inst;
static LONG      objekte;

/* ---------------------------------------------------------------- *
 *  Die Seite
 * ---------------------------------------------------------------- */

#define ID_LISTE    100
#define ID_STATUS   101
#define ID_WAHL     102
#define ID_SUCHEN   103
#define ID_VORGABE  104
#define ID_TASTE    105

static HWND   seite;
static HANDLE geraet = INVALID_HANDLE_VALUE;
static NTU_SONDER_ZUSTAND zustand;
static int    zustand_ok;
static char   gesetzt[NTU_SONDER_ANZAHL][NTU_SONDER_TEXT];   /* im Treiber */
static char   neu[NTU_SONDER_ANZAHL][NTU_SONDER_TEXT];       /* auf der Seite */
static int    gewaehlt = 0;
static int    fuellt;                                        /* Feld wird gesetzt */

static const char *auswahl[] = {
    "aus  (keine Wirkung)",
    "lauter  (Lautst\xe4rke +)",
    "leiser  (Lautst\xe4rke -)",
    "stumm  (Ton an/aus)",
    "programm:calc.exe  (Rechner)",
    "programm:explorer.exe  (Explorer)",
    "programm:notepad.exe  (Editor)",
    "alt+links  (Zur\xfc" "ck)",
    "alt+rechts  (Vorw\xe4rts)",
    "f5  (Aktualisieren)",
    "strg+c  (Kopieren)",
    "strg+v  (Einf\xfcgen)",
    "alt+f4  (Fenster schlie\xdf" "en)",
    "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24"
};
#define N_AUSWAHL (sizeof(auswahl) / sizeof(auswahl[0]))

static void
oeffnen (void)
{
    static const char *namen[] = { "\\\\.\\UsbNt", "\\\\.\\UhciNt", "\\\\.\\OhciNt",
                                   "\\\\.\\EhciNt", "\\\\.\\XhciNt" };
    int i;
    for (i = 0; i < 5 && geraet == INVALID_HANDLE_VALUE; i += 1) {
        geraet = CreateFileA(namen[i], GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
}

static void
abfragen (void)
{
    DWORD got = 0;
    zustand_ok = geraet != INVALID_HANDLE_VALUE &&
                 DeviceIoControl(geraet, IOCTL_NTU_SONDER_ZUSTAND, NULL, 0, &zustand,
                                 sizeof(zustand), &got, NULL) && got == sizeof(zustand);
}

static int
belegung (int i, char *text, int setzen)
{
    NTU_SONDER_BELEGUNG b;
    DWORD               got = 0;
    memset(&b, 0, sizeof(b));
    b.Index = (unsigned long)i;
    if (setzen) {
        lstrcpynA(b.Text, text, sizeof(b.Text));
        return DeviceIoControl(geraet, IOCTL_NTU_SONDER_SETZEN, &b, sizeof(b), NULL, 0, &got, NULL);
    }
    if (!DeviceIoControl(geraet, IOCTL_NTU_SONDER_HOLEN, &b, sizeof(b), &b, sizeof(b), &got, NULL)) {
        return 0;
    }
    b.Text[sizeof(b.Text) - 1] = '\0';
    lstrcpynA(text, b.Text, NTU_SONDER_TEXT);
    return 1;
}

/*  Feldtext -> Wert: bei einem Eintrag der Auswahl der Teil vor "  (".  */
static void
wert_aus_feld (char *w)
{
    char t[NTU_SONDER_TEXT + 40];
    int  i, j;
    GetWindowTextA(GetDlgItem(seite, ID_WAHL), t, sizeof(t));
    for (i = 0; t[i] == ' '; i += 1) {
    }
    for (j = 0; t[i + j] != '\0' && j < NTU_SONDER_TEXT - 1; j += 1) {
        if (t[i + j] == ' ' && t[i + j + 1] == ' ' && t[i + j + 2] == '(') break;
        w[j] = t[i + j];
    }
    w[j] = '\0';
    while (j > 0 && w[j - 1] == ' ') w[--j] = '\0';
}

static void
wert_ins_feld (const char *w)
{
    HWND c = GetDlgItem(seite, ID_WAHL);
    int  i, n = lstrlenA(w);
    fuellt = 1;
    for (i = 0; i < (int)N_AUSWAHL; i += 1) {
        if (CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, auswahl[i], n, w, n) == 2 &&
            (auswahl[i][n] == ' ' || auswahl[i][n] == '\0')) {
            SendMessageA(c, CB_SETCURSEL, (WPARAM)i, 0);
            fuellt = 0;
            return;
        }
    }
    SendMessageA(c, CB_SETCURSEL, (WPARAM)-1, 0);
    SetWindowTextA(c, w);
    fuellt = 0;
}

static RECT
du (int x, int y, int w, int h)
{
    RECT r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    MapDialogRect(seite, &r);
    return r;
}

static HWND
kind (const char *klasse, const char *text, DWORD stil, int x, int y, int w, int h, int id)
{
    RECT r = du(x, y, w, h);
    HWND c = CreateWindowExA(klasse[0] == 'L' ? WS_EX_CLIENTEDGE : 0, klasse, text,
                             WS_CHILD | WS_VISIBLE | stil, r.left, r.top,
                             r.right - r.left, r.bottom - r.top, seite, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(c, WM_SETFONT, SendMessageA(seite, WM_GETFONT, 0, 0), FALSE);
    return c;
}

static void
status_zeile (void)
{
    char z[160];
    if (geraet == INVALID_HANDLE_VALUE) {
        lstrcpyA(z, "Der USB-Treiber ist nicht geladen.");
    } else if (!zustand_ok) {
        lstrcpyA(z, "Der USB-Treiber kennt Sondertasten nicht -- bitte neuere Fassung einrichten.");
    } else {
        wsprintfA(z, "Dr\xfc" "cken Sie eine Sondertaste -- die Liste springt zu ihr.%s",
                  zustand.Tastaturen ? "" : "  (Medientasten: keine passende Tastatur angesteckt)");
    }
    SetDlgItemTextA(seite, ID_STATUS, z);
}

static void
waehlen (int i)
{
    char z[80];
    gewaehlt = i;
    SendDlgItemMessageA(seite, ID_LISTE, LB_SETCURSEL, (WPARAM)i, 0);
    wsprintfA(z, "Belegung f\xfcr \"%s\":", ntu_sonder_tabelle[i].name);
    SetDlgItemTextA(seite, ID_TASTE, z);
    wert_ins_feld(neu[i]);
}

static void
einrichten (void)
{
    int  i, j;
    HWND c;

    kind("STATIC", "", SS_LEFT, 8, 6, 230, 18, ID_STATUS);
    c = kind("LISTBOX", "", LBS_OWNERDRAWFIXED | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
             8, 26, 230, 124, ID_LISTE);
    for (i = 0; i < NTU_SONDER_ANZAHL; i += 1) {
        SendMessageA(c, LB_ADDSTRING, 0, (LPARAM)i);
    }
    kind("STATIC", "", SS_LEFT, 8, 156, 230, 10, ID_TASTE);
    c = kind("COMBOBOX", "", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
             8, 168, 170, 120, ID_WAHL);
    for (j = 0; j < (int)N_AUSWAHL; j += 1) {
        SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)auswahl[j]);
    }
    kind("BUTTON", "&Durchsuchen...", BS_PUSHBUTTON | WS_TABSTOP, 182, 167, 56, 14, ID_SUCHEN);
    kind("STATIC", "Tasten mit + verbinden (strg+umschalt+esc), programm:<Befehl>, "
                   "lauter, leiser, stumm, aus.", SS_LEFT, 8, 186, 230, 18, -1);
    kind("BUTTON", "&Vorgabe", BS_PUSHBUTTON | WS_TABSTOP, 8, 206, 56, 14, ID_VORGABE);

    oeffnen();
    abfragen();
    for (i = 0; i < NTU_SONDER_ANZAHL; i += 1) {
        if (!(zustand_ok && belegung(i, gesetzt[i], 0))) {
            lstrcpyA(gesetzt[i], ntu_sonder_tabelle[i].vorgabe);
        }
        lstrcpyA(neu[i], gesetzt[i]);
    }
    if (!zustand_ok) {
        EnableWindow(GetDlgItem(seite, ID_WAHL), FALSE);
        EnableWindow(GetDlgItem(seite, ID_SUCHEN), FALSE);
        EnableWindow(GetDlgItem(seite, ID_VORGABE), FALSE);
    }
    status_zeile();
    waehlen(0);
    SetTimer(seite, 1, 50, NULL);
}

static void
zeile_zeichnen (DRAWITEMSTRUCT *d)
{
    int    i  = (int)d->itemData;
    int    an = zustand_ok && (zustand.Gedrueckt & (1ul << i));
    int    sel = (d->itemState & ODS_SELECTED) != 0;
    HBRUSH hg = CreateSolidBrush(an ? RGB(160, 230, 160) : GetSysColor(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    RECT   r = d->rcItem;
    char   z[NTU_SONDER_TEXT + 40];
    int    h = r.bottom - r.top;

    if ((int)d->itemID < 0) return;
    FillRect(d->hDC, &r, hg);
    DeleteObject(hg);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, GetSysColor((sel && !an) ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    {
        HBRUSH b = CreateSolidBrush(an ? RGB(0, 170, 0) : RGB(255, 255, 255));
        HBRUSH ab = (HBRUSH)SelectObject(d->hDC, b);
        Ellipse(d->hDC, r.left + 3, r.top + 3, r.left + h - 3, r.bottom - 3);
        SelectObject(d->hDC, ab);
        DeleteObject(b);
    }
    TextOutA(d->hDC, r.left + h + 2, r.top + 1, ntu_sonder_tabelle[i].name,
             lstrlenA(ntu_sonder_tabelle[i].name));
    wsprintfA(z, "%s%s", neu[i], lstrcmpiA(neu[i], gesetzt[i]) ? "  *" : "");
    TextOutA(d->hDC, r.left + (r.right - r.left) * 45 / 100, r.top + 1, z, lstrlenA(z));
    if (d->itemState & ODS_FOCUS) DrawFocusRect(d->hDC, &r);
}

static int
uebernehmen (void)
{
    char meldung[300];
    int  i;
    if (geraet == INVALID_HANDLE_VALUE || !zustand_ok) return 1;
    for (i = 0; i < NTU_SONDER_ANZAHL; i += 1) {
        if (lstrcmpiA(neu[i], gesetzt[i]) == 0) continue;
        if (belegung(i, neu[i], 1)) {
            lstrcpyA(gesetzt[i], neu[i]);
        } else {
            wsprintfA(meldung, "\"%s\": \"%s\" ist keine g\xfcltige Belegung.\n\n"
                               "M\xf6glich: aus, lauter, leiser, stumm, programm:<Befehl>, "
                               "oder Tasten mit + verbunden (strg+umschalt+esc, alt+f4, f13).",
                      ntu_sonder_tabelle[i].name, neu[i]);
            MessageBoxA(seite, meldung, S(T_R_SONDERTASTEN), MB_OK | MB_ICONEXCLAMATION);
            waehlen(i);
            return 0;
        }
    }
    InvalidateRect(GetDlgItem(seite, ID_LISTE), NULL, FALSE);
    return 1;
}

static void
suchen (void)
{
    OPENFILENAMEA o;
    char          datei[MAX_PATH] = "";
    memset(&o, 0, sizeof(o));
    o.lStructSize = sizeof(o);
    o.hwndOwner   = seite;
    o.lpstrFilter = "Programme\0*.exe;*.com;*.bat;*.cmd\0Alle Dateien\0*.*\0";
    o.lpstrFile   = datei;
    o.nMaxFile    = sizeof(datei);
    o.lpstrTitle  = "Programm f\xfcr die Sondertaste";
    o.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&o)) {
        char w[NTU_SONDER_TEXT];
        int  leer = 0, k;
        for (k = 0; datei[k] != '\0'; k += 1) if (datei[k] == ' ') leer = 1;
        /*  Pfade mit Leerzeichen in Anfuehrungszeichen -- sonst startete
         *  USBTASTEN.EXE nur "C:\Programme".  */
        if (lstrlenA(datei) + 12 < NTU_SONDER_TEXT) {
            wsprintfA(w, leer ? "programm:\"%s\"" : "programm:%s", datei);
            lstrcpyA(neu[gewaehlt], w);
            wert_ins_feld(w);
            InvalidateRect(GetDlgItem(seite, ID_LISTE), NULL, FALSE);
            PropSheet_Changed(GetParent(seite), seite);
        }
    }
}

static BOOL CALLBACK
seiten_proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_INITDIALOG:
            seite = w;
            einrichten();
            return TRUE;

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
            RECT r = du(0, 0, 10, 10);
            mi->itemHeight = (UINT)(r.bottom - r.top);
            return TRUE;
        }

        case WM_DRAWITEM:
            zeile_zeichnen((DRAWITEMSTRUCT *)lp);
            return TRUE;

        case WM_TIMER: {
            unsigned long vorher = zustand.Gedrueckt, neu_an;
            int           vo = zustand_ok;
            abfragen();
            neu_an = zustand.Gedrueckt & ~vorher;
            if (zustand.Gedrueckt != vorher) {
                int i;
                for (i = 0; i < NTU_SONDER_ANZAHL; i += 1) {
                    if (neu_an & (1ul << i)) { waehlen(i); break; }
                }
                InvalidateRect(GetDlgItem(w, ID_LISTE), NULL, FALSE);
            }
            if (zustand_ok != vo) status_zeile();
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == ID_LISTE && HIWORD(wp) == LBN_SELCHANGE) {
                int i = (int)SendDlgItemMessageA(w, ID_LISTE, LB_GETCURSEL, 0, 0);
                if (i >= 0) waehlen(i);
            } else if (LOWORD(wp) == ID_WAHL && !fuellt &&
                       (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == CBN_EDITCHANGE)) {
                char v[NTU_SONDER_TEXT];
                if (HIWORD(wp) == CBN_SELCHANGE) {
                    int k = (int)SendDlgItemMessageA(w, ID_WAHL, CB_GETCURSEL, 0, 0);
                    if (k >= 0) SetDlgItemTextA(w, ID_WAHL, auswahl[k]);
                }
                wert_aus_feld(v);
                lstrcpyA(neu[gewaehlt], v[0] ? v : "aus");
                InvalidateRect(GetDlgItem(w, ID_LISTE), NULL, FALSE);
                PropSheet_Changed(GetParent(w), w);
            } else if (LOWORD(wp) == ID_SUCHEN) {
                suchen();
            } else if (LOWORD(wp) == ID_VORGABE) {
                int i;
                for (i = 0; i < NTU_SONDER_ANZAHL; i += 1) lstrcpyA(neu[i], ntu_sonder_tabelle[i].vorgabe);
                waehlen(gewaehlt);
                InvalidateRect(GetDlgItem(w, ID_LISTE), NULL, FALSE);
                PropSheet_Changed(GetParent(w), w);
            }
            return TRUE;

        case WM_NOTIFY: {
            NMHDR *n = (NMHDR *)lp;
            if (n->code == PSN_APPLY) {
                SetWindowLongA(w, DWL_MSGRESULT, uebernehmen() ? PSNRET_NOERROR
                                                               : PSNRET_INVALID_NOCHANGEPAGE);
                return TRUE;
            }
            return FALSE;
        }

        case WM_DESTROY:
            KillTimer(w, 1);
            if (geraet != INVALID_HANDLE_VALUE) {
                CloseHandle(geraet);
                geraet = INVALID_HANDLE_VALUE;
            }
            seite = NULL;
            return FALSE;
    }
    return FALSE;
}

static WORD vorlage[64];

static LPCDLGTEMPLATE
vorlage_bauen (void)
{
    DLGTEMPLATE *t = (DLGTEMPLATE *)vorlage;
    WORD        *p;
    const char  *f = "MS Sans Serif";

    memset(vorlage, 0, sizeof(vorlage));
    t->style = WS_CHILD | WS_VISIBLE | DS_SETFONT | DS_3DLOOK | WS_CLIPCHILDREN;
    t->cx = 246; t->cy = 226;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 8;
    while (*f) *p++ = (WORD)(unsigned char)*f++;
    *p++ = 0;
    return (LPCDLGTEMPLATE)t;
}

static UINT CALLBACK
seiten_rueckruf (HWND w, UINT m, LPPROPSHEETPAGEA p)
{
    if (m == PSPCB_CREATE) { InterlockedIncrement(&objekte); return 1; }
    if (m == PSPCB_RELEASE) InterlockedDecrement(&objekte);
    return 0;
}

/* ---------------------------------------------------------------- *
 *  IShellPropSheetExt
 * ---------------------------------------------------------------- */

typedef struct Erw Erw;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Erw *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Erw *);
    ULONG   (STDMETHODCALLTYPE *Release)(Erw *);
    HRESULT (STDMETHODCALLTYPE *AddPages)(Erw *, LPFNADDPROPSHEETPAGE, LPARAM);
    HRESULT (STDMETHODCALLTYPE *ReplacePage)(Erw *, UINT, LPFNADDPROPSHEETPAGE, LPARAM);
} ErwTabelle;
struct Erw {
    const ErwTabelle *vt;
    LONG              zahl;
};

static HRESULT STDMETHODCALLTYPE
erw_qi (Erw *e, REFIID riid, void **pv)
{
    if (gleich(riid, &IID_Unbekannt) || gleich(riid, &IID_SeitenErw)) {
        *pv = e;
        InterlockedIncrement(&e->zahl);
        return S_OK;
    }
    *pv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
erw_addref (Erw *e)
{
    return (ULONG)InterlockedIncrement(&e->zahl);
}

static ULONG STDMETHODCALLTYPE
erw_release (Erw *e)
{
    LONG n = InterlockedDecrement(&e->zahl);
    if (n == 0) {
        HeapFree(GetProcessHeap(), 0, e);
        InterlockedDecrement(&objekte);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE
erw_addpages (Erw *e, LPFNADDPROPSHEETPAGE hinzu, LPARAM lp)
{
    PROPSHEETPAGEA  ps;
    HPROPSHEETPAGE  h;

    memset(&ps, 0, sizeof(ps));
    ps.dwSize      = sizeof(ps);
    ps.dwFlags     = PSP_DLGINDIRECT | PSP_USETITLE | PSP_USECALLBACK;
    ps.hInstance   = inst;
    ps.pResource   = vorlage_bauen();
    ps.pszTitle    = S(T_R_SONDERTASTEN);
    ps.pfnDlgProc  = (DLGPROC)seiten_proz;
    ps.pfnCallback = seiten_rueckruf;

    h = CreatePropertySheetPageA(&ps);
    if (h == NULL) {
        return E_OUTOFMEMORY;
    }
    if (!hinzu(h, lp)) {
        DestroyPropertySheetPage(h);
        return E_FAIL;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
erw_replace (Erw *e, UINT id, LPFNADDPROPSHEETPAGE f, LPARAM lp)
{
    return E_NOTIMPL;
}

static const ErwTabelle erw_tabelle = {
    erw_qi, erw_addref, erw_release, erw_addpages, erw_replace
};

/* ---------------------------------------------------------------- *
 *  IClassFactory
 * ---------------------------------------------------------------- */

typedef struct Fab Fab;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Fab *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Fab *);
    ULONG   (STDMETHODCALLTYPE *Release)(Fab *);
    HRESULT (STDMETHODCALLTYPE *CreateInstance)(Fab *, IUnknown *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *LockServer)(Fab *, BOOL);
} FabTabelle;
struct Fab {
    const FabTabelle *vt;
};

static HRESULT STDMETHODCALLTYPE
fab_qi (Fab *f, REFIID riid, void **pv)
{
    if (gleich(riid, &IID_Unbekannt) || gleich(riid, &IID_Fabrik)) {
        *pv = f;
        return S_OK;
    }
    *pv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE fab_addref (Fab *f)  { return 2; }
static ULONG STDMETHODCALLTYPE fab_release (Fab *f) { return 1; }

static HRESULT STDMETHODCALLTYPE
fab_create (Fab *f, IUnknown *aussen, REFIID riid, void **pv)
{
    Erw     *e;
    HRESULT  hr;

    *pv = NULL;
    if (aussen != NULL) {
        return CLASS_E_NOAGGREGATION;
    }
    e = (Erw *)HeapAlloc(GetProcessHeap(), 0, sizeof(*e));
    if (e == NULL) {
        return E_OUTOFMEMORY;
    }
    e->vt   = &erw_tabelle;
    e->zahl = 1;
    InterlockedIncrement(&objekte);
    hr = erw_qi(e, riid, pv);
    erw_release(e);                 /* 1 -> (0 oder Zahl von qi) */
    return hr;
}

static HRESULT STDMETHODCALLTYPE
fab_lock (Fab *f, BOOL an)
{
    if (an) InterlockedIncrement(&objekte); else InterlockedDecrement(&objekte);
    return S_OK;
}

static const FabTabelle fab_tabelle = { fab_qi, fab_addref, fab_release, fab_create, fab_lock };
static Fab fabrik = { &fab_tabelle };

/* ---------------------------------------------------------------- *
 *  DLL-Einstiege
 * ---------------------------------------------------------------- */

HRESULT WINAPI
DllGetClassObject (REFCLSID rclsid, REFIID riid, void **pv)
{
    if (!gleich(rclsid, &CLSID_UsbMausTab)) {
        *pv = NULL;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return fab_qi(&fabrik, riid, pv);
}

HRESULT WINAPI
DllCanUnloadNow (void)
{
    return (objekte == 0) ? S_OK : S_FALSE;
}

#define ERW_SCHLUESSEL "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\Keyboard\\shellex\\PropertySheetHandlers\\USB-Sondertasten"
#define ERLAUBT        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved"

static int
wert (HKEY wurzel, const char *pfad, const char *name, const char *text)
{
    HKEY k;
    LONG r;
    if (RegCreateKeyExA(wurzel, pfad, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) {
        return 0;
    }
    r = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)text, (DWORD)lstrlenA(text) + 1);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

HRESULT WINAPI
DllRegisterServer (void)
{
    char pfad[MAX_PATH];
    GetModuleFileNameA(inst, pfad, sizeof(pfad));
    if (!wert(HKEY_CLASSES_ROOT, "CLSID\\" CLSID_TEXT, NULL, "USB-Sondertasten") ||
        !wert(HKEY_CLASSES_ROOT, "CLSID\\" CLSID_TEXT "\\InprocServer32", NULL, pfad) ||
        !wert(HKEY_CLASSES_ROOT, "CLSID\\" CLSID_TEXT "\\InprocServer32", "ThreadingModel", "Apartment") ||
        !wert(HKEY_LOCAL_MACHINE, ERW_SCHLUESSEL, NULL, CLSID_TEXT)) {
        return E_ACCESSDENIED;
    }
    wert(HKEY_LOCAL_MACHINE, ERLAUBT, CLSID_TEXT, "USB-Sondertasten");
    return S_OK;
}

HRESULT WINAPI
DllUnregisterServer (void)
{
    HKEY k;
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, ERW_SCHLUESSEL);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, "CLSID\\" CLSID_TEXT "\\InprocServer32");
    RegDeleteKeyA(HKEY_CLASSES_ROOT, "CLSID\\" CLSID_TEXT);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ERLAUBT, 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValueA(k, CLSID_TEXT);
        RegCloseKey(k);
    }
    return S_OK;
}

BOOL WINAPI
DllEntry (HINSTANCE h, DWORD grund, LPVOID r)
{
    if (grund == DLL_PROCESS_ATTACH) {
        inst = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
