/*
 *  ober/ntmaustasten.c -- Maustasten 1 bis 8 belegen.
 *
 *  NT 4.0 kennt nur drei Maustasten; die Seitentasten (XBUTTON) kamen mit
 *  Windows 2000.  Der Treiber sieht sie aber -- im Berichtsdeskriptor --
 *  und uebersetzt sie: in Tasten, die er ueber seine eigene
 *  Tastatur-Verbindung an kbdclass gibt, oder in eine Mausfunktion, die
 *  NT kennt.
 *
 *  Belegung in  Services\<dienst>\Parameters,  REG_SZ  Taste1 .. Taste8.
 *  Setzen zur Laufzeit: ntu_maustaste_setzen (Steuergeraet,
 *  IOCTL_NTU_MAUS_SETZEN, aus usbmaus.cpl und USBINST taste) -- wirkt
 *  sofort und schreibt TasteN zurueck.
 *
 *      alt+links         Tasten, mit + verbunden (hier: Zurueck)
 *      strg+umschalt+esc
 *      f5  bildab  enter
 *      code:1c           Rohcode (Satz 1), code:e05b mit E0-Vorsatz
 *      mitte             wie die mittlere Maustaste, auch zum Ziehen
 *      doppelklick       zwei Linksklicks
 *      klick-links       wie die linke Maustaste (auch zum Ziehen)
 *      klick-rechts      wie die rechte Maustaste
 *      aus               nichts
 *
 *  Vorgabe ohne Eintrag: Taste1 = klick-links, Taste2 = klick-rechts,
 *  Taste3 = mitte, Taste4 = alt+links (Zurueck), Taste5 = alt+rechts
 *  (Vorwaerts), 6..8 aus -- wie spaetere Windows-Versionen; Internet
 *  Explorer und Explorer unter NT 4.0 verstehen beides.
 *
 *  Buchstaben nach DEUTSCHER Belegung: Tastencodes sind Positionen, und
 *  "strg+z" soll die Taste mit dem Z meinen, nicht die mit dem Y.
 *
 *  Gehalten ist gehalten: druecken schickt die Tasten nieder, loslassen
 *  laesst sie in umgekehrter Reihenfolge los.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <ober/ntkbdmou.h>

#define E0 0x100

NTU_MAUSAKTION ntu_mausaktion[NTU_MAUS_TASTEN];
KSPIN_LOCK     ntu_maus_sperre;
volatile LONG  ntu_maus_roh, ntu_maus_berichte, ntu_maus_knoepfe, ntu_maus_anzahl;
volatile LONG  ntu_maus_leer;

static WCHAR   ntu_maus_pfad[260];          /* ...\Services\<dienst>\Parameters */

static const struct { const char *name; USHORT code; } ntu_tastennamen[] = {
    { "strg", 0x1D },      { "ctrl", 0x1D },
    { "alt", 0x38 },       { "altgr", E0 | 0x38 },
    { "umschalt", 0x2A },  { "shift", 0x2A },
    { "win", E0 | 0x5B },  { "menue", E0 | 0x5D },
    { "links", E0 | 0x4B },  { "rechts", E0 | 0x4D },
    { "hoch", E0 | 0x48 },   { "runter", E0 | 0x50 },
    { "bildauf", E0 | 0x49 },{ "bildab", E0 | 0x51 },
    { "pos1", E0 | 0x47 },   { "ende", E0 | 0x4F },
    { "einfg", E0 | 0x52 },  { "entf", E0 | 0x53 },
    { "enter", 0x1C },     { "esc", 0x01 },     { "tab", 0x0F },
    { "rueck", 0x0E },     { "leer", 0x39 },
    { "f1", 0x3B }, { "f2", 0x3C }, { "f3", 0x3D }, { "f4", 0x3E },
    { "f5", 0x3F }, { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 },
    { "f9", 0x43 }, { "f10", 0x44 }, { "f11", 0x57 }, { "f12", 0x58 },
    /*  F13..F24: Satz-1-Codes nach Microsofts Scan-Code-Tabelle; NT 4.0
     *  macht daraus VK_F13..VK_F24.  */
    { "f13", 0x64 }, { "f14", 0x65 }, { "f15", 0x66 }, { "f16", 0x67 },
    { "f17", 0x68 }, { "f18", 0x69 }, { "f19", 0x6A }, { "f20", 0x6B },
    { "f21", 0x6C }, { "f22", 0x6D }, { "f23", 0x6E }, { "f24", 0x76 },
    { 0, 0 }
};

/*  Buchstaben a..z, deutsche Belegung (Z und Y getauscht gegenueber US).  */

static const USHORT ntu_buchstaben[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,  /* a-j */
    0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,  /* k-t */
    0x16, 0x2F, 0x11, 0x2D, 0x2C, 0x15                           /* u-z */
};

static int
ntu_hex (char c)
{
    return (c >= '0' && c <= '9') ? c - '0' :
           (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/*  Ein Wort -> Tastencode, 0 = unbekannt.  */

static USHORT
ntu_wort (const char *w, int n)
{
    int k;

    if (n == 1 && w[0] >= 'a' && w[0] <= 'z') {
        return ntu_buchstaben[w[0] - 'a'];
    }

    if (n == 1 && w[0] >= '1' && w[0] <= '9') {
        return (USHORT)(0x02 + (w[0] - '1'));
    }

    if (n == 1 && w[0] == '0') {
        return 0x0B;
    }

    if (n >= 7 && n <= 9 && w[0] == 'c' && w[1] == 'o' && w[2] == 'd' &&
        w[3] == 'e' && w[4] == ':') {
        USHORT c = 0;
        for (k = 5; k < n; k += 1) {
            int h = ntu_hex(w[k]);
            if (h < 0) return 0;
            c = (USHORT)((c << 4) | h);
        }
        if ((c >> 8) == 0xE0) {
            return (USHORT)(E0 | (c & 0xFF));
        }
        return (c > 0 && c < 0x80) ? c : 0;
    }

    for (k = 0; ntu_tastennamen[k].name != 0; k += 1) {
        const char *t = ntu_tastennamen[k].name;
        int         i = 0;

        while (i < n && t[i] != '\0' && t[i] == w[i]) {
            i += 1;
        }

        if (i == n && t[i] == '\0') {
            return ntu_tastennamen[k].code;
        }
    }

    return 0;
}

/*  Eine Angabe zerlegen.  0 = verstanden.  */

static int
ntu_belegung (const char *s, NTU_MAUSAKTION *a)
{
    char w[16];
    int  n = 0;

    memset(a, 0, sizeof(*a));

    if (strcmp(s, "aus") == 0 || strcmp(s, "nichts") == 0 || s[0] == '\0') {
        a->art = NTU_MA_NICHTS;
        return 0;
    }

    if (strcmp(s, "mitte") == 0) {
        a->art = NTU_MA_MITTE;
        return 0;
    }

    if (strcmp(s, "doppelklick") == 0) {
        a->art = NTU_MA_DOPPEL;
        return 0;
    }

    /*  Maustasten selbst -- "links"/"rechts" allein sind Pfeiltasten
     *  (alt+links), deshalb mit Vorsatz.  */

    if (strcmp(s, "klick-links") == 0 || strcmp(s, "linke-taste") == 0) {
        a->art = NTU_MA_LINKS;
        return 0;
    }
    if (strcmp(s, "klick-rechts") == 0 || strcmp(s, "rechte-taste") == 0) {
        a->art = NTU_MA_RECHTS;
        return 0;
    }

    a->art = NTU_MA_TASTEN;

    for (;;) {
        char c = *s++;

        if (c == '+' || c == '\0') {
            USHORT code = ntu_wort(w, n);

            if (code == 0 || a->n >= (int)(sizeof(a->code) / sizeof(a->code[0]))) {
                a->art = NTU_MA_NICHTS;
                return -1;
            }

            a->code[a->n++] = code;
            n = 0;

            if (c == '\0') {
                return 0;
            }
        } else if (c != ' ' && n < (int)sizeof(w) - 1) {
            w[n++] = c;
        }
    }
}

/*  Fuer die Sondertasten (ntsondertasten.c): derselbe Leser.  */

int
ntu_belegung_lesen (const char *s, NTU_MAUSAKTION *a)
{
    return ntu_belegung(s, a);
}

/*  Den Text der Belegung am Eintrag merken (fuer die Systemsteuerung).  */

static void
ntu_text_merken (NTU_MAUSAKTION *a, const char *t)
{
    int i;
    for (i = 0; t[i] != '\0' && i < (int)sizeof(a->text) - 1; i += 1) {
        a->text[i] = t[i];
    }
    a->text[i] = '\0';
}

/*  Aus DriverEntry: Parameters\Taste1..Taste8 lesen.  */

VOID
ntu_maustasten_lesen (PUNICODE_STRING reg)
{
    static const WCHAR *namen[NTU_MAUS_TASTEN] = {
        L"Taste1", L"Taste2", L"Taste3", L"Taste4",
        L"Taste5", L"Taste6", L"Taste7", L"Taste8"
    };
    static const char  *vorgabe[NTU_MAUS_TASTEN] = {
        "klick-links", "klick-rechts", "mitte", "alt+links",
        "alt+rechts", "aus", "aus", "aus"
    };
    WCHAR pfad[260];
    ULONG n = reg->Length / sizeof(WCHAR);
    int   k;

    KeInitializeSpinLock(&ntu_maus_sperre);
    if (n < 240) {
        memcpy(ntu_maus_pfad, reg->Buffer, n * sizeof(WCHAR));
        memcpy(ntu_maus_pfad + n, L"\\Parameters", 12 * sizeof(WCHAR));
        ntu_maus_pfad[n + 11] = 0;
    }

    for (k = 0; k < NTU_MAUS_TASTEN; k += 1) {

        WCHAR                    puffer[64];
        UNICODE_STRING           u;
        RTL_QUERY_REGISTRY_TABLE t[2];
        char                     a[64];
        int                      i, gelesen = 0;

        if (n < 240) {

            memcpy(pfad, reg->Buffer, n * sizeof(WCHAR));
            memcpy(pfad + n, L"\\Parameters", 12 * sizeof(WCHAR));
            pfad[n + 11] = 0;

            u.Buffer        = puffer;
            u.Length        = 0;
            u.MaximumLength = sizeof(puffer) - sizeof(WCHAR);

            memset(t, 0, sizeof(t));
            t[0].Flags        = RTL_QUERY_REGISTRY_DIRECT;
            t[0].Name         = (PWSTR)namen[k];
            t[0].EntryContext = &u;

            gelesen = NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                                                        pfad, t, 0, 0)) &&
                      u.Length != 0;
        }

        if (gelesen) {

            /*  In Kleinbuchstaben, nur ASCII.  */

            for (i = 0; i < (int)(u.Length / sizeof(WCHAR)) && i < 63; i += 1) {
                WCHAR c = u.Buffer[i];
                a[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : (c < 128 ? c : '?'));
            }
            a[i] = '\0';

        } else {
            strcpy(a, vorgabe[k]);
        }

        if (ntu_belegung(a, &ntu_mausaktion[k]) != 0) {
            ntu_printf("ums: Taste%d = \"%s\" nicht verstanden -- aus\n", k + 1, a);
            ntu_text_merken(&ntu_mausaktion[k], "aus");
        } else {
            ntu_text_merken(&ntu_mausaktion[k], a);

            /*  Melden: was eingetragen ist, und von den Vorgaben nur die
             *  der Tasten 4..8, die etwas tun (1..3 sind selbstverstaendlich).  */

            if (gelesen || (k >= 3 && ntu_mausaktion[k].art != NTU_MA_NICHTS)) {
                ntu_printf("ums: Taste%d = %s%s\n", k + 1, a, gelesen ? "" : " (Vorgabe)");
            }
        }
    }
}


/*  Eine Belegung setzen -- aus der Systemsteuerung "USB-Maus" ueber das
 *  Steuergeraet.  Wirkt sofort und wird unter Parameters\TasteN
 *  gespeichert.  PASSIVE_LEVEL.
 *
 *  Sofort heisst: die naechste gedrueckte Taste nimmt die neue Belegung.
 *  Eine GEHALTENE Taste loest beim Loslassen die Belegung, die beim
 *  Druecken galt (ntums.c merkt sie sich) -- sonst bliebe z. B. "klick-
 *  links" gedrueckt, wenn man waehrenddessen umbelegt.  */

NTSTATUS
ntu_maustaste_setzen (int taste, const char *text)
{
    NTU_MAUSAKTION neu;
    char           a[48];
    WCHAR          w[48], name[8];
    KIRQL          alt;
    int            i;

    if (taste < 1 || taste > NTU_MAUS_TASTEN) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; text[i] != '\0' && i < (int)sizeof(a) - 1; i += 1) {
        char c = text[i];
        a[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    a[i] = '\0';

    if (ntu_belegung(a, &neu) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    ntu_text_merken(&neu, a[0] ? a : "aus");

    KeAcquireSpinLock(&ntu_maus_sperre, &alt);
    ntu_mausaktion[taste - 1] = neu;
    KeReleaseSpinLock(&ntu_maus_sperre, alt);

    ntu_printf("ums: Taste%d = %s (Systemsteuerung)\n", taste, neu.text);

    if (ntu_maus_pfad[0] != 0) {
        for (i = 0; neu.text[i] != '\0'; i += 1) {
            w[i] = (WCHAR)(unsigned char)neu.text[i];
        }
        w[i] = 0;
        name[0] = L'T'; name[1] = L'a'; name[2] = L's'; name[3] = L't'; name[4] = L'e';
        name[5] = (WCHAR)(L'0' + taste); name[6] = 0;
        RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, ntu_maus_pfad, name, REG_SZ,
                              w, (ULONG)(i + 1) * sizeof(WCHAR));
    }
    return STATUS_SUCCESS;
}

void
ntu_maustaste_holen (int taste, char *ziel, int max)
{
    KIRQL alt;
    int   i;

    ziel[0] = '\0';
    if (taste < 1 || taste > NTU_MAUS_TASTEN || max < 2) {
        return;
    }
    KeAcquireSpinLock(&ntu_maus_sperre, &alt);
    for (i = 0; ntu_mausaktion[taste - 1].text[i] != '\0' && i < max - 1; i += 1) {
        ziel[i] = ntu_mausaktion[taste - 1].text[i];
    }
    ziel[i] = '\0';
    KeReleaseSpinLock(&ntu_maus_sperre, alt);
}
