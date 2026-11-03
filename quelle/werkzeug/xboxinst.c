/*
 *  werkzeug/xboxinst.c -- Gamepads (Xbox 360, Xbox One/Series, HID) fuer
 *  den FreeBSD-USB-Stapel auf Windows NT 4.0 einrichten.
 *
 *      xboxinst            Zustand anzeigen
 *      xboxinst ein        einrichten (wirkt ab dem naechsten Start)
 *      xboxinst aus        entfernen, alles Gesicherte zurueck
 *
 *  Getrennt vom USB-Paket (USBINST): der Kerntreiber USBXBOX.SYS meldet
 *  sich beim Laden bei usbnt.sys an und importiert dessen Stapel-
 *  funktionen.  Voraussetzung ist deshalb das USB-Paket in der Fassung
 *  OHNE eingebauten Joystick-Teil (usbnt.sys exportiert dann
 *  ntu_treiber_anmelden).
 *
 *  Vor dem Einrichten wird das Alte abgeschaltet -- in dieser Reihenfolge,
 *  und bei 1 wird abgebrochen statt weitergemacht:
 *    1. alter, in usbnt.sys eingebauter Joystick-Teil: dann bekaeme der
 *       alte Treiber den Controller zuerst, USBXBOX nie.  Er laesst sich
 *       nur durch das neue USB-Paket abschalten -> Hinweis, Abbruch.
 *    2. winmm-Joystick-Treiber (Drivers32\joystick, z. B. joystick.dll):
 *       gesichert, ersetzt durch usbjoy.dll.
 *    3. NT-Gameport-Dienst "Joystick", falls eingerichtet: gesichert,
 *       abgeschaltet (Start 4).
 *    4. Systemsteuerungssymbol joy.cpl ausgeblendet (usbjoy.cpl ersetzt es).
 *  "aus" schreibt alles Gesicherte zurueck.
 *
 *  Nur kernel32, advapi32, user32 (NT 4.0).
 */

#include <windows.h>
#include "ntbasis.h"

#define SICHERUNG    "SOFTWARE\\UsbNt\\Xbox"
#define ALT_SICH     "SOFTWARE\\UsbNt\\Sicherung"        /* kombinierte Vorfassung */
#define DIENSTE      "SYSTEM\\CurrentControlSet\\Services\\"
#define DRIVERS32    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"
#define JOY_OEM      "System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\"
#define JOY_MR       "System\\CurrentControlSet\\Control\\MediaResources\\Joystick\\usbjoy.dll"
#define JOY_CJS      JOY_MR "\\CurrentJoystickSettings"
#define DONT_LOAD    "Control Panel\\don't load"

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

#define D32(o) (*(DWORD *)(d + (o)))
#define W16(o) (*(WORD  *)(d + (o)))
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

/* ---------------------------------------------------------------- *
 *  1.  Voraussetzung und alter, eingebauter Joystick-Teil
 * ---------------------------------------------------------------- */

static int
usb_paket_pruefen (int laut)
{
    static const char *einzeln[] = { "uhcint", "ohcint", "ehcint", "xhcint", 0 };
    char  schl[120], pfad[MAX_PATH];
    DWORD start;
    int   i, e;

    strcpy(schl, DIENSTE); dran(schl, "usbnt");
    if (!dw_lesen(schl, "Start", &start) || start == 4) {
        for (i = 0; einzeln[i] != 0; i += 1) {
            strcpy(schl, DIENSTE); dran(schl, einzeln[i]);
            if (dw_lesen(schl, "Start", &start) && start != 4) {
                if (laut) {
                    printf("  Eingerichtet ist die Einzelbauform %s.  USBXBOX.SYS braucht\n"
                           "  die volle Fassung usbnt.sys:  USBINST ein  (ohne Bauart).\n",
                           einzeln[i]);
                }
                return 0;
            }
        }
        if (laut) {
            printf("  Das USB-Paket ist nicht eingerichtet.  Zuerst:  USBINST ein\n");
        }
        return 0;
    }

    strcpy(pfad, treiberordner); dran(pfad, "\\usbnt.sys");
    e = exportiert(pfad, "ntu_treiber_anmelden");
    if (e < 0) {
        if (laut) printf("  %s nicht lesbar.\n", pfad);
        return 0;
    }
    if (e == 0) {
        if (laut) {
            printf("  Die eingerichtete usbnt.sys ist eine alte Fassung mit EINGEBAUTEM\n"
                   "  Joystick-Teil.  Der muss weg, sonst bekommt er den Controller\n"
                   "  zuerst und USBXBOX nie.  Abschalten laesst er sich nur durch das\n"
                   "  neue USB-Paket:\n"
                   "      1.  USBINST ein      (aus dem neuen USB-Paket)\n"
                   "      2.  neu starten\n"
                   "      3.  XBOXINST ein\n");
        }
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- *
 *  2.-4.  Altes abschalten, Gesichertes zurueck
 * ---------------------------------------------------------------- */

static void
altes_abschalten (void)
{
    char  alt[MAX_PATH], schl[120];
    DWORD start;
    HKEY  k;

    /*  2.  winmm-Joystick-Treiber.  Hat die kombinierte Vorfassung von
     *  USBINST schon einen fremden Eintrag gesichert, ist DAS der echte
     *  alte -- uebernehmen, nicht usbjoy.dll als "alt" merken.  */

    if (sz_lesen(ALT_SICH, "Drivers32_joystick", alt, sizeof(alt)) &&
        !sz_lesen(SICHERUNG, "Drivers32_joystick", schl, sizeof(schl))) {
        sz_schreiben(SICHERUNG, "Drivers32_joystick", alt);
        wert_loeschen(ALT_SICH, "Drivers32_joystick");
        printf("  Sicherung der Vorfassung uebernommen: Joystick-Treiber \"%s\"\n", alt);
    }
    if (sz_lesen(DRIVERS32, "joystick", alt, sizeof(alt)) &&
        lstrcmpiA(alt, "usbjoy.dll") != 0) {
        sz_schreiben(SICHERUNG, "Drivers32_joystick", alt);
        printf("  alter Joystick-Treiber \"%s\" gesichert und abgeloest\n", alt);
    }
    sz_schreiben(DRIVERS32, "joystick", "usbjoy.dll");

    /*  3.  NT-Gameport-Dienst.  */

    strcpy(schl, DIENSTE); dran(schl, "Joystick");
    if (dw_lesen(schl, "Start", &start) && start != 4) {
        dw_schreiben(SICHERUNG, "Dienst_Joystick_Start", start);
        dw_schreiben(schl, "Start", 4);
        printf("  Gameport-Dienst \"Joystick\" abgeschaltet (Start %lu gesichert)\n", start);
    }

    /*  4.  joy.cpl ausblenden (je Benutzer, HKCU).  */

    if (RegCreateKeyExA(HKEY_CURRENT_USER, DONT_LOAD, 0, NULL, 0, KEY_WRITE,
                        NULL, &k, NULL) == 0) {
        RegSetValueExA(k, "joy.cpl", 0, REG_SZ, (const BYTE *)"no", 3);
        RegCloseKey(k);
        printf("  altes Joystick-Symbol (joy.cpl) ausgeblendet -- fuer diesen Benutzer\n");
    }
}

static void
altes_zurueck (void)
{
    char  alt[MAX_PATH], schl[120];
    DWORD start;
    HKEY  k;

    if (sz_lesen(DRIVERS32, "joystick", alt, sizeof(alt)) &&
        lstrcmpiA(alt, "usbjoy.dll") == 0) {
        if (sz_lesen(SICHERUNG, "Drivers32_joystick", alt, sizeof(alt))) {
            sz_schreiben(DRIVERS32, "joystick", alt);
            printf("  Joystick-Treiber zurueck auf \"%s\"\n", alt);
        } else {
            wert_loeschen(DRIVERS32, "joystick");
            printf("  Joystick-Treiber usbjoy.dll ausgetragen\n");
        }
    }
    wert_loeschen(SICHERUNG, "Drivers32_joystick");

    if (dw_lesen(SICHERUNG, "Dienst_Joystick_Start", &start)) {
        strcpy(schl, DIENSTE); dran(schl, "Joystick");
        dw_schreiben(schl, "Start", start);
        wert_loeschen(SICHERUNG, "Dienst_Joystick_Start");
        printf("  Gameport-Dienst \"Joystick\" wieder auf Start %lu\n", start);
    }

    if (RegOpenKeyExA(HKEY_CURRENT_USER, DONT_LOAD, 0, KEY_WRITE, &k) == 0) {
        RegDeleteValueA(k, "joy.cpl");
        RegCloseKey(k);
    }
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, SICHERUNG);
}

/* ---------------------------------------------------------------- *
 *  Joystick-Typen und Einstellungen (winmm, joy.cpl, DirectInput)
 * ---------------------------------------------------------------- */

static void
joy_typ (const char *schluessel, const char *name, DWORD flags, DWORD knoepfe)
{
    char  schl[200];
    DWORD d[2];
    HKEY  k;

    strcpy(schl, JOY_OEM); dran(schl, schluessel);
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, schl, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, NULL) == 0) {
        d[0] = flags; d[1] = knoepfe;
        RegSetValueExA(k, "OEMName", 0, REG_SZ, (const BYTE *)name, (DWORD)strlen(name) + 1);
        RegSetValueExA(k, "OEMData", 0, REG_BINARY, (const BYTE *)d, sizeof(d));
        RegCloseKey(k);
    }
}

/*  Joystick<n>Configuration = JOYREGHWCONFIG (mmddk.h, 112 Byte), mit
 *  JOY_US_PRESENT und dwType >= 12 (OEM): sonst listet DirectInput 3 den
 *  Controller nicht bzw. ohne Namen (DINPUT.DLL, NT 4.0 SP6a).  Eine
 *  gueltige Auswahl aus der Systemsteuerung bleibt stehen.  */

static void
joy_konfig (int n)
{
    char  name[40], wert[40];
    DWORD d[28], typ, groesse = sizeof(d);
    HKEY  k;
    int   i;

    wsprintfA(name, "Joystick%dConfiguration", n);
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, JOY_CJS, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, NULL) != 0) {
        return;
    }
    if (RegQueryValueExA(k, name, NULL, &typ, (BYTE *)d, &groesse) != 0 ||
        typ != REG_BINARY || groesse != sizeof(d) ||
        (d[26] == 1 && d[2] == 6)) {              /* Wert der Vorfassung */
        memset(d, 0, sizeof(d));
        d[0] = 0x01880003ul;                      /* HASZ|HASPOV|HASR|HASU|HASV */
        d[1] = 11;
        d[2] = 0x2 | 0x4;                         /* JOY_US_PRESENT | JOY_US_ISOEM */
        for (i = 0; i < 6; i += 1) {
            d[3 + i] = 0; d[9 + i] = 65535; d[15 + i] = 32768;
        }
        d[21] = 0; d[22] = 9000; d[23] = 18000; d[24] = 27000;
        d[25] = 0x3F;
        d[26] = 12;                               /* OEM-Typ */
        RegSetValueExA(k, name, 0, REG_BINARY, (const BYTE *)d, sizeof(d));
    }
    wsprintfA(name, "Joystick%dOEMName", n);
    groesse = sizeof(wert);
    if (RegQueryValueExA(k, name, NULL, &typ, (BYTE *)wert, &groesse) != 0 || typ != REG_SZ) {
        RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)"USBJOY_GAMEPAD", 15);
    }
    RegCloseKey(k);
}

/* ---------------------------------------------------------------- *
 *  ein / aus / Zustand
 * ---------------------------------------------------------------- */

static int
ein (void)
{
    SC_HANDLE s;
    HKEY      k;

    printf("\n  Einrichten: Gamepads (USBXBOX.SYS + usbjoy.dll)\n\n");

    if (!usb_paket_pruefen(1)) {
        printf("\n  Nichts geaendert.\n\n");
        return 0;
    }
    printf("  USB-Paket: usbnt.sys ohne eingebauten Joystick-Teil -- passt.\n");

    altes_abschalten();

    if (!ersetzen("usbxbox.sys", treiberordner)) {
        printf("  usbxbox.sys fehlt neben xboxinst.exe.\n");
        return 0;
    }

    /*  Fehlerbehandlung IGNORE: fehlt usbnt.sys einmal (USB-Paket
     *  entfernt), laedt dieser Treiber nicht -- das soll den Start nicht
     *  stoeren.  Gruppe "Extended Base": laedt nach "Keyboard Port"
     *  (usbnt).  Die Reihenfolge ist aber nicht zwingend: wer spaeter
     *  kommt, wird vom Stapel per Neu-Aufzaehlen nachbedient.  */

    s = CreateServiceA(scm, "usbxbox", "USB-Gamepads (Xbox, HID)",
                       SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START,
                       SERVICE_ERROR_IGNORE, "System32\\drivers\\usbxbox.sys",
                       "Extended Base", NULL, NULL, NULL, NULL);
    if (s == NULL && GetLastError() == 1073) {
        s = OpenServiceA(scm, "usbxbox", SERVICE_ALL_ACCESS);
        if (s != NULL) {
            ChangeServiceConfigA(s, SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START,
                                 SERVICE_ERROR_IGNORE, "System32\\drivers\\usbxbox.sys",
                                 "Extended Base", NULL, NULL, NULL, NULL,
                                 "USB-Gamepads (Xbox, HID)");
        }
    }
    if (s == NULL) {
        fehlertext("CreateService usbxbox", GetLastError());
        return 0;
    }
    CloseServiceHandle(s);
    printf("  Dienst usbxbox eingerichtet (Start mit dem System)\n");

    ersetzen("usbjoy.dll", systemordner);
    ersetzen("usbjoy.cpl", systemordner);
    ersetzen("joytest.exe", systemordner);

    joy_typ("USBJOY_XBOX360", "Xbox 360 / Xbox One Controller (USB)", 0x01880023ul, 11);
    joy_typ("USBJOY_GAMEPAD", "USB-Gamepad / -Joystick (usbjoy)", 0x01880003ul, 32);
    sz_schreiben(JOY_MR, "Description", "USB-Gamepad (usbjoy)");
    sz_schreiben(JOY_MR, "Driver", "usbjoy.dll");
    joy_konfig(1);
    joy_konfig(2);

    /*  Merken, dass eingerichtet ist (fuer USBINST aus: Hinweis).  */
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, SICHERUNG, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &k, NULL) == 0) {
        RegCloseKey(k);
    }

    printf("  Joystick: winmm -> usbjoy.dll; Typen fuer die Systemsteuerung\n"
           "  angelegt.  Alle Tasten und Achsen: Systemsteuerung -> Joystick.\n\n"
           "  Fertig.  Wirkt ab dem naechsten Start.\n\n");
    return 1;
}

static int
aus (void)
{
    SC_HANDLE      s;
    SERVICE_STATUS ss;
    char           schl[200];
    HKEY           k;

    printf("\n  Entfernen: Gamepads\n\n");

    s = OpenServiceA(scm, "usbxbox", SERVICE_ALL_ACCESS);
    if (s != NULL) {
        ControlService(s, SERVICE_CONTROL_STOP, &ss);  /* geht nicht: kein Unload */
        if (DeleteService(s)) {
            printf("  Dienst usbxbox entfernt (der Treiber bleibt bis zum Neustart geladen)\n");
        } else {
            fehlertext("DeleteService usbxbox", GetLastError());
        }
        CloseServiceHandle(s);
    }
    wegraeumen("usbxbox.sys", treiberordner);
    wegraeumen("usbjoy.cpl", systemordner);
    wegraeumen("usbjoy.dll", systemordner);
    wegraeumen("joytest.exe", systemordner);

    strcpy(schl, JOY_OEM);
    schl[strlen(schl) - 1] = '\0';
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, schl, 0, KEY_ALL_ACCESS, &k) == 0) {
        RegDeleteKeyA(k, "USBJOY_XBOX360");
        RegDeleteKeyA(k, "USBJOY_GAMEPAD");
        RegCloseKey(k);
    }
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, JOY_CJS);       /* NT 4.0: innen zuerst */
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, JOY_MR);

    altes_zurueck();

    printf("\n  Fertig.  Neu starten, damit der Treiber nicht mehr laedt.\n\n");
    return 1;
}

static void
zustand (void)
{
    char  schl[120], wert[MAX_PATH], pfad[MAX_PATH];
    DWORD start;

    printf("\n");
    strcpy(schl, DIENSTE); dran(schl, "usbxbox");
    if (dw_lesen(schl, "Start", &start)) {
        printf("  Dienst usbxbox:      Start %lu%s\n", start, start == 4 ? " (aus)" : "");
    } else {
        printf("  Dienst usbxbox:      nicht eingerichtet  (xboxinst ein)\n");
    }
    strcpy(pfad, treiberordner); dran(pfad, "\\usbxbox.sys");
    printf("  usbxbox.sys:         %s\n",
           GetFileAttributesA(pfad) != 0xFFFFFFFFul ? "vorhanden" : "fehlt");
    printf("  USB-Paket:           %s\n",
           usb_paket_pruefen(0) ? "passt (usbnt.sys ohne eingebauten Joystick-Teil)"
                                : "fehlt oder alte Fassung -- xboxinst ein sagt mehr");
    printf("  winmm-Joystick:      %s\n",
           sz_lesen(DRIVERS32, "joystick", wert, sizeof(wert)) ? wert : "(keiner)");
    if (sz_lesen(SICHERUNG, "Drivers32_joystick", wert, sizeof(wert))) {
        printf("  gesichert (alt):     %s\n", wert);
    }
    printf("\n");
}

int
main (int argc, char **argv)
{
    char *p;
    int   ok = 1;

    printf("\nxboxinst -- Gamepads fuer FreeBSD-USB auf Windows NT 4.0\n");

    GetModuleFileNameA(NULL, quellordner, sizeof(quellordner));
    p = strrchr(quellordner, '\\');
    if (p != 0) {
        p[1] = '\0';
    }
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
        printf("\n  xboxinst [ein|aus]\n\n");
        ok = 0;
    }
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}
