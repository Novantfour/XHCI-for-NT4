/*
 *  werkzeug/usbmausx.c -- USBMAUSX.DLL: Reiter S(T_R_MAUSTASTEN) in der
 *  normalen Maus-Systemsteuerung (main.cpl, "Maus").
 *
 *  Die Maus-Systemsteuerung laedt Eigenschaftsseiten-Erweiterungen aus
 *      HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Controls Folder\
 *          Mouse\shellex\PropertySheetHandlers\<Name> = {CLSID}
 *  (so hat es auch Logitechs Maussoftware gemacht).  Diese DLL ist ein
 *  COM-Server mit IShellPropSheetExt; AddPages liefert EINE Seite mit
 *  dem Inhalt von usbmaus.cpl:
 *    - acht Zeilen, je Maustaste ein Laempchen (IOCTL_NTU_MAUS_ZUSTAND,
 *      alle 30 ms -- auch Tasten 4..8, die NT 4.0 selbst nicht kennt) und
 *      die Belegung zur Auswahl oder selbst getippt;
 *    - "Vorgabe" traegt die Vorgaben ein.
 *  Uebernehmen/OK sind die Knoepfe der Systemsteuerung (PSN_APPLY): jede
 *  geaenderte Belegung geht an den Treiber (IOCTL_NTU_MAUS_SETZEN), wirkt
 *  sofort und steht danach unter Parameters\TasteN.  Ist dann keine Taste
 *  mehr die linke, wird erst gefragt.
 *
 *  Einrichten: DllRegisterServer (USBINST oder regsvr32) oder USBMAUSX.REG;
 *  die DLL liegt in %SystemRoot%\system32.
 *
 *  kernel32, user32, gdi32, advapi32, comctl32.  Kein CRT, kein ole32.
 */

#include <windows.h>
#include <commctrl.h>
#include <prsht.h>
#include "../ober/ntusbioctl.h"
#include "sprache.h"

void *
memset (void *z, int c, size_t n)
{
    unsigned char *p = (unsigned char *)z;
    while (n--) *p++ = (unsigned char)c;
    return z;
}

/* ---------------------------------------------------------------- *
 *  COM-Kennungen (selbst, damit kein uuid.lib/ole32 noetig ist)
 * ---------------------------------------------------------------- */

/* {6E1B7A12-3C44-4F5A-9B21-7D0C5A1E4B31} */
static const GUID CLSID_UsbMausTab =
    { 0x6e1b7a12, 0x3c44, 0x4f5a, { 0x9b, 0x21, 0x7d, 0x0c, 0x5a, 0x1e, 0x4b, 0x31 } };
#define CLSID_TEXT "{6E1B7A12-3C44-4F5A-9B21-7D0C5A1E4B31}"

static const GUID IID_Unbekannt =                    /* IUnknown */
    { 0x00000000, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_Fabrik =                       /* IClassFactory */
    { 0x00000001, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_SeitenErw =                    /* IShellPropSheetExt */
    { 0x000214e9, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static int
gleich (const GUID *a, const GUID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    int i;
    for (i = 0; i < 16; i += 1) if (x[i] != y[i]) return 0;
    return 1;
}

static HINSTANCE inst;
static LONG      objekte;          /* lebende Objekte + Seiten: DllCanUnloadNow */

/* ---------------------------------------------------------------- *
 *  Die Seite
 * ---------------------------------------------------------------- */

#define N_TASTEN   8
#define ID_STATUS  100
#define ID_WAHL    200             /* +0..7 */
#define ID_VORGABE 301

/*  Aufbau in Dialogeinheiten (Seite 246 x 226).  */
#define DX_LAMPE    8
#define DX_NAME    22
#define DX_WAHL    58
#define DW_WAHL   180
#define DY_ZEILE0  38
#define DY_ZEILE   18

static HWND      seite;
static HANDLE    geraet = INVALID_HANDLE_VALUE;
static NTU_MAUS_ZUSTAND zustand;
static int       zustand_ok;
static char      gesetzt[N_TASTEN][48];

static const char *vorgaben[N_TASTEN] = {
    "klick-links", "klick-rechts", "mitte", "alt+links",
    "alt+rechts", "aus", "aus", "aus"
};

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
                 DeviceIoControl(geraet, IOCTL_NTU_MAUS_ZUSTAND, NULL, 0, &zustand,
                                 sizeof(zustand), &got, NULL) && got == sizeof(zustand);
}

static int
belegung_holen (int taste, char *ziel)
{
    NTU_MAUS_BELEGUNG b;
    DWORD             got = 0;
    memset(&b, 0, sizeof(b));
    b.Taste = (unsigned long)taste;
    if (!DeviceIoControl(geraet, IOCTL_NTU_MAUS_HOLEN, &b, sizeof(b), &b, sizeof(b), &got, NULL)) {
        return 0;
    }
    b.Text[sizeof(b.Text) - 1] = '\0';
    lstrcpynA(ziel, b.Text, 48);
    return 1;
}

static int
belegung_setzen (int taste, const char *text)
{
    NTU_MAUS_BELEGUNG b;
    DWORD             got = 0;
    memset(&b, 0, sizeof(b));
    b.Taste = (unsigned long)taste;
    lstrcpynA(b.Text, text, sizeof(b.Text));
    return DeviceIoControl(geraet, IOCTL_NTU_MAUS_SETZEN, &b, sizeof(b), NULL, 0, &got, NULL);
}

static void
wert_aus_feld (int k, char *w)
{
    char t[80];
    int  i;
    GetWindowTextA(GetDlgItem(seite, ID_WAHL + k), t, sizeof(t));
    for (i = 0; t[i] == ' '; i += 1) {
    }
    lstrcpynA(w, t + i, 48);
    for (i = 0; w[i] != '\0'; i += 1) {
        if (w[i] == ' ' || w[i] == '(') { w[i] = '\0'; break; }
        if (w[i] >= 'A' && w[i] <= 'Z') w[i] = (char)(w[i] + 32);
    }
}

static void
wert_ins_feld (int k, const char *w)
{
    HWND c = GetDlgItem(seite, ID_WAHL + k);
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
status_zeile (void)
{
    char z[160];
    if (geraet == INVALID_HANDLE_VALUE) {
        lstrcpyA(z, "Der USB-Treiber ist nicht geladen.");
    } else if (!zustand_ok) {
        lstrcpyA(z, "Der USB-Treiber kennt diese Anfrage nicht -- bitte neuere Fassung einrichten.");
    } else if (zustand.Maeuse == 0) {
        lstrcpyA(z, "Keine USB-Maus angesteckt.  Belegen geht trotzdem.");
    } else {
        wsprintfA(z, "%lu %s, bis zu %lu Tasten.  Dr\xfc" "cken Sie eine Taste -- ihre Zeile leuchtet.",
                  zustand.Maeuse, zustand.Maeuse == 1 ? "USB-Maus" : "USB-M\xe4use", zustand.Knoepfe);
    }
    SetDlgItemTextA(seite, ID_STATUS, z);
}

/*  Rechteck in Dialogeinheiten -> Bildpunkte.  */
static RECT
du (int x, int y, int w, int h)
{
    RECT r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    MapDialogRect(seite, &r);
    return r;
}

static void
kind (const char *klasse, const char *text, DWORD stil, int x, int y, int w, int h, int id)
{
    RECT r = du(x, y, w, h);
    HWND c = CreateWindowExA(0, klasse, text, WS_CHILD | WS_VISIBLE | stil,
                             r.left, r.top, r.right - r.left, r.bottom - r.top,
                             seite, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(c, WM_SETFONT, SendMessageA(seite, WM_GETFONT, 0, 0), FALSE);
}

static void
zeichne_laempchen (HDC dc)
{
    int k;
    for (k = 0; k < N_TASTEN; k += 1) {
        RECT   r  = du(DX_LAMPE, DY_ZEILE0 + k * DY_ZEILE + 2, 8, 8);
        int    an = zustand_ok && (zustand.Roh & (1ul << k));
        HBRUSH b  = CreateSolidBrush(an ? RGB(0, 190, 0) : RGB(255, 255, 255));
        HPEN   p  = CreatePen(PS_SOLID, 1, RGB(64, 64, 64));
        HBRUSH ab = (HBRUSH)SelectObject(dc, b);
        HPEN   ap = (HPEN)SelectObject(dc, p);
        int    d  = r.bottom - r.top;
        Ellipse(dc, r.left, r.top, r.left + d, r.top + d);
        SelectObject(dc, ab); SelectObject(dc, ap);
        DeleteObject(b); DeleteObject(p);
    }
}

/*  PSN_APPLY.  1 = uebernommen, 0 = Seite nicht verlassen.  */
static int
uebernehmen (void)
{
    char w[N_TASTEN][48], meldung[300];
    int  k, links = 0;

    if (geraet == INVALID_HANDLE_VALUE || !zustand_ok) {
        return 1;                           /* nichts zu tun */
    }
    for (k = 0; k < N_TASTEN; k += 1) {
        wert_aus_feld(k, w[k]);
        if (w[k][0] == '\0') lstrcpyA(w[k], "aus");
        if (lstrcmpiA(w[k], "klick-links") == 0) links = 1;
    }
    if (!links &&
        MessageBoxA(seite, "Keine Taste ist dann mehr die linke Maustaste.\n\n"
                           "Ohne eine zweite Maus kommen Sie so nur noch mit der Tastatur "
                           "weiter.  Trotzdem \xfc" "bernehmen?",
                    S(T_R_MAUSTASTEN), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return 0;
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
            MessageBoxA(seite, meldung, S(T_R_MAUSTASTEN), MB_OK | MB_ICONEXCLAMATION);
            SetFocus(GetDlgItem(seite, ID_WAHL + k));
            return 0;
        }
    }
    return 1;
}

static void
einrichten (void)
{
    int i, j;

    kind("STATIC", "Maustasten der USB-Maus erkennen und belegen", SS_LEFT, 8, 6, 230, 10, -1);
    kind("STATIC", "", SS_LEFT, 8, 18, 230, 16, ID_STATUS);
    for (i = 0; i < N_TASTEN; i += 1) {
        char n[16];
        HWND c;
        wsprintfA(n, "Taste %d", i + 1);
        kind("STATIC", n, SS_LEFT, DX_NAME, DY_ZEILE0 + i * DY_ZEILE + 2, 34, 10, -1);
        kind("COMBOBOX", "", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
             DX_WAHL, DY_ZEILE0 + i * DY_ZEILE, DW_WAHL, 110, ID_WAHL + i);
        c = GetDlgItem(seite, ID_WAHL + i);
        for (j = 0; j < (int)N_AUSWAHL; j += 1) {
            SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)auswahl[j]);
        }
    }
    kind("STATIC", "Eigene Tasten mit + verbinden, z. B. strg+umschalt+esc, alt+f4, f5.",
         SS_LEFT, DX_WAHL, DY_ZEILE0 + N_TASTEN * DY_ZEILE + 2, DW_WAHL, 18, -1);
    kind("BUTTON", "&Vorgabe", BS_PUSHBUTTON | WS_TABSTOP,
         DX_WAHL, DY_ZEILE0 + N_TASTEN * DY_ZEILE + 24, 60, 14, ID_VORGABE);

    oeffnen();
    abfragen();
    for (i = 0; i < N_TASTEN; i += 1) {
        if (!(zustand_ok && belegung_holen(i + 1, gesetzt[i]))) {
            lstrcpyA(gesetzt[i], vorgaben[i]);
        }
        wert_ins_feld(i, gesetzt[i]);
    }
    if (!zustand_ok) {
        for (i = 0; i < N_TASTEN; i += 1) EnableWindow(GetDlgItem(seite, ID_WAHL + i), FALSE);
        EnableWindow(GetDlgItem(seite, ID_VORGABE), FALSE);
    }
    status_zeile();
    SetTimer(seite, 1, 30, NULL);
}

static BOOL CALLBACK
seiten_proz (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_INITDIALOG:
            seite = w;
            einrichten();
            return TRUE;

        case WM_TIMER: {
            unsigned long vorher = zustand.Roh, vz = zustand.Maeuse;
            int           vo = zustand_ok;
            abfragen();
            if (zustand.Roh != vorher) {
                RECT r = du(0, DY_ZEILE0, DX_NAME, N_TASTEN * DY_ZEILE);
                InvalidateRect(w, &r, TRUE);
            }
            if (zustand.Maeuse != vz || zustand_ok != vo) {
                status_zeile();
            }
            return TRUE;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(w, &ps);
            zeichne_laempchen(dc);
            EndPaint(w, &ps);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == ID_VORGABE) {
                int k;
                for (k = 0; k < N_TASTEN; k += 1) wert_ins_feld(k, vorgaben[k]);
                PropSheet_Changed(GetParent(w), w);
            } else if (LOWORD(wp) >= ID_WAHL && LOWORD(wp) < ID_WAHL + N_TASTEN &&
                       (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == CBN_EDITCHANGE)) {
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

/*  Leere Dialogvorlage im Speicher (PSP_DLGINDIRECT): 246 x 226 Einheiten,
 *  Schrift "MS Sans Serif" 8 wie die anderen Reiter.  Alles Weitere legt
 *  einrichten() an.  */
static WORD vorlage[64];

static LPCDLGTEMPLATE
vorlage_bauen (void)
{
    DLGTEMPLATE *t = (DLGTEMPLATE *)vorlage;
    WORD        *p;
    const char  *f = "MS Sans Serif";

    memset(vorlage, 0, sizeof(vorlage));
    t->style = WS_CHILD | WS_VISIBLE | DS_SETFONT | DS_3DLOOK | WS_CLIPCHILDREN;
    t->cdit  = 0;
    t->x = 0; t->y = 0; t->cx = 246; t->cy = 226;
    p = (WORD *)(t + 1);
    *p++ = 0;                      /* kein Menue */
    *p++ = 0;                      /* Standardklasse */
    *p++ = 0;                      /* kein Titel */
    *p++ = 8;                      /* Schriftgroesse */
    while (*f) *p++ = (WORD)(unsigned char)*f++;
    *p++ = 0;
    return (LPCDLGTEMPLATE)t;
}

static UINT CALLBACK
seiten_rueckruf (HWND w, UINT m, LPPROPSHEETPAGEA p)
{
    if (m == PSPCB_CREATE) {
        InterlockedIncrement(&objekte);
        return 1;
    }
    if (m == PSPCB_RELEASE) {
        InterlockedDecrement(&objekte);
    }
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
    ps.pszTitle    = S(T_R_MAUSTASTEN);
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

#define ERW_SCHLUESSEL "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\Mouse\\shellex\\PropertySheetHandlers\\USB-Maustasten"
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
    if (!wert(HKEY_CLASSES_ROOT, "CLSID\\" CLSID_TEXT, NULL, S(T_R_MAUSTASTEN)) ||
        !wert(HKEY_CLASSES_ROOT, "CLSID\\" CLSID_TEXT "\\InprocServer32", NULL, pfad) ||
        !wert(HKEY_CLASSES_ROOT, "CLSID\\" CLSID_TEXT "\\InprocServer32", "ThreadingModel", "Apartment") ||
        !wert(HKEY_LOCAL_MACHINE, ERW_SCHLUESSEL, NULL, CLSID_TEXT)) {
        return E_ACCESSDENIED;
    }
    wert(HKEY_LOCAL_MACHINE, ERLAUBT, CLSID_TEXT, S(T_R_MAUSTASTEN));
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
