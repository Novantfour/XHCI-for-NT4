/*
 *  werkzeug/hidtest.c -- HIDTEST.EXE: HID-Geraete auf NT 4.0 anzeigen.
 *
 *  Macht das sichtbar, was unter Windows 98/2000 der Geraetemanager
 *  zeigt und was DirectInput beim Start tut:  welche HID-Geraete gibt
 *  es, was melden sie, und was kommt heraus, wenn man daran dreht.
 *
 *      hidtest                 gefundene Geraete auflisten
 *      hidtest N               Geraet N ausfuehrlich (Caps, Knoepfe, Werte)
 *      hidtest N lesen [S]     S Sekunden lang Berichte mitlesen (Vorgabe 10)
 *      hidtest N roh           Berichtsdeskriptor als Hex
 *
 *  Benutzt ausschliesslich HID.DLL -- also genau den Weg, den auch ein
 *  Programm wie DirectInput nehmen wuerde.  Scheitert etwas hier,
 *  scheitert es dort genauso.
 */

#include <windows.h>
#include "ntbasis.h"

/* ---- aus HID.DLL (Originalnamen) ---------------------------------- */
typedef LONG NTSTATUS_HID;
typedef enum { HidP_Input = 0, HidP_Output = 1, HidP_Feature = 2 } HIDP_REPORT_TYPE;

typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } HIDD_ATTRIBUTES;

typedef struct {
    USHORT Usage, UsagePage;
    USHORT InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT NumberLinkCollectionNodes;
    USHORT NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices;
    USHORT NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices;
    USHORT NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
} HIDP_CAPS;

typedef struct { USHORT UsageMin, UsageMax, StringMin, StringMax,
                        DesignatorMin, DesignatorMax, DataIndexMin, DataIndexMax; } HIDP_RANGE;
typedef struct { USHORT Usage, R1, StringIndex, R2, DesignatorIndex, R3, DataIndex, R4; } HIDP_NOTRANGE;

typedef struct {
    USHORT UsagePage; UCHAR ReportID; BOOLEAN IsAlias; USHORT BitField;
    USHORT LinkCollection, LinkUsage, LinkUsagePage;
    BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute;
    ULONG Reserved[10];
    union { HIDP_RANGE Range; HIDP_NOTRANGE NotRange; } u;
} HIDP_BUTTON_CAPS;

typedef struct {
    USHORT UsagePage; UCHAR ReportID; BOOLEAN IsAlias; USHORT BitField;
    USHORT LinkCollection, LinkUsage, LinkUsagePage;
    BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute, HasNull;
    UCHAR Reserved; USHORT BitSize, ReportCount, Reserved2[5];
    ULONG UnitsExp, Units; LONG LogicalMin, LogicalMax, PhysicalMin, PhysicalMax;
    union { HIDP_RANGE Range; HIDP_NOTRANGE NotRange; } u;
} HIDP_VALUE_CAPS;

typedef BOOLEAN (WINAPI *P_Attr)(HANDLE, HIDD_ATTRIBUTES *);
typedef BOOLEAN (WINAPI *P_Str)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI *P_Pre)(HANDLE, void **);
typedef BOOLEAN (WINAPI *P_Free)(void *);
typedef NTSTATUS_HID (WINAPI *P_Caps)(void *, HIDP_CAPS *);
typedef NTSTATUS_HID (WINAPI *P_BCaps)(HIDP_REPORT_TYPE, HIDP_BUTTON_CAPS *, USHORT *, void *);
typedef NTSTATUS_HID (WINAPI *P_VCaps)(HIDP_REPORT_TYPE, HIDP_VALUE_CAPS *, USHORT *, void *);
typedef NTSTATUS_HID (WINAPI *P_Usages)(HIDP_REPORT_TYPE, USHORT, USHORT, USHORT *, ULONG *,
                                        void *, CHAR *, ULONG);
typedef NTSTATUS_HID (WINAPI *P_UVal)(HIDP_REPORT_TYPE, USHORT, USHORT, USHORT, ULONG *,
                                      void *, CHAR *, ULONG);
typedef BOOLEAN (WINAPI *P_Enum)(char *, ULONG *);

static P_Attr   pAttr;
static P_Str    pProd, pHerst;
static P_Pre    pPre;
static P_Free   pFree;
static P_Caps   pCaps;
static P_BCaps  pBCaps;
static P_VCaps  pVCaps;
static P_Usages pUsages;
static P_UVal   pUVal;
static P_Enum   pEnum;

static int laden(void)
{
    HMODULE m = LoadLibraryA("HID.DLL");
    if (m == NULL) { printf("  HID.DLL nicht gefunden\n"); return 0; }
    pAttr   = (P_Attr)  GetProcAddress(m, "HidD_GetAttributes");
    pProd   = (P_Str)   GetProcAddress(m, "HidD_GetProductString");
    pHerst  = (P_Str)   GetProcAddress(m, "HidD_GetManufacturerString");
    pPre    = (P_Pre)   GetProcAddress(m, "HidD_GetPreparsedData");
    pFree   = (P_Free)  GetProcAddress(m, "HidD_FreePreparsedData");
    pCaps   = (P_Caps)  GetProcAddress(m, "HidP_GetCaps");
    pBCaps  = (P_BCaps) GetProcAddress(m, "HidP_GetButtonCaps");
    pVCaps  = (P_VCaps) GetProcAddress(m, "HidP_GetValueCaps");
    pUsages = (P_Usages)GetProcAddress(m, "HidP_GetUsages");
    pUVal   = (P_UVal)  GetProcAddress(m, "HidP_GetUsageValue");
    pEnum   = (P_Enum)  GetProcAddress(m, "HidNt_Enumerate");
    if (!pAttr || !pPre || !pCaps || !pEnum) { printf("  HID.DLL unvollstaendig\n"); return 0; }
    return 1;
}

/*  Was ist das fuer ein Geraet?  Aus Usage Page und Usage der obersten
 *  Collection -- genau so entscheidet es auch DirectInput.  */
static const char *art(USHORT seite, USHORT nutzung)
{
    if (seite == 0x01) {
        switch (nutzung) {
        case 0x02: return "Maus";
        case 0x04: return "Joystick";
        case 0x05: return "Gamepad";
        case 0x06: return "Tastatur";
        case 0x07: return "Tastenfeld";
        case 0x08: return "Multi-Achsen-Regler";
        case 0x80: return "Systemsteuerung";
        default:   return "Allgemeine Bedienung";
        }
    }
    if (seite == 0x0C) return "Medientasten (Consumer)";
    if (seite == 0x0D) return "Digitalisierer";
    return "anderes";
}

static int pfade_holen(char *puf, ULONG groesse)
{
    ULONG n = groesse;
    if (pEnum(puf, &n)) return 1;
    return 0;
}

static void liste(void)
{
    static char pfade[4096];
    const char *p;
    int i = 0;

    if (!pfade_holen(pfade, sizeof(pfade))) { printf("  keine HID-Geraete gefunden\n"); return; }
    printf("  Nr  Pfad                 VID:PID    Art                       Produkt\n");
    printf("  --------------------------------------------------------------------------\n");
    for (p = pfade; *p; p += lstrlenA(p) + 1, i += 1) {
        HANDLE h = CreateFileA(p, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        HIDD_ATTRIBUTES a;
        HIDP_CAPS c;
        void *pp = NULL;
        WCHAR w[64];
        char  name[64];
        int   k;

        if (h == INVALID_HANDLE_VALUE) { printf("  %2d  %-20s (nicht zu oeffnen)\n", i, p); continue; }
        a.Size = sizeof(a);
        name[0] = 0;
        if (pProd && pProd(h, w, sizeof(w))) {
            for (k = 0; w[k] && k < 40; k += 1) name[k] = (char)w[k];
            name[k] = 0;
        }
        if (pAttr(h, &a) && pPre(h, &pp) && pCaps(pp, &c) >= 0) {
            printf("  %2d  %-20s %04X:%04X  %-25s %s\n", i, p,
                   a.VendorID, a.ProductID, art(c.UsagePage, c.Usage), name);
        } else {
            printf("  %2d  %-20s (keine HID-Angaben)\n", i, p);
        }
        if (pp && pFree) pFree(pp);
        CloseHandle(h);
    }
    if (i == 0) printf("  keine HID-Geraete gefunden\n");
}

static HANDLE oeffnen(int nr, char *pfadaus)
{
    static char pfade[4096];
    const char *p;
    int i = 0;
    if (!pfade_holen(pfade, sizeof(pfade))) return INVALID_HANDLE_VALUE;
    for (p = pfade; *p; p += lstrlenA(p) + 1, i += 1) {
        if (i != nr) continue;
        if (pfadaus) lstrcpyA(pfadaus, p);
        return CreateFileA(p, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    return INVALID_HANDLE_VALUE;
}

static void zeige(int nr)
{
    char pfad[64] = "";
    HANDLE h = oeffnen(nr, pfad);
    HIDD_ATTRIBUTES a;
    HIDP_CAPS c;
    void *pp = NULL;
    static HIDP_BUTTON_CAPS bc[64];
    static HIDP_VALUE_CAPS  vc[64];
    USHORT n;
    WCHAR w[64];
    int k;

    if (h == INVALID_HANDLE_VALUE) { printf("  Geraet %d nicht zu oeffnen\n", nr); return; }
    a.Size = sizeof(a);
    if (!pAttr(h, &a) || !pPre(h, &pp) || pCaps(pp, &c) < 0) {
        printf("  keine HID-Angaben\n"); CloseHandle(h); return;
    }
    printf("  Pfad        %s\n", pfad);
    printf("  Kennung     %04X:%04X, Ausgabe %04X\n", a.VendorID, a.ProductID, a.VersionNumber);
    if (pHerst && pHerst(h, w, sizeof(w))) {
        printf("  Hersteller  "); for (k = 0; w[k]; k += 1) printf("%c", (char)w[k]); printf("\n");
    }
    if (pProd && pProd(h, w, sizeof(w))) {
        printf("  Produkt     "); for (k = 0; w[k]; k += 1) printf("%c", (char)w[k]); printf("\n");
    }
    printf("  Art         %s (Seite %04X, Nutzung %04X)\n",
           art(c.UsagePage, c.Usage), c.UsagePage, c.Usage);
    printf("  Berichte    Eingabe %u Byte, Ausgabe %u, Merkmal %u\n",
           c.InputReportByteLength, c.OutputReportByteLength, c.FeatureReportByteLength);

    n = 64;
    if (pBCaps && pBCaps(HidP_Input, bc, &n, pp) >= 0 && n > 0) {
        printf("  Knoepfe     %u\n", n);
        for (k = 0; k < n && k < 16; k += 1)
            printf("      Seite %04X  Nutzung %04X  Bericht %u\n",
                   bc[k].UsagePage, bc[k].u.NotRange.Usage, bc[k].ReportID);
        if (n > 16) printf("      ... und %u weitere\n", n - 16);
    }
    n = 64;
    if (pVCaps && pVCaps(HidP_Input, vc, &n, pp) >= 0 && n > 0) {
        printf("  Werte       %u\n", n);
        for (k = 0; k < n && k < 16; k += 1)
            printf("      Seite %04X  Nutzung %04X  %u Bit, %ld..%ld\n",
                   vc[k].UsagePage, vc[k].u.NotRange.Usage, vc[k].BitSize,
                   vc[k].LogicalMin, vc[k].LogicalMax);
        if (n > 16) printf("      ... und %u weitere\n", n - 16);
    }
    if (pFree) pFree(pp);
    CloseHandle(h);
}

static void mitlesen(int nr, int sek)
{
    char pfad[64] = "";
    HANDLE h = oeffnen(nr, pfad);
    HIDP_CAPS c;
    void *pp = NULL;
    static char puf[256];
    DWORD ende, got;
    int zeilen = 0;

    if (h == INVALID_HANDLE_VALUE) { printf("  Geraet %d nicht zu oeffnen\n", nr); return; }
    if (!pPre(h, &pp) || pCaps(pp, &c) < 0) { printf("  keine HID-Angaben\n"); CloseHandle(h); return; }
    printf("  %s -- %d s mitlesen, jetzt bedienen (Taste bricht ab)\n\n", pfad, sek);

    ende = GetTickCount() + (DWORD)sek * 1000;
    while (GetTickCount() < ende) {
        ULONG  len = c.InputReportByteLength ? c.InputReportByteLength : 8;
        USHORT liste[32];
        ULONG  anz = 32;
        int i;

        if (len > sizeof(puf)) len = sizeof(puf);
        if (!ReadFile(h, puf, len, &got, NULL) || got == 0) { Sleep(5); continue; }

        printf("  [");
        for (i = 0; i < (int)got && i < 16; i += 1) printf("%02X ", (UCHAR)puf[i]);
        printf("]");

        if (pUsages && pUsages(HidP_Input, 0, 0, liste, &anz, pp, puf, got) >= 0 && anz > 0) {
            printf("  gedrueckt:");
            for (i = 0; i < (int)anz && i < 8; i += 1) printf(" %04X", liste[i]);
        }
        /*  Lautstaerke, die haeufigsten Medientasten (Seite 0x0C).  */
        {
            ULONG v;
            if (pUVal && pUVal(HidP_Input, 0x0C, 0, 0xE0, &v, pp, puf, got) >= 0)
                printf("  Lautstaerke %lu", v);
        }
        printf("\n");
        if (++zeilen > 200) break;
    }
    printf("\n  fertig, %d Berichte\n", zeilen);
    if (pFree) pFree(pp);
    CloseHandle(h);
}

int main(int argc, char **argv)
{
    printf("\nhidtest -- HID-Geraete unter Windows NT 4.0 (ueber HID.DLL)\n\n");
    if (!laden()) return 1;

    if (argc < 2) { liste(); printf("\n  hidtest N | hidtest N lesen [S]\n\n"); return 0; }
    {
        int nr = 0, i;
        for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i += 1) nr = nr * 10 + argv[1][i] - '0';
        if (argc > 2 && lstrcmpiA(argv[2], "lesen") == 0) {
            int s = 10, k;
            if (argc > 3) { s = 0; for (k = 0; argv[3][k] >= '0' && argv[3][k] <= '9'; k += 1) s = s*10 + argv[3][k]-'0'; }
            mitlesen(nr, s > 0 ? s : 10);
        } else {
            zeige(nr);
        }
    }
    printf("\n");
    return 0;
}
