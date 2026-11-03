/*
 *  ober/ntsondertasten.c -- Sondertasten der USB-Tastatur belegen:
 *  F13..F24, Medientasten, Systemtasten (Tabelle: ntsonder.h).
 *
 *  Woher:  F13..F24 aus ntukbd.c (Haupt-Schnittstelle), Medien- und
 *  Systemtasten aus ntumedien.c (zweite HID-Schnittstelle).  Beide rufen
 *  ntu_sonder_taste(index, gedrueckt).
 *
 *  Was:    Belegung je Taste in Parameters\Sonder_<Kennung> (REG_SZ):
 *    Tasten (wie bei den Maustasten, "strg+c", "f13") -- fuehrt der
 *      Treiber selbst aus, ueber seine Tastatur-Verbindung zu kbdclass;
 *      gehalten ist gehalten.
 *    "lauter", "leiser", "stumm", "programm:<befehl>" -- kann ein Treiber
 *      nicht; beim Druecken ein Auftrag in die Warteschlange, das
 *      benannte Ereignis "UsbNtSondertasten" wird gesetzt, USBTASTEN.EXE
 *      holt ihn ab (IOCTL_NTU_SONDER_ABHOLEN) und fuehrt ihn aus.
 *    "aus" -- nichts.
 *
 *  Setzen zur Laufzeit: IOCTL_NTU_SONDER_SETZEN (Reiter "Sondertasten"),
 *  wirkt sofort und schreibt Sonder_<Kennung> zurueck.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <ober/ntkbdmou.h>
#define NTU_SONDER_TABELLE
#include <ober/ntsonder.h>

typedef struct {
    NTU_MAUSAKTION a;                    /* NTU_MA_TASTEN oder NTU_MA_NICHTS */
    int            helfer;               /* 1: Auftrag fuer USBTASTEN.EXE */
    char           text[NTU_SONDER_TEXT];
} ntu_sonder_bel;

static ntu_sonder_bel ntu_sonder[NTU_SONDER_ANZAHL];
static KSPIN_LOCK     ntu_sonder_sperre;
static int            ntu_sonder_bereit;
static WCHAR          ntu_sonder_pfad[260];

volatile LONG         ntu_sonder_gedrueckt, ntu_sonder_gesehen, ntu_sonder_tastaturen;

/*  Auftraege fuer USBTASTEN.EXE  */
#define NTU_AUFTRAEGE 8
static NTU_SONDER_AUFTRAG ntu_auftrag[NTU_AUFTRAEGE];
static ULONG              ntu_auftrag_kopf, ntu_auftrag_n;
static PKEVENT            ntu_sonder_ereignis;
static HANDLE             ntu_sonder_ereignis_h;

/*  Das benannte Ereignis fuer USBTASTEN.EXE.  Beim Laden des Treibers
 *  (fruehe Startphase) gibt es \BaseNamedObjects oft noch nicht -- das
 *  legt erst das Win32-Subsystem an; dann scheitert IoCreateNotification-
 *  Event, und USBTASTEN.EXE fand das Ereignis nie (gemessen: OpenEvent
 *  Fehler 2, Auftraege blieben liegen).  Deshalb noch einmal beim ersten
 *  Aufruf aus einem Programm (IOCTL, PASSIVE_LEVEL).  */

static VOID
ntu_sonder_ereignis_anlegen (VOID)
{
    UNICODE_STRING en;
    KIRQL          alt;

    if (ntu_sonder_ereignis != 0 || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlInitUnicodeString(&en, NTU_SONDER_EREIGNIS_NT);
    ntu_sonder_ereignis = IoCreateNotificationEvent(&en, &ntu_sonder_ereignis_h);
    if (ntu_sonder_ereignis == 0) {
        return;                             /* spaeter noch einmal */
    }
    KeAcquireSpinLock(&ntu_sonder_sperre, &alt);
    if (ntu_auftrag_n > 0) {
        KeSetEvent(ntu_sonder_ereignis, 0, FALSE);      /* es wartet schon etwas */
    } else {
        KeClearEvent(ntu_sonder_ereignis);
    }
    KeReleaseSpinLock(&ntu_sonder_sperre, alt);
    ntu_printf("usb: Ereignis %s angelegt\n", NTU_SONDER_EREIGNIS);
}

int
ntu_sonder_index (unsigned seite, unsigned nutzung)
{
    int i;
    for (i = 0; i < NTU_SONDER_ANZAHL; i += 1) {
        if (ntu_sonder_tabelle[i].seite == seite && ntu_sonder_tabelle[i].nutzung == nutzung) {
            return i;
        }
    }
    return -1;
}

static int
ntu_beginnt (const char *s, const char *vor)
{
    while (*vor != '\0') {
        if (*s++ != *vor++) return 0;
    }
    return 1;
}

/*  Text -> Belegung.  0 = verstanden.  */
static int
ntu_sonder_zerlegen (const char *roh, ntu_sonder_bel *b)
{
    int i;

    memset(b, 0, sizeof(*b));
    for (i = 0; roh[i] != '\0' && i < NTU_SONDER_TEXT - 1; i += 1) {
        char c = roh[i];
        /*  Programmpfade NICHT klein machen -- nur die Angabe davor.  */
        if (i < 9) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        b->text[i] = c;
    }
    b->text[i] = '\0';
    if (b->text[0] == '\0') {
        strlcpy(b->text, "aus", sizeof(b->text));
    }

    if (strcmp(b->text, "lauter") == 0 || strcmp(b->text, "leiser") == 0 ||
        strcmp(b->text, "stumm") == 0) {
        b->helfer = 1;
        return 0;
    }
    if (ntu_beginnt(b->text, "programm:")) {
        b->helfer = 1;
        return (b->text[9] != '\0') ? 0 : -1;
    }

    /*  Tasten -- nur Tasten und "aus", keine Mausfunktionen.  Tasten-
     *  namen klein: der Maustasten-Leser kennt sie so.  */
    for (i = 0; b->text[i] != '\0'; i += 1) {
        char c = b->text[i];
        b->text[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    if (ntu_belegung_lesen(b->text, &b->a) != 0) {
        return -1;
    }
    if (b->a.art != NTU_MA_TASTEN && b->a.art != NTU_MA_NICHTS) {
        return -1;
    }
    return 0;
}

VOID
ntu_sondertasten_lesen (PUNICODE_STRING reg)
{
    ULONG          m = reg->Length / sizeof(WCHAR);
    int            i;

    KeInitializeSpinLock(&ntu_sonder_sperre);

    if (m < 240) {
        memcpy(ntu_sonder_pfad, reg->Buffer, m * sizeof(WCHAR));
        memcpy(ntu_sonder_pfad + m, L"\\Parameters", 12 * sizeof(WCHAR));
    }

    for (i = 0; i < NTU_SONDER_ANZAHL; i += 1) {
        RTL_QUERY_REGISTRY_TABLE q[2];
        WCHAR                    name[32], wert[NTU_SONDER_TEXT];
        UNICODE_STRING           u;
        char                     t[NTU_SONDER_TEXT];
        const char              *k = ntu_sonder_tabelle[i].kennung;
        int                      j, n = 0;

        memcpy(name, L"Sonder_", 7 * sizeof(WCHAR));
        for (j = 0; k[j] != '\0' && j < 24; j += 1) name[7 + j] = (WCHAR)k[j];
        name[7 + j] = 0;

        wert[0] = 0;
        u.Buffer = wert; u.Length = 0; u.MaximumLength = sizeof(wert) - sizeof(WCHAR);
        memset(q, 0, sizeof(q));
        q[0].Flags        = RTL_QUERY_REGISTRY_DIRECT;
        q[0].Name         = name;
        q[0].EntryContext = &u;
        if (ntu_sonder_pfad[0] != 0 &&
            NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, ntu_sonder_pfad, q, 0, 0)) &&
            u.Length > 0) {
            for (n = 0; n < (int)(u.Length / sizeof(WCHAR)) && n < NTU_SONDER_TEXT - 1; n += 1) {
                t[n] = (char)wert[n];
            }
        }
        t[n] = '\0';
        if (n == 0 || ntu_sonder_zerlegen(t, &ntu_sonder[i]) != 0) {
            ntu_sonder_zerlegen(ntu_sonder_tabelle[i].vorgabe, &ntu_sonder[i]);
        } else {
            ntu_printf("usb: Sonder_%s = %s\n", k, ntu_sonder[i].text);
        }
    }

    ntu_sonder_bereit = 1;
    ntu_sonder_ereignis_anlegen();
}

/*  Eine Sondertaste gedrueckt (1) oder losgelassen (0).  Aus dem Faden
 *  der Tastatur (PASSIVE_LEVEL).  */
VOID
ntu_sonder_taste (int i, int gedrueckt)
{
    ntu_sonder_bel b;
    KIRQL          alt;

    if (!ntu_sonder_bereit || i < 0 || i >= NTU_SONDER_ANZAHL) {
        return;
    }
    /*  Bits unter der Sperre: InterlockedOr/And gibt es im NT-4.0-Kern
     *  nicht.  Lesen (Zustand) ist ein einzelnes LONG, das geht ohne.  */
    KeAcquireSpinLock(&ntu_sonder_sperre, &alt);
    if (gedrueckt) {
        ntu_sonder_gedrueckt |= (LONG)(1ul << i);
        ntu_sonder_gesehen   |= (LONG)(1ul << i);
    } else {
        ntu_sonder_gedrueckt &= (LONG)~(1ul << i);
    }
    b = ntu_sonder[i];
    if (b.helfer && gedrueckt) {
        ULONG platz = (ntu_auftrag_kopf + ntu_auftrag_n) % NTU_AUFTRAEGE;
        if (ntu_auftrag_n < NTU_AUFTRAEGE) {
            ntu_auftrag[platz].Index = (unsigned long)i;
            strlcpy(ntu_auftrag[platz].Text, b.text, sizeof(ntu_auftrag[platz].Text));
            ntu_auftrag_n += 1;
        }
        if (ntu_sonder_ereignis != 0) {
            KeSetEvent(ntu_sonder_ereignis, 0, FALSE);
        }
    }
    KeReleaseSpinLock(&ntu_sonder_sperre, alt);

    if (!b.helfer && b.a.art == NTU_MA_TASTEN) {
        ntu_kbd_einspeisen(b.a.code, b.a.n, !gedrueckt);
    }
}

void
ntu_sonder_zustand (NTU_SONDER_ZUSTAND *z)
{
    KIRQL alt;
    ntu_sonder_ereignis_anlegen();
    memset(z, 0, sizeof(*z));
    z->Anzahl     = NTU_SONDER_ANZAHL;
    z->Gedrueckt  = (unsigned long)ntu_sonder_gedrueckt;
    z->Gesehen    = (unsigned long)ntu_sonder_gesehen;
    z->Tastaturen = (unsigned long)ntu_sonder_tastaturen;
    if (ntu_sonder_bereit) {
        KeAcquireSpinLock(&ntu_sonder_sperre, &alt);
        z->Wartend = ntu_auftrag_n;
        KeReleaseSpinLock(&ntu_sonder_sperre, alt);
    }
}

void
ntu_sonder_holen (int i, char *ziel, int max)
{
    KIRQL alt;
    ziel[0] = '\0';
    if (!ntu_sonder_bereit || i < 0 || i >= NTU_SONDER_ANZAHL || max < 2) {
        return;
    }
    KeAcquireSpinLock(&ntu_sonder_sperre, &alt);
    strlcpy(ziel, ntu_sonder[i].text, (size_t)max);
    KeReleaseSpinLock(&ntu_sonder_sperre, alt);
}

NTSTATUS
ntu_sonder_setzen (int i, const char *text)
{
    ntu_sonder_bel neu;
    KIRQL          alt;
    WCHAR          name[32], w[NTU_SONDER_TEXT];
    const char    *k;
    int            j;

    if (!ntu_sonder_bereit || i < 0 || i >= NTU_SONDER_ANZAHL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (ntu_sonder_zerlegen(text, &neu) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    KeAcquireSpinLock(&ntu_sonder_sperre, &alt);
    ntu_sonder[i] = neu;
    KeReleaseSpinLock(&ntu_sonder_sperre, alt);

    k = ntu_sonder_tabelle[i].kennung;
    ntu_printf("usb: Sonder_%s = %s (Systemsteuerung)\n", k, neu.text);
    if (ntu_sonder_pfad[0] != 0) {
        memcpy(name, L"Sonder_", 7 * sizeof(WCHAR));
        for (j = 0; k[j] != '\0' && j < 24; j += 1) name[7 + j] = (WCHAR)k[j];
        name[7 + j] = 0;
        for (j = 0; neu.text[j] != '\0'; j += 1) w[j] = (WCHAR)(unsigned char)neu.text[j];
        w[j] = 0;
        RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, ntu_sonder_pfad, name, REG_SZ,
                              w, (ULONG)(j + 1) * sizeof(WCHAR));
    }
    return STATUS_SUCCESS;
}

/*  Einen Auftrag abholen.  1 = geholt.  Leer: Ereignis zuruecksetzen.  */
int
ntu_sonder_abholen (NTU_SONDER_AUFTRAG *a)
{
    KIRQL alt;
    int   da = 0;

    if (!ntu_sonder_bereit) {
        return 0;
    }
    ntu_sonder_ereignis_anlegen();
    KeAcquireSpinLock(&ntu_sonder_sperre, &alt);
    if (ntu_auftrag_n > 0) {
        *a = ntu_auftrag[ntu_auftrag_kopf];
        ntu_auftrag_kopf = (ntu_auftrag_kopf + 1) % NTU_AUFTRAEGE;
        ntu_auftrag_n -= 1;
        da = 1;
    }
    if (ntu_auftrag_n == 0 && ntu_sonder_ereignis != 0) {
        KeClearEvent(ntu_sonder_ereignis);
    }
    KeReleaseSpinLock(&ntu_sonder_sperre, alt);
    return da;
}
