/*
 *  werkzeug/hiddll.c -- HID.DLL fuer Windows NT 4.0.
 *
 *  NT 4.0 hat keinen HID-Stapel: hidclass.sys, hidusb.sys und hid.dll
 *  kamen erst mit Windows 98/2000.  Programme, die HID erwarten -- allen
 *  voran DirectInput ab Fassung 5 --, finden hier nichts vor.
 *
 *  Diese DLL legt die ORIGINALEN Schnittstellen darueber.  Sie braucht
 *  keinen eigenen Treiber: unser USB-Stapel (usbnt.sys) kann bereits
 *  alles Noetige und bietet es ueber \\.\UsbGenN an --
 *      NTUG_INFO        Geraet auflisten, Hersteller/Produkt/Endpunkte
 *      NTUG_DESKRIPTOR  Deskriptoren, ausdruecklich auch Typ 0x22
 *                       (HID-Berichtsdeskriptor, an die Schnittstelle
 *                       gerichtet, wie die HID-Spezifikation es verlangt)
 *      NTUG_LESEN       Eingabeberichte vom Interrupt-Endpunkt
 *      NTUG_SCHREIBEN   Ausgabeberichte
 *      NTUG_STEUER      Get/Set Report, Set Idle, Get/Set Protocol
 *  Die Auswertung des Berichtsdeskriptors steckt hier, weil die
 *  HidP_*-Funktionen genau das nach aussen sichtbar machen.
 *
 *  EINE Abweichung vom Original, unvermeidlich:  Unter Windows 98/2000
 *  findet man HID-Geraete ueber SetupDiGetClassDevs mit GUID_DEVINTERFACE_HID.
 *  Das sind Geraeteschnittstellen aus Plug and Play, die es auf NT 4.0
 *  nicht gibt; die setupapi.dll von NT 4.0 kann das nicht.  Statt dessen:
 *      HidNt_Enumerate()   -- liefert die Pfade der HID-Geraete
 *  Genau ein Aufruf Unterschied; ab CreateFile ist alles original.
 *  (Dasselbe Verfahren wie beim Geraetemanager, der die USB- und
 *  PCI-Geraete ohne PnP auflistet -- siehe PCISCAN.)
 *
 *  Gebaut ohne C-Laufzeit, nur kernel32 (wie die uebrigen Werkzeuge).
 */

#include <windows.h>
#include <ober/ntusbgen.h>

#define HIDAPI __declspec(dllexport)

/* ---- Originale Typen (aus hidsdi.h / hidpi.h) ---------------------- */

typedef LONG NTSTATUS_HID;
#define HIDP_STATUS_SUCCESS              ((NTSTATUS_HID)0x00110000L)
#define HIDP_STATUS_INVALID_PREPARSED_DATA ((NTSTATUS_HID)0xC0110001L)
#define HIDP_STATUS_INVALID_REPORT_TYPE  ((NTSTATUS_HID)0xC0110002L)
#define HIDP_STATUS_BUFFER_TOO_SMALL     ((NTSTATUS_HID)0xC0110007L)
#define HIDP_STATUS_USAGE_NOT_FOUND      ((NTSTATUS_HID)0xC0110004L)

typedef enum { HidP_Input = 0, HidP_Output = 1, HidP_Feature = 2 } HIDP_REPORT_TYPE;

typedef struct _HIDD_ATTRIBUTES {
    ULONG  Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

typedef struct _HIDP_CAPS {
    USHORT Usage;
    USHORT UsagePage;
    USHORT InputReportByteLength;
    USHORT OutputReportByteLength;
    USHORT FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT NumberLinkCollectionNodes;
    USHORT NumberInputButtonCaps;
    USHORT NumberInputValueCaps;
    USHORT NumberInputDataIndices;
    USHORT NumberOutputButtonCaps;
    USHORT NumberOutputValueCaps;
    USHORT NumberOutputDataIndices;
    USHORT NumberFeatureButtonCaps;
    USHORT NumberFeatureValueCaps;
    USHORT NumberFeatureDataIndices;
} HIDP_CAPS, *PHIDP_CAPS;

typedef struct _HIDP_RANGE {
    USHORT UsageMin, UsageMax;
    USHORT StringMin, StringMax;
    USHORT DesignatorMin, DesignatorMax;
    USHORT DataIndexMin, DataIndexMax;
} HIDP_RANGE;

typedef struct _HIDP_NOTRANGE {
    USHORT Usage, Reserved1;
    USHORT StringIndex, Reserved2;
    USHORT DesignatorIndex, Reserved3;
    USHORT DataIndex, Reserved4;
} HIDP_NOTRANGE;

typedef struct _HIDP_BUTTON_CAPS {
    USHORT   UsagePage;
    UCHAR    ReportID;
    BOOLEAN  IsAlias;
    USHORT   BitField;
    USHORT   LinkCollection;
    USHORT   LinkUsage;
    USHORT   LinkUsagePage;
    BOOLEAN  IsRange;
    BOOLEAN  IsStringRange;
    BOOLEAN  IsDesignatorRange;
    BOOLEAN  IsAbsolute;
    ULONG    Reserved[10];
    union { HIDP_RANGE Range; HIDP_NOTRANGE NotRange; } u;
} HIDP_BUTTON_CAPS, *PHIDP_BUTTON_CAPS;

typedef struct _HIDP_VALUE_CAPS {
    USHORT   UsagePage;
    UCHAR    ReportID;
    BOOLEAN  IsAlias;
    USHORT   BitField;
    USHORT   LinkCollection;
    USHORT   LinkUsage;
    USHORT   LinkUsagePage;
    BOOLEAN  IsRange;
    BOOLEAN  IsStringRange;
    BOOLEAN  IsDesignatorRange;
    BOOLEAN  IsAbsolute;
    BOOLEAN  HasNull;
    UCHAR    Reserved;
    USHORT   BitSize;
    USHORT   ReportCount;
    USHORT   Reserved2[5];
    ULONG    UnitsExp, Units;
    LONG     LogicalMin, LogicalMax;
    LONG     PhysicalMin, PhysicalMax;
    union { HIDP_RANGE Range; HIDP_NOTRANGE NotRange; } u;
} HIDP_VALUE_CAPS, *PHIDP_VALUE_CAPS;

typedef void *PHIDP_PREPARSED_DATA;

typedef USHORT USAGE;

typedef struct _HIDP_DATA {
    USHORT DataIndex;
    USHORT Reserved;
    union { ULONG RawValue; BOOLEAN On; } u;
} HIDP_DATA, *PHIDP_DATA;

typedef struct _USAGE_AND_PAGE {
    USAGE Usage;
    USAGE UsagePage;
} USAGE_AND_PAGE, *PUSAGE_AND_PAGE;

typedef struct _HIDP_LINK_COLLECTION_NODE {
    USAGE  LinkUsage;
    USAGE  LinkUsagePage;
    USHORT Parent;
    USHORT NumberOfChildren;
    USHORT NextSibling;
    USHORT FirstChild;
    ULONG  CollectionType : 8;
    ULONG  IsAlias : 1;
    ULONG  Reserved : 23;
    PVOID  UserContext;
} HIDP_LINK_COLLECTION_NODE, *PHIDP_LINK_COLLECTION_NODE;

#define HIDP_STATUS_DATA_INDEX_NOT_FOUND ((NTSTATUS_HID)0xC0110009L)
#define HIDP_STATUS_INCOMPATIBLE_REPORT_ID ((NTSTATUS_HID)0xC011000AL)

/* ---- Eigener Aufbau hinter PHIDP_PREPARSED_DATA -------------------- */

#define HID_MAX_FELDER 256
#define HID_KENNUNG    0x4E544844ul        /* "NTHD" */

typedef struct {
    USHORT  UsagePage, Usage;
    UCHAR   ReportID;
    UCHAR   Art;                           /* 0 Eingabe 1 Ausgabe 2 Merkmal */
    UCHAR   IstKnopf;
    UCHAR   IstBereich;
    USHORT  UsageMin, UsageMax;
    USHORT  BitVersatz;                    /* im Bericht, ab Bit 0 */
    USHORT  BitGroesse, Anzahl;
    LONG    LogMin, LogMax;
    USHORT  DatenIndex;
    USHORT  BitFeld;                       /* Input/Output-Flags */
} HID_FELD;

typedef struct {
    ULONG     Kennung;
    HIDP_CAPS Caps;
    USHORT    Anzahl;
    HID_FELD  Feld[HID_MAX_FELDER];
} HID_PARSED;

/* ---- kleine Helfer ohne C-Laufzeit --------------------------------- */

static void nullen(void *z, int n)
{ unsigned char *p = (unsigned char *)z; while (n-- > 0) *p++ = 0; }

static void kopiere(void *z, const void *q, int n)
{ unsigned char *d = (unsigned char *)z; const unsigned char *s = (const unsigned char *)q;
  while (n-- > 0) *d++ = *s++; }

/* ---- Berichtsdeskriptor auswerten ---------------------------------- *
 *  Ein knapper, vollstaendiger Leser: Hauptposten (Input/Output/Feature,
 *  Collection), globale Posten (Usage Page, Logical Min/Max, Report
 *  Size/Count/ID) und lokale (Usage, Usage Min/Max).  Mehr braucht die
 *  HidP_*-Schicht nicht.                                               */

typedef struct {
    USHORT usagepage, usages[64]; int nusage;
    USHORT usage_min, usage_max; int hat_bereich;
    LONG   logmin, logmax;
    USHORT bitsize, anzahl;
    UCHAR  reportid;
} HID_ZUSTAND;

static LONG hole(const UCHAR *d, int n, int vorzeichen)
{
    LONG v = 0; int i;
    for (i = 0; i < n; i += 1) v |= ((LONG)d[i]) << (8 * i);
    if (vorzeichen && n > 0 && (d[n-1] & 0x80)) v |= ~0L << (8 * n);
    return v;
}

static void feld_anlegen(HID_PARSED *p, HID_ZUSTAND *z, int art, USHORT bitfeld,
                         USHORT *versatz)
{
    int k, n = z->anzahl;
    for (k = 0; k < n && p->Anzahl < HID_MAX_FELDER; k += 1) {
        HID_FELD *f = &p->Feld[p->Anzahl];
        nullen(f, sizeof(*f));
        f->UsagePage  = z->usagepage;
        f->ReportID   = z->reportid;
        f->Art        = (UCHAR)art;
        f->BitVersatz = *versatz;
        f->BitGroesse = z->bitsize;
        f->Anzahl     = 1;
        f->LogMin     = z->logmin;
        f->LogMax     = z->logmax;
        f->BitFeld    = bitfeld;
        /*  Knopf oder Wert: ein Bit und logisch 0..1 ist ein Knopf.  */
        f->IstKnopf   = (UCHAR)(z->bitsize == 1);
        if (z->hat_bereich) {
            f->IstBereich = 1;
            f->UsageMin = (USHORT)(z->usage_min + k);
            f->UsageMax = f->UsageMin;
            f->Usage    = f->UsageMin;
        } else {
            f->Usage    = (USHORT)(k < z->nusage ? z->usages[k]
                                                 : (z->nusage ? z->usages[z->nusage-1] : 0));
            f->UsageMin = f->UsageMax = f->Usage;
        }
        f->DatenIndex = p->Anzahl;
        *versatz = (USHORT)(*versatz + z->bitsize);
        p->Anzahl += 1;
    }
    if (p->Anzahl >= HID_MAX_FELDER) *versatz = (USHORT)(*versatz + z->bitsize * (n - k));
}

static int deskriptor_lesen(const UCHAR *d, int len, HID_PARSED *p)
{
    HID_ZUSTAND z;
    USHORT versatz[3];
    int i = 0, art;
    UCHAR letzte_id = 0;

    nullen(p, sizeof(*p));
    nullen(&z, sizeof(z));
    versatz[0] = versatz[1] = versatz[2] = 0;
    p->Kennung = HID_KENNUNG;

    while (i < len) {
        UCHAR  kopf = d[i];
        int    gr   = kopf & 0x03;
        int    typ  = (kopf >> 2) & 0x03;
        int    tag  = (kopf >> 4) & 0x0F;
        const UCHAR *dat;
        if (gr == 3) gr = 4;
        i += 1;
        dat = d + i;
        if (i + gr > len) break;
        i += gr;

        if (typ == 0) {                        /* Hauptposten */
            art = (tag == 8) ? 0 : (tag == 9) ? 1 : (tag == 11) ? 2 : -1;
            if (art >= 0) {
                USHORT bf = (USHORT)hole(dat, gr, 0);
                feld_anlegen(p, &z, art, bf, &versatz[art]);
            } else if (tag == 10) {            /* Collection */
                if (p->Caps.Usage == 0 && z.nusage > 0) {
                    p->Caps.Usage     = z.usages[0];
                    p->Caps.UsagePage = z.usagepage;
                }
            }
            z.nusage = 0; z.hat_bereich = 0;   /* lokale Posten zuruecksetzen */
        } else if (typ == 1) {                 /* globale Posten */
            switch (tag) {
            case 0:  z.usagepage = (USHORT)hole(dat, gr, 0); break;
            case 1:  z.logmin    = hole(dat, gr, 1); break;
            case 2:  z.logmax    = hole(dat, gr, 1); break;
            case 7:  z.bitsize   = (USHORT)hole(dat, gr, 0); break;
            case 8:  z.reportid  = (UCHAR)hole(dat, gr, 0);
                     letzte_id   = z.reportid;
                     versatz[0] = versatz[1] = versatz[2] = 8; /* Platz fuer die ID */
                     break;
            case 9:  z.anzahl    = (USHORT)hole(dat, gr, 0); break;
            default: break;
            }
        } else if (typ == 2) {                 /* lokale Posten */
            switch (tag) {
            case 0: if (z.nusage < 64) z.usages[z.nusage++] = (USHORT)hole(dat, gr, 0); break;
            case 1: z.usage_min = (USHORT)hole(dat, gr, 0); z.hat_bereich = 1; break;
            case 2: z.usage_max = (USHORT)hole(dat, gr, 0); z.hat_bereich = 1; break;
            default: break;
            }
        }
    }
    (void)letzte_id;

    /*  Berichtslaengen in Byte, aufgerundet; ID zaehlt mit.  */
    p->Caps.InputReportByteLength   = (USHORT)((versatz[0] + 7) / 8);
    p->Caps.OutputReportByteLength  = (USHORT)((versatz[1] + 7) / 8);
    p->Caps.FeatureReportByteLength = (USHORT)((versatz[2] + 7) / 8);
    {
        int k;
        for (k = 0; k < p->Anzahl; k += 1) {
            HID_FELD *f = &p->Feld[k];
            if (f->Art == 0) { if (f->IstKnopf) p->Caps.NumberInputButtonCaps += 1;
                               else             p->Caps.NumberInputValueCaps  += 1; }
            if (f->Art == 1) { if (f->IstKnopf) p->Caps.NumberOutputButtonCaps += 1;
                               else             p->Caps.NumberOutputValueCaps  += 1; }
            if (f->Art == 2) { if (f->IstKnopf) p->Caps.NumberFeatureButtonCaps += 1;
                               else             p->Caps.NumberFeatureValueCaps  += 1; }
        }
        p->Caps.NumberInputDataIndices   = p->Caps.NumberInputButtonCaps + p->Caps.NumberInputValueCaps;
        p->Caps.NumberOutputDataIndices  = p->Caps.NumberOutputButtonCaps + p->Caps.NumberOutputValueCaps;
        p->Caps.NumberFeatureDataIndices = p->Caps.NumberFeatureButtonCaps + p->Caps.NumberFeatureValueCaps;
        p->Caps.NumberLinkCollectionNodes = 1;
    }
    return p->Anzahl > 0;
}

/* ---- Zugriff auf den Treiber --------------------------------------- */

static BOOL rufe(HANDLE h, DWORD code, void *ein, DWORD neing,
                 void *aus, DWORD naus, DWORD *raus)
{
    DWORD got = 0;
    BOOL  ok = DeviceIoControl(h, code, ein, neing, aus, naus, &got, NULL);
    if (raus) *raus = got;
    return ok;
}

/*  Berichtsdeskriptor eines offenen Geraets holen.  */
static int desk_holen(HANDLE h, UCHAR *puf, int max)
{
    static UCHAR roh[4096];
    NTUG_DESK q;
    NTUG_ERGEBNIS *e = (NTUG_ERGEBNIS *)roh;
    DWORD got = 0;

    nullen(&q, sizeof(q));
    q.Typ    = 0x22;                       /* HID-Berichtsdeskriptor */
    q.Index  = 0;
    q.Laenge= (unsigned short)(max > 2048 ? 2048 : max);
    if (!rufe(h, IOCTL_NTUG_DESKRIPTOR, &q, sizeof(q), roh, sizeof(roh), &got)) return 0;
    if (got < sizeof(NTUG_ERGEBNIS) || e->Fehler != 0) return 0;
    if ((int)e->Laenge > max) e->Laenge = max;
    kopiere(puf, roh + sizeof(NTUG_ERGEBNIS), (int)e->Laenge);
    return (int)e->Laenge;
}

/* ---- HidD_*: die originalen Funktionen ----------------------------- */

HIDAPI void WINAPI HidD_GetHidGuid(GUID *g)
{
    /*  Der originale GUID_DEVINTERFACE_HID.  Auf NT 4.0 gibt es keine
     *  Geraeteschnittstellen, aber Programme reichen den Wert nur weiter;
     *  er muss stimmen, damit Vergleiche aufgehen.  */
    static const GUID hid = { 0x4d1e55b2, 0xf16f, 0x11cf,
                              { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    if (g) kopiere(g, &hid, sizeof(GUID));
}

HIDAPI BOOLEAN WINAPI HidD_GetAttributes(HANDLE h, PHIDD_ATTRIBUTES a)
{
    static UCHAR roh[sizeof(NTUG_ERGEBNIS) + sizeof(NTUG_INFO) + 16];
    NTUG_ERGEBNIS *e = (NTUG_ERGEBNIS *)roh;
    NTUG_INFO     *inf = (NTUG_INFO *)(roh + sizeof(NTUG_ERGEBNIS));
    DWORD got = 0;

    if (a == NULL || a->Size < sizeof(HIDD_ATTRIBUTES)) return FALSE;
    if (!rufe(h, IOCTL_NTUG_INFO, NULL, 0, roh, sizeof(roh), &got)) return FALSE;
    if (got < sizeof(NTUG_ERGEBNIS) + sizeof(NTUG_INFO) || e->Fehler != 0) return FALSE;
    a->VendorID      = inf->Hersteller;
    a->ProductID     = inf->Produkt;
    a->VersionNumber = inf->Ausgabe;
    return TRUE;
}

/*  Zeichenketten: der Treiber liefert sie schon in NTUG_INFO (als
 *  Schmalzeichen); HID gibt sie in Breitzeichen zurueck.  */
static BOOLEAN text_holen(HANDLE h, int welcher, WCHAR *z, ULONG n)
{
    static UCHAR roh[sizeof(NTUG_ERGEBNIS) + sizeof(NTUG_INFO) + 16];
    NTUG_ERGEBNIS *e = (NTUG_ERGEBNIS *)roh;
    NTUG_INFO *inf = (NTUG_INFO *)(roh + sizeof(NTUG_ERGEBNIS));
    const char *q;
    ULONG i;
    DWORD got = 0;

    if (z == NULL || n < 2) return FALSE;
    if (!rufe(h, IOCTL_NTUG_INFO, NULL, 0, roh, sizeof(roh), &got)) return FALSE;
    if (got < sizeof(NTUG_ERGEBNIS) + sizeof(NTUG_INFO) || e->Fehler != 0) return FALSE;
    q = (welcher == 0) ? inf->HerstellerText
      : (welcher == 1) ? inf->ProduktText : inf->Seriennummer;
    for (i = 0; q[i] && i < n / sizeof(WCHAR) - 1; i += 1) z[i] = (WCHAR)(UCHAR)q[i];
    z[i] = 0;
    return TRUE;
}

HIDAPI BOOLEAN WINAPI HidD_GetManufacturerString(HANDLE h, PVOID z, ULONG n)
{ return text_holen(h, 0, (WCHAR *)z, n); }

HIDAPI BOOLEAN WINAPI HidD_GetProductString(HANDLE h, PVOID z, ULONG n)
{ return text_holen(h, 1, (WCHAR *)z, n); }

HIDAPI BOOLEAN WINAPI HidD_GetSerialNumberString(HANDLE h, PVOID z, ULONG n)
{ return text_holen(h, 2, (WCHAR *)z, n); }

HIDAPI BOOLEAN WINAPI HidD_GetPreparsedData(HANDLE h, PHIDP_PREPARSED_DATA *pp)
{
    static UCHAR desk[2048];
    HID_PARSED *p;
    int len;

    if (pp == NULL) return FALSE;
    len = desk_holen(h, desk, sizeof(desk));
    if (len <= 0) return FALSE;
    p = (HID_PARSED *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HID_PARSED));
    if (p == NULL) return FALSE;
    if (!deskriptor_lesen(desk, len, p)) {
        HeapFree(GetProcessHeap(), 0, p);
        return FALSE;
    }
    *pp = (PHIDP_PREPARSED_DATA)p;
    return TRUE;
}

HIDAPI BOOLEAN WINAPI HidD_FreePreparsedData(PHIDP_PREPARSED_DATA pp)
{
    if (pp == NULL) return FALSE;
    HeapFree(GetProcessHeap(), 0, pp);
    return TRUE;
}

/*  Berichte: Get/Set ueber Steuerleitung (HID-Klasse), Eingabe ueber
 *  den Interrupt-Endpunkt leistet ReadFile auf dem Geraet.  */
HIDAPI BOOLEAN WINAPI HidD_GetFeature(HANDLE h, PVOID z, ULONG n)
{
    NTUG_STEUER q;
    static UCHAR roh[1024];
    NTUG_ERGEBNIS *e = (NTUG_ERGEBNIS *)roh;
    DWORD got = 0;
    if (z == NULL || n == 0 || n > sizeof(roh) - sizeof(NTUG_ERGEBNIS)) return FALSE;
    nullen(&q, sizeof(q));
    q.bmRequestType = 0xA1;                    /* IN | Klasse | Schnittstelle */
    q.bRequest     = 0x01;                    /* GET_REPORT */
    q.wValue      = (unsigned short)(0x0300 | ((UCHAR *)z)[0]);   /* Feature|ID */
    q.wLength    = (unsigned short)n;
    if (!rufe(h, IOCTL_NTUG_STEUER, &q, sizeof(q), roh, sizeof(roh), &got)) return FALSE;
    if (got < sizeof(NTUG_ERGEBNIS) || e->Fehler != 0) return FALSE;
    kopiere(z, roh + sizeof(NTUG_ERGEBNIS), (int)(e->Laenge < n ? e->Laenge : n));
    return TRUE;
}

HIDAPI BOOLEAN WINAPI HidD_SetFeature(HANDLE h, PVOID z, ULONG n)
{
    static UCHAR puf[sizeof(NTUG_STEUER) + 512];
    NTUG_STEUER *q = (NTUG_STEUER *)puf;
    static UCHAR roh[64];
    DWORD got = 0;
    if (z == NULL || n == 0 || n > 512) return FALSE;
    nullen(q, sizeof(*q));
    q->bmRequestType = 0x21;                   /* OUT | Klasse | Schnittstelle */
    q->bRequest     = 0x09;                   /* SET_REPORT */
    q->wValue      = (unsigned short)(0x0300 | ((UCHAR *)z)[0]);
    q->wLength    = (unsigned short)n;
    kopiere(puf + sizeof(NTUG_STEUER), z, (int)n);
    return rufe(h, IOCTL_NTUG_STEUER, puf, sizeof(NTUG_STEUER) + n, roh, sizeof(roh), &got);
}

HIDAPI BOOLEAN WINAPI HidD_SetOutputReport(HANDLE h, PVOID z, ULONG n)
{
    static UCHAR puf[sizeof(NTUG_STEUER) + 512];
    NTUG_STEUER *q = (NTUG_STEUER *)puf;
    static UCHAR roh[64];
    DWORD got = 0;
    if (z == NULL || n == 0 || n > 512) return FALSE;
    nullen(q, sizeof(*q));
    q->bmRequestType = 0x21;
    q->bRequest     = 0x09;
    q->wValue      = (unsigned short)(0x0200 | ((UCHAR *)z)[0]);  /* Output|ID */
    q->wLength    = (unsigned short)n;
    kopiere(puf + sizeof(NTUG_STEUER), z, (int)n);
    return rufe(h, IOCTL_NTUG_STEUER, puf, sizeof(NTUG_STEUER) + n, roh, sizeof(roh), &got);
}

HIDAPI BOOLEAN WINAPI HidD_SetNumInputBuffers(HANDLE h, ULONG n)
{ (void)h; (void)n; return TRUE; }        /* der Treiber puffert selbst */

HIDAPI BOOLEAN WINAPI HidD_FlushQueue(HANDLE h)
{ (void)h; return TRUE; }

/* ---- HidP_*: Auswertung ------------------------------------------- */

static HID_PARSED *pruefe(PHIDP_PREPARSED_DATA pp)
{
    HID_PARSED *p = (HID_PARSED *)pp;
    return (p != NULL && p->Kennung == HID_KENNUNG) ? p : NULL;
}

HIDAPI NTSTATUS_HID WINAPI HidP_GetCaps(PHIDP_PREPARSED_DATA pp, PHIDP_CAPS c)
{
    HID_PARSED *p = pruefe(pp);
    if (p == NULL || c == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    kopiere(c, &p->Caps, sizeof(HIDP_CAPS));
    return HIDP_STATUS_SUCCESS;
}

HIDAPI NTSTATUS_HID WINAPI HidP_GetButtonCaps(HIDP_REPORT_TYPE art, PHIDP_BUTTON_CAPS c,
                                              PUSHORT anz, PHIDP_PREPARSED_DATA pp)
{
    HID_PARSED *p = pruefe(pp);
    USHORT n = 0, k;
    if (p == NULL || c == NULL || anz == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        if (f->Art != (UCHAR)art || !f->IstKnopf) continue;
        if (n >= *anz) return HIDP_STATUS_BUFFER_TOO_SMALL;
        nullen(&c[n], sizeof(c[n]));
        c[n].UsagePage     = f->UsagePage;
        c[n].ReportID      = f->ReportID;
        c[n].BitField      = f->BitFeld;
        c[n].IsAbsolute    = (BOOLEAN)((f->BitFeld & 0x04) == 0);
        c[n].IsRange       = FALSE;
        c[n].u.NotRange.Usage     = f->Usage;
        c[n].u.NotRange.DataIndex = f->DatenIndex;
        n += 1;
    }
    *anz = n;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI NTSTATUS_HID WINAPI HidP_GetValueCaps(HIDP_REPORT_TYPE art, PHIDP_VALUE_CAPS c,
                                             PUSHORT anz, PHIDP_PREPARSED_DATA pp)
{
    HID_PARSED *p = pruefe(pp);
    USHORT n = 0, k;
    if (p == NULL || c == NULL || anz == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        if (f->Art != (UCHAR)art || f->IstKnopf) continue;
        if (n >= *anz) return HIDP_STATUS_BUFFER_TOO_SMALL;
        nullen(&c[n], sizeof(c[n]));
        c[n].UsagePage  = f->UsagePage;
        c[n].ReportID   = f->ReportID;
        c[n].BitField   = f->BitFeld;
        c[n].BitSize    = f->BitGroesse;
        c[n].ReportCount= 1;
        c[n].LogicalMin = f->LogMin;
        c[n].LogicalMax = f->LogMax;
        c[n].IsAbsolute = (BOOLEAN)((f->BitFeld & 0x04) == 0);
        c[n].IsRange    = FALSE;
        c[n].u.NotRange.Usage     = f->Usage;
        c[n].u.NotRange.DataIndex = f->DatenIndex;
        n += 1;
    }
    *anz = n;
    return HIDP_STATUS_SUCCESS;
}

/*  Bits aus dem Bericht holen.  */
static ULONG bits_holen(const UCHAR *b, ULONG len, USHORT versatz, USHORT groesse)
{
    ULONG v = 0; USHORT i;
    for (i = 0; i < groesse; i += 1) {
        USHORT bit = (USHORT)(versatz + i);
        if ((ULONG)(bit / 8) >= len) break;
        if (b[bit / 8] & (1 << (bit % 8))) v |= (1ul << i);
    }
    return v;
}

HIDAPI NTSTATUS_HID WINAPI HidP_GetUsageValue(HIDP_REPORT_TYPE art, USHORT seite,
                                              USHORT collection, USHORT usage,
                                              PULONG wert, PHIDP_PREPARSED_DATA pp,
                                              PCHAR bericht, ULONG len)
{
    HID_PARSED *p = pruefe(pp);
    USHORT k;
    (void)collection;
    if (p == NULL || wert == NULL || bericht == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        if (f->Art != (UCHAR)art || f->IstKnopf) continue;
        if (seite != 0 && f->UsagePage != seite) continue;
        if (f->Usage != usage) continue;
        *wert = bits_holen((const UCHAR *)bericht, len, f->BitVersatz, f->BitGroesse);
        return HIDP_STATUS_SUCCESS;
    }
    return HIDP_STATUS_USAGE_NOT_FOUND;
}

HIDAPI NTSTATUS_HID WINAPI HidP_GetUsages(HIDP_REPORT_TYPE art, USHORT seite,
                                          USHORT collection, PUSHORT liste, PULONG anz,
                                          PHIDP_PREPARSED_DATA pp, PCHAR bericht, ULONG len)
{
    HID_PARSED *p = pruefe(pp);
    ULONG n = 0;
    USHORT k;
    (void)collection;
    if (p == NULL || liste == NULL || anz == NULL || bericht == NULL)
        return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        if (f->Art != (UCHAR)art || !f->IstKnopf) continue;
        if (seite != 0 && f->UsagePage != seite) continue;
        if (bits_holen((const UCHAR *)bericht, len, f->BitVersatz, 1) == 0) continue;
        if (n >= *anz) { *anz = n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
        liste[n++] = f->Usage;
    }
    *anz = n;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI ULONG WINAPI HidP_MaxUsageListLength(HIDP_REPORT_TYPE art, USHORT seite,
                                            PHIDP_PREPARSED_DATA pp)
{
    HID_PARSED *p = pruefe(pp);
    ULONG n = 0; USHORT k;
    if (p == NULL) return 0;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        if (f->Art == (UCHAR)art && f->IstKnopf && (seite == 0 || f->UsagePage == seite)) n += 1;
    }
    return n;
}

/* ---- Von DirectInput zusaetzlich verlangt ------------------------- *
 *  Microsofts DirectInput laedt 21 HID-Funktionen.  Die folgenden acht
 *  fehlten in der ersten Fassung; ohne HidP_GetData liest DirectInput
 *  ueberhaupt keine Eingabeberichte.                                   */

/*  Bits in den Bericht schreiben (Gegenstueck zu bits_holen).  */
static void bits_setzen(UCHAR *b, ULONG len, USHORT versatz, USHORT groesse, ULONG wert)
{
    USHORT i;
    for (i = 0; i < groesse; i += 1) {
        USHORT bit = (USHORT)(versatz + i);
        if ((ULONG)(bit / 8) >= len) break;
        if (wert & (1ul << i)) b[bit / 8] |= (UCHAR)(1 << (bit % 8));
        else                   b[bit / 8] &= (UCHAR)~(1 << (bit % 8));
    }
}

/*  Gehoert das Feld zu diesem Bericht?  Ohne Bericht-IDs (ReportID 0)
 *  gehoert alles dazu; sonst entscheidet das erste Byte.               */
static int passt(HID_FELD *f, const UCHAR *bericht, ULONG len)
{
    if (f->ReportID == 0) return 1;
    return (len > 0 && bericht[0] == f->ReportID);
}

HIDAPI ULONG WINAPI HidP_MaxDataListLength(HIDP_REPORT_TYPE art, PHIDP_PREPARSED_DATA pp)
{
    HID_PARSED *p = pruefe(pp);
    ULONG n = 0; USHORT k;
    if (p == NULL) return 0;
    for (k = 0; k < p->Anzahl; k += 1)
        if (p->Feld[k].Art == (UCHAR)art) n += 1;
    return n;
}

/*  HidP_GetData: ALLE Daten eines Berichts auf einmal.  Knoepfe
 *  erscheinen nur, wenn sie gedrueckt sind (On = TRUE), Werte immer mit
 *  ihrem Rohwert.  Genau so liest DirectInput seine Geraete.           */
HIDAPI NTSTATUS_HID WINAPI HidP_GetData(HIDP_REPORT_TYPE art, PHIDP_DATA daten,
                                        PULONG anz, PHIDP_PREPARSED_DATA pp,
                                        PCHAR bericht, ULONG len)
{
    HID_PARSED *p = pruefe(pp);
    ULONG n = 0;
    USHORT k;

    if (p == NULL || anz == NULL || bericht == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        ULONG v;
        if (f->Art != (UCHAR)art) continue;
        if (!passt(f, (const UCHAR *)bericht, len)) continue;
        v = bits_holen((const UCHAR *)bericht, len, f->BitVersatz, f->BitGroesse);
        if (f->IstKnopf && v == 0) continue;         /* nur gedrueckte */
        if (daten == NULL || n >= *anz) { n += 1; continue; }
        daten[n].DataIndex = f->DatenIndex;
        daten[n].Reserved  = 0;
        if (f->IstKnopf) daten[n].u.On = TRUE;
        else             daten[n].u.RawValue = v;
        n += 1;
    }
    if (daten == NULL || n > *anz) { *anz = n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    *anz = n;
    return HIDP_STATUS_SUCCESS;
}

/*  HidP_SetData: Gegenstueck fuer Ausgabe- und Merkmalsberichte.  */
HIDAPI NTSTATUS_HID WINAPI HidP_SetData(HIDP_REPORT_TYPE art, PHIDP_DATA daten,
                                        PULONG anz, PHIDP_PREPARSED_DATA pp,
                                        PCHAR bericht, ULONG len)
{
    HID_PARSED *p = pruefe(pp);
    ULONG i;
    USHORT k;

    if (p == NULL || daten == NULL || anz == NULL || bericht == NULL)
        return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (i = 0; i < *anz; i += 1) {
        int gefunden = 0;
        for (k = 0; k < p->Anzahl; k += 1) {
            HID_FELD *f = &p->Feld[k];
            if (f->Art != (UCHAR)art || f->DatenIndex != daten[i].DataIndex) continue;
            bits_setzen((UCHAR *)bericht, len, f->BitVersatz, f->BitGroesse,
                        f->IstKnopf ? (daten[i].u.On ? 1ul : 0ul) : daten[i].u.RawValue);
            gefunden = 1;
            break;
        }
        if (!gefunden) { *anz = i; return HIDP_STATUS_DATA_INDEX_NOT_FOUND; }
    }
    return HIDP_STATUS_SUCCESS;
}

/*  HidP_GetUsagesEx: wie HidP_GetUsages, aber mit Seite je Eintrag --
 *  noetig, wenn ein Bericht Knoepfe mehrerer Seiten mischt.            */
HIDAPI NTSTATUS_HID WINAPI HidP_GetUsagesEx(HIDP_REPORT_TYPE art, USHORT collection,
                                            PUSAGE_AND_PAGE liste, ULONG *anz,
                                            PHIDP_PREPARSED_DATA pp, PCHAR bericht, ULONG len)
{
    HID_PARSED *p = pruefe(pp);
    ULONG n = 0;
    USHORT k;
    (void)collection;
    if (p == NULL || anz == NULL || bericht == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        if (f->Art != (UCHAR)art || !f->IstKnopf) continue;
        if (!passt(f, (const UCHAR *)bericht, len)) continue;
        if (bits_holen((const UCHAR *)bericht, len, f->BitVersatz, 1) == 0) continue;
        if (liste == NULL || n >= *anz) { n += 1; continue; }
        liste[n].Usage     = f->Usage;
        liste[n].UsagePage = f->UsagePage;
        n += 1;
    }
    if (liste == NULL || n > *anz) { *anz = n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    *anz = n;
    return HIDP_STATUS_SUCCESS;
}

/*  HidP_GetScaledUsageValue: Rohwert auf den physikalischen Bereich
 *  umrechnen und vorzeichenrichtig liefern.  Haben wir keine
 *  physikalischen Grenzen, gilt der logische Bereich.                  */
HIDAPI NTSTATUS_HID WINAPI HidP_GetScaledUsageValue(HIDP_REPORT_TYPE art, USHORT seite,
                                                    USHORT collection, USHORT usage,
                                                    PLONG wert, PHIDP_PREPARSED_DATA pp,
                                                    PCHAR bericht, ULONG len)
{
    HID_PARSED *p = pruefe(pp);
    USHORT k;
    (void)collection;
    if (p == NULL || wert == NULL || bericht == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        LONG v;
        if (f->Art != (UCHAR)art || f->IstKnopf) continue;
        if (seite != 0 && f->UsagePage != seite) continue;
        if (f->Usage != usage) continue;
        if (!passt(f, (const UCHAR *)bericht, len)) continue;
        v = (LONG)bits_holen((const UCHAR *)bericht, len, f->BitVersatz, f->BitGroesse);
        /*  Vorzeichen nachziehen, wenn der logische Bereich negativ wird.  */
        if (f->LogMin < 0 && f->BitGroesse < 32 &&
            (v & (1L << (f->BitGroesse - 1))))
            v |= ~0L << f->BitGroesse;
        *wert = v;
        return HIDP_STATUS_SUCCESS;
    }
    return HIDP_STATUS_USAGE_NOT_FOUND;
}

/*  HidP_GetSpecificButtonCaps: wie HidP_GetButtonCaps, aber auf Seite,
 *  Collection und Nutzung eingegrenzt.  0 heisst "alle".               */
HIDAPI NTSTATUS_HID WINAPI HidP_GetSpecificButtonCaps(HIDP_REPORT_TYPE art, USHORT seite,
                                                      USHORT collection, USHORT usage,
                                                      PHIDP_BUTTON_CAPS c, PUSHORT anz,
                                                      PHIDP_PREPARSED_DATA pp)
{
    HID_PARSED *p = pruefe(pp);
    USHORT n = 0, k;
    (void)collection;
    if (p == NULL || anz == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    for (k = 0; k < p->Anzahl; k += 1) {
        HID_FELD *f = &p->Feld[k];
        if (f->Art != (UCHAR)art || !f->IstKnopf) continue;
        if (seite != 0 && f->UsagePage != seite) continue;
        if (usage != 0 && f->Usage != usage) continue;
        if (c == NULL || n >= *anz) { n += 1; continue; }
        nullen(&c[n], sizeof(c[n]));
        c[n].UsagePage  = f->UsagePage;
        c[n].ReportID   = f->ReportID;
        c[n].BitField   = f->BitFeld;
        c[n].IsAbsolute = (BOOLEAN)((f->BitFeld & 0x04) == 0);
        c[n].IsRange    = FALSE;
        c[n].u.NotRange.Usage     = f->Usage;
        c[n].u.NotRange.DataIndex = f->DatenIndex;
        n += 1;
    }
    if (c == NULL || n > *anz) { *anz = n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    *anz = n;
    return HIDP_STATUS_SUCCESS;
}

/*  HidP_GetLinkCollectionNodes: wir fuehren bisher eine einzige
 *  oberste Collection -- die meldet diese Funktion.  Fuer DirectInput
 *  reicht das; es braucht sie, um Achsen einer Collection zuzuordnen.  */
HIDAPI NTSTATUS_HID WINAPI HidP_GetLinkCollectionNodes(PHIDP_LINK_COLLECTION_NODE knoten,
                                                       PULONG anz, PHIDP_PREPARSED_DATA pp)
{
    HID_PARSED *p = pruefe(pp);
    if (p == NULL || anz == NULL) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    if (knoten == NULL || *anz < 1) { *anz = 1; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    nullen(knoten, sizeof(*knoten));
    knoten[0].LinkUsage       = p->Caps.Usage;
    knoten[0].LinkUsagePage   = p->Caps.UsagePage;
    knoten[0].Parent          = 0;
    knoten[0].NumberOfChildren= 0;
    knoten[0].NextSibling     = 0;
    knoten[0].FirstChild      = 0;
    knoten[0].CollectionType  = 1;            /* Application */
    *anz = 1;
    return HIDP_STATUS_SUCCESS;
}

/*  HidD_GetInputReport: Eingabebericht ueber die Steuerleitung holen
 *  (GET_REPORT, Typ Input) -- fuer Geraete, die man abfragen statt
 *  abwarten will.                                                      */
HIDAPI BOOLEAN WINAPI HidD_GetInputReport(HANDLE h, PVOID z, ULONG n)
{
    NTUG_STEUER q;
    static UCHAR roh[1024];
    NTUG_ERGEBNIS *e = (NTUG_ERGEBNIS *)roh;
    DWORD got = 0;
    if (z == NULL || n == 0 || n > sizeof(roh) - sizeof(NTUG_ERGEBNIS)) return FALSE;
    nullen(&q, sizeof(q));
    q.bmRequestType = 0xA1;
    q.bRequest      = 0x01;                   /* GET_REPORT */
    q.wValue        = (unsigned short)(0x0100 | ((UCHAR *)z)[0]);   /* Input|ID */
    q.wLength       = (unsigned short)n;
    if (!rufe(h, IOCTL_NTUG_STEUER, &q, sizeof(q), roh, sizeof(roh), &got)) return FALSE;
    if (got < sizeof(NTUG_ERGEBNIS) || e->Fehler != 0) return FALSE;
    kopiere(z, roh + sizeof(NTUG_ERGEBNIS), (int)(e->Laenge < n ? e->Laenge : n));
    return TRUE;
}

/* ---- Auffinden der Geraete (NT-4.0-Ersatz fuer SetupAPI) ----------- *
 *  Liefert die Pfade aller HID-Schnittstellen, durch Null getrennt, am
 *  Ende eine zweite Null -- dieselbe Form, die SetupAPI-Aufrufer ohnehin
 *  verarbeiten.  Ohne Puffer: nur die noetige Groesse in *n.           */

HIDAPI BOOLEAN WINAPI HidNt_Enumerate(char *puffer, ULONG *n)
{
    char pfad[64];
    ULONG gebraucht = 0;
    int i;

    if (n == NULL) return FALSE;
    for (i = 0; i < 64; i += 1) {
        HANDLE h;
        static UCHAR roh[sizeof(NTUG_ERGEBNIS) + sizeof(NTUG_INFO) + 16];
        NTUG_ERGEBNIS *e = (NTUG_ERGEBNIS *)roh;
        NTUG_INFO *inf = (NTUG_INFO *)(roh + sizeof(NTUG_ERGEBNIS));
        DWORD got = 0;
        int len = 0;

        wsprintfA(pfad, "\\\\.\\UsbGen%d", i);
        h = CreateFileA(pfad, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (rufe(h, IOCTL_NTUG_INFO, NULL, 0, roh, sizeof(roh), &got) &&
            got >= sizeof(NTUG_ERGEBNIS) + sizeof(NTUG_INFO) && e->Fehler == 0 &&
            inf->IfKlasse == 3) {                     /* 3 = HID */
            while (pfad[len]) len += 1;
            len += 1;                                  /* Null mitzaehlen */
            if (puffer != NULL && gebraucht + len + 1 <= *n)
                kopiere(puffer + gebraucht, pfad, len);
            gebraucht += len;
        }
        CloseHandle(h);
    }
    if (puffer != NULL && gebraucht + 1 <= *n) puffer[gebraucht] = 0;
    gebraucht += 1;
    if (puffer == NULL || gebraucht > *n) { *n = gebraucht; return FALSE; }
    *n = gebraucht;
    return TRUE;
}

BOOL WINAPI DllEntry(HINSTANCE h, DWORD grund, LPVOID r)
{ (void)h; (void)grund; (void)r; return TRUE; }
