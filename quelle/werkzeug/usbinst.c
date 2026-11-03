/*
 *  werkzeug/usbinst.c -- den USB-Treiber auf NT 4.0 einrichten.
 *
 *      usbinst                   Zustand: was eingerichtet ist, was stoert
 *      usbinst probe  [art]      Treiber kopieren und SOFORT von Hand
 *                                starten -- ohne Neustart, ohne Risiko
 *      usbinst ein    [art]      fest einrichten, laedt ab dem naechsten
 *                                Start mit dem System
 *      usbinst aus               alles entfernen, geaenderte Werte zurueck
 *      usbinst pruefen           fremde Maus-/USB-Treiber, PS/2 auflisten
 *      usbinst abschalten <fund> einen Fund abschalten (gesichert)
 *      usbinst wieder            Abgeschaltetes zurueck
 *
 *      art        usbnt (Vorgabe, alle vier Bauarten),
 *                 uhci, ohci, ehci, xhci (nur diese eine)
 *      -seriell N Bericht: 0 aus, 1 COM1 (Vorgabe), 2 COM2
 *      -neustart  danach neu starten
 *
 *  Der Weg, den dieser Installer nahelegt:  erst "probe" -- der Treiber
 *  wird geladen, haengt die Bausteine an und meldet sich; geht dabei
 *  etwas schief, genuegt ein Neustart, denn eingetragen ist er nur fuer
 *  den Start von Hand.  Erst danach "ein".  Ein Starttreiber, der beim
 *  Booten abstuerzt, macht NT unstartbar; der Rueckweg ist dann "Letzte
 *  als funktionierend bekannte Konfiguration" (Leertaste beim Booten).
 *
 *  Was "ein" aendert, und warum:
 *
 *    Services\<dienst>      Start 1 (System), Gruppe "Keyboard Port".
 *        Die Gruppe ist Pflicht: kbdclass zaehlt die Tastaturanschluesse
 *        genau einmal, in seinem DriverEntry.  Laedt der Treiber spaeter,
 *        ist die USB-Tastatur tot.
 *    Services\<dienst>\Parameters\Ausgabe
 *        wohin der Bericht geht.  An COM1 haengt auf NT-4.0-Rechnern oft
 *        eine serielle Maus; der Bericht stellt den Baustein auf 115200
 *        Baud, und die Maus waere beim Booten tot.
 *    Services\Kbdclass\Parameters\ConnectMultiplePorts = 1
 *    Services\Mouclass\Parameters\ConnectMultiplePorts = 1
 *        sonst liest USER nur den ersten Anschluss -- PS/2 oder USB,
 *        nicht beide.  Die alten Werte werden gesichert
 *        (HKLM\SOFTWARE\UsbNt\Sicherung) und von "aus" zurueckgeschrieben.
 *
 *  Es darf immer nur EINE Bauform eingerichtet sein; die anderen werden
 *  beim Einrichten entfernt.
 *
 *  Nur kernel32, advapi32 und -- fuer den Neustart -- user32.
 */

#include <windows.h>
#include "ntbasis.h"
#include "../ober/ntusbioctl.h"

#define DIENSTE_PFAD  "SYSTEM\\CurrentControlSet\\Services\\"
#define SICHERUNG     "SOFTWARE\\UsbNt\\Sicherung"

static const char *dienste[5] = { "usbnt", "uhcint", "ohcint", "ehcint", "xhcint" };
static const char *arten[5]   = { "usbnt", "uhci",   "ohci",   "ehci",   "xhci"   };
static const char *geraete[5] = { "\\\\.\\UsbNt", "\\\\.\\UhciNt", "\\\\.\\OhciNt",
                                  "\\\\.\\EhciNt", "\\\\.\\XhciNt" };

static char quellordner[MAX_PATH];
static char systemordner[MAX_PATH];


/* ==================================================================== */

static void
dran (char *z, const char *q)
{
    strcpy(z + strlen(z), q);
}

static void
fehlertext (const char *was, DWORD e)
{
    const char *t = "";

    switch (e) {
        case 2:    t = "Datei nicht gefunden"; break;
        case 5:    t = "Zugriff verweigert -- als Administrator angemeldet?  "
                       "Sonst: Datei schreibgeschuetzt (attrib -r) oder "
                       "von einem Programm geoeffnet"; break;
        case 32:   t = "Datei in Benutzung"; break;
        case 193:  t = "kein gueltiges Treiberabbild"; break;
        case 1056: t = "laeuft bereits"; break;
        case 1058: t = "Dienst abgeschaltet"; break;
        case 1060: t = "Dienst nicht eingerichtet"; break;
        case 1073: t = "Dienst gibt es schon"; break;
        case 1275: t = "Treiber wurde nicht geladen"; break;
        default:   break;
    }

    printf("  %s: Fehler %lu %s\n", was, e, t);
}

/*  Schreibschutz aufheben.
 *
 *  CopyFile uebertraegt die Dateiattribute -- auch den Schreibschutz.
 *  Liegt der Installer-Ordner auf CD oder kommt er schreibgeschuetzt aus
 *  einem Archiv, legt PROBE die Treiberdatei schreibgeschuetzt nach
 *  drivers, und das naechste EINRICHT scheitert beim Ueberschreiben mit
 *  Fehler 5 -- "Zugriff verweigert", obwohl als Administrator
 *  angemeldet.  So gemeldet vom ersten Einsatz auf echter Hardware.
 *
 *  Deshalb: am Ziel vor dem Kopieren aufheben, und an der frischen Kopie
 *  danach auch, damit er sich nicht weitertraegt.  */

static void
schreibbar (const char *pfad)
{
    DWORD a = GetFileAttributesA(pfad);

    if (a != 0xFFFFFFFFul && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesA(pfad, a & ~FILE_ATTRIBUTE_READONLY);
    }
}

static BOOL
kopieren (const char *quelle, const char *ziel)
{
    BOOL ok;

    schreibbar(ziel);
    ok = CopyFileA(quelle, ziel, FALSE);

    if (ok) {
        schreibbar(ziel);
    }

    return ok;
}

static int
art_index (const char *a)
{
    int i;

    for (i = 0; i < 5; i += 1) {
        if (strcmp(a, arten[i]) == 0 || strcmp(a, dienste[i]) == 0) {
            return i;
        }
    }

    return -1;
}


/* ==================================================================== *
 *  Registrierung
 * ==================================================================== */

static int
dword_lesen (const char *schluessel, const char *name, DWORD *wert)
{
    HKEY  k;
    DWORD typ = 0, n = sizeof(*wert);
    LONG  r;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, schluessel, 0, KEY_READ, &k) != 0) {
        return 0;
    }

    r = RegQueryValueExA(k, name, NULL, &typ, (BYTE *)wert, &n);
    RegCloseKey(k);
    return r == 0 && typ == REG_DWORD;
}

static int
dword_schreiben (const char *schluessel, const char *name, DWORD wert)
{
    HKEY  k;
    DWORD neu;
    LONG  r;

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, schluessel, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &k, &neu) != 0) {
        return 0;
    }

    r = RegSetValueExA(k, name, 0, REG_DWORD, (BYTE *)&wert, sizeof(wert));
    RegCloseKey(k);
    return r == 0;
}

static void
wert_loeschen (const char *schluessel, const char *name)
{
    HKEY k;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, schluessel, 0, KEY_ALL_ACCESS, &k) == 0) {
        RegDeleteValueA(k, name);
        RegCloseKey(k);
    }
}

/*  Einen kbdclass/mouclass-Wert sichern -- nur beim ersten Mal, damit ein
 *  zweites "ein" nicht unseren eigenen Wert als Original sichert.  Ein
 *  fehlender Wert wird als 0xFFFFFFFF vermerkt: "aus" loescht ihn dann,
 *  statt ihn auf irgendeine Zahl zu setzen.  */

static void
sichern_und_setzen (const char *klasse, const char *name, DWORD neu)
{
    char  schl[200], merk[80];
    DWORD alt, schon;

    strcpy(schl, DIENSTE_PFAD);
    dran(schl, klasse);
    dran(schl, "\\Parameters");

    strcpy(merk, klasse);
    dran(merk, "_");
    dran(merk, name);

    if (!dword_lesen(SICHERUNG, merk, &schon)) {
        if (!dword_lesen(schl, name, &alt)) {
            alt = 0xFFFFFFFFul;
        }
        dword_schreiben(SICHERUNG, merk, alt);
    }

    dword_schreiben(schl, name, neu);
}

static void
zurueckschreiben (const char *klasse, const char *name)
{
    char  schl[200], merk[80];
    DWORD alt;

    strcpy(schl, DIENSTE_PFAD);
    dran(schl, klasse);
    dran(schl, "\\Parameters");

    strcpy(merk, klasse);
    dran(merk, "_");
    dran(merk, name);

    if (!dword_lesen(SICHERUNG, merk, &alt)) {
        return;                         /* nie geaendert */
    }

    if (alt == 0xFFFFFFFFul) {
        wert_loeschen(schl, name);
    } else {
        dword_schreiben(schl, name, alt);
    }

    wert_loeschen(SICHERUNG, merk);
    printf("  %s\\%s zurueck auf %s\n", klasse, name,
           (alt == 0xFFFFFFFFul) ? "(nicht gesetzt)" : "den alten Wert");
}


/* ==================================================================== *
 *  Dienste
 * ==================================================================== */

static SC_HANDLE scm;

static int
dienst_zustand (const char *name, DWORD *start, int *laeuft)
{
    SC_HANDLE      s;
    SERVICE_STATUS st;
    char           puffer[1024];
    QUERY_SERVICE_CONFIGA *q = (QUERY_SERVICE_CONFIGA *)puffer;
    DWORD          n = 0;

    s = OpenServiceA(scm, name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);

    if (s == NULL) {
        return 0;
    }

    *start  = 0xFFFFFFFFul;
    *laeuft = 0;

    if (QueryServiceConfigA(s, q, sizeof(puffer), &n)) {
        *start = q->dwStartType;
    }

    if (QueryServiceStatus(s, &st)) {
        *laeuft = (st.dwCurrentState == SERVICE_RUNNING);
    }

    CloseServiceHandle(s);
    return 1;
}

static void
dienst_entfernen (const char *name, int leise)
{
    SC_HANDLE      s;
    SERVICE_STATUS st;

    s = OpenServiceA(scm, name, SERVICE_ALL_ACCESS);

    if (s == NULL) {
        return;
    }

    /*  Anhalten gelingt nur, wenn der Treiber von Hand gestartet wurde;
     *  ein Starttreiber bleibt bis zum Neustart geladen.  DeleteService
     *  merkt ihn dann nur zum Loeschen vor (DeleteFlag), und geloescht
     *  wird erst von der Dienstverwaltung beim naechsten Start -- die
     *  laeuft aber NACH dem Kern, und der laedt Systemtreiber vorher.
     *  Der Treiber kam so noch einmal hoch, obwohl "aus" versprochen
     *  hatte, er sei ab dem naechsten Start weg; im QEMU-Lauf am
     *  seriellen Bericht zu sehen.
     *
     *  Deshalb zuerst auf "Deaktiviert": Start = 4 liest der Kern, und der
     *  Treiber bleibt beim naechsten Start aus.  */

    /*  Anhalten NUR, wenn der Treiber von Hand gestartet war (Start 3,
     *  "Ausprobieren").  Ein Systemstart-Treiber haengt an Tastatur und
     *  Maus des Systems; NT 4.0 versucht trotzdem, ihn zu entladen (er
     *  hat DriverUnload).  Gemessen: USBINST blieb dabei haengen (Wechsel
     *  usbnt -> uhcint, alte usbnt.sys geladen), mit ihm der Agent und
     *  die Dienstverwaltung.  Entfernt wird ohnehin erst beim Neustart.  */

    {
        BYTE                    puffer[512];
        QUERY_SERVICE_CONFIGA  *qc = (QUERY_SERVICE_CONFIGA *)puffer;
        DWORD                   n  = 0;
        int                     von_hand = 0;

        if (QueryServiceConfigA(s, qc, sizeof(puffer), &n)) {
            von_hand = (qc->dwStartType == SERVICE_DEMAND_START);
        }

        ChangeServiceConfigA(s, SERVICE_NO_CHANGE, SERVICE_DISABLED,
                             SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL);

        if (von_hand) {
            ControlService(s, SERVICE_CONTROL_STOP, &st);
        }
    }

    if (!DeleteService(s) && GetLastError() != 1072) {
        if (!leise) {
            fehlertext("DeleteService", GetLastError());
        }
    } else if (!leise) {
        printf("  Dienst %s entfernt\n", name);
    }

    CloseServiceHandle(s);
}

static int
datei_kopieren (const char *name)
{
    char quelle[MAX_PATH], ziel[MAX_PATH], neu[MAX_PATH];

    strcpy(quelle, quellordner);
    dran(quelle, name);
    dran(quelle, ".sys");

    strcpy(ziel, systemordner);
    dran(ziel, "\\drivers\\");
    dran(ziel, name);
    dran(ziel, ".sys");

    if (GetFileAttributesA(quelle) == 0xFFFFFFFFul) {
        printf("  %s fehlt -- es muss neben usbinst.exe liegen.\n", quelle);
        return 0;
    }

    if (kopieren(quelle, ziel)) {
        printf("  %s -> %s\n", quelle, ziel);
        return 1;
    }

    /*  Ist der alte Treiber geladen, liegt seine Datei fest.  Dann die
     *  neue daneben legen und beim naechsten Start austauschen lassen.  */

    if (GetLastError() == 32) {

        strcpy(neu, ziel);
        dran(neu, ".neu");

        if (kopieren(quelle, neu) &&
            MoveFileExA(neu, ziel, MOVEFILE_REPLACE_EXISTING |
                                   MOVEFILE_DELAY_UNTIL_REBOOT)) {
            printf("  %s ist geladen -- die neue Fassung wird beim\n"
                   "  naechsten Start eingesetzt.\n", ziel);
            return 1;
        }
    }

    fehlertext("CopyFile", GetLastError());
    return 0;
}

static int
dienst_einrichten (int art, DWORD start)
{
    SC_HANDLE s;
    char      pfad[80], anzeige[64];
    const char *name = dienste[art];

    strcpy(pfad, "System32\\drivers\\");
    dran(pfad, name);
    dran(pfad, ".sys");

    /*  Anzeigename je Bauart verschieden.  Gleicher Name fuer alle hiess:
     *  Wechsel usbnt -> uhcint scheiterte mit Fehler 1078 (Anzeigename
     *  belegt), weil der alte Dienst bis zum Neustart nur zum Loeschen
     *  vorgemerkt ist und seinen Namen behaelt.  */

    strcpy(anzeige, "FreeBSD-USB fuer NT 4.0 (");
    dran(anzeige, name);
    dran(anzeige, ")");

    s = CreateServiceA(scm, name, anzeige,
                       SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, start,
                       SERVICE_ERROR_NORMAL, pfad, "Keyboard Port",
                       NULL, NULL, NULL, NULL);

    if (s == NULL && GetLastError() == 1073) {

        s = OpenServiceA(scm, name, SERVICE_ALL_ACCESS);

        if (s != NULL &&
            !ChangeServiceConfigA(s, SERVICE_KERNEL_DRIVER, start,
                                  SERVICE_ERROR_NORMAL, pfad, "Keyboard Port",
                                  NULL, NULL, NULL, NULL, anzeige)) {
            fehlertext("ChangeServiceConfig", GetLastError());
            CloseServiceHandle(s);
            return 0;
        }
    }

    if (s == NULL) {
        if (GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE) {

            /*  Nach "aus" ohne Neustart: der Treiber ist noch geladen, der
             *  Dienst bis zum Neustart nur zum Loeschen vorgemerkt.  */

            printf("  Der Dienst wurde mit \"aus\" entfernt und ist bis zum Neustart\n"
                   "  nur zum Loeschen vorgemerkt.  Erst neu starten, dann einrichten.\n");
        } else {
            fehlertext("CreateService", GetLastError());
        }
        return 0;
    }

    CloseServiceHandle(s);
    return 1;
}


/* ==================================================================== *
 *  Stoerenfriede
 * ==================================================================== */

static void
pruefe_umfeld (DWORD seriell)
{
    static const char *fremde[] = { "usbd", "usbhub", "openhci", "uhcd",
                                    "ionusb", "usbport", 0 };
    DWORD start;
    int   laeuft, k, gewarnt = 0;

    /*  Serielle Maus: an welchem Anschluss sie haengt, ist von aussen
     *  nicht sicher festzustellen.  Also warnen, sobald sie eingerichtet
     *  ist und der Bericht auf einen seriellen Anschluss gehen soll.  */

    if (seriell != 0 && dienst_zustand("Sermouse", &start, &laeuft) &&
        start != SERVICE_DISABLED) {
        printf("\n  ACHTUNG: eine serielle Maus (Sermouse) ist eingerichtet.\n"
               "  Der Bericht auf COM%lu stellt den Anschluss auf 115200 Baud;\n"
               "  haengt die Maus dort, ist sie nach dem Start tot.\n"
               "  Abhilfe: -seriell 0 (aus) oder den anderen Anschluss waehlen.\n",
               seriell);
    }

    for (k = 0; fremde[k] != 0; k += 1) {
        if (dienst_zustand(fremde[k], &start, &laeuft) &&
            start != SERVICE_DISABLED) {
            if (!gewarnt) {
                printf("\n  ACHTUNG: ein anderer USB-Stapel ist eingerichtet:\n");
                gewarnt = 1;
            }
            printf("    %-10s Start %lu%s\n", fremde[k], start,
                   laeuft ? ", laeuft" : "");
        }
    }

    if (gewarnt) {
        printf("  Zwei Stapel an denselben Bausteinen vertragen sich nicht.\n"
               "  Den anderen vorher abschalten (Systemsteuerung, Geraete:\n"
               "  Start 'Deaktiviert').\n");
    }
}


/* ==================================================================== *
 *  Pruefen: was sich mit dem USB-Treiber nicht vertraegt
 *
 *      usbinst pruefen              alles auflisten
 *      usbinst abschalten <fund>    einen Fund abschalten (alter Wert
 *                                   unter Sicherung), z. B.
 *                                   dienst:LMouFilt  run:hklm:MouseWare
 *      usbinst wieder               alles Abgeschaltete zurueck
 *
 *  Jeder Fund steht als Zeile  FUND|<fund>|<art>|<text>  in der Ausgabe
 *  (fuer USBSETUP), darunter erklaert fuer Menschen.  Arten:
 *    maus       fremder Maus-/Tastaturfilter (Logitech MouseWare,
 *               IntelliPoint ...) -- Logitech hielt USB-Klicks ~3 s fest
 *    autostart  deren Programm im Autostart (em_exec.exe, point32.exe ...)
 *    usb        ein anderer USB-Stapel -- zwei an denselben Bausteinen
 *               vertragen sich nicht
 *    ps2        i8042prt: ohne PS/2-Tastatur (BIOS-Emulation aus) schreibt
 *               er Meldungen 15/17/19/20/26; abschalten nur, wenn weder
 *               PS/2-Tastatur noch PS/2-Maus gebraucht wird
 * ==================================================================== */

/*  Weiter unten definiert.  */
static int  sz_lesen (const char *schl, const char *name, char *wert, DWORD n);
static void sz_schreiben (const char *schl, const char *name, const char *wert);
static int  ersetzen (const char *name);
static void wegraeumen (const char *name);

static int
enthaelt (const char *heu, const char *nadel)
{
    int i, j;
    for (i = 0; heu[i] != '\0'; i += 1) {
        for (j = 0; nadel[j] != '\0'; j += 1) {
            char a = heu[i + j], b = nadel[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != b) break;
        }
        if (nadel[j] == '\0') return 1;
    }
    return 0;
}

static int
eines_von (const char *s, const char *const *liste)
{
    int k;
    for (k = 0; liste[k] != 0; k += 1) {
        if (enthaelt(s, liste[k])) return 1;
    }
    return 0;
}

static int
gleich_ohne_gross (const char *a, const char *b)
{
    return lstrcmpiA(a, b) == 0;
}

static const char *const maus_muster[] = { "logi", "lmou", "lhid", "l8042", "mouseware",
                                           "msh_zwf", "mshzwf", "intellipoint", "ipoint",
                                           "point32", 0 };
static const char *const run_muster[]  = { "em_exec", "mousewar", "logitech", "lgmsgd",
                                           "point32", "ipoint", "intellipoint", 0 };
static const char *const eigene[]      = { "usbnt", "uhcint", "ohcint", "ehcint", "xhcint", 0 };
static const char *const standard_ein[] = { "i8042prt", "mouclass", "kbdclass", "sermouse",
                                            "inport", "busmouse", 0 };

static int
in_liste (const char *name, const char *const *liste)
{
    int k;
    for (k = 0; liste[k] != 0; k += 1) {
        if (gleich_ohne_gross(name, liste[k])) return 1;
    }
    return 0;
}

static int funde;

static void
fund (const char *kennung, const char *art, const char *text, const char *erklaerung)
{
    printf("FUND|%s|%s|%s\n", kennung, art, text);
    printf("    %s\n", erklaerung);
    funde += 1;
}

static void
run_pruefen (HKEY wurzel, const char *kurz)
{
    HKEY  k;
    DWORD i;
    if (RegOpenKeyExA(wurzel, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_READ, &k) != ERROR_SUCCESS) {
        return;
    }
    for (i = 0; ; i += 1) {
        char  name[128], wert[MAX_PATH];
        DWORD nl = sizeof(name), wl = sizeof(wert), typ;
        if (RegEnumValueA(k, i, name, &nl, NULL, &typ, (BYTE *)wert, &wl) != ERROR_SUCCESS) break;
        if (typ != REG_SZ && typ != REG_EXPAND_SZ) continue;
        if (eines_von(wert, run_muster) || eines_von(name, run_muster)) {
            char kenn[160], text[400];
            wsprintfA(kenn, "run:%s:%s", kurz, name);
            wsprintfA(text, "Autostart \"%s\": %s", name, wert);
            fund(kenn, "autostart", text,
                 "Maus-Hilfsprogramm; ohne seinen Filtertreiber ueberfluessig.");
        }
    }
    RegCloseKey(k);
}

static int
pruefen (void)
{
    HKEY  k;
    DWORD i;

    printf("\n  Pruefen: fremde Maus-/Tastatur- und USB-Treiber, PS/2.\n\n");
    funde = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services", 0,
                      KEY_READ, &k) != ERROR_SUCCESS) {
        printf("  Dienste nicht lesbar.\n");
        return 0;
    }
    for (i = 0; ; i += 1) {
        char  name[128], schl[200], gruppe[128] = "", bild[MAX_PATH] = "", anzeige[200] = "";
        DWORD nl = sizeof(name), start = 0;
        int   eingabe;

        if (RegEnumKeyExA(k, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        strcpy(schl, DIENSTE_PFAD); dran(schl, name);
        if (!dword_lesen(schl, "Start", &start) || start == SERVICE_DISABLED) continue;
        if (in_liste(name, eigene)) continue;
        sz_lesen(schl, "Group", gruppe, sizeof(gruppe));
        sz_lesen(schl, "ImagePath", bild, sizeof(bild));
        sz_lesen(schl, "DisplayName", anzeige, sizeof(anzeige));

        if (gleich_ohne_gross(name, "i8042prt")) {
            fund("dienst:i8042prt", "ps2", "i8042prt (PS/2-Tastatur und -Maus)",
                 "Ohne PS/2-Tastatur Meldungen 15/17/19/20/26 -- harmlos. NUR abschalten,\n"
                 "    wenn weder PS/2-Tastatur noch PS/2-Maus gebraucht wird.");
            continue;
        }
        eingabe = enthaelt(gruppe, "pointer") || enthaelt(gruppe, "keyboard");
        if ((eingabe && !in_liste(name, standard_ein)) ||
            eines_von(name, maus_muster) || eines_von(bild, maus_muster) ||
            eines_von(anzeige, maus_muster)) {
            char kenn[160], text[400];
            wsprintfA(kenn, "dienst:%s", name);
            wsprintfA(text, "Maus-/Tastaturtreiber %s%s%s (Start %lu)", name,
                      anzeige[0] ? " -- " : "", anzeige, start);
            fund(kenn, "maus", text,
                 "Fremder Eingabefilter.  Logitech MouseWare hielt Klicks der USB-Maus\n"
                 "    ~3 s fest.  Abschalten empfohlen.");
            continue;
        }
        if ((enthaelt(name, "usb") || enthaelt(bild, "usb") || enthaelt(name, "hci") ||
             enthaelt(name, "ionusb")) && !enthaelt(name, "usbnt")) {
            char kenn[160], text[400];
            wsprintfA(kenn, "dienst:%s", name);
            wsprintfA(text, "USB-Treiber %s%s%s (Start %lu)", name,
                      anzeige[0] ? " -- " : "", anzeige, start);
            fund(kenn, "usb", text,
                 "Ein anderer USB-Stapel an denselben Bausteinen vertraegt sich nicht.\n"
                 "    Abschalten empfohlen.");
        }
    }
    RegCloseKey(k);
    run_pruefen(HKEY_LOCAL_MACHINE, "hklm");
    run_pruefen(HKEY_CURRENT_USER, "hkcu");

    printf("\n  %d Fund(e).%s\n\n", funde,
           funde ? "  Abschalten: usbinst abschalten <fund>; zurueck: usbinst wieder" : "");
    return 1;
}

static int
abschalten (const char *kenn)
{
    char schl[200], sich[200];

    if (strncmp(kenn, "dienst:", 7) == 0) {
        DWORD start, alt;
        strcpy(schl, DIENSTE_PFAD); dran(schl, kenn + 7);
        if (!dword_lesen(schl, "Start", &start)) {
            printf("  %s: kein solcher Dienst\n", kenn + 7);
            return 0;
        }
        strcpy(sich, "Dienst_"); dran(sich, kenn + 7);
        if (!dword_lesen(SICHERUNG, sich, &alt)) {
            dword_schreiben(SICHERUNG, sich, start);
        }
        dword_schreiben(schl, "Start", SERVICE_DISABLED);
        printf("  %s abgeschaltet (war Start %lu; gesichert).  Wirkt ab dem naechsten Start.\n",
               kenn + 7, start);
        return 1;
    }
    if (strncmp(kenn, "run:", 4) == 0) {
        HKEY  wurzel = (strncmp(kenn + 4, "hkcu:", 5) == 0) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        const char *name = kenn + 9;
        char  wert[MAX_PATH];
        HKEY  k;
        DWORD n = sizeof(wert), typ;
        if (RegOpenKeyExA(wurzel, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                          KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) {
            return 0;
        }
        if (RegQueryValueExA(k, name, NULL, &typ, (BYTE *)wert, &n) == ERROR_SUCCESS) {
            wsprintfA(sich, "Run_%.4s_%s", kenn + 4, name);
            sz_schreiben(SICHERUNG, sich, wert);
            RegDeleteValueA(k, name);
            printf("  Autostart \"%s\" entfernt (gesichert).\n", name);
        }
        RegCloseKey(k);
        return 1;
    }
    printf("  unbekannter Fund: %s\n", kenn);
    return 0;
}

static int
wieder (void)
{
    HKEY  k;
    DWORD i;
    char  namen[24][96];                /* unter 4 KB: ohne CRT keine Stapelpruefung */
    int   n = 0, j;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, SICHERUNG, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        return 1;
    }
    for (i = 0; n < 24; i += 1) {
        DWORD nl = sizeof(namen[0]);
        if (RegEnumValueA(k, i, namen[n], &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (strncmp(namen[n], "Dienst_", 7) == 0 || strncmp(namen[n], "Run_", 4) == 0) n += 1;
    }
    RegCloseKey(k);

    for (j = 0; j < n; j += 1) {
        char schl[200];
        if (strncmp(namen[j], "Dienst_", 7) == 0) {
            DWORD start;
            if (dword_lesen(SICHERUNG, namen[j], &start)) {
                strcpy(schl, DIENSTE_PFAD); dran(schl, namen[j] + 7);
                dword_schreiben(schl, "Start", start);
                printf("  %s wieder eingeschaltet (Start %lu)\n", namen[j] + 7, start);
            }
        } else {
            char  wert[MAX_PATH];
            HKEY  wurzel = (strncmp(namen[j] + 4, "hkcu_", 5) == 0) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
            if (sz_lesen(SICHERUNG, namen[j], wert, sizeof(wert))) {
                HKEY r;
                if (RegCreateKeyExA(wurzel, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                                    NULL, 0, KEY_WRITE, NULL, &r, NULL) == ERROR_SUCCESS) {
                    RegSetValueExA(r, namen[j] + 9, 0, REG_SZ, (const BYTE *)wert,
                                   (DWORD)strlen(wert) + 1);
                    RegCloseKey(r);
                    printf("  Autostart \"%s\" wieder eingetragen\n", namen[j] + 9);
                }
            }
        }
        wert_loeschen(SICHERUNG, namen[j]);
    }
    return 1;
}

/* ==================================================================== *
 *  Zusaetze: Reiter "USB-Maustasten" und "Sondertasten", USBTASTEN.EXE
 *  im Autostart, SONDERSET.EXE.
 * ==================================================================== */

static void
dll_melden (const char *name, int an)
{
    char    z[MAX_PATH];
    HMODULE m;
    strcpy(z, systemordner); dran(z, "\\"); dran(z, name);
    m = LoadLibraryA(z);
    if (m != NULL) {
        typedef LONG (WINAPI *RF)(void);
        RF f = (RF)GetProcAddress(m, an ? "DllRegisterServer" : "DllUnregisterServer");
        if (f != 0 && f() == 0) {
            printf("  %s %s\n", name, an ? "angemeldet" : "abgemeldet");
        }
        FreeLibrary(m);
    }
}

static void
zusatz_ein (void)
{
    HKEY k;
    if (ersetzen("usbmausx.dll")) {
        dll_melden("usbmausx.dll", 1);
        printf("  Systemsteuerung -> Maus: Reiter \"USB-Maustasten\"\n");
    }
    if (ersetzen("usbtastx.dll")) {
        dll_melden("usbtastx.dll", 1);
        printf("  Systemsteuerung -> Tastatur: Reiter \"Sondertasten\"\n");
        printf("    (Befehlszeile/Skript: rundll32 usbtastx.dll,Sondertaste ...)\n");
    }
    if (ersetzen("devmgr.cpl")) {
        printf("  Systemsteuerung -> Geraetemanager (PCI- und USB-Geraete)\n");
    }
    ersetzen("devmgr.exe");
    /*  PCISCAN: kleiner Treiber, mit dem der Geraetemanager die PCI-Geraete
        liest.  Frueher legte der Geraetemanager ihn bei jedem Start selbst ab
        und startete ihn -- das ist ein Dropper-Muster, das Virenscanner an
        der CPL anschlugen.  Jetzt liegt PCISCAN.SYS als Datei bei und der
        Dienst wird hier EINMALIG eingerichtet.  */
    if (datei_kopieren("pciscan")) {
        SC_HANDLE psm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        SC_HANDLE s;
        if (psm != NULL) {
            s = CreateServiceA(psm, "PciScan", "PciScan (Geraetemanager)",
                        SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
                        SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                        "System32\\drivers\\pciscan.sys",
                        NULL, NULL, NULL, NULL, NULL);
            if (s == NULL && GetLastError() == 1073 /* schon da */) {
                s = OpenServiceA(psm, "PciScan", SERVICE_ALL_ACCESS);
            }
            if (s != NULL) {
                StartServiceA(s, 0, NULL);   /* Demand-Start; laeuft evtl. schon */
                CloseServiceHandle(s);
                printf("  PCISCAN: PCI-Lesedienst fuer den Geraetemanager eingerichtet\n");
            }
            CloseServiceHandle(psm);
        }
    }
    /*  HTTPS fuer "PCI.IDS..." im Geraetemanager: PCIHOLE (wolfSSL, TLS 1.2)
        und die Zertifikatsliste.  Beide gehoeren zusammen.  */
    if (ersetzen("pcihole.exe") && ersetzen("cacert.pem")) {
        printf("  PCIHOLE.EXE + CACERT.PEM: PCI.IDS auch ueber https://\n");
    }
    /*  PCI.IDS (Geraetenamen) nur, wenn noch keine da ist -- eine im
        Geraetemanager aktualisierte Datei nicht durch den aelteren Stand
        aus dem Paket ersetzen.  */
    {
        char z[MAX_PATH];
        strcpy(z, systemordner); dran(z, "\\pci.ids");
        if (GetFileAttributesA(z) == 0xFFFFFFFFul) {
            if (ersetzen("pci.ids")) {
                printf("  PCI.IDS (Geraetenamen) nach system32\n");
            }
        } else {
            printf("  PCI.IDS schon vorhanden -- bleibt (Geraetemanager: \"PCI.IDS...\" aktualisiert)\n");
        }
    }
    if (ersetzen("usbtasten.exe")) {
        sz_schreiben("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", "USBTASTEN", "usbtasten.exe");
        printf("  USBTASTEN.EXE im Autostart (Lautstaerke, Programme per Sondertaste)\n");
    }
}

static void
zusatz_aus (void)
{
    HKEY k;
    dll_melden("usbmausx.dll", 0);
    dll_melden("usbtastx.dll", 0);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValueA(k, "USBTASTEN");
        RegCloseKey(k);
    }
    wegraeumen("usbmausx.dll");
    wegraeumen("usbtastx.dll");
    wegraeumen("usbtasten.exe");
    wegraeumen("devmgr.cpl");
    wegraeumen("devmgr.exe");
    {
        SC_HANDLE psm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        SC_HANDLE s = (psm != NULL) ? OpenServiceA(psm, "PciScan", SERVICE_ALL_ACCESS) : NULL;
        if (s != NULL) {
            SERVICE_STATUS ss;
            ControlService(s, SERVICE_CONTROL_STOP, &ss);
            DeleteService(s);
            CloseServiceHandle(s);
        }
        if (psm != NULL) CloseServiceHandle(psm);
    }
    { char z[MAX_PATH]; strcpy(z,systemordner); dran(z,"\\drivers\\pciscan.sys"); DeleteFileA(z); }
    wegraeumen("pcihole.exe");
    wegraeumen("cacert.pem");
}


/* ==================================================================== *
 *  Befehle
 * ==================================================================== */

static void
zustand (void)
{
    DWORD start, wert;
    int   laeuft, i, eingerichtet = 0;

    static const char *startname[] = { "Boot", "System", "Automatisch",
                                       "Manuell", "Deaktiviert" };

    printf("\n  Bauform   Dienst   Start          laeuft  Bericht\n");
    printf("  ---------------------------------------------------\n");

    for (i = 0; i < 5; i += 1) {

        char schl[160];

        if (!dienst_zustand(dienste[i], &start, &laeuft)) {
            continue;
        }

        eingerichtet += 1;
        strcpy(schl, DIENSTE_PFAD);
        dran(schl, dienste[i]);
        dran(schl, "\\Parameters");

        if (!dword_lesen(schl, "Ausgabe", &wert)) {
            wert = 1;
        }

        printf("  %-8s  %-7s  %-13s  %-6s  %s\n", arten[i], dienste[i],
               (start <= 4) ? startname[start] : "?", laeuft ? "ja" : "nein",
               (wert == 0) ? "aus" : (wert == 1) ? "COM1" :
               (wert == 2) ? "COM2" : "Port");
    }

    if (eingerichtet == 0) {
        printf("  keiner eingerichtet\n");
    }

    if (dword_lesen(DIENSTE_PFAD "Kbdclass\\Parameters", "ConnectMultiplePorts",
                    &wert)) {
        printf("\n  Kbdclass ConnectMultiplePorts = %lu%s\n", wert,
               wert ? "" : "   (USB-Tastatur wird so nicht gelesen)");
    }

    if (dword_lesen(DIENSTE_PFAD "Mouclass\\Parameters", "ConnectMultiplePorts",
                    &wert)) {
        printf("  Mouclass ConnectMultiplePorts = %lu\n", wert);
    }

    pruefe_umfeld(1);
    printf("\n");
}

static int
probe (int art, DWORD seriell)
{
    SC_HANDLE s;
    char      schl[160];
    HANDLE    h;
    int       i;

    printf("\n  Probe: %s -- einmal von Hand starten, nichts fest eintragen.\n\n",
           dienste[art]);

    for (i = 0; i < 5; i += 1) {
        if (i != art) {
            dienst_entfernen(dienste[i], 1);
        }
    }

    if (!datei_kopieren(dienste[art]) ||
        !dienst_einrichten(art, SERVICE_DEMAND_START)) {
        return 0;
    }

    strcpy(schl, DIENSTE_PFAD);
    dran(schl, dienste[art]);
    dran(schl, "\\Parameters");
    dword_schreiben(schl, "Ausgabe", seriell);

    s = OpenServiceA(scm, dienste[art], SERVICE_ALL_ACCESS);

    if (s == NULL || !StartServiceA(s, 0, NULL)) {
        DWORD e = GetLastError();
        if (e != 1056) {
            fehlertext("StartService", e);
            printf("\n  Der Treiber laedt nicht.  Mit \"usbinst aus\" entfernen;\n"
                   "  der serielle Bericht sagt, woran es lag.\n\n");
            if (s != NULL) CloseServiceHandle(s);
            return 0;
        }
    }

    if (s != NULL) {
        CloseServiceHandle(s);
    }

    h = CreateFileA(geraete[art], GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (h == INVALID_HANDLE_VALUE) {
        printf("  Geladen, aber %s antwortet nicht.\n", geraete[art]);
        return 0;
    }

    CloseHandle(h);

    printf("\n  Der Treiber laeuft.  Bausteine und Geraete zeigen:\n"
           "      usbtest bausteine      rundll32 usbgeraet.dll,Geraete\n\n"
           "  Tastatur und Maus gehen bei dieser Probe noch NICHT -- kbdclass\n"
           "  zaehlt seine Anschluesse nur beim Booten.  Dafuer:  usbinst ein\n\n");
    return 1;
}

/* ==================================================================== *
 *  Registrierungshilfen.  Der Joystick-Teil (usbjoy.dll, Typen,
 *  Einstellungen) ist ins eigene Paket XBOXINST gewandert -- mit dem
 *  Kerntreiber USBXBOX.SYS, der nicht mehr in usbnt.sys steckt.
 * ==================================================================== */

static int
sz_lesen (const char *schl, const char *name, char *wert, DWORD n)
{
    HKEY  k;
    DWORD typ;
    int   ok = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, schl, 0, KEY_READ, &k) == 0) {
        ok = RegQueryValueExA(k, name, NULL, &typ, (BYTE *)wert, &n) == 0 &&
             typ == REG_SZ;
        RegCloseKey(k);
    }
    return ok;
}

static void
sz_schreiben (const char *schl, const char *name, const char *wert)
{
    HKEY k;

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, schl, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, NULL) == 0) {
        RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)wert, (DWORD)strlen(wert) + 1);
        RegCloseKey(k);
    }
}



/*  Nach system32 legen; ist die alte Fassung gerade geladen (die
 *  Systemsteuerung haelt usbjoy.cpl, ein Spiel usbjoy.dll), die neue
 *  daneben legen und beim naechsten Start austauschen -- wie beim
 *  Treiber.  */

static int
ersetzen (const char *name)
{
    char q[MAX_PATH], z[MAX_PATH], neu[MAX_PATH];

    strcpy(q, quellordner);  dran(q, name);
    strcpy(z, systemordner); dran(z, "\\"); dran(z, name);

    if (GetFileAttributesA(q) == 0xFFFFFFFFul) {
        return 0;
    }

    if (kopieren(q, z)) {
        printf("  %s -> %s\n", name, z);
        return 1;
    }

    if (GetLastError() == 32) {
        strcpy(neu, z);
        dran(neu, ".neu");
        if (kopieren(q, neu) &&
            MoveFileExA(neu, z, MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
            printf("  %s ist in Benutzung -- die neue Fassung wird beim\n"
                   "  naechsten Start eingesetzt.\n", name);
            return 1;
        }
    }

    fehlertext(name, GetLastError());
    return 0;
}

/*  Entfernen; ist die Datei belegt, beim naechsten Start.  */

static void
wegraeumen (const char *name)
{
    char z[MAX_PATH];

    strcpy(z, systemordner); dran(z, "\\"); dran(z, name);

    if (GetFileAttributesA(z) == 0xFFFFFFFFul) {
        return;
    }
    schreibbar(z);
    if (DeleteFileA(z)) {
        printf("  %s entfernt\n", name);
    } else if (MoveFileExA(z, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        printf("  %s ist in Benutzung -- wird beim naechsten Start entfernt\n", name);
    }
}



/* ==================================================================== *
 *  Lokale Anschluesse \\.\USBPRN1..4 fuer den Spooler
 *
 *  Der Monitor "Lokaler Anschluss" nimmt nur Anschluesse, die unter Ports
 *  stehen (REG_SZ, leer), und liest sie beim Start des Spoolers.
 *
 *  Zustaendigkeit (abgestimmt mit CUPS4NT): angelegt wird nur, was fehlt;
 *  was USBINST selbst angelegt hat, steht unter Sicherung\Anschluesse
 *  ("1234").  "aus" entfernt nur diese, und nur solche, die kein Drucker
 *  benutzt -- benutzte bleiben stehen und bleiben vorgemerkt.
 * ==================================================================== */

#define PORTS    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Ports"
#define DRUCKER  "SYSTEM\\CurrentControlSet\\Control\\Print\\Printers"

static void
usbprn_name (char *z, int n)
{
    strcpy(z, "\\\\.\\USBPRN0");
    z[10] = (char)('0' + n);
}

/*  Benutzt ein Drucker diesen Anschluss?  Port kann eine Liste sein
 *  ("LPT1:,\\.\USBPRN1").  Liefert den Druckernamen oder 0.  */

static const char *
anschluss_benutzt (const char *anschluss, char *drucker, DWORD n)
{
    HKEY  k, d;
    DWORD i;
    char  port[512];

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, DRUCKER, 0, KEY_READ, &k) != 0) {
        return 0;
    }
    for (i = 0; ; i += 1) {
        DWORD ln = n, lp = sizeof(port), typ;
        char *a;
        if (RegEnumKeyExA(k, i, drucker, &ln, NULL, NULL, NULL, NULL) != 0) {
            break;
        }
        if (RegOpenKeyExA(k, drucker, 0, KEY_READ, &d) != 0) {
            continue;
        }
        if (RegQueryValueExA(d, "Port", NULL, &typ, (BYTE *)port, &lp) == 0 && typ == REG_SZ) {
            for (a = port; *a != '\0'; ) {
                char *e = a;
                while (*e != '\0' && *e != ',') e += 1;
                if ((size_t)(e - a) == strlen(anschluss) &&
                    CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, a, (int)(e - a),
                                   anschluss, (int)(e - a)) == 2) {
                    RegCloseKey(d); RegCloseKey(k);
                    return drucker;
                }
                a = (*e == ',') ? e + 1 : e;
            }
        }
        RegCloseKey(d);
    }
    RegCloseKey(k);
    return 0;
}

/*  Spooler neu starten, damit er die Anschluesse liest.  Laeuft er
 *  nicht, bleibt er aus.
 *
 *  Nicht sofort nach "angehalten" wieder starten: im Test endete der
 *  sofort neu gestartete Spooler mit Fehler 12 (Ereignis 7023), obwohl
 *  StartService Erfolg meldete -- StartService bestaetigt nur den Beginn.
 *  Deshalb: 2 s warten, dann pruefen, ob er LAEUFT und laufen BLEIBT;
 *  sonst bis zu zweimal erneut.  Gemeldet wird das Ergebnis.  */

static int
spooler_warte (SC_HANDLE s, DWORD ziel, int halbsek)
{
    SERVICE_STATUS st;
    int            t;

    for (t = 0; t < halbsek; t += 1) {
        if (!QueryServiceStatus(s, &st)) return 0;
        if (st.dwCurrentState == ziel) return 1;
        if (ziel == SERVICE_RUNNING && st.dwCurrentState == SERVICE_STOPPED) return 0;
        Sleep(500);
    }
    return 0;
}

static void
spooler_neu (void)
{
    SC_HANDLE      m, s;
    SERVICE_STATUS st;
    int            versuch, laeuft = 0;

    m = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (m == NULL) return;
    s = OpenServiceA(m, "Spooler", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (s == NULL) { CloseServiceHandle(m); return; }

    if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_RUNNING) {

        ControlService(s, SERVICE_CONTROL_STOP, &st);
        spooler_warte(s, SERVICE_STOPPED, 60);

        for (versuch = 0; versuch < 3 && !laeuft; versuch += 1) {
            Sleep(2000);
            StartServiceA(s, 0, NULL);
            if (spooler_warte(s, SERVICE_RUNNING, 30)) {
                Sleep(2000);                    /* bleibt er? */
                laeuft = QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_RUNNING;
            }
        }
        if (laeuft) {
            printf("  Spooler neu gestartet (liest die Anschluesse)%s\n",
                   versuch > 1 ? " -- im zweiten Anlauf" : "");
        } else {
            printf("  Spooler startet nicht wieder -- bitte von Hand: net start spooler\n");
        }
    }
    CloseServiceHandle(s);
    CloseServiceHandle(m);
}

static void
anschluesse_ein (void)
{
    char  merk[16] = "", name[20];
    HKEY  k;
    int   n, neu = 0;
    DWORD typ, l;

    sz_lesen(SICHERUNG, "Anschluesse", merk, sizeof(merk));

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, PORTS, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, NULL) != 0) {
        return;
    }
    for (n = 1; n <= 4; n += 1) {
        usbprn_name(name, n);
        l = 0;
        if (RegQueryValueExA(k, name, NULL, &typ, NULL, &l) == 0) {
            continue;                   /* gibt es schon -- nicht unseres */
        }
        if (RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)"", 1) == 0) {
            size_t e = strlen(merk);
            if (e + 1 < sizeof(merk)) { merk[e] = (char)('0' + n); merk[e + 1] = '\0'; }
            neu += 1;
        }
    }
    RegCloseKey(k);

    if (neu) {
        sz_schreiben(SICHERUNG, "Anschluesse", merk);
        printf("  Druckeranschluesse \\\\.\\USBPRN1..4 fuer den Spooler angelegt (%d neu)\n", neu);
        spooler_neu();
    }
}

static void
anschluesse_aus (void)
{
    char        merk[16] = "", rest[16] = "", name[20], drucker[128];
    const char *wer;
    HKEY        k;
    int         i, weg = 0;

    if (!sz_lesen(SICHERUNG, "Anschluesse", merk, sizeof(merk)) ||
        RegOpenKeyExA(HKEY_LOCAL_MACHINE, PORTS, 0, KEY_ALL_ACCESS, &k) != 0) {
        return;
    }
    for (i = 0; merk[i] != '\0'; i += 1) {
        usbprn_name(name, merk[i] - '0');
        wer = anschluss_benutzt(name, drucker, sizeof(drucker));
        if (wer != 0) {
            size_t e = strlen(rest);
            rest[e] = merk[i]; rest[e + 1] = '\0';
            printf("  %s bleibt -- Drucker \"%s\" benutzt ihn\n", name, wer);
        } else if (RegDeleteValueA(k, name) == 0) {
            weg += 1;
        }
    }
    RegCloseKey(k);

    if (rest[0] != '\0') sz_schreiben(SICHERUNG, "Anschluesse", rest);
    else                  wert_loeschen(SICHERUNG, "Anschluesse");

    if (weg) {
        printf("  %d Druckeranschluss/-anschluesse entfernt\n", weg);
        spooler_neu();
    }
}

/*  Gamepads sind ein eigenes Paket (XBOXINST, Kerntreiber USBXBOX.SYS).
 *  Hier nur Hinweise, damit niemand ratlos vor einem stummen Controller
 *  sitzt.  */

static void
gamepad_hinweis (int beim_entfernen)
{
    char  wert[MAX_PATH];
    DWORD start;
    int   xbox = dword_lesen("SYSTEM\\CurrentControlSet\\Services\\usbxbox", "Start", &start)
                 && start != 4;

    if (beim_entfernen) {
        if (xbox) {
            printf("  Hinweis: das Gamepad-Paket (usbxbox) ist noch eingerichtet.  Ohne\n"
                   "  USB-Paket laedt es nicht (stoert nicht).  Entfernen: xboxinst aus\n");
        }
        return;
    }
    if (!xbox && sz_lesen("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
                          "joystick", wert, sizeof(wert)) &&
        lstrcmpiA(wert, "usbjoy.dll") == 0) {
        printf("  Gamepads: der Joystick-Teil steckt nicht mehr in usbnt.sys.  Fuer\n"
               "  Xbox-Controller und Gamepads das Paket XBOXINST einrichten:\n"
               "      xboxinst ein   (nach dem Neustart)\n");
    }
}

static int
ein (int art, DWORD seriell)
{
    char schl[160];
    int  i;

    printf("\n  Einrichten: %s, Start mit dem System, Gruppe \"Keyboard Port\".\n\n",
           dienste[art]);

    for (i = 0; i < 5; i += 1) {
        if (i != art) {
            dienst_entfernen(dienste[i], 1);
        }
    }

    if (!datei_kopieren(dienste[art]) ||
        !dienst_einrichten(art, SERVICE_SYSTEM_START)) {
        return 0;
    }

    strcpy(schl, DIENSTE_PFAD);
    dran(schl, dienste[art]);
    dran(schl, "\\Parameters");
    dword_schreiben(schl, "Ausgabe", seriell);
    printf("  Bericht: %s\n", (seriell == 0) ? "aus" :
                              (seriell == 1) ? "COM1" : "COM2");

    sichern_und_setzen("Kbdclass", "ConnectMultiplePorts", 1);
    sichern_und_setzen("Mouclass", "ConnectMultiplePorts", 1);
    printf("  Kbdclass/Mouclass: ConnectMultiplePorts = 1 (alte Werte gesichert)\n");

    /*  Die Werkzeuge gleich mit, wenn sie daneben liegen.  */

    {
        static const char *w[] = { "usbtest.exe", "usbgeraet.dll", "xhcitest.exe",
                                   "usbdruck.exe", 0 };
        int k;

        for (k = 0; w[k] != 0; k += 1) {
            char q[MAX_PATH], z[MAX_PATH];
            strcpy(q, quellordner);  dran(q, w[k]);
            strcpy(z, systemordner); dran(z, "\\"); dran(z, w[k]);
            if (GetFileAttributesA(q) == 0xFFFFFFFFul) {
                continue;               /* liegt nicht daneben */
            }

            if (kopieren(q, z)) {
                printf("  %s -> %s\n", w[k], z);
            } else {

                /*  Die erste Fassung schwieg hier -- dann blieb eine alte
                 *  Fassung des Werkzeugs liegen, ohne dass es jemand
                 *  merkte.  */

                fehlertext(w[k], GetLastError());
            }
        }
    }

    zusatz_ein();
    if (ersetzen("usbmaus.cpl")) {
        printf("  Maustasten 1..8 erkennen und belegen: Systemsteuerung -> USB-Maus\n");
    }
    anschluesse_ein();

    pruefe_umfeld(seriell);
    gamepad_hinweis(0);

    printf("\n  Fertig.  Der Treiber laedt ab dem naechsten Start.\n"
           "  Geht beim Booten etwas schief: beim Start die LEERTASTE\n"
           "  druecken und \"Letzte als funktionierend bekannte\n"
           "  Konfiguration\" waehlen -- dann ist alles wie vorher.\n\n");
    return 1;
}

static int
aus (void)
{
    int i;

    printf("\n  Entfernen.\n\n");

    for (i = 0; i < 5; i += 1) {
        dienst_entfernen(dienste[i], 0);
    }

    zurueckschreiben("Kbdclass", "ConnectMultiplePorts");
    zurueckschreiben("Mouclass", "ConnectMultiplePorts");
    zusatz_aus();
    wieder();                           /* Abgeschaltetes (usbinst abschalten) zurueck */
    wegraeumen("usbmaus.cpl");
    anschluesse_aus();
    gamepad_hinweis(1);

    printf("\n  Die Treiberdatei bleibt in system32\\drivers liegen; ohne\n"
           "  Dienst wird sie nicht geladen.  Ab dem naechsten Start ist\n"
           "  der Treiber weg.\n\n");
    return 1;
}

static void
neustart (void)
{
    HANDLE           t;
    TOKEN_PRIVILEGES p;

    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) {
        LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &p.Privileges[0].Luid);
        p.PrivilegeCount = 1;
        p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(t, FALSE, &p, 0, NULL, NULL);
        CloseHandle(t);
    }

    printf("  Neustart ...\n");

    if (!ExitWindowsEx(EWX_REBOOT, 0)) {
        fehlertext("ExitWindowsEx", GetLastError());
    }
}


/* ==================================================================== *
 *  Maustasten 4..8 belegen
 *
 *      usbinst taste               Belegung zeigen
 *      usbinst taste 4 alt+links   Taste 4 belegen
 *      usbinst taste 4 vorgabe     zurueck auf die Vorgabe
 *
 *  Die Namen muessen zu ober/ntmaustasten.c passen -- hier nur geprueft,
 *  damit ein Tippfehler sofort auffaellt und nicht erst beim Start.
 * ==================================================================== */

static const char *tastennamen[] = {
    "strg", "ctrl", "alt", "altgr", "umschalt", "shift", "win", "menue",
    "links", "rechts", "hoch", "runter", "bildauf", "bildab", "pos1", "ende",
    "einfg", "entf", "enter", "esc", "tab", "rueck", "leer",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    0
};

static int
wort_gueltig (const char *w, int n)
{
    int k;

    if (n == 1 && ((w[0] >= 'a' && w[0] <= 'z') || (w[0] >= '0' && w[0] <= '9'))) {
        return 1;
    }
    if (n >= 7 && n <= 9 && strncmp(w, "code:", 5) == 0) {
        return 1;
    }
    for (k = 0; tastennamen[k] != 0; k += 1) {
        if ((int)strlen(tastennamen[k]) == n && strncmp(tastennamen[k], w, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
belegung_gueltig (const char *s)
{
    const char *a = s;

    if (strcmp(s, "aus") == 0 || strcmp(s, "mitte") == 0 ||
        strcmp(s, "doppelklick") == 0 ||
        strcmp(s, "klick-links") == 0 || strcmp(s, "klick-rechts") == 0 ||
        strcmp(s, "linke-taste") == 0 || strcmp(s, "rechte-taste") == 0) {
        return 1;
    }

    for (;;) {
        const char *e = strchr(a, '+');
        int         n = e ? (int)(e - a) : (int)strlen(a);

        if (n == 0 || !wort_gueltig(a, n)) {
            char w[40];
            int  i;

            /*  Das Wort kopieren -- ntbasis' printf kennt kein %.*s.  */

            for (i = 0; i < n && i < (int)sizeof(w) - 1; i += 1) {
                w[i] = a[i];
            }
            w[i] = '\0';
            printf("  \"%s\" kenne ich nicht.  usbinst taste  zeigt die Namen.\n", w);
            return 0;
        }
        if (e == 0) {
            return 1;
        }
        a = e + 1;
    }
}

static int
taste_befehl (int argc, char **argv)
{
    static const char *vorgabe[8] = { "klick-links", "klick-rechts", "mitte", "alt+links",
                                      "alt+rechts", "aus", "aus", "aus" };
    char   schl[160], name[8], wert[80], klein[80];
    HKEY   k;
    DWORD  start, typ, n;
    int    laeuft, i, nr;
    const char *dienst = 0;

    for (i = 0; i < 5 && dienst == 0; i += 1) {
        if (dienst_zustand(dienste[i], &start, &laeuft)) {
            dienst = dienste[i];
        }
    }

    if (dienst == 0) {
        printf("  Kein Treiber eingerichtet -- erst  usbinst ein.\n\n");
        return 0;
    }

    strcpy(schl, DIENSTE_PFAD);
    dran(schl, dienst);
    dran(schl, "\\Parameters");

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, schl, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, NULL) != 0) {
        fehlertext("RegCreateKeyEx", GetLastError());
        return 0;
    }

    if (argc == 2) {

        printf("\n  Maustasten 1..8 (%s):\n\n", dienst);

        for (nr = 1; nr <= 8; nr += 1) {
            strcpy(name, "Taste");
            name[5] = (char)('0' + nr);
            name[6] = '\0';
            n = sizeof(wert);
            if (RegQueryValueExA(k, name, NULL, &typ, (BYTE *)wert, &n) == 0 &&
                typ == REG_SZ && wert[0] != '\0') {
                printf("    Taste %d  %s\n", nr, wert);
            } else {
                printf("    Taste %d  %s  (Vorgabe)\n", nr, vorgabe[nr - 1]);
            }
        }

        printf("\n  Aendern:  usbinst taste 4 strg+c      Namen: alt strg umschalt\n"
               "  win links rechts hoch runter bildauf bildab pos1 ende enter esc\n"
               "  tab entf rueck f1..f12 a..z 0..9 code:xx  mitte doppelklick aus\n\n");
        RegCloseKey(k);
        return 1;
    }

    nr = atoi(argv[2]);

    if (nr < 1 || nr > 8 || argc < 4) {
        printf("  usbinst taste <1..8> <belegung>\n\n");
        RegCloseKey(k);
        return 0;
    }

    strcpy(name, "Taste");
    name[5] = (char)('0' + nr);
    name[6] = '\0';

    for (i = 0; argv[3][i] != '\0' && i < (int)sizeof(klein) - 1; i += 1) {
        char c = argv[3][i];
        klein[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    klein[i] = '\0';

    if (strcmp(klein, "vorgabe") == 0) {
        RegDeleteValueA(k, name);
        printf("  Taste %d: Vorgabe (%s)\n", nr, vorgabe[nr - 1]);
    } else if (!belegung_gueltig(klein)) {
        RegCloseKey(k);
        return 0;
    } else {
        RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)klein,
                       (DWORD)strlen(klein) + 1);
        printf("  Taste %d = %s\n", nr, klein);
    }

    RegCloseKey(k);

    /*  Laeuft der Treiber, gleich dort setzen (wie die Systemsteuerung
     *  "USB-Maus") -- dann wirkt es sofort.  */

    {
        NTU_MAUS_BELEGUNG b;
        HANDLE            h = INVALID_HANDLE_VALUE;
        DWORD             got;
        int               j, sofort = 0;

        for (j = 0; j < 5 && h == INVALID_HANDLE_VALUE; j += 1) {
            h = CreateFileA(geraete[j], GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        }
        if (h != INVALID_HANDLE_VALUE) {
            memset(&b, 0, sizeof(b));
            b.Taste = (unsigned long)nr;
            lstrcpynA(b.Text, strcmp(klein, "vorgabe") == 0 ? vorgabe[nr - 1] : klein,
                      sizeof(b.Text));
            sofort = DeviceIoControl(h, IOCTL_NTU_MAUS_SETZEN, &b, sizeof(b), NULL, 0,
                                     &got, NULL);
            CloseHandle(h);
        }
        printf(sofort ? "  Wirkt sofort.\n\n" : "  Wirkt ab dem naechsten Start.\n\n");
    }
    return 1;
}


/* ==================================================================== */

int
main (int argc, char **argv)
{
    const char *befehl = 0;
    int         art = 0, i, nachher_neu = 0;
    DWORD       seriell = 1;
    char       *p;
    int         ok = 1;

    printf("\nusbinst -- FreeBSD-USB fuer Windows NT 4.0 einrichten\n");

    for (i = 1; i < argc; i += 1) {
        if (strcmp(argv[i], "-seriell") == 0 && i + 1 < argc) {
            seriell = (DWORD)atoi(argv[i + 1]);
            i += 1;
        } else if (strcmp(argv[i], "-neustart") == 0) {
            nachher_neu = 1;
        } else if (befehl == 0) {
            befehl = argv[i];
            if (strcmp(befehl, "taste") == 0) {
                break;                  /* Rest wertet taste_befehl aus */
            }
            if (strcmp(befehl, "abschalten") == 0) {
                break;                  /* argv[i+1] ist der Fund */
            }
        } else if (art_index(argv[i]) >= 0) {
            art = art_index(argv[i]);
        } else {
            printf("  unbekannt: %s\n", argv[i]);
            return 1;
        }
    }

    if (seriell > 2) {
        printf("  -seriell 0, 1 oder 2\n");
        return 1;
    }

    GetModuleFileNameA(NULL, quellordner, sizeof(quellordner));
    p = strrchr(quellordner, '\\');
    if (p != 0) {
        p[1] = '\0';
    }

    GetSystemDirectoryA(systemordner, sizeof(systemordner));

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (scm == NULL) {
        fehlertext("OpenSCManager", GetLastError());
        return 1;
    }

    if (befehl != 0 && strcmp(befehl, "taste") == 0) {
        ok = taste_befehl(argc, argv);
    } else if (befehl == 0) {
        zustand();
    } else if (strcmp(befehl, "probe") == 0) {
        ok = probe(art, seriell);
    } else if (strcmp(befehl, "ein") == 0) {
        ok = ein(art, seriell);
    } else if (strcmp(befehl, "pruefen") == 0) {
        ok = pruefen();
    } else if (strcmp(befehl, "abschalten") == 0) {
        int b;
        for (b = 1; b < argc - 1 && strcmp(argv[b], "abschalten") != 0; b += 1) {
        }
        ok = (b < argc - 1) ? abschalten(argv[b + 1]) : 0;
    } else if (strcmp(befehl, "wieder") == 0) {
        ok = wieder();
    } else if (strcmp(befehl, "aus") == 0) {
        ok = aus();
    } else {
        printf("\n  usbinst [probe|ein|aus] [usbnt|uhci|ohci|ehci|xhci]\n"
               "          [-seriell 0|1|2] [-neustart]\n"
               "  usbinst taste [4..8 belegung]\n\n");
        ok = 0;
    }

    CloseServiceHandle(scm);

    if (ok && nachher_neu) {
        neustart();
    }

    return ok ? 0 : 1;
}
