/*
 *  werkzeug/usbtest.c -- Diagnose und Probelauf aus dem Benutzermodus.
 *
 *  Uebersetzen (auf der NT-4.0-Maschine oder quer):
 *
 *      cl /W3 /O1 /MT usbtest.c advapi32.lib
 *      clang-cl --target=i386-pc-windows-msvc /O1 /MT usbtest.c advapi32.lib
 *
 *  Nur kernel32 und advapi32, kein C++, kein MFC, kein Win32-API jenseits
 *  von NT 4.0.  Das Programm laeuft auch auf einer Maschine, auf der der
 *  Treiber noch nie gestartet wurde -- und gerade dann ist es nuetzlich.
 *
 *  Aufrufe:
 *
 *      usbtest              alles der Reihe nach
 *      usbtest umgebung     Registrierung, Dienste, DEVICEMAP
 *                           -- braucht den Treiber NICHT
 *      usbtest bausteine    PCI-Tabelle aus dem Treiber
 *      usbtest geraete      Geraetebaum aus dem Treiber
 *      usbtest register N   Registerabzug von Baustein N
 *      usbtest zaehler      Betriebszaehler
 *      usbtest beobachten   Zaehler im Sekundentakt, bis Strg-C
 *      usbtest dienst ein [name]   Dienst anlegen und starten
 *      usbtest dienst aus [name]   anhalten und entfernen
 *
 *  [name] ist usbnt, uhcint, ohcint, ehcint oder xhcint; ohne Angabe
 *  usbnt.  Beim Auslesen wird der gefunden, der gerade liegt.
 *
 *  Warum der Treiber die PCI-Arbeit tut und nicht dieses Programm: ein
 *  Benutzerprozess unter NT 4.0 kommt weder an CF8/CFC noch an den
 *  Registerblock des Bausteins.  Es gibt dafuer keinen Weg ohne
 *  Kernbeteiligung; wer einen kennt, kennt eine Sicherheitsluecke.
 */

#include <windows.h>
#include "ntbasis.h"
#include "../ober/ntusbioctl.h"
#include "sprache.h"

static const char *DIENST = "usbnt";


/* ==================================================================== *
 *  Kleinkram
 * ==================================================================== */

static const char *
bauart (unsigned char progif)
{
    switch (progif) {
        case 0x00: return "uhci";
        case 0x10: return "ohci";
        case 0x20: return "ehci";
        case 0x30: return "xhci";
        case 0xFE: return "geraet";
        default:   return "eigen";
    }
}

static void
fehler (const char *was)
{
    DWORD e = GetLastError();
    char  puffer[256];

    puffer[0] = '\0';
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, e, 0,
                   puffer, sizeof(puffer), NULL);
    printf("  %s: Fehler %lu  %s", was, (unsigned long)e, puffer);

    if (puffer[0] == '\0') {
        printf("\n");
    }
}

static const char *ntu_namen[5] = {
    NTU_WIN32_ALLE, NTU_WIN32_XHCI, NTU_WIN32_EHCI,
    NTU_WIN32_OHCI, NTU_WIN32_UHCI
};

static const char *ntu_dienste[5] = {
    "usbnt", "xhcint", "ehcint", "ohcint", "uhcint"
};

static HANDLE
oeffnen (void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    int    i;

    /*  Fuenf Bauformen, fuenf Geraetenamen.  Der Reihe nach probieren
     *  ist billiger als eine Einstellung, die richtig gesetzt sein
     *  muss -- und sagt gleich, welcher liegt.  */

    for (i = 0; i < 5; i += 1) {

        h = CreateFileA(ntu_namen[i], GENERIC_READ | GENERIC_WRITE,
                        0, NULL, OPEN_EXISTING, 0, NULL);

        if (h != INVALID_HANDLE_VALUE) {
            printf("  Treiber: %s\n\n", ntu_dienste[i]);
            return h;
        }
    }

    if (h == INVALID_HANDLE_VALUE) {

        printf("  Kein Treiber antwortet.\n");

        printf("  Keiner der fuenf Namen antwortet -- UsbNt, XhciNt,\n"
               "  EhciNt, OhciNt, UhciNt.  Entweder laeuft keiner, oder\n"
               "  einer laeuft und hat seinen Verweis unter \\DosDevices\n"
               "  nicht anlegen koennen.  Die serielle Ausgabe sagt,\n"
               "  welches von beidem.\n");

        printf("\n  'usbtest umgebung' laeuft auch ohne Treiber.\n");
    }

    return h;
}

static int
rufen (HANDLE h, DWORD code, void *ein, DWORD einlen,
       void *aus, DWORD auslen, const char *name)
{
    DWORD got = 0;

    if (!DeviceIoControl(h, code, ein, einlen, aus, auslen, &got, NULL)) {
        fehler(name);
        return 0;
    }

    return 1;
}


/* ==================================================================== *
 *  Umgebung -- der Teil, der ohne Treiber laeuft
 *
 *  Er beantwortet die Fragen, an denen es in der Praxis haengt, und
 *  zwar bevor man den Treiber ueberhaupt startet.
 * ==================================================================== */

static void
regwert (HKEY wurzel, const char *pfad, const char *name, const char *hinweis,
         DWORD soll)
{
    HKEY  k;
    DWORD typ = 0;
    DWORD wert = 0;
    DWORD len = sizeof(wert);

    if (RegOpenKeyExA(wurzel, pfad, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        printf("    %-24s Schluessel fehlt\n", name);
        return;
    }

    if (RegQueryValueExA(k, name, NULL, &typ, (BYTE *)&wert, &len)
            != ERROR_SUCCESS) {
        printf("    %-24s nicht gesetzt  (Vorgabe wirkt)  %s\n",
               name, hinweis);
    } else {
        printf("    %-24s %lu%s\n", name, (unsigned long)wert,
               (wert >= soll) ? "" : "   <-- zu klein");
    }

    RegCloseKey(k);
}

static void
devicemap (const char *unter)
{
    char  pfad[128];
    HKEY  k;
    DWORD i;

    sprintf(pfad, "HARDWARE\\DEVICEMAP\\%s", unter);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, pfad, 0, KEY_READ, &k)
            != ERROR_SUCCESS) {
        printf("    %-14s kein Eintrag\n", unter);
        return;
    }

    for (i = 0; ; i += 1) {

        char  name[256];
        char  daten[256];
        DWORD nlen = sizeof(name);
        DWORD dlen = sizeof(daten);
        DWORD typ  = 0;

        if (RegEnumValueA(k, i, name, &nlen, NULL, &typ,
                          (BYTE *)daten, &dlen) != ERROR_SUCCESS) {
            break;
        }

        daten[sizeof(daten) - 1] = '\0';
        printf("    %-14s [%lu] %-28s %s\n", (i == 0) ? unter : "",
               (unsigned long)i, name, daten);
    }

    if (i == 0) {
        printf("    %-14s leer\n", unter);
    }

    RegCloseKey(k);
}

static void
dienstlage (void)
{
    SC_HANDLE scm;
    SC_HANDLE d;
    SERVICE_STATUS st;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);

    if (scm == NULL) {
        printf("    Dienstverwaltung nicht erreichbar\n");
        return;
    }

    d = OpenServiceA(scm, DIENST, SERVICE_QUERY_STATUS);

    if (d == NULL) {
        printf("    Dienst '%s' ist nicht eingetragen\n", DIENST);
        printf("    -> usbtest dienst ein\n");
    } else {

        if (QueryServiceStatus(d, &st)) {
            printf("    Dienst '%s' Zustand %lu%s\n", DIENST,
                   (unsigned long)st.dwCurrentState,
                   (st.dwCurrentState == SERVICE_RUNNING) ? "  (laeuft)"
                                                          : "");
        }

        CloseServiceHandle(d);
    }

    CloseServiceHandle(scm);
}

static void
umgebung (void)
{
    printf("Umgebung\n\n");

    printf("  Dienst\n");
    dienstlage();

    printf("\n  DEVICEMAP -- so und nur so findet kbdclass einen Anschluss\n");
    devicemap("KeyboardPort");
    devicemap("PointerPort");

    printf("\n  Kbdclass\\Parameters\n");
    regwert(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Services\\Kbdclass\\Parameters",
            "MaximumPortsServiced", "(Vorgabe 1)", 2);
    regwert(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Services\\Kbdclass\\Parameters",
            "ConnectMultiplePorts", "(Vorgabe 0)", 1);

    printf("\n  Mouclass\\Parameters\n");
    regwert(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Services\\Mouclass\\Parameters",
            "MaximumPortsServiced", "(Vorgabe 1)", 2);
    regwert(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Services\\Mouclass\\Parameters",
            "ConnectMultiplePorts", "(Vorgabe 0)", 1);

    printf("\n"
           "  Steht bei ConnectMultiplePorts eine 0 und ist ausserdem eine\n"
           "  PS/2-Tastatur da, landet die USB-Tastatur auf KeyboardClass1,\n"
           "  und der Raw Input Thread von USER liest nur KeyboardClass0.\n"
           "  Sie tippen dann ins Leere, ohne dass irgendwo ein Fehler steht.\n"
           "  Abhilfe: pruef\\kbdmou.reg einspielen und neu starten.\n\n");
}


/* ==================================================================== *
 *  Treiberseitige Abfragen
 * ==================================================================== */

static void
bausteine (HANDLE h)
{
    static NTU_HC_ANTWORT a;   /* statisch: ueber 4 kB
                                  Rahmen laesst __chkstk rufen */
    DWORD          i;

    if (!rufen(h, IOCTL_NTU_HOSTBAUSTEINE, NULL, 0, &a, sizeof(a),
               S(T_W_HOSTBAUSTEINE))) {
        return;
    }

    printf("Hostbausteine  (%lu)\n\n", (unsigned long)a.Anzahl);
    printf("  nr  bus:dev.f  art   hst:bstn   anschluss irq sbrn cmd  legsup"
           " stand\n");
    printf("  ---------------------------------------------------------"
           "-----------------\n");

    for (i = 0; i < a.Anzahl; i += 1) {

        NTU_HC_EINTRAG *e = &a.Eintrag[i];

        printf("  %2lu  %02x:%02x.%x   %-5s %04x:%04x  %08lx  %3u  %02x  "
               "%04x %04x   %s\n",
               (unsigned long)i, e->Bus, e->Slot, e->Func, bauart(e->ProgIf),
               e->Vendor, e->Device, (unsigned long)e->Bar, e->Irq,
               e->Sbrn, e->Command, e->LegSup,
               (e->Angehaengt == 1) ? "an" :
               (e->Angehaengt == 2) ? "uebergangen" : "NICHT an");
    }

    printf("\n");

    for (i = 0; i < a.Anzahl; i += 1) {

        NTU_HC_EINTRAG *e = &a.Eintrag[i];

        if ((e->Command & 0x0004) == 0 && e->Angehaengt == 1) {
            printf("  [%lu] Busmeister aus, obwohl angehaengt -- DMA laeuft"
                   " nicht.\n", (unsigned long)i);
        }

        if (e->Bar == 0) {
            printf("  [%lu] Kein Registerblock zugeteilt.  Das BIOS hat dem"
                   " Baustein\n      keine Adresse gegeben.\n",
                   (unsigned long)i);
        }

        if (e->ProgIf == 0x00 && (e->LegSup & 0x8000) != 0) {
            printf("  [%lu] Das BIOS emuliert noch PS/2 ueber diesen"
                   " Baustein.\n", (unsigned long)i);
        }

        if (e->ProgIf == 0x20 && e->Sbrn != 0x20) {
            printf("  [%lu] EHCI mit SBRN %02x statt 20 -- das Brett ist"
                   " falsch beschrieben.\n", (unsigned long)i, e->Sbrn);
        }

        if (e->Irq == 0 || e->Irq == 0xFF) {
            printf("  [%lu] Keine Leitungsnummer.  Ohne die gibt es keine"
                   " Unterbrechung.\n", (unsigned long)i);
        }
    }

    printf("\n");
}

static void
geraete (HANDLE h)
{
    static NTU_DEV_ANTWORT a;  /* dito */
    DWORD           i;

    if (!rufen(h, IOCTL_NTU_GERAETE, NULL, 0, &a, sizeof(a), S(T_W_GERAETE))) {
        return;
    }

    printf("Geraetebaum  (%lu Knoten)\n\n", (unsigned long)a.Anzahl);

    for (i = 0; i < a.Anzahl; i += 1) {

        NTU_DEV_EINTRAG *e = &a.Eintrag[i];
        DWORD            t;

        printf("  ");

        for (t = 0; t < e->Tiefe; t += 1) {
            printf("  ");
        }

        printf("%-10s %-8s %s\n",
               (e->Treiber[0] != '\0') ? e->Treiber : "(ohne)",
               e->Belegt ? "an" : "frei",
               e->Text);
    }

    if (a.Anzahl <= 1) {
        printf("\n  Nur der Hostbaustein.  Der Wurzelhub hat noch nicht"
               " angehaengt --\n  entweder laeuft die Aufzaehlung noch, oder"
               " sie steht.  Die Zaehler\n  sagen, was davon:"
               " 'usbtest zaehler'.\n");
    }

    printf("\n");
}

static void
register_abzug (HANDLE h, DWORD nr)
{
    NTU_REG_ANFRAGE        q;
    static NTU_REG_ANTWORT a;
    DWORD                  i;

    memset(&q, 0, sizeof(q));
    q.Nummer  = nr;
    q.Anzahl  = 16;
    q.Versatz = 0;

    if (!rufen(h, IOCTL_NTU_REGISTER, &q, sizeof(q), &a, sizeof(a),
               "Register")) {
        return;
    }

    printf("Registerabzug Baustein %lu  (%lu Werte, %lu Byte breit)\n\n",
           (unsigned long)nr, (unsigned long)a.Anzahl,
           (unsigned long)a.Breite);

    for (i = 0; i < a.Anzahl; i += 1) {

        if ((i % 4) == 0) {
            printf("  %04lx  ", (unsigned long)(i * a.Breite));
        }

        printf((a.Breite == 2) ? "%04lx     " : "%08lx  ",
               (unsigned long)a.Wert[i]);

        if ((i % 4) == 3) {
            printf("\n");
        }
    }

    printf("\n\n");
}

static void
zaehler_zeigen (HANDLE h, int einmal)
{
    NTU_ZAEHLER z;
    NTU_ZAEHLER v;
    int         erst = 1;

    memset(&v, 0, sizeof(v));

    for (;;) {

        if (!rufen(h, IOCTL_NTU_ZAEHLER, NULL, 0, &z, sizeof(z),
                   "Zaehler")) {
            return;
        }

        if (einmal) {

            printf("Zaehler\n\n");
            printf("  Unterbrechungen        %lu\n", z.Unterbrechungen);
            printf("  davon fremd            %lu\n", z.Fremdunterbr);
            printf("  Handlerlaeufe im Faden %lu\n", z.Fadenlaeufe);
            printf("  Arbeit verloren        %lu\n", z.ArbeitVerloren);
            printf("  Anschlaege geliefert   %lu\n", z.AnschlaegeGesamt);
            printf("  davon verworfen        %lu\n", z.AnschlaegeVerworf);
            printf("  Mausmeldungen          %lu\n", z.MausMeldungen);
            printf("  Ticks                  %lu\n", z.Ticks);
            printf("\n");
            printf("  Pool: %lu Vergaben, %lu Bloecke stehen, "
                   "%lu Freigaben abgewiesen\n\n",
                   z.PoolVergaben, z.PoolOffen, z.PoolGemeckert);

            if (z.PoolGemeckert != 0) {
                printf("  Die Wache hat Freigaben abgewiesen.  JEDE davon\n"
                       "  waere ein Bugcheck in MiFreePoolPages gewesen.\n"
                       "  Die serielle Ausgabe nennt Vergabenummer und\n"
                       "  Groesse -- damit laesst sich die Stelle finden.\n\n");
            }

            if (z.Unterbrechungen == 0) {
                printf("  Keine einzige Unterbrechung.  Entweder ist die\n"
                       "  Leitung falsch zugeteilt, oder der Busmeister ist\n"
                       "  aus, oder der Baustein wurde nie gestartet.\n"
                       "  'usbtest bausteine' zeigt welches davon.\n\n");
            }

            if (z.Fadenlaeufe < z.Unterbrechungen) {
                printf("  Es kommen mehr Unterbrechungen an, als der Faden\n"
                       "  abarbeitet.  Wenn der Abstand waechst, laeuft die\n"
                       "  Maskierung im ISR nicht -- dann steht der Rechner\n"
                       "  in einem Sturm.\n\n");
            }

            if (z.ArbeitVerloren != 0) {
                printf("  Der callout-Ring ist uebergelaufen.  Zeitgeber\n"
                       "  sind verlorengegangen; die Aufzaehlung kann\n"
                       "  dadurch stehenbleiben.\n\n");
            }

            return;
        }

        if (!erst) {
            printf("  unt %6lu (+%lu)   faden %6lu (+%lu)   tasten %6lu"
                   "   maus %6lu   verl %lu\n",
                   z.Unterbrechungen, z.Unterbrechungen - v.Unterbrechungen,
                   z.Fadenlaeufe, z.Fadenlaeufe - v.Fadenlaeufe,
                   z.AnschlaegeGesamt, z.MausMeldungen, z.ArbeitVerloren);
        } else {
            printf("Beobachten -- Strg-C beendet.  Tippen Sie auf der\n"
                   "USB-Tastatur, bewegen Sie die Maus.\n\n");
            erst = 0;
        }

        v = z;
        Sleep(1000);
    }
}


/* ==================================================================== *
 *  Dienst an- und abmelden
 *
 *  Die Treiberdatei wird dort gesucht, wo usbtest.exe liegt.  Das ist
 *  die Annahme, die beim Ausprobieren immer stimmt und beim Ausliefern
 *  nie -- deshalb steht der Pfad in der Ausgabe.
 * ==================================================================== */



/* ==================================================================== */

int
main (int argc, char **argv)
{
    const char *was = (argc > 1) ? argv[1] : "alles";
    HANDLE      h;

    printf("\nusbtest -- FreeBSD-USB-Stapel auf NT 4.0\n\n");

    /*  Das Anlegen/Starten des Treiberdienstes macht USBINST -- ein
     *  Diagnosewerkzeug soll keine Dienste einrichten (das ist ein
     *  Installer-Muster, das Virenscanner zu Recht misstrauen).  */
    if (strcmp(was, "dienst") == 0) {
        printf("  Zum Ein-/Ausschalten des Treibers: USBINST ein usbnt  /  USBINST aus\n");
        return 0;
    }

    if (strcmp(was, "umgebung") == 0) {
        umgebung();
        return 0;
    }

    h = oeffnen();

    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }

    {
        NTU_VERSION v;

        if (rufen(h, IOCTL_NTU_VERSION, NULL, 0, &v, sizeof(v), "Version")) {

            if (v.Fassung != NTU_ABI_FASSUNG) {

                /*  Ungleiche Fassungen heisst ungleiche Strukturlagen.
                 *  Weiterlesen waere Raten.  */

                printf("  Treiber spricht Fassung %lu, das Werkzeug %u.\n"
                       "  Abbruch -- die Strukturen passen nicht.\n",
                       v.Fassung, NTU_ABI_FASSUNG);
                CloseHandle(h);
                return 1;
            }

            printf("  Treiber gebaut am %s\n\n", v.Bau);
        }
    }

    if (strcmp(was, "bausteine") == 0) {
        bausteine(h);
    } else if (strcmp(was, "geraete") == 0) {
        geraete(h);
    } else if (strcmp(was, "register") == 0) {
        register_abzug(h, (argc > 2) ? (DWORD)atoi(argv[2]) : 0);
    } else if (strcmp(was, "zaehler") == 0) {
        zaehler_zeigen(h, 1);
    } else if (strcmp(was, "beobachten") == 0) {
        zaehler_zeigen(h, 0);
    } else {
        umgebung();
        bausteine(h);
        geraete(h);
        zaehler_zeigen(h, 1);
    }

    CloseHandle(h);
    return 0;
}
