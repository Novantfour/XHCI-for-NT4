/*
 *  werkzeug/audioinst.c -- USB-Audio (UAC1) fuer den FreeBSD-USB-Stapel auf
 *  Windows NT 4.0 einrichten.
 *
 *      audioinst            Zustand anzeigen
 *      audioinst ein        einrichten (wirkt ab dem naechsten Start)
 *      audioinst aus        entfernen
 *
 *  Richtet ein:  USBAUDIO.SYS (Kerntreiber, Dienst usbaudio; meldet sich
 *  bei usbnt.sys an), USBAUDIO.DLL (winmm-Wave-Treiber, Drivers32 "wave"
 *  oder der erste freie Name wave1..wave9), USBTON.EXE (Pruefung).
 *
 *  Voraussetzung: das USB-Paket (usbnt.sys) muss JEDE Funktion
 *  exportieren, die USBAUDIO.SYS von ihm importiert.  Geprueft wird das an
 *  den Dateien selbst (Importtabelle gegen Exporttabelle), nicht an einer
 *  Fassungsnummer -- so faellt auch eine usbnt.sys auf, die schon
 *  Zusatztreiber kennt, aber aelter ist als die Audiofunktionen.
 *
 *  Nur kernel32, advapi32, user32 (NT 4.0).
 */

#include <windows.h>
#include "ntbasis.h"

#define SICHERUNG    "SOFTWARE\\UsbNt\\Audio"
#define DIENSTE      "SYSTEM\\CurrentControlSet\\Services\\"
#define DRIVERS32    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"

static char quellordner[MAX_PATH];
static char systemordner[MAX_PATH];
static char treiberordner[MAX_PATH];
static SC_HANDLE scm;

/* ---------------------------------------------------------------- *
 *  Hilfen
 * ---------------------------------------------------------------- */

static void
dran (char *z, const char *q)
{
    while (*z) z++;
    while ((*z++ = *q++) != 0) ;
}

static void
fehlertext (const char *was, DWORD e)
{
    const char *t = "";
    switch (e) {
        case 2:    t = "Datei nicht gefunden"; break;
        case 5:    t = "Zugriff verweigert -- als Administrator angemeldet?"; break;
        case 32:   t = "Datei in Benutzung"; break;
        case 1072: t = "Dienst zum Loeschen vorgemerkt -- erst neu starten"; break;
        case 1073: t = "Dienst gibt es schon"; break;
        default:   break;
    }
    printf("  %s: Fehler %lu %s\n", was, e, t);
}

static int
sz_lesen (const char *schl, const char *name, char *wert, DWORD n)
{
    HKEY  k;
    DWORD typ, groesse = n;
    LONG  r;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, schl, 0, KEY_READ, &k) != 0) {
        return 0;
    }
    r = RegQueryValueExA(k, name, NULL, &typ, (BYTE *)wert, &groesse);
    RegCloseKey(k);
    return r == 0 && (typ == REG_SZ || typ == REG_EXPAND_SZ) && groesse > 0;
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

static int
dw_lesen (const char *schl, const char *name, DWORD *wert)
{
    HKEY  k;
    DWORD typ, groesse = sizeof(*wert);
    LONG  r;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, schl, 0, KEY_READ, &k) != 0) {
        return 0;
    }
    r = RegQueryValueExA(k, name, NULL, &typ, (BYTE *)wert, &groesse);
    RegCloseKey(k);
    return r == 0 && typ == REG_DWORD;
}

static void
dw_schreiben (const char *schl, const char *name, DWORD wert)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, schl, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, NULL) == 0) {
        RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&wert, sizeof(wert));
        RegCloseKey(k);
    }
}

static void
wert_loeschen (const char *schl, const char *name)
{
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, schl, 0, KEY_ALL_ACCESS, &k) == 0) {
        RegDeleteValueA(k, name);
        RegCloseKey(k);
    }
}

/*  Datei aus dem Quellordner nach <ordner> legen; belegt -> beim
 *  naechsten Start.  0 = liegt nicht daneben oder Fehler.  */

static int
ersetzen (const char *name, const char *ordner)
{
    char q[MAX_PATH], z[MAX_PATH], neu[MAX_PATH];
    DWORD a;

    strcpy(q, quellordner); dran(q, name);
    strcpy(z, ordner);      dran(z, "\\"); dran(z, name);

    if (GetFileAttributesA(q) == 0xFFFFFFFFul) {
        return 0;
    }
    a = GetFileAttributesA(z);
    if (a != 0xFFFFFFFFul && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesA(z, a & ~FILE_ATTRIBUTE_READONLY);
    }
    if (CopyFileA(q, z, FALSE)) {
        printf("  %s -> %s\n", name, z);
        return 1;
    }
    if (GetLastError() == 32) {
        strcpy(neu, z); dran(neu, ".neu");
        if (CopyFileA(q, neu, FALSE) &&
            MoveFileExA(neu, z, MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
            printf("  %s ist in Benutzung -- die neue Fassung wird beim\n"
                   "  naechsten Start eingesetzt.\n", name);
            return 1;
        }
    }
    fehlertext(name, GetLastError());
    return 0;
}

static void
wegraeumen (const char *name, const char *ordner)
{
    char z[MAX_PATH];

    strcpy(z, ordner); dran(z, "\\"); dran(z, name);
    if (GetFileAttributesA(z) == 0xFFFFFFFFul) {
        return;
    }
    SetFileAttributesA(z, FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileA(z)) {
        printf("  %s entfernt\n", name);
    } else if (MoveFileExA(z, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        printf("  %s ist in Benutzung -- wird beim naechsten Start entfernt\n", name);
    }
}

/*  PE lesen: d und groesse, pe, nsek, optgr muessen im Umfeld stehen.  */
#define D32(o) (*(DWORD *)(d + (o)))
#define W16(o) (*(WORD  *)(d + (o)))

/*  Exportiert die PE-Datei <pfad> den Namen <name>?  -1 = nicht lesbar.  */

static int
exportiert (const char *pfad, const char *name)
{
    HANDLE h;
    DWORD  n = 0, groesse, pe, exp_rva, exp_off = 0, anz, namen, i, k;
    WORD   nsek, optgr;
    BYTE  *d;
    int    erg = 0;

    h = CreateFileA(pfad, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    groesse = GetFileSize(h, NULL);
    if (groesse < 0x400 || groesse > 4 * 1024 * 1024 ||
        (d = (BYTE *)LocalAlloc(LMEM_FIXED, groesse)) == NULL) {
        CloseHandle(h);
        return -1;
    }
    ReadFile(h, d, groesse, &n, NULL);
    CloseHandle(h);
    if (n != groesse) {
        LocalFree(d);
        return -1;
    }

    pe = D32(0x3c);
    if (pe + 0x100 > groesse || D32(pe) != 0x00004550ul) {
        LocalFree(d);
        return -1;
    }
    nsek    = W16(pe + 6);
    optgr   = W16(pe + 20);
    exp_rva = D32(pe + 24 + 96);                    /* Datenverzeichnis 0 */

    /*  RVA -> Dateiversatz ueber die Abschnittstabelle.  */
#define VERSATZ(rva, aus) do { WORD s_; aus = 0; \
        for (s_ = 0; s_ < nsek; s_ += 1) { DWORD b_ = pe + 24 + optgr + s_ * 40u; \
            if ((rva) >= D32(b_ + 12) && (rva) < D32(b_ + 12) + D32(b_ + 16)) { \
                aus = (rva) - D32(b_ + 12) + D32(b_ + 20); break; } } } while (0)

    if (exp_rva != 0) {
        VERSATZ(exp_rva, exp_off);
    }
    if (exp_off != 0 && exp_off + 40 <= groesse) {
        anz = D32(exp_off + 24);
        VERSATZ(D32(exp_off + 32), namen);
        for (i = 0; namen != 0 && i < anz && namen + 4 * i + 4 <= groesse; i += 1) {
            VERSATZ(D32(namen + 4 * i), k);
            if (k != 0 && k < groesse && lstrcmpA((const char *)d + k, name) == 0) {
                erg = 1;
                break;
            }
        }
    }
    LocalFree(d);
    return erg;
}


/*  Alle Importe von <treiber> aus <dllname> stehen in der Exporttabelle
 *  von <anbieter>?  1 = ja, 0 = nein (fehlend: erster fehlender Name),
 *  -1 = nicht lesbar.  */

static int
importe_erfuellt (const char *treiber, const char *dllname, const char *anbieter,
                  char *fehlend, int platz)
{
    HANDLE h;
    DWORD  n = 0, groesse, pe, imp_rva, imp_off = 0, k, z, nam;
    WORD   nsek, optgr;
    BYTE  *d;
    int    erg = 1, gefunden = 0;

    h = CreateFileA(treiber, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    groesse = GetFileSize(h, NULL);
    if (groesse < 0x400 || groesse > 4 * 1024 * 1024 ||
        (d = (BYTE *)LocalAlloc(LMEM_FIXED, groesse)) == NULL) {
        CloseHandle(h);
        return -1;
    }
    ReadFile(h, d, groesse, &n, NULL);
    CloseHandle(h);
    if (n != groesse) { LocalFree(d); return -1; }

    pe = D32(0x3c);
    if (pe + 0x100 > groesse || D32(pe) != 0x00004550ul) { LocalFree(d); return -1; }
    nsek    = W16(pe + 6);
    optgr   = W16(pe + 20);
    imp_rva = D32(pe + 24 + 96 + 8);                 /* Datenverzeichnis 1 */
    if (imp_rva != 0) VERSATZ(imp_rva, imp_off);

    for (k = 0; imp_off != 0 && imp_off + 20 * (k + 1) <= groesse; k += 1) {
        DWORD e = imp_off + 20 * k, thunk, name_off;
        if (D32(e + 12) == 0) break;                  /* Ende der Liste */
        VERSATZ(D32(e + 12), name_off);
        if (name_off == 0 || lstrcmpiA((const char *)d + name_off, dllname) != 0) continue;
        gefunden = 1;
        thunk = D32(e) ? D32(e) : D32(e + 16);         /* OriginalFirstThunk, sonst FirstThunk */
        VERSATZ(thunk, z);
        for (; z != 0 && z + 4 <= groesse && D32(z) != 0; z += 4) {
            if (D32(z) & 0x80000000ul) continue;      /* nach Nummer -- gibt es hier nicht */
            VERSATZ(D32(z), nam);
            if (nam == 0) continue;
            if (exportiert(anbieter, (const char *)d + nam + 2) != 1) {
                lstrcpynA(fehlend, (const char *)d + nam + 2, platz);
                erg = 0;
                break;
            }
        }
    }
    LocalFree(d);
    return gefunden ? erg : 1;
}

/* ---------------------------------------------------------------- *
 *  Voraussetzung: usbnt.sys liefert alles, was usbaudio.sys braucht
 * ---------------------------------------------------------------- */

static int
usb_paket_pruefen (int laut)
{
    static const char *einzeln[] = { "uhcint", "ohcint", "ehcint", "xhcint", 0 };
    char  schl[120], usbnt[MAX_PATH], treiber[MAX_PATH], fehlt[80];
    DWORD start;
    int   i, e;

    strcpy(schl, DIENSTE); dran(schl, "usbnt");
    if (!dw_lesen(schl, "Start", &start) || start == 4) {
        for (i = 0; einzeln[i] != 0; i += 1) {
            strcpy(schl, DIENSTE); dran(schl, einzeln[i]);
            if (dw_lesen(schl, "Start", &start) && start != 4) {
                if (laut) printf("  Eingerichtet ist die Einzelbauform %s.  USBAUDIO.SYS braucht\n"
                                 "  die volle Fassung usbnt.sys:  USBINST ein  (ohne Bauart).\n", einzeln[i]);
                return 0;
            }
        }
        if (laut) printf("  Das USB-Paket ist nicht eingerichtet.  Zuerst:  USBINST ein\n");
        return 0;
    }

    strcpy(usbnt, treiberordner); dran(usbnt, "\\usbnt.sys");
    strcpy(treiber, quellordner); dran(treiber, "usbaudio.sys");
    fehlt[0] = '\0';
    e = importe_erfuellt(treiber, "usbnt.sys", usbnt, fehlt, sizeof(fehlt));
    if (e < 0) {
        if (laut) printf("  usbaudio.sys (neben audioinst.exe) oder usbnt.sys nicht lesbar.\n");
        return 0;
    }
    if (e == 0) {
        if (laut) printf("  Die eingerichtete usbnt.sys ist zu alt: ihr fehlt \"%s\", das\n"
                         "  USBAUDIO.SYS braucht.  Erst das USB-Paket aktualisieren:\n"
                         "      1.  USBINST ein      (aus dem neuen USB-Paket)\n"
                         "      2.  neu starten\n"
                         "      3.  AUDIOINST ein\n", fehlt);
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- *
 *  ein / aus / Zustand
 * ---------------------------------------------------------------- */

/*  Freien Drivers32-Namen suchen: "<art>", dann <art>1..9.  Schon von
 *  usbaudio.dll belegt zaehlt als frei (erneutes "ein").  */
static int
freier_name (const char *art, char *name)
{
    char wert[MAX_PATH];
    int  k;

    for (k = 0; k < 10; k += 1) {
        if (k == 0) strcpy(name, art); else wsprintfA(name, "%s%d", art, k);
        if (!sz_lesen(DRIVERS32, name, wert, sizeof(wert)) || lstrcmpiA(wert, "usbaudio.dll") == 0) {
            return 1;
        }
    }
    return 0;
}

static int
ein (void)
{
    SC_HANDLE s;
    char      name[16];

    printf("\n  Einrichten: USB-Audio (USBAUDIO.SYS + usbaudio.dll)\n\n");

    if (!usb_paket_pruefen(1)) {
        printf("\n  Nichts geaendert.\n\n");
        return 0;
    }
    printf("  USB-Paket: usbnt.sys liefert alle Funktionen fuer USBAUDIO.SYS -- passt.\n");

    if (!ersetzen("usbaudio.sys", treiberordner)) {
        printf("  usbaudio.sys fehlt neben audioinst.exe.\n");
        return 0;
    }

    /*  Wie usbxbox: Fehler ignorieren (ohne usbnt.sys laedt er nicht, das
     *  soll den Start nicht stoeren), Gruppe "Extended Base".  Wer spaeter
     *  laedt, wird vom Stapel per Neu-Aufzaehlen nachbedient.  */
    s = CreateServiceA(scm, "usbaudio", "USB-Audio (UAC1)", SERVICE_ALL_ACCESS,
                       SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START, SERVICE_ERROR_IGNORE,
                       "System32\\drivers\\usbaudio.sys", "Extended Base",
                       NULL, NULL, NULL, NULL);
    if (s == NULL && GetLastError() == 1073) {
        s = OpenServiceA(scm, "usbaudio", SERVICE_ALL_ACCESS);
        if (s != NULL) {
            ChangeServiceConfigA(s, SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START,
                                 SERVICE_ERROR_IGNORE, "System32\\drivers\\usbaudio.sys",
                                 "Extended Base", NULL, NULL, NULL, NULL, "USB-Audio (UAC1)");
        }
    }
    if (s == NULL) {
        fehlertext("CreateService usbaudio", GetLastError());
        return 0;
    }
    CloseServiceHandle(s);
    printf("  Dienst usbaudio eingerichtet (Start mit dem System)\n");

    ersetzen("usbaudio.dll", systemordner);
    ersetzen("usbton.exe", systemordner);

    if (!freier_name("wave", name)) {
        printf("  Kein freier winmm-Eintrag (wave, wave1..wave9 belegt).\n");
        return 0;
    }
    sz_schreiben(DRIVERS32, name, "usbaudio.dll");
    sz_schreiben(SICHERUNG, "Drivers32_Name", name);
    printf("  winmm: Drivers32 \"%s\" = usbaudio.dll\n", name);
    if (freier_name("mixer", name)) {
        sz_schreiben(DRIVERS32, name, "usbaudio.dll");
        sz_schreiben(SICHERUNG, "Drivers32_Mixer", name);
        printf("  winmm: Drivers32 \"%s\" = usbaudio.dll (Lautstaerkeregelung)\n", name);
    } else {
        printf("  Kein freier Mixer-Eintrag (mixer, mixer1..mixer9 belegt) -- ohne Lautstaerkeregelung.\n");
    }

    printf("\n  Fertig.  Wirkt ab dem naechsten Start.  Pruefen:  usbton liste,\n"
           "  usbton winmm 440 3\n\n");
    return 1;
}

static int
aus (void)
{
    SC_HANDLE      s;
    SERVICE_STATUS ss;
    char           name[16], wert[MAX_PATH];

    printf("\n  Entfernen: USB-Audio\n\n");

    s = OpenServiceA(scm, "usbaudio", SERVICE_ALL_ACCESS);
    if (s != NULL) {
        ControlService(s, SERVICE_CONTROL_STOP, &ss);  /* geht nicht: kein Unload */
        if (DeleteService(s)) {
            printf("  Dienst usbaudio entfernt (der Treiber bleibt bis zum Neustart geladen)\n");
        } else {
            fehlertext("DeleteService usbaudio", GetLastError());
        }
        CloseServiceHandle(s);
    }

    if (sz_lesen(SICHERUNG, "Drivers32_Name", name, sizeof(name)) &&
        sz_lesen(DRIVERS32, name, wert, sizeof(wert)) && lstrcmpiA(wert, "usbaudio.dll") == 0) {
        wert_loeschen(DRIVERS32, name);
        printf("  winmm: Drivers32 \"%s\" ausgetragen\n", name);
    }
    if (sz_lesen(SICHERUNG, "Drivers32_Mixer", name, sizeof(name)) &&
        sz_lesen(DRIVERS32, name, wert, sizeof(wert)) && lstrcmpiA(wert, "usbaudio.dll") == 0) {
        wert_loeschen(DRIVERS32, name);
        printf("  winmm: Drivers32 \"%s\" ausgetragen\n", name);
    }
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, SICHERUNG);

    wegraeumen("usbaudio.sys", treiberordner);
    wegraeumen("usbaudio.dll", systemordner);
    wegraeumen("usbton.exe", systemordner);

    printf("\n  Fertig.  Neu starten, damit der Treiber nicht mehr laedt.\n\n");
    return 1;
}

static void
zustand (void)
{
    char  schl[120], wert[MAX_PATH], pfad[MAX_PATH], name[16];
    DWORD start;

    printf("\n");
    strcpy(schl, DIENSTE); dran(schl, "usbaudio");
    if (dw_lesen(schl, "Start", &start)) {
        printf("  Dienst usbaudio:     Start %lu%s\n", start, start == 4 ? " (aus)" : "");
    } else {
        printf("  Dienst usbaudio:     nicht eingerichtet  (audioinst ein)\n");
    }
    strcpy(pfad, treiberordner); dran(pfad, "\\usbaudio.sys");
    printf("  usbaudio.sys:        %s\n", GetFileAttributesA(pfad) != 0xFFFFFFFFul ? "vorhanden" : "fehlt");
    if (sz_lesen(SICHERUNG, "Drivers32_Name", name, sizeof(name)) &&
        sz_lesen(DRIVERS32, name, wert, sizeof(wert))) {
        printf("  winmm:               Drivers32 \"%s\" = %s\n", name, wert);
    } else {
        printf("  winmm:               nicht eingetragen\n");
    }
    if (sz_lesen(SICHERUNG, "Drivers32_Mixer", name, sizeof(name)) &&
        sz_lesen(DRIVERS32, name, wert, sizeof(wert))) {
        printf("  Mixer:               Drivers32 \"%s\" = %s\n", name, wert);
    } else {
        printf("  Mixer:               nicht eingetragen (audioinst ein)\n");
    }
    printf("  USB-Paket:           %s\n", usb_paket_pruefen(0)
           ? "passt" : "fehlt oder zu alt -- audioinst ein sagt mehr");
    printf("\n");
}

int
main (int argc, char **argv)
{
    char *p;
    int   ok = 1;

    printf("\naudioinst -- USB-Audio fuer FreeBSD-USB auf Windows NT 4.0\n");

    GetModuleFileNameA(NULL, quellordner, sizeof(quellordner));
    p = strrchr(quellordner, '\\');
    if (p != 0) p[1] = '\0';
    GetSystemDirectoryA(systemordner, sizeof(systemordner));
    strcpy(treiberordner, systemordner);
    dran(treiberordner, "\\drivers");

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == NULL) {
        fehlertext("OpenSCManager", GetLastError());
        return 1;
    }
    if (argc < 2) {
        zustand();
    } else if (strcmp(argv[1], "ein") == 0) {
        ok = ein();
    } else if (strcmp(argv[1], "aus") == 0) {
        ok = aus();
    } else {
        printf("\n  audioinst [ein|aus]\n\n");
        ok = 0;
    }
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}
