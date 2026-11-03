/*
 *  werkzeug/xhcitest.c -- xHCI im Klartext.
 *
 *  Der xHCI ist der einzige der vier Bausteine, bei dem man ohne
 *  Registerdeutung nicht weiterkommt: er hat drei getrennte
 *  Registerbloecke an Stellen, die er selbst angibt, eine verkettete
 *  Liste erweiterter Faehigkeiten, und eine BIOS-Uebergabe, die
 *  fehlschlagen kann, ohne dass irgendwo ein Fehler steht.
 *
 *  Dieses Programm liest alles davon durch den Treiber aus und schreibt
 *  es mit Namen hin.  Es schreibt nichts -- ein Schreibpfad aus dem
 *  Benutzermodus in die Register eines Busmeisters waere ein Werkzeug,
 *  mit dem man den Rechner aus einer Eingabeaufforderung heraus
 *  abschiesst, und zwar aus Versehen.
 *
 *      xhcitest              alles, was gerade steckt
 *      xhcitest N            nur Baustein N
 *      xhcitest N roh        zusaetzlich der ungedeutete Abzug
 *      xhcitest laden [art]  Dienst anlegen und starten
 *      xhcitest entladen[art] anhalten und entfernen
 *
 *  [art] ist uhci, ohci, ehci, xhci oder usbnt; ohne Angabe xhci.
 *  Beim Auslesen wird der gefunden, der gerade liegt -- egal welcher.
 *      xhcitest probelauf    laden, auslesen, entladen -- in einem Zug
 *      xhcitest wechsel 10   zehnmal laden und entladen
 *      xhcitest zaehler      Betriebszaehler mit Deutung
 *      xhcitest beobachten   Zaehler und PORTSC im Sekundentakt
 *      xhcitest spur         die letzten Unterbrechungen im Rohzustand
 *
 *  Der Probelauf ist der eigentliche Zweck: er sagt in einem Durchgang,
 *  ob der Treiber den Baustein bekommt, ob das BIOS ihn hergibt und ob
 *  er sich danach wieder sauber loesen laesst.  Das Loesen ist der Teil,
 *  der selten stimmt -- ein Treiber, der sich einmal laden laesst,
 *  laesst sich fast immer laden; einer, der sich zehnmal entladen
 *  laesst, ist selten.  Deshalb 'wechsel'.
 *
 *  Uebersetzen: siehe bauen.md
 */

#include <windows.h>
#include "ntbasis.h"
#include "../ober/ntusbioctl.h"

static HANDLE H;


/* ==================================================================== *
 *  Den Treiber finden, der gerade liegt
 *
 *  Fuenf Bauformen, fuenf Geraetenamen.  Welche liegt, weiss das
 *  Werkzeug nicht -- also probiert es der Reihe nach, mit dem Vollbau
 *  zuerst.  Das ist billiger als eine Einstellung, die der Benutzer
 *  richtig setzen muss.
 * ==================================================================== */

static const char *ntu_namen[5] = {
    NTU_WIN32_ALLE, NTU_WIN32_XHCI, NTU_WIN32_EHCI,
    NTU_WIN32_OHCI, NTU_WIN32_UHCI
};

static const char *ntu_dienste[5] = {
    "usbnt", "xhcint", "ehcint", "ohcint", "uhcint"
};

static const char *ntu_gefunden;

static HANDLE
ntu_oeffnen (void)
{
    int i;

    for (i = 0; i < 5; i += 1) {

        HANDLE h = CreateFileA(ntu_namen[i], GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);

        if (h != INVALID_HANDLE_VALUE) {
            ntu_gefunden = ntu_dienste[i];
            printf("  Treiber: %s  (%s)\n\n", ntu_dienste[i], ntu_namen[i]);
            return h;
        }
    }

    return INVALID_HANDLE_VALUE;
}

static int
ntu_dienst_waehlen (const char *wunsch, const char **dienst, char *datei)
{
    int i;

    if (wunsch == 0 || wunsch[0] == '\0') {
        wunsch = "xhcint";
    }

    for (i = 0; i < 5; i += 1) {

        const char *d = ntu_dienste[i];

        /*  "xhci" und "xhcint" sollen beide gehen -- der Benutzer denkt
         *  in Bauarten, nicht in Dateinamen.  */

        if (strcmp(wunsch, d) == 0 ||
            (strlen(wunsch) == 4 && d[0] == wunsch[0] && d[1] == wunsch[1] &&
             d[2] == wunsch[2] && d[3] == wunsch[3])) {

            *dienst = d;
            strcpy(datei, d);
            strcpy(datei + strlen(d), ".sys");
            return 1;
        }
    }

    printf("  Unbekannte Bauart '%s'.  Moeglich: usbnt uhci ohci ehci xhci\n",
           wunsch);
    return 0;
}


/* ==================================================================== *
 *  Laden und entladen
 *
 *  NT laedt einen Kerntreiber ueber die Dienstverwaltung: eintragen,
 *  starten, anhalten, austragen.  Ein Neustart ist dafuer nicht noetig,
 *  solange der Treiber SERVICE_DEMAND_START ist und ein DriverUnload
 *  hat -- ohne DriverUnload verweigert NT das Anhalten, und dann hilft
 *  wirklich nur der Neustart.
 * ==================================================================== */

static void
meldung (const char *was, unsigned long e)
{
    printf("  %s: Fehler %lu", was, e);

    switch (e) {
        case 2:    printf("  (Datei nicht gefunden)");           break;
        case 5:    printf("  (Zugriff verweigert -- Administrator?)"); break;
        case 31:   printf("  (Geraet nicht betriebsbereit -- DriverEntry"
                          " hat einen Fehler zurueckgegeben)");  break;
        case 577:  printf("  (Datei nicht als Treiber erkannt)"); break;
        case 1056: printf("  (laeuft schon)");                   break;
        case 1060: printf("  (Dienst nicht eingetragen)");       break;
        case 1072: printf("  (zum Loeschen vorgemerkt -- ein Griff auf\n"
                          "        \\\\.\\XhciNt ist noch offen)");  break;
        case 1073: printf("  (schon eingetragen)");              break;
        case 1275: printf("  (Treiber vom System gesperrt)");    break;
        default: break;
    }

    printf("\n");
}

static int
laden (const char *wunsch)
{
    const char *DIENST = 0;
    char        DATEI[24];

    SC_HANDLE scm;
    SC_HANDLE d;
    char      pfad[MAX_PATH];
    char     *p;
    int       ok = 0;

    if (!ntu_dienst_waehlen(wunsch, &DIENST, DATEI)) {
        return 0;
    }

    GetModuleFileNameA(NULL, pfad, sizeof(pfad));
    p = strrchr(pfad, '\\');

    if (p != NULL) {
        strcpy(p + 1, DATEI);
    }

    printf("  Treiberdatei: %s\n", pfad);

    if (GetFileAttributesA(pfad) == 0xFFFFFFFFul) {
        printf("  Die Datei liegt nicht neben xhcitest.exe.\n");
        return 0;
    }

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (scm == NULL) {
        meldung("OpenSCManager", GetLastError());
        return 0;
    }

    d = CreateServiceA(scm, DIENST, DIENST, SERVICE_ALL_ACCESS,
                       SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                       SERVICE_ERROR_NORMAL, pfad, NULL, NULL, NULL,
                       NULL, NULL);

    if (d == NULL) {

        unsigned long e = GetLastError();

        if (e != 1073) {
            meldung("CreateService", e);
            CloseServiceHandle(scm);
            return 0;
        }

        /*  Schon eingetragen -- aber vielleicht mit einem alten Pfad.
         *  Oeffnen genuegt; der Pfad wird nicht nachgezogen, und das
         *  soll auffallen statt still zu wirken.  */

        d = OpenServiceA(scm, DIENST, SERVICE_ALL_ACCESS);
        printf("  Dienst war eingetragen (Pfad wird NICHT nachgezogen).\n");
    }

    if (d != NULL) {

        if (StartServiceA(d, 0, NULL)) {
            printf("  gestartet.\n");
            ok = 1;
        } else {

            unsigned long e = GetLastError();

            if (e == 1056) {
                printf("  laeuft bereits.\n");
                ok = 1;
            } else {
                meldung("StartService", e);
                printf("  Die serielle Ausgabe sagt, wie weit DriverEntry\n"
                       "  gekommen ist.  Kommt dort gar nichts, ist der\n"
                       "  Treiber nicht geladen worden -- dann liegt es an\n"
                       "  der Datei, nicht am Treiber.\n");
            }
        }

        CloseServiceHandle(d);
    }

    CloseServiceHandle(scm);
    return ok;
}

static int
entladen2 (const char *DIENST, int leise)
{
    SC_HANDLE      scm;
    SC_HANDLE      d;
    SERVICE_STATUS st;
    int            ok = 0;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (scm == NULL) {
        meldung("OpenSCManager", GetLastError());
        return 0;
    }

    d = OpenServiceA(scm, DIENST, SERVICE_ALL_ACCESS);

    if (d == NULL) {

        if (!leise) {
            printf("  Dienst war nicht eingetragen.\n");
        }

        CloseServiceHandle(scm);
        return 1;
    }

    if (ControlService(d, SERVICE_CONTROL_STOP, &st)) {

        if (!leise) {
            printf("  angehalten.\n");
        }

        ok = 1;

    } else {

        unsigned long e = GetLastError();

        if (e == 1062) {                /* laeuft nicht */
            ok = 1;
        } else {
            meldung("ControlService", e);
            printf("  Haelt der Treiber nicht an, fehlt ihm ein\n"
                   "  DriverUnload -- oder das Entladen ist mitten"
                   " darin\n  haengengeblieben.\n");
        }
    }

    /*  Kurz warten: das Austragen greift erst, wenn der letzte Griff
     *  weg ist, und der Dienstverwalter braucht dafuer einen Moment.  */

    Sleep(200);

    if (DeleteService(d)) {

        if (!leise) {
            printf("  ausgetragen.\n");
        }

    } else {

        unsigned long e = GetLastError();

        if (e != 1072) {
            meldung("DeleteService", e);
        }
    }

    CloseServiceHandle(d);
    CloseServiceHandle(scm);
    return ok;
}

static const char *ntu_letzte_wahl = "xhcint";

static int
entladen (int leise)
{
    return entladen2(ntu_letzte_wahl, leise);
}

static HANDLE
oeffnen (void)
{
    return ntu_oeffnen();
}


/* ==================================================================== *
 *  Lesen
 * ==================================================================== */

static int
lies (DWORD nr, DWORD versatz, DWORD anzahl, unsigned long *ziel)
{
    NTU_REG_ANFRAGE        q;
    static NTU_REG_ANTWORT a;  /* ueber 4 kB: nicht auf den Stapel */
    DWORD                  got = 0;
    DWORD           i;

    memset(&q, 0, sizeof(q));
    q.Nummer  = nr;
    q.Versatz = versatz;
    q.Anzahl  = anzahl;

    if (!DeviceIoControl(H, IOCTL_NTU_REGISTER, &q, sizeof(q),
                         &a, sizeof(a), &got, NULL)) {
        return 0;
    }

    if (a.Anzahl < anzahl) {

        /*  Der Treiber klemmt an der Groesse des abgebildeten Blocks.
         *  Das ist kein Fehler des Werkzeugs, sondern die Aussage, dass
         *  dort nichts mehr liegt -- und die gehoert gesagt.  */

        for (i = a.Anzahl; i < anzahl; i += 1) {
            ziel[i] = 0xFFFFFFFFul;
        }

        anzahl = a.Anzahl;
    }

    for (i = 0; i < anzahl; i += 1) {
        ziel[i] = a.Wert[i];
    }

    return 1;
}

static unsigned long
lies1 (DWORD nr, DWORD versatz)
{
    unsigned long w = 0xFFFFFFFFul;

    lies(nr, versatz, 1, &w);
    return w;
}

static void
bit (unsigned long wert, unsigned long maske, const char *name)
{
    if ((wert & maske) != 0) {
        printf(" %s", name);
    }
}


/* ==================================================================== *
 *  Hersteller und bekannte Eigenheiten
 *
 *  Der Treiber kennt keine Kennungstabelle und soll keine bekommen: er
 *  bindet ueber die Klasse und behandelt jeden xHCI gleich.  Was hier
 *  steht, ist deshalb ein BERICHT und keine Umgehung -- damit man nicht
 *  eine Stunde lang einen Treiber verdaechtigt, wo die Karte gemeint
 *  ist.
 *
 *  Quelle der Angaben ist Linux, drivers/usb/host/xhci-pci.c und
 *  pci-quirks.c.  Das sind Jahre an Felderfahrung; alles, was hier
 *  daraufhin steht, ist von dort und nicht aus eigener Messung.
 * ==================================================================== */

static void
hersteller (unsigned short ven, unsigned short dev, unsigned short rev)
{
    unsigned long id = ((unsigned long)ven << 16) | dev;
    const char   *n  = "unbekannt";

    switch (ven) {
        case 0x8086: n = "Intel";        break;
        case 0x1033: n = "NEC";          break;
        case 0x1912: n = "Renesas";      break;
        case 0x1b21: n = "ASMedia";      break;
        case 0x1106: n = "VIA Labs";     break;
        case 0x1b6f: n = "Etron";        break;
        case 0x1022: n = "AMD";          break;
        case 0x1002: n = "AMD/ATI";      break;
        case 0x1b73: n = "Fresco Logic"; break;
        case 0x104c: n = "Texas Instr."; break;
        case 0x1db7: n = "Phytium";      break;
        case 0x80ee: n = "VirtualBox";   break;
        default: break;
    }

    printf("  Hersteller     %04x:%04x rev %02x  %s\n\n", ven, dev, rev, n);

    /* ---- was FreeBSD selbst nach Kennung tut (xhci_pci.c) ---- */

    printf("    FreeBSD behandelt nach Kennung:\n");

    switch (id) {

        case 0x1b731009ul:
        case 0x104c8241ul:
            printf("      Endpunkt-Abmeldebefehl wird ausgelassen\n");
            break;

        case 0x10331033ul:
        case 0x10330194ul:
        case 0x19120014ul:
            printf("      nur 32-Bit-DMA\n");
            break;

        case 0x1b731000ul:
            printf("      kein MSI  (unter NT 4.0 ohnehin nie)\n");
            break;

        case 0x80860f35ul:
        case 0x80869c31ul:
        case 0x80861e31ul:
        case 0x80868c31ul:
        case 0x80868cb1ul:
        case 0x80869cb1ul:
            printf("      XUSB2PR: Anschluesse vom EHCI zum xHCI\n");
            printf("      IMOD_LP, Steuerpakete gestueckelt\n");
            break;

        default:
            printf("      nichts -- der Baustein gilt als regelkonform\n");
            break;
    }

    printf("\n");

    /* ---- was Linux zusaetzlich weiss, und was davon hier bleibt ---- */

    if (ven == 0x1912 && (dev == 0x0014 || dev == 0x0015)) {
        printf("    Renesas uPD7202%s laedt seine Firmware beim Einschalten\n"
               "    aus einem EEPROM auf der Karte.  Fehlt es, muss der Wirt\n"
               "    sie hineinschieben (Linux: renesas_xhci_check_request_fw).\n"
               "    Das tut dieser Treiber NICHT.  Erkennbar an einer\n"
               "    unsinnigen HCIVERSION oder CAPLENGTH unten.\n\n",
               (dev == 0x0014) ? "01" : "02");
    }

    if (ven == 0x1b21 &&
        (dev == 0x1142 || dev == 0x2142 || dev == 0x3242 || dev == 0x1142)) {
        printf("    Linux: NO_64BIT_SUPPORT -- dieser Baustein meldet AC64\n"
               "    und kann es nicht.  Hier ohne Folgen: die Unterlage\n"
               "    liefert nie eine Adresse ueber 4 GB und weist sie ab.\n\n");
    }

    if (ven == 0x1106 && (dev == 0x3483 || dev == 0x3432)) {
        printf("    Linux: TRB_OVERFETCH -- der VL805 holt TRBs ueber das\n"
               "    Ringende hinaus.  Hier ohne Folgen: jeder gemeinsame\n"
               "    Puffer bekommt eine Seite Zugabe, unabhaengig von der\n"
               "    Kennung.\n\n");
    }

    if (ven == 0x1022 || ven == 0x1002) {
        printf("    Linux: AMD_PLL_FIX fuer SB7x0/SB8x0 -- betrifft nur\n"
               "    isochrone Uebertragungen.  Weder dieser Treiber noch\n"
               "    der Stapel macht sie zur Zeit.\n\n");
    }

    printf("    Was Linux sonst nach Kennung fuehrt, faellt unter NT 4.0 weg:\n"
           "      RESET_ON_RESUME, SUSPEND_DELAY, PME_STUCK und Verwandte\n"
           "        -- NT 4.0 kennt kein Schlafen\n"
           "      BROKEN_MSI  -- es gibt kein MSI, immer die Leitung\n"
           "      BROKEN_STREAMS  -- der Stapel benutzt keine Stroeme\n"
           "      AVOID_BEI   -- FreeBSD setzt BEI nirgends\n"
           "      ZERO_64B_REGS -- FreeBSD schreibt jedes _HI ausdruecklich\n\n");
}


/* ==================================================================== *
 *  Faehigkeitsblock
 * ==================================================================== */

typedef struct {
    DWORD         nr;
    unsigned long caplen;
    unsigned long version;
    unsigned long hcs1, hcs2, hcs3, hcc1, hcc2;
    unsigned long dboff, rtsoff;
    unsigned long plaetze, unterbr, anschl;
    unsigned long xecp;
    int           ac64;
    unsigned char prot[32];       /* je Anschluss: 2 oder 3, 0 = unbekannt */
} XHCI;

static int
faehigkeiten (XHCI *x)
{
    unsigned long w[8];

    if (!lies(x->nr, 0x00, 8, w)) {
        printf("  Registerblock nicht lesbar.\n");
        return 0;
    }

    if (w[0] == 0xFFFFFFFFul) {
        printf("  Alles Einsen.  Der Baustein antwortet nicht -- kein\n"
               "  Registerblock zugeteilt, oder MEM im Command-Register\n"
               "  aus.\n");
        return 0;
    }

    x->caplen  = w[0] & 0xFF;
    x->version = (w[0] >> 16) & 0xFFFF;
    x->hcs1    = w[1];
    x->hcs2    = w[2];
    x->hcs3    = w[3];
    x->hcc1    = w[4];
    x->dboff   = w[5] & ~0x3ul;
    x->rtsoff  = w[6] & ~0x1Ful;
    x->hcc2    = w[7];

    x->plaetze = x->hcs1 & 0xFF;
    x->unterbr = (x->hcs1 >> 8) & 0x7FF;
    x->anschl  = (x->hcs1 >> 24) & 0xFF;
    x->xecp    = ((x->hcc1 >> 16) & 0xFFFF) * 4;
    x->ac64    = (x->hcc1 & 1) != 0;

    printf("  Faehigkeiten\n");
    printf("    CAPLENGTH      %02lx        Betriebsblock ab hier\n",
           x->caplen);
    printf("    HCIVERSION     %lx.%02lx\n",
           x->version >> 8, x->version & 0xFF);
    printf("    HCSPARAMS1     %08lx  Plaetze %lu  Unterbrecher %lu"
           "  Anschluesse %lu\n",
           x->hcs1, x->plaetze, x->unterbr, x->anschl);
    printf("    HCSPARAMS2     %08lx  IST %lu  ERST max %lu  "
           "Streupuffer %lu\n",
           x->hcs2, x->hcs2 & 0xF, 1ul << ((x->hcs2 >> 4) & 0xF),
           (((x->hcs2 >> 21) & 0x1F) << 5) | ((x->hcs2 >> 27) & 0x1F));
    printf("    HCSPARAMS3     %08lx\n", x->hcs3);
    printf("    HCCPARAMS1     %08lx ", x->hcc1);
    bit(x->hcc1, 0x0001, "AC64");
    bit(x->hcc1, 0x0002, "BNC");
    bit(x->hcc1, 0x0004, "CSZ64");
    bit(x->hcc1, 0x0008, "PPC");
    bit(x->hcc1, 0x0010, "PIND");
    bit(x->hcc1, 0x0020, "LHRC");
    bit(x->hcc1, 0x0040, "LTC");
    bit(x->hcc1, 0x0080, "NSS");
    bit(x->hcc1, 0x0100, "PAE");
    printf("\n");
    printf("    DBOFF          %08lx  Tuerklopfer\n", x->dboff);
    printf("    RTSOFF         %08lx  Laufzeitblock\n", x->rtsoff);

    if (x->version < 0x0090 || x->version > 0x0130) {
        printf("\n    HCIVERSION sieht falsch aus.  Erwartet wird 0.96 bis\n"
               "    1.20.  Entweder liegt der Block woanders -- oder es\n"
               "    laeuft keine Firmware auf der Karte, siehe oben.\n");
    }

    if (x->caplen < 0x20 || x->caplen > 0x80) {
        printf("\n    CAPLENGTH %02lx ist unglaubwuerdig.  Derselbe\n"
               "    Verdacht wie bei der Fassung.\n", x->caplen);
    }

    if (!x->ac64) {
        printf("\n    AC64 ist AUS -- der Baustein kann nur 32 Bit"
               " adressieren.\n    Das passt hier gut: die Unterlage haelt"
               " alle gemeinsamen\n    Puffer ohnehin unter 4 GB.\n");
    }

    if (x->unterbr == 0) {
        printf("\n    Null Unterbrecher.  Dann gibt es kein IMAN, und die\n"
               "    Maskierung im ISR hat nichts, woran sie ansetzen"
               " koennte.\n");
    }

    return 1;
}


/* ==================================================================== *
 *  Erweiterte Faehigkeiten -- hier steht die BIOS-Uebergabe
 *
 *  Das ist der wichtigste Abschnitt des ganzen Programms.  Steht in
 *  USBLEGSUP noch das BIOS-Besitzbit, bedient der SMI-Handler den
 *  Baustein weiter und setzt ihn hinter dem Ruecken des Treibers
 *  zurueck.  Der Treiber laeuft dann -- und rauscht.
 * ==================================================================== */

static void
erweiterungen (XHCI *x)
{
    unsigned long off = x->xecp;
    int           n   = 0;

    printf("\n  Erweiterte Faehigkeiten\n");

    if (off == 0) {
        printf("    keine.\n");
        return;
    }

    while (off != 0 && n < 32) {

        unsigned long k = lies1(x->nr, off);
        unsigned long id;
        unsigned long weiter;

        if (k == 0xFFFFFFFFul || k == 0) {
            printf("    %05lx  nicht lesbar -- der abgebildete Block"
                   " reicht nicht so weit\n", off);
            return;
        }

        id     = k & 0xFF;
        weiter = ((k >> 8) & 0xFF) * 4;
        n += 1;

        switch (id) {

            case 1: {

                unsigned long legctl = lies1(x->nr, off + 4);

                printf("    %05lx  01 USBLEGSUP   %08lx\n", off, k);
                printf("             BIOS besitzt %s    OS besitzt %s\n",
                       (k & 0x00010000ul) ? "JA " : "nein",
                       (k & 0x01000000ul) ? "JA " : "nein");
                printf("    %05lx  .. USBLEGCTLSTS %08lx  SMI:",
                       off + 4, legctl);
                bit(legctl, 0x0001, "ENABLE");
                bit(legctl, 0x0010, "HOSTERR");
                bit(legctl, 0x2000, "OSOWN");
                bit(legctl, 0x4000, "PCICMD");
                bit(legctl, 0x8000, "BAR");
                printf("\n");

                if ((k & 0x00010000ul) != 0) {
                    printf("\n      >>> Das BIOS haelt den Baustein noch."
                           "  Bis die Uebergabe\n"
                           "          durchlaeuft, setzt der SMI-Handler ihn"
                           " hinter dem Ruecken\n"
                           "          des Treibers zurueck.  Das sieht aus"
                           " wie ein sporadischer\n"
                           "          Fehler und ist keiner.\n"
                           "          Der Treiber bittet beim Anhaengen um"
                           " die Uebergabe; steht\n"
                           "          das Bit danach immer noch, antwortet"
                           " das BIOS nicht.\n\n");
                }

                if ((k & 0x01000000ul) == 0 && (k & 0x00010000ul) == 0) {
                    printf("\n      Weder BIOS noch OS besitzen ihn."
                           "  Ungewoehnlich, aber\n"
                           "      unschaedlich -- es haelt ihn niemand"
                           " fest.\n\n");
                }

                break;
            }

            case 2: {

                /*  Unterstuetzte Protokolle.  Sagt, welche Anschlussnummern
                 *  USB 2 sind und welche USB 3 -- und genau das braucht
                 *  man, um einen Anschluss zu deuten, der "nicht geht":
                 *  an einer USB-3-Buchse haengt oft derselbe Stecker
                 *  zweimal, einmal je Protokoll.  */

                unsigned long name = lies1(x->nr, off + 4);
                unsigned long pv   = lies1(x->nr, off + 8);
                char          txt[5];

                txt[0] = (char)(name & 0xFF);
                txt[1] = (char)((name >> 8) & 0xFF);
                txt[2] = (char)((name >> 16) & 0xFF);
                txt[3] = (char)((name >> 24) & 0xFF);
                txt[4] = '\0';

                {
                    unsigned long erst = pv & 0xFF;
                    unsigned long anz  = (pv >> 8) & 0xFF;
                    unsigned long haupt = (k >> 24) & 0xFF;
                    unsigned long i;

                    printf("    %05lx  02 PROTOKOLL   %s %lu.%lu   "
                           "Anschluesse %lu bis %lu\n",
                           off, txt, haupt, (k >> 16) & 0xFF,
                           erst, erst + anz - 1);

                    /*  Merken, damit jede Anschlusszeile unten sagen
                     *  kann, welches Protokoll sie fuehrt.  Ohne das
                     *  deutet man einen "toten" Anschluss falsch: an
                     *  einer USB-3-Buchse zaehlt derselbe Stecker
                     *  zweimal, einmal je Protokoll, und nur einer der
                     *  beiden meldet je ein Geraet.  */

                    for (i = erst; i < erst + anz && i < 32; i += 1) {
                        x->prot[i] = (unsigned char)haupt;
                    }
                }

                break;
            }

            case 3:
                printf("    %05lx  03 Stromverwaltung\n", off);
                break;

            default:
                printf("    %05lx  %02lx (unbekannt)  %08lx\n", off, id, k);
                break;
        }

        if (weiter == 0) {
            break;
        }

        off += weiter;
    }
}


/* ==================================================================== *
 *  Betriebsblock
 * ==================================================================== */

static void
betrieb (XHCI *x)
{
    unsigned long cmd = lies1(x->nr, x->caplen + 0x00);
    unsigned long sts = lies1(x->nr, x->caplen + 0x04);
    unsigned long pgs = lies1(x->nr, x->caplen + 0x08);
    unsigned long cfg = lies1(x->nr, x->caplen + 0x38);
    unsigned long crl = lies1(x->nr, x->caplen + 0x18);
    unsigned long dcl = lies1(x->nr, x->caplen + 0x30);

    printf("\n  Betrieb  (ab %02lx)\n", x->caplen);

    printf("    USBCMD         %08lx ", cmd);
    bit(cmd, 0x0001, "RUN");
    bit(cmd, 0x0002, "HCRST");
    bit(cmd, 0x0004, "INTE");
    bit(cmd, 0x0008, "HSEE");
    bit(cmd, 0x0080, "LHCRST");
    bit(cmd, 0x0100, "CSS");
    bit(cmd, 0x0200, "CRS");
    bit(cmd, 0x0400, "EWE");
    printf("\n");

    printf("    USBSTS         %08lx ", sts);
    bit(sts, 0x0001, "HCH");
    bit(sts, 0x0004, "HSE");
    bit(sts, 0x0008, "EINT");
    bit(sts, 0x0010, "PCD");
    bit(sts, 0x0100, "SSS");
    bit(sts, 0x0200, "RSS");
    bit(sts, 0x0400, "SRE");
    bit(sts, 0x0800, "CNR");
    bit(sts, 0x1000, "HCE");
    printf("\n");

    printf("    PAGESIZE       %08lx  Seiten %lu Byte\n",
           pgs, 4096ul * (pgs & 0xFFFF));
    /*  Bit 3 ist CRR -- Command Ring Running.  Der Baustein setzt es
     *  nur, solange er gerade einen Befehl abarbeitet; im Leerlauf ist
     *  es null, und das ist der Normalfall.  Die erste Fassung schrieb
     *  dort "Ring steht" und machte daraus ein Problem, wo keines ist.  */

    printf("    CRCR           %08lx  Befehlsring %s\n", crl,
           (crl & 0x8) ? "arbeitet gerade" : "im Leerlauf (normal)");
    printf("    DCBAAP         %08lx\n", dcl);
    printf("    CONFIG         %08lx  aktivierte Plaetze %lu\n",
           cfg, cfg & 0xFF);

    if ((sts & 0x0800) != 0) {
        printf("\n    CNR ist gesetzt: der Baustein ist noch nicht"
               " bereit.\n    Vor dem Zuruecksetzen darf kein Register"
               " geschrieben werden.\n");
    }

    if ((sts & 0x1000) != 0) {
        printf("\n    HCE -- schwerer Fehler im Baustein.  Nur ein"
               " Zuruecksetzen\n    hilft; alles davor ist Aufraeumarbeit.\n");
    }

    if ((sts & 0x0004) != 0) {
        printf("\n    HSE -- der Baustein hat beim Zugriff auf den"
               " Hauptspeicher\n    einen Fehler bekommen.  Meist eine"
               " falsche physische Adresse\n    in DCBAAP oder im Ring:"
               " genau die Stelle, an der 64-Bit-\n    Adressen halb"
               " geschrieben werden.\n");
    }

    if ((cmd & 0x0001) == 0 && (sts & 0x0001) != 0) {
        printf("\n    Der Baustein steht (RUN aus, HCH an).  Das ist der"
               " Zustand\n    vor dem Anhaengen -- und der Zustand, in dem"
               " er bleibt, wenn\n    das Anhaengen gescheitert ist.\n");
    }

    if ((cmd & 0x0004) == 0) {
        printf("\n    INTE ist aus: der Baustein meldet keine"
               " Unterbrechungen.\n");
    }
}


/* ==================================================================== *
 *  Anschluesse
 * ==================================================================== */

static const char *
plsname (unsigned long pls)
{
    static const char *n[] = {
        "U0", "U1", "U2", "U3susp", "Disabled", "RxDetect", "Inactive",
        "Polling", "Recovery", "HotReset", "ComplMode", "TestMode",
        "?", "?", "?", "Resume"
    };

    return n[pls & 15];
}

static void
anschluesse (XHCI *x)
{
    unsigned long i;
    int           belegt = 0;

    printf("\n  Anschluesse  (%lu)\n", x->anschl);
    printf("    nr  PORTSC    ge  pls        zustand\n");
    printf("    ----------------------------------------------------------\n");

    for (i = 1; i <= x->anschl && i <= 30; i += 1) {

        unsigned long p = lies1(x->nr, x->caplen + 0x400 + (i - 1) * 0x10);
        unsigned long ge;

        if (p == 0xFFFFFFFFul) {
            printf("    %2lu  nicht lesbar\n", i);
            continue;
        }

        ge = (p >> 10) & 0xF;

        printf("    %2lu  %08lx  %-2lu  %-10s %s ", i, p, ge,
               plsname((p >> 5) & 0xF),
               (x->prot[i] == 3) ? "usb3" :
               (x->prot[i] == 2) ? "usb2" : "  ? ");

        bit(p, 0x00000001, "CCS");
        bit(p, 0x00000002, "PED");
        bit(p, 0x00000008, "OCA");
        bit(p, 0x00000010, "PR");
        bit(p, 0x00000200, "PP");
        bit(p, 0x00020000, "CSC");
        bit(p, 0x00040000, "PEC");
        bit(p, 0x00200000, "PRC");
        printf("\n");

        if ((p & 1) != 0) {
            belegt += 1;
        }
    }

    printf("\n    %d Anschluesse belegt.\n", belegt);
    (void)0;

    if (belegt == 0) {
        printf("\n    Nichts angesteckt -- oder PP ist aus und der Baustein\n"
               "    gibt keinen Strom.  An einer USB-3-Buchse zaehlt jeder\n"
               "    Stecker doppelt: einmal als USB-2-Anschluss, einmal als\n"
               "    USB-3.  Welche Nummer welches Protokoll hat, sagt die\n"
               "    Faehigkeit 02 oben.\n");
    }
}


/* ==================================================================== *
 *  Laufzeitblock -- hier greift die Maskierung der Unterlage
 * ==================================================================== */

static void
laufzeit (XHCI *x)
{
    unsigned long mfi  = lies1(x->nr, x->rtsoff + 0x00);
    unsigned long iman = lies1(x->nr, x->rtsoff + 0x20);
    unsigned long imod = lies1(x->nr, x->rtsoff + 0x24);
    unsigned long ersz = lies1(x->nr, x->rtsoff + 0x28);

    printf("\n  Laufzeit  (ab %05lx)\n", x->rtsoff);
    printf("    MFINDEX        %08lx\n", mfi);
    printf("    IMAN[0]        %08lx ", iman);
    bit(iman, 0x1, "IP");
    bit(iman, 0x2, "IE");
    printf("\n");
    printf("    IMOD[0]        %08lx  Abstand %lu x 250 ns\n",
           imod, imod & 0xFFFF);
    printf("    ERSTSZ[0]      %08lx  %lu Eintraege\n", ersz, ersz & 0xFFFF);

    printf("\n    Auf IMAN.IE setzt die Maskierung im ISR an.  Sie zieht"
           " beim\n    Sperren 0x3 ab und setzt beim Freigeben Bit 0"
           " ausdruecklich\n    NICHT -- IP ist write-1-to-clear, eine Eins"
           " dort wuerde eine\n    Unterbrechung quittieren, die der Treiber"
           " noch nicht gesehen hat.\n");

    if (mfi == 0) {
        printf("\n    MFINDEX steht bei null.  Der Rahmenzaehler laeuft"
               " nicht --\n    der Baustein ist nicht gestartet.\n");
    }
}


/* ==================================================================== *
 *  Zaehler und Anschlussbeobachtung
 *
 *  Die eine Frage, die aus den Registern allein nicht zu beantworten
 *  ist: kommen ueberhaupt Unterbrechungen an, und arbeitet der Faden
 *  sie ab.  Steht PED auf null, waehrend CRR laeuft, heisst das, der
 *  Treiber wartet auf ein Ereignis -- und dann entscheidet sich hier,
 *  ob es nie gemeldet oder nie abgeholt wurde.
 * ==================================================================== */

static void
zaehler (int schleife, DWORD nr, unsigned long caplen, unsigned long anschl)
{
    NTU_ZAEHLER z;
    NTU_ZAEHLER v;
    DWORD       got = 0;
    int         erst = 1;
    unsigned long altport[31];
    DWORD       i;

    memset(&v, 0, sizeof(v));
    memset(altport, 0, sizeof(altport));

    for (;;) {

        if (!DeviceIoControl(H, IOCTL_NTU_ZAEHLER, NULL, 0, &z, sizeof(z),
                             &got, NULL)) {
            printf("  Zaehler nicht lesbar.\n");
            return;
        }

        if (!schleife) {

            printf("  Zaehler\n\n");
            printf("    Unterbrechungen        %lu\n", z.Unterbrechungen);
            printf("    davon fremd            %lu\n", z.Fremdunterbr);
            printf("    Handlerlaeufe im Faden %lu\n", z.Fadenlaeufe);
            printf("    Arbeit verloren        %lu\n", z.ArbeitVerloren);
            printf("    Ticks                  %lu\n", z.Ticks);
            printf("    Pool                   %lu Vergaben, %lu stehen, "
                   "%lu abgewiesen\n\n",
                   z.PoolVergaben, z.PoolOffen, z.PoolGemeckert);

            if (z.Unterbrechungen == 0) {
                printf("    KEINE Unterbrechung.  Der Baustein meldet"
                       " nichts, oder die\n"
                       "    Leitung kommt nicht an.  Bei einem Anschluss"
                       " mit CCS und\n"
                       "    ohne PED heisst das: der Treiber wartet auf"
                       " ein Ereignis,\n"
                       "    das nie gemeldet wurde.  Dann liegt es an"
                       " IoConnectInterrupt\n"
                       "    oder an der Leitungsnummer, nicht am"
                       " xHCI-Treiber.\n\n");
            } else if (z.Fadenlaeufe == 0) {
                printf("    Unterbrechungen kommen an, aber der Faden"
                       " laeuft nicht.\n"
                       "    Dann haengt es zwischen DPC und Faden --"
                       " das Ereignis wird\n"
                       "    gemeldet und nie abgeholt.\n\n");
            } else {

                /*  Dass die beiden Zahlen auseinanderlaufen, ist Bauart
                 *  und kein Fehler: der DPC setzt ein
                 *  SynchronizationEvent, und kommen zwei
                 *  Unterbrechungen, bevor der Faden drankommt, wird es
                 *  zweimal gesetzt und einmal geerntet.  Der Faden
                 *  leert den Ereignisring dann in einem Durchlauf.  */

                printf("    Die Kette steht: Leitung, DPC und Faden"
                       " arbeiten.\n"
                       "    Dass 2 zu 1 auseinanderlaeuft, ist Bauart --"
                       " ein Ereignis je\n"
                       "    Fadenlauf, und der Faden leert den ganzen"
                       " Ring auf einmal.\n\n");
            }

            if (z.PoolGemeckert != 0) {
                printf("    Die Pool-Wache hat Freigaben abgewiesen."
                       "  Jede davon waere\n"
                       "    ein Bugcheck gewesen; die serielle Ausgabe"
                       " nennt Nummer und\n    Groesse.\n\n");
            }

            return;
        }

        if (erst) {
            printf("  Beobachten -- Strg-C beendet.  Stecken Sie das"
                   " Geraet ab und wieder an.\n\n");
            erst = 0;
        } else {
            printf("  unt %6lu (+%lu)  faden %6lu (+%lu)  verl %lu",
                   z.Unterbrechungen, z.Unterbrechungen - v.Unterbrechungen,
                   z.Fadenlaeufe, z.Fadenlaeufe - v.Fadenlaeufe,
                   z.ArbeitVerloren);

            for (i = 1; i <= anschl && i <= 30; i += 1) {

                unsigned long p = lies1(nr, caplen + 0x400 + (i - 1) * 0x10);

                if (p != altport[i]) {
                    printf("   [%lu %08lx->%08lx]", i, altport[i], p);
                }

                altport[i] = p;
            }

            printf("\n");
        }

        v = z;
        Sleep(1000);
    }
}


/* ==================================================================== *
 *  Spur
 *
 *  Was der ISR vorgefunden hat, bevor er gesperrt hat.  Beantwortet die
 *  Frage, die aus den Zaehlern nicht hervorgeht: meldet der Baustein ein
 *  Ereignis, und raeumt der Treiber es weg.
 *
 *      USBSTS Bit 3  EINT   der Baustein hat ein Ereignis abgelegt
 *      IMAN   Bit 0  IP     der Unterbrecher haelt es fest
 *
 *  Sind beide immer gesetzt und der Tick laeuft weiter, wird derselbe
 *  Eintrag endlos wiederholt -- dann wird der Ereignisring nicht
 *  weitergeschoben.  Ist EINT immer AUS, meldet sich etwas anderes.
 * ==================================================================== */

static void
spur (void)
{
    NTU_SPUR_ANTWORT a;
    DWORD            got = 0;
    DWORD            k;
    int              eint = 0;
    int              ip   = 0;

    if (!DeviceIoControl(H, IOCTL_NTU_SPUR, NULL, 0, &a, sizeof(a),
                         &got, NULL)) {
        printf("  Spur nicht lesbar -- aelterer Treiber?\n\n");
        return;
    }

    printf("  Spur der letzten %lu Unterbrechungen "
           "(insgesamt %lu)\n\n", a.Anzahl, a.Kopf);
    printf("    nr   tick    IMAN      USBSTS\n");
    printf("    ---------------------------------------------\n");

    for (k = 0; k < a.Anzahl; k += 1) {

        NTU_SPUR_EINTRAG *e = &a.Eintrag[k];

        if (e->Tick == 0 && e->Iman == 0 && e->Usbsts == 0) {
            continue;
        }

        printf("    %2lu  %6lu  %08lx  %08lx ", k, e->Tick, e->Iman,
               e->Usbsts);

        if (e->Iman   & 0x01) { printf(" IP");   ip   += 1; }
        if (e->Iman   & 0x02)   printf(" IE");
        if (e->Usbsts & 0x08) { printf(" EINT"); eint += 1; }
        if (e->Usbsts & 0x10)   printf(" PCD");
        if (e->Usbsts & 0x01)   printf(" HCH");
        if (e->Usbsts & 0x04)   printf(" HSE");
        if (e->Usbsts & 0x400)  printf(" SRE");
        printf("\n");
    }

    printf("\n");

    if (eint == 0) {
        printf("    EINT war NIE gesetzt.  Der Baustein hat kein Ereignis\n"
               "    abgelegt -- es meldet sich etwas anderes auf derselben\n"
               "    Leitung, und unser ISR nimmt es faelschlich an.  Dann\n"
               "    gehoert ein Filter davor.\n\n");
    } else if (ip == (int)a.Anzahl) {
        printf("    IP war jedesmal gesetzt und der Tick laeuft weiter.\n"
               "    Dasselbe Ereignis wird endlos wiederholt: der\n"
               "    Ereignisring wird nicht weitergeschoben.  Dann stimmt\n"
               "    ERDP nicht, oder der Ring liegt physisch woanders, als\n"
               "    der Baustein glaubt.\n\n");
    }
}


/* ==================================================================== *
 *  Urteil
 *
 *  Vier Zeilen aus vier Registerbloecken zusammensuchen muss man nicht
 *  von Hand.  Hier steht, ob der Baustein laeuft -- und wenn nicht,
 *  woran es liegt.
 * ==================================================================== */

static void
urteil (XHCI *x)
{
    unsigned long cmd = lies1(x->nr, x->caplen + 0x00);
    unsigned long sts = lies1(x->nr, x->caplen + 0x04);
    unsigned long cfg = lies1(x->nr, x->caplen + 0x38);
    unsigned long dcb = lies1(x->nr, x->caplen + 0x30);
    unsigned long man = lies1(x->nr, x->rtsoff + 0x20);
    unsigned long mfi = lies1(x->nr, x->rtsoff + 0x00);
    int           gut = 1;

    printf("\n  Urteil\n");

    if ((sts & 0x0800) != 0) { printf("    CNR    -- noch nicht bereit\n");           gut = 0; }
    if ((sts & 0x1000) != 0) { printf("    HCE    -- schwerer Fehler im Baustein\n"); gut = 0; }
    if ((sts & 0x0004) != 0) { printf("    HSE    -- Fehler beim Speicherzugriff\n"); gut = 0; }
    if ((cmd & 0x0001) == 0) { printf("    RUN ist aus -- nicht gestartet\n");        gut = 0; }
    if ((sts & 0x0001) != 0) { printf("    HCH ist an -- angehalten\n");              gut = 0; }
    if ((cmd & 0x0004) == 0) { printf("    INTE ist aus -- keine Unterbrechungen\n"); gut = 0; }
    if ((man & 0x0002) == 0) { printf("    IMAN.IE ist aus -- Unterbrecher 0 "
                                      "schweigt\n");                                 gut = 0; }
    if (dcb == 0)            { printf("    DCBAAP ist leer -- kein Geraetefeld\n");   gut = 0; }
    if ((cfg & 0xFF) == 0)   { printf("    CONFIG 0 -- kein Platz aktiviert\n");      gut = 0; }
    if (mfi == 0)            { printf("    MFINDEX steht -- der Rahmenzaehler "
                                      "laeuft nicht\n");                             gut = 0; }

    if (gut) {
        printf("    Der Baustein laeuft.  Jetzt fehlt nur noch ein Geraet.\n");
    }

    printf("\n");
}


/* ==================================================================== */

static void
einen (DWORD nr, NTU_HC_EINTRAG *e, int roh)
{
    XHCI x;

    memset(&x, 0, sizeof(x));
    x.nr = nr;

    printf("\n======================================================"
           "==============\n");
    printf(" Baustein %lu   %02x:%02x.%x   %04x:%04x   %s\n",
           (unsigned long)nr, e->Bus, e->Slot, e->Func,
           e->Vendor, e->Device,
           (e->Angehaengt == 1) ? "angehaengt" : "NICHT angehaengt");
    printf("========================================================"
           "============\n\n");

    hersteller(e->Vendor, e->Device, e->Revision);

    if (!faehigkeiten(&x)) {
        return;
    }

    erweiterungen(&x);
    betrieb(&x);
    anschluesse(&x);
    laufzeit(&x);
    urteil(&x);

    if (roh) {

        static unsigned long w[64];
        DWORD                i;

        printf("\n  Roh, ab 0\n");
        lies(nr, 0, 64, w);

        for (i = 0; i < 64; i += 1) {

            if ((i % 4) == 0) {
                printf("    %04lx  ", (unsigned long)(i * 4));
            }

            printf("%08lx  ", w[i]);

            if ((i % 4) == 3) {
                printf("\n");
            }
        }
    }

    printf("\n");
}

int
main (int argc, char **argv)
{
    static NTU_HC_ANTWORT a;   /* dito */
    DWORD          got = 0;
    DWORD          i;
    const char    *argv1 = (argc > 1) ? argv[1] : "";
    int            nur  = (argc > 1 && argv1[0] >= '0' && argv1[0] <= '9')
                              ? atoi(argv1) : -1;
    int            roh  = (argc > 2 && strcmp(argv[2], "roh") == 0);
    int            fund = 0;
    int            probelauf = 0;
    int            nur_zaehler = 0;

    printf("\nxhcitest -- xHCI im Klartext\n");

    if (strcmp(argv1, "laden") == 0) {
        printf("\n");
        ntu_letzte_wahl = (argc > 2) ? argv[2] : "xhcint";
        return laden(ntu_letzte_wahl) ? 0 : 1;
    }

    if (strcmp(argv1, "entladen") == 0) {
        printf("\n");
        ntu_letzte_wahl = (argc > 2) ? argv[2] : "xhcint";
        return entladen2(ntu_letzte_wahl, 0) ? 0 : 1;
    }

    if (strcmp(argv1, "wechsel") == 0) {

        int n = (argc > 2 && argv[2][0] >= '0' && argv[2][0] <= '9')
                    ? atoi(argv[2]) : 10;

        ntu_letzte_wahl = (argc > 3) ? argv[3] : "xhcint";
        int k;

        printf("\n  %d Durchgaenge laden/entladen.\n\n", n);

        for (k = 1; k <= n; k += 1) {

            printf("  [%3d] ", k);

            if (!laden(ntu_letzte_wahl)) {
                printf("\n  Abbruch beim Laden im Durchgang %d.\n\n", k);
                return 1;
            }

            Sleep(300);

            if (!entladen(1)) {
                printf("\n  Abbruch beim Entladen im Durchgang %d.\n"
                       "  Genau hier faellt ein Treiber auf, der sich\n"
                       "  einmal entladen laesst und nicht zehnmal.\n\n", k);
                return 1;
            }

            Sleep(300);
            printf("  ok\n");
        }

        printf("\n  %d Durchgaenge ohne Fehler.\n\n", n);
        return 0;
    }

    if (strcmp(argv1, "zaehler") == 0 || strcmp(argv1, "beobachten") == 0) {
        nur_zaehler = (strcmp(argv1, "beobachten") == 0) ? 2 : 1;
    }

    if (strcmp(argv1, "spur") == 0) {
        nur_zaehler = 3;
    }

    if (strcmp(argv1, "probelauf") == 0) {
        printf("\n  Probelauf: laden, auslesen, entladen.\n\n");
        ntu_letzte_wahl = (argc > 2) ? argv[2] : "xhcint";
        entladen(1);

        if (!laden(ntu_letzte_wahl)) {
            return 1;
        }

        Sleep(500);
        probelauf = 1;
    }

    H = oeffnen();

    if (H == INVALID_HANDLE_VALUE) {

        printf("\n  Weder \\\\.\\XhciNt noch \\\\.\\UsbNt antworten.\n"
               "  'xhcitest laden' traegt den Treiber ein und startet"
               " ihn.\n\n");

        if (probelauf) {
            entladen(1);
        }

        return 1;
    }

    if (!DeviceIoControl(H, IOCTL_NTU_HOSTBAUSTEINE, NULL, 0,
                         &a, sizeof(a), &got, NULL)) {
        printf("\n  Die Hostbausteintabelle ist nicht lesbar.\n\n");
        CloseHandle(H);
        return 1;
    }

    for (i = 0; i < a.Anzahl; i += 1) {

        if (a.Eintrag[i].ProgIf != 0x30) {
            continue;
        }

        if (nur >= 0 && (DWORD)nur != i) {
            continue;
        }

        fund += 1;

        if (nur_zaehler != 0) {

            unsigned long caplen = lies1(i, 0x00) & 0xFF;
            unsigned long anschl = (lies1(i, 0x04) >> 24) & 0xFF;

            printf("\n");

            if (nur_zaehler == 3) {
                spur();
            } else {
                zaehler(nur_zaehler == 2, i, caplen, anschl);
            }

            break;
        }

        einen(i, &a.Eintrag[i], roh);
    }

    if (fund == 0) {

        printf("\n  Kein xHCI gefunden.  Gemeldet wurden %lu Bausteine:\n\n",
               (unsigned long)a.Anzahl);

        for (i = 0; i < a.Anzahl; i += 1) {
            printf("    %2lu  %02x:%02x.%x  progif %02x\n",
                   (unsigned long)i, a.Eintrag[i].Bus, a.Eintrag[i].Slot,
                   a.Eintrag[i].Func, a.Eintrag[i].ProgIf);
        }

        printf("\n  Auf einer SB7x0/SB8x0-Suedbruecke ist das der"
               " Normalfall:\n  dort gibt es OHCI und EHCI, aber keinen"
               " xHCI.  Ein xHCI kommt\n  nur ueber eine Steckkarte dazu"
               " (Renesas uPD720200, ASMedia,\n  Etron, VIA).\n\n");
    }

    CloseHandle(H);

    if (probelauf) {
        printf("  Entladen.\n");

        if (!entladen(0)) {
            printf("\n  Der Treiber ist nicht wieder losgeworden."
                   "  Bis zum\n  naechsten Neustart bleibt er im"
                   " Speicher.\n");
            return 1;
        }

        printf("\n  Probelauf beendet.\n\n");
    }

    return 0;
}
