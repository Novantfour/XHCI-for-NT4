/*
 *  werkzeug/usbgeraet.c -- jedes bereitgestellte USB-Geraet ansehen.
 *
 *  Zugleich das Beispiel dafuer, wie ein Treiber im Benutzermodus die
 *  Schnittstelle aus ober/ntusbgen.h benutzt: Geraet im DEVICEMAP
 *  suchen, oeffnen, INFO lesen, Deskriptoren lesen, Daten lesen.  Ein
 *  Joystick- oder Druckertreiber tut im Kern nichts anderes.
 *
 *      usbgeraet                    alle Geraete, eine Zeile je Stueck
 *      usbgeraet N                  Geraet N ausfuehrlich
 *      usbgeraet N hid              HID-Berichtsdeskriptor (Joystick!)
 *      usbgeraet N lesen EP [n]     n Berichte vom Endpunkt EP lesen
 *      usbgeraet N steuer RT RQ WV WI WL
 *                                   rohe Steueranfrage, Werte hex
 *      usbgeraet N schreiben EP text
 *                                   Text auf einen Bulk-OUT-Endpunkt --
 *                                   so bekommt ein Drucker Rohdaten
 *      usbgeraet N stall EP         Endpunktsperre aufheben
 *      usbgeraet auswerfen X:       USB-Laufwerk sicher entfernen:
 *                                   Cache ausschreiben, sperren, das
 *                                   Dateisystem abloesen
 *      usbgeraet N lesetest [MB]    Massenspeicher: roh ueber Bulk-Only
 *                                   lesen, Durchsatz messen und pruefen,
 *                                   ob jeder Sektor seine Nummer traegt
 *                                   (Pruefstick vom Wirt, siehe vm/)
 *      usbgeraet N alt A            alternative Einstellung A waehlen
 *      usbgeraet N iso EP n [L]     n isochrone Rahmen; bei OUT je L Byte
 *                                   eines Pruefmusters (16-Bit-Werte, die
 *                                   Rahmen und Stelle kodieren)
 *      usbgeraet N schreiben EP -x 55 53 42 43 ...
 *                                   beliebige Bytes, hex -- fuer
 *                                   binaere Geraetebefehle
 *
 *  Nur kernel32 und advapi32, keine Laufzeitbibliothek -- dieselbe
 *  Machart wie die anderen Werkzeuge.
 */

#include <windows.h>
#include "ntbasis.h"
#include "../ober/ntusbgen.h"

static char puffer[sizeof(NTUG_ERGEBNIS) + NTUG_MAX_DATEN];


/* ==================================================================== */

static unsigned long
hex (const char *s)
{
    unsigned long v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    for (;;) {
        char c = *s;

        if      (c >= '0' && c <= '9') v = v * 16 + (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (unsigned long)(c - 'A' + 10);
        else break;

        if (v > 0x0FFFFFFFUL) break;      /* Ueberlauf: nicht weiter schieben */
        s += 1;
    }

    return v;
}

/*  Dezimalzahl 0..grenze aus s; bei ungueltiger Eingabe oder ueber der
 *  Grenze wird die Vorgabe zurueckgegeben und *ok (falls != 0) auf 0
 *  gesetzt.  Zentrale Pruefung fuer die Zaehlwerte der Unterbefehle.  */
static unsigned long
zahl_bis (const char *s, unsigned long vorgabe, unsigned long grenze, int *ok)
{
    unsigned long v = 0;
    int           n = 0;

    if (ok != 0) *ok = 1;
    if (s == 0 || *s == '\0') { if (ok) *ok = 0; return vorgabe; }
    for (; *s != '\0'; s += 1) {
        if (*s < '0' || *s > '9') { if (ok) *ok = 0; return vorgabe; }
        v = v * 10 + (unsigned long)(*s - '0');
        if (v > grenze) { if (ok) *ok = 0; return grenze; }
        n += 1;
    }
    if (n == 0) { if (ok) *ok = 0; return vorgabe; }
    return v;
}

static HANDLE
oeffnen (int n)
{
    char name[32];

    strcpy(name, "\\\\.\\UsbDev");
    {
        char z[8];
        int  k = 0, i = (int)strlen(name);

        do { z[k++] = (char)('0' + n % 10); n /= 10; } while (n != 0);
        while (k != 0) { name[i++] = z[--k]; }
        name[i] = '\0';
    }

    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
}

static NTUG_ERGEBNIS *
rufe (HANDLE h, DWORD code, void *ein, DWORD einlen)
{
    DWORD got = 0;

    if (!DeviceIoControl(h, code, ein, einlen, puffer, sizeof(puffer),
                         &got, NULL) || got < sizeof(NTUG_ERGEBNIS)) {
        printf("  Aufruf gescheitert (Fehler %lu)\n", GetLastError());
        return 0;
    }

    return (NTUG_ERGEBNIS *)puffer;
}

static const char *
klasse (unsigned char k)
{
    switch (k) {
        case 0x00: return "(je Schnittstelle)";
        case 0x01: return "Audio";
        case 0x02: return "Kommunikation";
        case 0x03: return "HID";
        case 0x05: return "Physikalisch";
        case 0x06: return "Bild (Scanner/Kamera)";
        case 0x07: return "Drucker";
        case 0x08: return "Massenspeicher";
        case 0x09: return "Hub";
        case 0x0A: return "CDC-Daten";
        case 0x0B: return "Chipkarte";
        case 0x0E: return "Video";
        case 0xDC: return "Diagnose";
        case 0xE0: return "Funk";
        case 0xEF: return "Verschiedenes";
        case 0xFE: return "Anwendungsspezifisch";
        case 0xFF: return "Herstellereigen";
        default:   return "?";
    }
}

static const char *
geschw (unsigned char g)
{
    switch (g) {
        case NTUG_GESCHW_LOW:   return "1,5M";
        case NTUG_GESCHW_FULL:  return "12M";
        case NTUG_GESCHW_HIGH:  return "480M";
        case NTUG_GESCHW_SUPER: return "5G";
        default:                return "?";
    }
}

static void
hexzeilen (const unsigned char *p, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i += 1) {

        if ((i % 16) == 0) {
            printf("    %04lx  ", i);
        }

        printf("%02x ", p[i]);

        if ((i % 16) == 15 || i + 1 == n) {
            printf("\n");
        }
    }
}


/* ==================================================================== *
 *  Liste
 * ==================================================================== */

static int
info_holen (HANDLE h, NTUG_INFO *i)
{
    NTUG_ERGEBNIS *e = rufe(h, IOCTL_NTUG_INFO, NULL, 0);

    if (e == 0 || e->Fehler != 0) {
        return 0;
    }

    memcpy(i, e + 1, sizeof(*i));
    return 1;
}

static void
liste (void)
{
    int n, gefunden = 0;

    printf("\n  nr  hst:prd    if  klasse                  geschw  "
           "ep  produkt\n");
    printf("  -----------------------------------------------------------"
           "-----------\n");

    for (n = 0; n < 64; n += 1) {

        HANDLE    h = oeffnen(n);
        NTUG_INFO i;

        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }

        if (info_holen(h, &i)) {
            gefunden += 1;
            printf("  %2d  %04x:%04x  %2u  %02x %-20s %-6s  %2u  %s\n",
                   n, i.Hersteller, i.Produkt, i.IfNummer, i.IfKlasse,
                   klasse(i.IfKlasse), geschw(i.Geschwindigkeit),
                   i.AnzahlEp, i.ProduktText[0] ? i.ProduktText : "-");
        }

        CloseHandle(h);
    }

    if (gefunden == 0) {
        printf("  keine.  Tastatur und Maus erscheinen hier nicht -- die\n"
               "  gehen an kbdclass und mouclass.  Alles andere, was\n"
               "  steckt, sollte hier stehen.\n");
    }

    printf("\n");
}


/* ==================================================================== *
 *  Ein Geraet ausfuehrlich
 * ==================================================================== */

static void
ausfuehrlich (HANDLE h)
{
    NTUG_INFO      i;
    NTUG_DESK      q;
    NTUG_ERGEBNIS *e;
    unsigned       k;

    if (!info_holen(h, &i)) {
        printf("  INFO nicht lesbar.\n");
        return;
    }

    printf("\n  Geraet    %04x:%04x  Ausgabe %x.%02x\n",
           i.Hersteller, i.Produkt, i.Ausgabe >> 8, i.Ausgabe & 0xFF);
    printf("  Hersteller  %s\n", i.HerstellerText[0] ? i.HerstellerText : "-");
    printf("  Produkt     %s\n", i.ProduktText[0] ? i.ProduktText : "-");
    printf("  Seriennr.   %s\n", i.Seriennummer[0] ? i.Seriennummer : "-");
    printf("  Geraeteklasse   %02x/%02x/%02x  %s\n", i.GeraetKlasse,
           i.GeraetUnter, i.GeraetProtokoll, klasse(i.GeraetKlasse));
    printf("  Schnittstelle   %u, Alternative %u, %02x/%02x/%02x  %s\n",
           i.IfNummer, i.IfAlternative, i.IfKlasse, i.IfUnter,
           i.IfProtokoll, klasse(i.IfKlasse));
    printf("  Bus         Adresse %u, Buchse %u, Tiefe %u, %s\n",
           i.Adresse, i.Anschluss, i.Tiefe, geschw(i.Geschwindigkeit));

    printf("\n  Endpunkte (%u)\n", i.AnzahlEp);

    for (k = 0; k < i.AnzahlEp; k += 1) {

        static const char *art[] = { "Steuer", "isochron", "Bulk", "Interrupt" };
        NTUG_ENDPUNKT     *p = &i.Ep[k];

        printf("    %02x  %-3s %-9s  %4u Byte  Intervall %u\n",
               p->Adresse, (p->Adresse & 0x80) ? "IN" : "OUT",
               art[p->Art & 3], p->MaxPaket, p->Intervall);
    }

    /*  Den vollstaendigen Konfigurationsdeskriptor roh dazu.  Aus ihm
     *  liest ein Treiber alles, was INFO zusammenfasst -- und alles, was
     *  es nicht tut, etwa die Klassendeskriptoren eines Druckers.  */

    memset(&q, 0, sizeof(q));
    q.Typ    = 2;
    q.Laenge = 1024;
    e = rufe(h, IOCTL_NTUG_DESKRIPTOR, &q, sizeof(q));

    if (e != 0 && e->Fehler == 0) {
        printf("\n  Konfigurationsdeskriptor (%lu Byte)\n", e->Laenge);
        hexzeilen((unsigned char *)(e + 1), e->Laenge);
    } else if (e != 0) {
        printf("\n  Konfigurationsdeskriptor: USB-Fehler %ld\n", e->Fehler);
    }

    printf("\n");
}

static void
hid (HANDLE h)
{
    NTUG_DESK      q;
    NTUG_ERGEBNIS *e;

    memset(&q, 0, sizeof(q));
    q.Typ    = 0x22;              /* HID-Bericht */
    q.Laenge = 4096;
    e = rufe(h, IOCTL_NTUG_DESKRIPTOR, &q, sizeof(q));

    if (e == 0) {
        return;
    }

    if (e->Fehler != 0) {
        printf("\n  HID-Berichtsdeskriptor: USB-Fehler %ld\n"
               "  (Ist das ueberhaupt eine HID-Schnittstelle?)\n\n",
               e->Fehler);
        return;
    }

    printf("\n  HID-Berichtsdeskriptor (%lu Byte)\n"
           "  Hier steht, welches Bit welcher Knopf und welche Achse ist.\n"
           "  Ein Joysticktreiber liest ihn einmal und deutet danach jeden\n"
           "  Bericht von LESEN.\n\n", e->Laenge);
    hexzeilen((unsigned char *)(e + 1), e->Laenge);
    printf("\n");
}

static void
lesen (HANDLE h, unsigned char ep, int anzahl)
{
    int k;

    printf("\n  Lese %d Berichte von Endpunkt %02x (je bis 2 s).\n\n",
           anzahl, ep);

    for (k = 0; k < anzahl; k += 1) {

        NTUG_DATEN     q;
        NTUG_ERGEBNIS *e;

        memset(&q, 0, sizeof(q));
        q.Endpunkt  = ep;
        q.Laenge    = 64;
        q.Zeitlimit = 2000;

        e = rufe(h, IOCTL_NTUG_LESEN, &q, sizeof(q));

        if (e == 0) {
            return;
        }

        if (e->Fehler == NTUG_FEHLER_ZEIT) {
            printf("  [%2d] nichts -- Zeitlimit\n", k);
            continue;
        }

        if (e->Fehler == NTUG_FEHLER_WEG) {
            printf("  [%2d] Geraet abgezogen\n", k);
            return;
        }

        if (e->Fehler != 0 && e->Fehler != NTUG_FEHLER_KURZ) {
            printf("  [%2d] USB-Fehler %ld\n", k, e->Fehler);
            continue;
        }

        {
            unsigned long i;
            unsigned char *p = (unsigned char *)(e + 1);

            /*  Den rohen Fehlerwert immer mit ausgeben.  Die erste Fassung
             *  tat es nicht -- und hielt mit falschen Konstanten eine
             *  Zeitueberschreitung fuer ein Kurzpaket, ohne dass man es
             *  sehen konnte.  */

            printf("  [%2d] %2lu Byte (usb %ld): ", k, e->Laenge, e->Fehler);

            for (i = 0; i < e->Laenge && i < 32; i += 1) {
                printf("%02x ", p[i]);
            }

            printf("\n");
        }
    }

    printf("\n");
}

static void
steuer (HANDLE h, int argc, char **argv, int ab)
{
    static struct {
        NTUG_STEUER   s;
        unsigned char d[512];
    } q;
    NTUG_ERGEBNIS *e;
    unsigned long  n = 0;

    if (argc < ab + 5) {
        printf("  usbgeraet N steuer RT RQ WV WI WL [-x bytes]  (alles hex)\n");
        return;
    }

    memset(&q, 0, sizeof(q));
    q.s.bmRequestType = (unsigned char)hex(argv[ab + 0]);
    q.s.bRequest      = (unsigned char)hex(argv[ab + 1]);
    q.s.wValue        = (unsigned short)hex(argv[ab + 2]);
    q.s.wIndex        = (unsigned short)hex(argv[ab + 3]);
    q.s.wLength       = (unsigned short)hex(argv[ab + 4]);

    /*  OUT mit Nutzdaten:  ... WL -x 00 08  -- die Bytes folgen hex.  */

    if (argc > ab + 6 && strcmp(argv[ab + 5], "-x") == 0) {
        int k;
        for (k = ab + 6; k < argc && n < sizeof(q.d); k += 1) {
            q.d[n++] = (unsigned char)hex(argv[k]);
        }
    }

    if ((q.s.bmRequestType & 0x80) == 0 && q.s.wLength != n) {
        printf("  OUT: wLength %u, aber %lu Datenbytes (-x) angegeben.\n",
               q.s.wLength, n);
        return;
    }

    /*  IN mit uebergrossem wLength wuerde den Treiber ueber den Puffer hinaus
     *  lesen lassen -- auf die Puffergroesse begrenzen.  */
    if ((q.s.bmRequestType & 0x80) != 0 && q.s.wLength > sizeof(q.d)) {
        printf("  IN: wLength %u ist zu gross (hoechstens %u).\n",
               q.s.wLength, (unsigned)sizeof(q.d));
        return;
    }

    e = rufe(h, IOCTL_NTUG_STEUER, &q, sizeof(q.s) + n);

    if (e != 0) {
        printf("\n  USB-Fehler %ld, %lu Byte\n", e->Fehler, e->Laenge);
        hexzeilen((unsigned char *)(e + 1), e->Laenge);
        printf("\n");
    }
}


static void
roh_schreiben (HANDLE h, unsigned char ep, int argc, char **argv, int ab)
{
    static char    q[sizeof(NTUG_DATEN) + 512];
    NTUG_DATEN    *d = (NTUG_DATEN *)q;
    NTUG_ERGEBNIS *e;
    unsigned long  n = 0;
    int            k;

    memset(q, 0, sizeof(NTUG_DATEN));

    for (k = ab; k < argc && n < 512; k += 1) {
        q[sizeof(NTUG_DATEN) + n] = (char)hex(argv[k]);
        n += 1;
    }

    d->Endpunkt  = ep;
    d->Laenge    = n;
    d->Zeitlimit = 2000;

    e = rufe(h, IOCTL_NTUG_SCHREIBEN, q, sizeof(NTUG_DATEN) + n);

    if (e != 0) {
        printf("\n  %lu Byte an Endpunkt %02x geschrieben (usb %ld)\n\n",
               e->Laenge, ep, e->Fehler);
    }
}

static void
schreiben (HANDLE h, unsigned char ep, const char *text)
{
    static char    q[sizeof(NTUG_DATEN) + 512];
    NTUG_DATEN    *d = (NTUG_DATEN *)q;
    NTUG_ERGEBNIS *e;
    unsigned long  n = strlen(text);

    if (n > 510) {
        n = 510;
    }

    memset(q, 0, sizeof(NTUG_DATEN));
    d->Endpunkt  = ep;
    d->Laenge    = n + 2;
    d->Zeitlimit = 2000;
    memcpy(q + sizeof(NTUG_DATEN), text, n);
    q[sizeof(NTUG_DATEN) + n]     = '\r';
    q[sizeof(NTUG_DATEN) + n + 1] = '\n';

    e = rufe(h, IOCTL_NTUG_SCHREIBEN, q, sizeof(NTUG_DATEN) + n + 2);

    if (e != 0) {
        printf("\n  %lu Byte an Endpunkt %02x geschrieben (usb %ld)\n\n",
               e->Laenge, ep, e->Fehler);
    }
}


static void
iso (HANDLE h, unsigned char ep, unsigned long n, unsigned long l)
{
    static char       q[sizeof(NTUG_ISO) + NTUG_MAX_ISO_RAHMEN * 4 + 65536];
    NTUG_ISO         *iq = (NTUG_ISO *)q;
    unsigned long    *lg = (unsigned long *)(q + sizeof(NTUG_ISO));
    unsigned char    *d  = (unsigned char *)(lg + n);
    NTUG_ERGEBNIS    *e;
    NTUG_ISO_ANTWORT *a;
    unsigned long    *ist;
    unsigned long     k, i, einlen = sizeof(NTUG_ISO);
    int               lesen = (ep & 0x80) != 0;

    if (n == 0 || n > NTUG_MAX_ISO_RAHMEN) {
        printf("  1 bis %d Rahmen\n", NTUG_MAX_ISO_RAHMEN);
        return;
    }

    memset(iq, 0, sizeof(*iq));
    iq->Endpunkt  = ep;
    iq->Rahmen    = n;
    iq->Zeitlimit = 2000;

    if (!lesen) {

        /*  Pruefmuster: 16-Bit-Werte, Wert = 0x1000 * (Rahmen & 7) + Stelle.
         *  Auf dem Wirt laesst sich jeder Rahmen daran wiedererkennen.  */

        for (k = 0; k < n; k += 1) {
            lg[k] = l;
            for (i = 0; i + 1 < l; i += 2) {
                unsigned short w = (unsigned short)(0x1000 * (k & 7) + i / 2);
                d[k * l + i]     = (unsigned char)(w & 0xFF);
                d[k * l + i + 1] = (unsigned char)(w >> 8);
            }
        }

        einlen += n * 4 + n * l;
    }

    e = rufe(h, IOCTL_NTUG_ISO, q, einlen);

    if (e == 0) {
        return;
    }

    a   = (NTUG_ISO_ANTWORT *)(e + 1);
    ist = (unsigned long *)(a + 1);

    printf("\n  isochron %s, Endpunkt %02x: %lu Rahmen, Schrittweite %lu, "
           "%lu Byte gesamt (usb %ld)\n",
           lesen ? "IN" : "OUT", ep, a->Rahmen, a->RahmenLaenge,
           e->Laenge, e->Fehler);

    if (a->Geschaetzt) {
        printf("  (Laengen GESCHAETZT: OHCI meldet bei OUT normgemaess 0)\n");
    }

    printf("  Laengen je Rahmen:");

    for (k = 0; k < a->Rahmen && k < 32; k += 1) {
        printf(" %lu", ist[k]);
    }

    printf("%s\n\n", (a->Rahmen > 32) ? " ..." : "");
}


/* ==================================================================== *
 *  Lesetest ueber Bulk-Only
 *
 *  Kein Speichertreiber -- ein Messinstrument fuer den Bulk-Pfad.  Liest
 *  mit SCSI READ(10) in Bloecken zu 64 kB, prueft jeden Sektor gegen das
 *  Muster des Pruefsticks (Sektornummer vorn, Nummer XOR A5A5A5A5 dahinter)
 *  und misst die Zeit.  Die Statusphase wird bei Zeitlimit einmal
 *  wiederholt, wie die Bulk-Only-Norm es vorsieht.
 * ==================================================================== */

static int
bot_befehl (HANDLE h, unsigned char aus, unsigned char ein, unsigned long tag,
            unsigned long lba, unsigned long bloecke, unsigned char *ziel,
            unsigned long *gelesen)
{
    static char    q[sizeof(NTUG_DATEN) + 31];
    NTUG_DATEN    *d = (NTUG_DATEN *)q;
    unsigned char *c = (unsigned char *)(q + sizeof(NTUG_DATEN));
    NTUG_ERGEBNIS *e;
    unsigned long  laenge = bloecke * 512, rest, k;

    memset(q, 0, sizeof(q));
    d->Endpunkt  = aus;
    d->Laenge    = 31;
    d->Zeitlimit = 3000;

    c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'C';
    c[4] = (unsigned char)tag; c[5] = (unsigned char)(tag >> 8);
    c[6] = (unsigned char)(tag >> 16); c[7] = (unsigned char)(tag >> 24);
    c[8] = (unsigned char)laenge; c[9] = (unsigned char)(laenge >> 8);
    c[10] = (unsigned char)(laenge >> 16); c[11] = (unsigned char)(laenge >> 24);
    c[12] = 0x80; c[13] = 0; c[14] = 10;
    c[15] = 0x28;                               /* READ(10) */
    c[17] = (unsigned char)(lba >> 24); c[18] = (unsigned char)(lba >> 16);
    c[19] = (unsigned char)(lba >> 8);  c[20] = (unsigned char)lba;
    c[22] = (unsigned char)(bloecke >> 8); c[23] = (unsigned char)bloecke;

    e = rufe(h, IOCTL_NTUG_SCHREIBEN, q, sizeof(q));
    if (e == 0 || e->Fehler != 0) {
        printf("  CBW bei LBA %lu: usb %ld\n", lba, e ? e->Fehler : -1L);
        return 0;
    }

    /*  Datenphase -- bei 64 kB ein Aufruf, aber vorsichtshalber in einer
     *  Schleife, falls das Geraet kuerzer liefert.  */

    *gelesen = 0;
    rest = laenge;

    while (rest != 0) {
        NTUG_DATEN r;

        memset(&r, 0, sizeof(r));
        r.Endpunkt  = ein;
        r.Laenge    = rest;
        r.Zeitlimit = 3000;

        e = rufe(h, IOCTL_NTUG_LESEN, &r, sizeof(r));
        if (e == 0 || (e->Fehler != 0 && e->Fehler != NTUG_FEHLER_KURZ) ||
            e->Laenge == 0) {
            printf("  Daten bei LBA %lu: usb %ld, %lu Byte\n", lba,
                   e ? e->Fehler : -1L, e ? e->Laenge : 0);
            return 0;
        }

        memcpy(ziel + *gelesen, e + 1, e->Laenge);
        *gelesen += e->Laenge;
        rest     -= e->Laenge;
    }

    /*  Statusphase, bei Zeitlimit ein zweiter Versuch.  */

    for (k = 0; k < 2; k += 1) {
        NTUG_DATEN r;

        memset(&r, 0, sizeof(r));
        r.Endpunkt  = ein;
        r.Laenge    = 13;
        r.Zeitlimit = 2000;

        e = rufe(h, IOCTL_NTUG_LESEN, &r, sizeof(r));
        if (e != 0 && e->Fehler == 0 && e->Laenge == 13) {
            unsigned char *w = (unsigned char *)(e + 1);
            if (w[0] != 'U' || w[1] != 'S' || w[2] != 'B' || w[3] != 'S' ||
                w[12] != 0) {
                printf("  CSW bei LBA %lu: Status %u\n", lba, w[12]);
                return 0;
            }
            return 1;
        }
        if (e == 0 || e->Fehler != NTUG_FEHLER_ZEIT) {
            break;
        }
    }

    printf("  CSW bei LBA %lu fehlt (usb %ld)\n", lba, e ? e->Fehler : -1L);
    return 0;
}

static void
lesetest (HANDLE h, unsigned long mb)
{
    NTUG_INFO      i;
    unsigned char  ein = 0, aus = 0;
    unsigned long  k, lba, bloecke = 128, gesamt, fehler = 0, t0, t1, ms;
    static unsigned char puffer[65536];

    if (!info_holen(h, &i) || i.IfKlasse != 0x08 || i.IfProtokoll != 0x50) {
        printf("  Kein Massenspeicher mit Bulk-Only (08/xx/50).\n");
        return;
    }

    for (k = 0; k < i.AnzahlEp; k += 1) {
        if (i.Ep[k].Art == 2) {
            if (i.Ep[k].Adresse & 0x80) ein = i.Ep[k].Adresse;
            else                        aus = i.Ep[k].Adresse;
        }
    }

    gesamt = mb * 2048;                 /* Sektoren */

    printf("\n  Lesetest: %lu MB ueber Bulk-Only, Endpunkte %02x/%02x, %s\n",
           mb, aus, ein, geschw(i.Geschwindigkeit));

    t0 = GetTickCount();

    for (lba = 0; lba < gesamt; lba += bloecke) {

        unsigned long gelesen = 0, s;

        if (!bot_befehl(h, aus, ein, lba + 1, lba, bloecke, puffer, &gelesen)) {
            printf("  abgebrochen bei LBA %lu\n", lba);
            return;
        }

        for (s = 0; s < gelesen / 512; s += 1) {
            unsigned char *p = puffer + s * 512;
            unsigned long  soll = lba + s;
            unsigned long  ist  = p[0] | (p[1] << 8) | (p[2] << 16) |
                                  ((unsigned long)p[3] << 24);
            unsigned long  x    = p[10] | (p[11] << 8) | (p[12] << 16) |
                                  ((unsigned long)p[13] << 24);

            if (ist != soll || x != (soll ^ 0xA5A5A5A5ul) ||
                p[4] != 'N' || p[9] != 'B') {
                if (fehler < 5) {
                    printf("  Sektor %lu: traegt %lu\n", soll, ist);
                }
                fehler += 1;
            }
        }
    }

    t1 = GetTickCount();
    ms = (t1 - t0) ? (t1 - t0) : 1;

    printf("  %lu Sektoren gelesen in %lu ms  =  %lu kB/s\n", gesamt, ms,
           (gesamt / 2) * 1000 / ms);
    printf("  falsche Sektoren: %lu%s\n\n", fehler,
           fehler ? "" : "   -- jeder Sektor traegt seine Nummer");
}


/* ==================================================================== *
 *  Auswerfen
 *
 *  NT 4.0 kennt kein "Hardware sicher entfernen".  Das Dateisystem haelt
 *  Geschriebenes im Cache; wer den Stick zu frueh zieht, verliert Daten
 *  oder beschaedigt die FAT.  Die drei Schritte hier machen ihn sicher:
 *
 *    FlushFileBuffers        alles Gepufferte auf den Stick
 *    FSCTL_LOCK_VOLUME       niemand hat mehr etwas offen
 *    FSCTL_DISMOUNT_VOLUME   das Dateisystem gibt den Datentraeger frei
 *
 *  Scheitert das Sperren, ist noch eine Datei offen -- dann ist trotzdem
 *  ausgeschrieben, aber der Stick sollte noch nicht gezogen werden.
 * ==================================================================== */

#define FSCTL_LOCK_VOLUME_NT     0x00090018
#define FSCTL_DISMOUNT_VOLUME_NT 0x00090020

static int
auswerfen (const char *lw)
{
    char   pfad[8];
    HANDLE h;
    DWORD  n;

    if (lw[0] == '\0' || lw[1] != ':') {
        printf("  usbgeraet auswerfen X:\n");
        return 1;
    }

    pfad[0] = '\\'; pfad[1] = '\\'; pfad[2] = '.'; pfad[3] = '\\';
    pfad[4] = lw[0]; pfad[5] = ':'; pfad[6] = '\0';

    h = CreateFileA(pfad, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    0, NULL);

    if (h == INVALID_HANDLE_VALUE) {
        printf("\n  %c: laesst sich nicht oeffnen (Fehler %lu).\n\n", lw[0],
               GetLastError());
        return 1;
    }

    printf("\n  %c: ausschreiben ... %s\n", lw[0],
           FlushFileBuffers(h) ? "ok" : "Fehler");

    /*  Sperren bis zu zehnmal im Sekundentakt versuchen.  Nach dem
     *  Schliessen einer Datei haelt der Cache-Manager noch einige
     *  Sekunden einen Verweis, bis sein verzoegertes Schreiben durch ist;
     *  so lange meldet FastFat "in Benutzung".  Im Test: erster Versuch
     *  scheiterte, sechs Sekunden spaeter ging es.  */

    {
        int k;

        for (k = 0; k < 10; k += 1) {
            if (DeviceIoControl(h, FSCTL_LOCK_VOLUME_NT, NULL, 0, NULL, 0,
                                &n, NULL)) {
                break;
            }
            if (k == 0) {
                printf("  %c: sperren ... warte auf den Cache", lw[0]);
            } else {
                printf(".");
            }
            Sleep(1000);
            FlushFileBuffers(h);
        }

        if (k > 0) {
            printf("\n");
        }

        if (k == 10) {
            printf("  %c: sperren ... noch in Benutzung (Fehler %lu).\n"
                   "  Ausgeschrieben ist, aber eine Datei ist offen -- erst\n"
                   "  schliessen, dann noch einmal.\n\n", lw[0], GetLastError());
            CloseHandle(h);
            return 1;
        }
    }

    printf("  %c: sperren ... ok\n", lw[0]);

    if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME_NT, NULL, 0, NULL, 0, &n, NULL)) {
        /*  Schlaegt das Abloesen fehl, darf man den Stick NICHT ziehen --
         *  ungeschriebene Daten gingen sonst verloren.  */
        printf("  %c: Dateisystem abloesen ... Fehler %lu\n", lw[0], GetLastError());
        printf("\n  ACHTUNG: Nicht ziehen -- das Dateisystem ist noch eingebunden.\n\n");
        CloseHandle(h);
        return 1;
    }
    printf("  %c: Dateisystem abloesen ... ok\n", lw[0]);
    CloseHandle(h);

    printf("\n  Der Stick kann jetzt gezogen werden.\n\n");
    return 0;
}


/* ==================================================================== */

int
usbg_main (int argc, char **argv)
{
    HANDLE h;
    int    n;

    printf("\nusbgeraet -- bereitgestellte USB-Geraete\n");

    if (argc < 2) {
        liste();
        return 0;
    }

    if (strcmp(argv[1], "auswerfen") == 0) {
        return auswerfen((argc > 2) ? argv[2] : "");
    }

    {
        const char *z = argv[1];
        int         gueltig = (z[0] != '\0');
        for (; *z != '\0'; z += 1) {
            if (*z < '0' || *z > '9') { gueltig = 0; break; }
        }
        n = atoi(argv[1]);
        if (!gueltig || n < 0 || n > 255) {
            printf("\n  Ungueltige Geraetenummer: %s\n\n", argv[1]);
            return 1;
        }
    }
    h = oeffnen(n);

    if (h == INVALID_HANDLE_VALUE) {
        printf("\n  \\\\.\\UsbDev%d gibt es nicht.\n\n", n);
        return 1;
    }

    if (argc < 3) {
        ausfuehrlich(h);
    } else if (strcmp(argv[2], "hid") == 0) {
        hid(h);
    } else if (strcmp(argv[2], "lesen") == 0 && argc > 3) {
        lesen(h, (unsigned char)hex(argv[3]),
              (int)zahl_bis((argc > 4) ? argv[4] : "10", 10, 100000, 0));
    } else if (strcmp(argv[2], "schreiben") == 0 && argc > 5 &&
               strcmp(argv[4], "-x") == 0) {

        roh_schreiben(h, (unsigned char)hex(argv[3]), argc, argv, 5);

    } else if (strcmp(argv[2], "schreiben") == 0 && argc > 4) {

        /*  Alles ab argv[4] wieder zu einem Text zusammensetzen -- die
         *  Befehlszeile hat ihn an den Leerzeichen zerlegt.  */

        static char t[512];
        int         k;

        t[0] = '\0';

        for (k = 4; k < argc; k += 1) {
            if (k > 4 && strlen(t) + 1 < sizeof(t)) {
                strcpy(t + strlen(t), " ");
            }
            if (strlen(t) + strlen(argv[k]) < sizeof(t)) {
                strcpy(t + strlen(t), argv[k]);
            }
        }

        schreiben(h, (unsigned char)hex(argv[3]), t);

    } else if (strcmp(argv[2], "alt") == 0 && argc > 3) {

        unsigned char  alt = (unsigned char)atoi(argv[3]);
        NTUG_ERGEBNIS *e   = rufe(h, IOCTL_NTUG_ALTERNATIVE, &alt, 1);

        if (e != 0) {
            printf("\n  Alternative %u gewaehlt (usb %ld)\n\n", alt, e->Fehler);
        }

    } else if (strcmp(argv[2], "iso") == 0 && argc > 4) {

        iso(h, (unsigned char)hex(argv[3]),
            zahl_bis(argv[4], 0, 100000, 0),
            (argc > 5) ? zahl_bis(argv[5], 0, NTUG_MAX_DATEN, 0) : 0);

    } else if (strcmp(argv[2], "lesetest") == 0) {

        lesetest(h, (argc > 3) ? zahl_bis(argv[3], 16, 100000, 0) : 16);

    } else if (strcmp(argv[2], "stall") == 0 && argc > 3) {

        unsigned char  ep = (unsigned char)hex(argv[3]);
        NTUG_ERGEBNIS *e  = rufe(h, IOCTL_NTUG_STALL_WEG, &ep, 1);

        if (e != 0) {
            printf("\n  Sperre an Endpunkt %02x aufgehoben (usb %ld)\n\n",
                   ep, e->Fehler);
        }

    } else if (strcmp(argv[2], "steuer") == 0) {
        steuer(h, argc, argv, 3);
    } else {
        printf("  unbekannt: %s\n", argv[2]);
    }

    CloseHandle(h);
    return 0;
}
