/*
 *  agent/ntagent.c -- ein sehr kleiner Gastagent fuer Windows NT 4.0.
 *
 *  Der qemu-guest-agent laeuft auf NT 4.0 nicht: er haengt an GLib, und
 *  GLib laesst sich seit Jahren nicht mehr auf NT 4.0 bauen; ausserdem
 *  spricht er ueblicherweise ueber virtio-serial, wofuer es keinen
 *  NT-4.0-Treiber gibt.  Beides zusammen ist aussichtslos.
 *
 *  Dieser hier tut nur das eine, was fuer die Treiberarbeit gebraucht
 *  wird: er liest Befehle von einer seriellen Leitung, fuehrt sie aus
 *  und schickt die Ausgabe zurueck.  Damit lassen sich xhcitest und
 *  usbtest im Gast aufrufen, Treiber laden und entladen, Dateien lesen
 *  -- ohne einen einzigen Bildschirmabzug.
 *
 *  Aufbau bewusst wie bei den Werkzeugen:
 *
 *      nur kernel32, kein msvcrt          laeuft auf jedem NT 4.0
 *      eigener Einsprung ueber ntbasis    keine Laufzeitbibliothek
 *      alles ueber CreateFile/ReadFile    kein Sondertreiber noetig
 *
 *  Der Kanal ist COM2.  COM1 bleibt dem seriellen Treiberbericht --
 *  die beiden duerfen sich nicht ins Wort fallen.
 *
 *  Zwei Betriebsarten:
 *
 *    als DIENST (Vorgabe, "ntagent -einrichten")
 *        startet mit dem System, vor jeder Anmeldung, ohne Fenster.  Was
 *        er ausfuehrt, laeuft auf dem unsichtbaren Dienst-Desktop -- es
 *        stiehlt keinem Dialog den Fokus.  Rechte: LocalSystem.
 *        Fuer Tests, die den sichtbaren Desktop brauchen (Tastatur,
 *        Fenster), startet  IA <befehl>  dort.
 *
 *    als Programm ("ntagent -konsole", oder wenn keine Dienstverwaltung
 *        ihn gestartet hat)
 *        wie die erste Fassung: im Konsolenfenster des Angemeldeten.
 *
 *  Das Vorgehen (Wirt schreibt, Gast antwortet):
 *
 *      Wirt  ->  eine Zeile, mit \n abgeschlossen: der Befehl
 *      Gast  ->  die ganze Ausgabe des Befehls
 *            ->  eine Schlusszeile  "\x04EXIT <code>\n"
 *
 *  Das Zeichen 0x04 (EOT) am Zeilenanfang trennt die Nutzausgabe von
 *  der Schlussmeldung, damit der Wirt weiss, wann er aufhoeren kann zu
 *  lesen.  Ein Befehl, dessen Ausgabe selbst ein 0x04 am Zeilenanfang
 *  enthaelt, gibt es bei unseren Werkzeugen nicht.
 *
 *  Uebersetzen: siehe agent/bauen.md
 */

#include <windows.h>
#include "../werkzeug/ntbasis.h"

#define KANAL   "COM2"


/* ==================================================================== *
 *  Serielle Leitung
 *
 *  serial.sys bedient COM2; wir kommen mit CreateFile heran.  Die
 *  Einstellung setzen wir selbst, damit sie unabhaengig davon stimmt,
 *  was das System sonst mit dem Anschluss gemacht hat.  Unter QEMU ist
 *  die Baudrate ohne Belang -- die Leitung ist ein Unix-Socket --, aber
 *  auf echter serieller Verbindung zaehlt sie.
 * ==================================================================== */

static HANDLE
kanal_oeffnen (void)
{
    HANDLE h;
    DCB    dcb;
    COMMTIMEOUTS to;

    h = CreateFileA(KANAL, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);

    if (h == INVALID_HANDLE_VALUE) {
        return h;
    }

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (GetCommState(h, &dcb)) {
        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity   = 0;          /* NOPARITY  */
        dcb.StopBits = 0;          /* ONESTOPBIT */
        dcb.fBinary  = 1;
        dcb.fOutxCtsFlow = 0;
        dcb.fOutxDsrFlow = 0;
        dcb.fDtrControl  = 1;      /* DTR_CONTROL_ENABLE */
        dcb.fRtsControl  = 1;      /* RTS_CONTROL_ENABLE */

        /*  Wirklich binaer.  Fuer PUT und GET gehen Dateien roh ueber die
         *  Leitung; ein XON/XOFF-Zeichen darin oder ein ersetztes
         *  Fehlerzeichen wuerde sie still verfaelschen.  */

        dcb.fOutX        = 0;
        dcb.fInX         = 0;
        dcb.fErrorChar   = 0;
        dcb.fNull        = 0;
        dcb.fParity      = 0;
        dcb.fAbortOnError = 0;
        SetCommState(h, &dcb);
    }

    /*  Lesen soll nicht ewig blockieren, sonst kommt der Agent nach
     *  einem halben Befehl nie wieder heraus.  Ein Zeichen darf hoechstens
     *  fuenfzig Millisekunden auf sich warten lassen; danach kehrt
     *  ReadFile mit dem zurueck, was da ist.  */

    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout         = 50;
    to.ReadTotalTimeoutConstant    = 50;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.WriteTotalTimeoutConstant   = 1000;
    to.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(h, &to);

    return h;
}

static void
sende (HANDLE h, const char *p, DWORD n)
{
    DWORD hin;

    while (n != 0) {

        if (!WriteFile(h, p, n, &hin, NULL) || hin == 0) {
            break;
        }

        p += hin;
        n -= hin;
    }
}

static void
sende_text (HANDLE h, const char *s)
{
    sende(h, s, strlen(s));
}


/* ==================================================================== *
 *  Eine Befehlszeile einlesen
 *
 *  Zeichen fuer Zeichen bis zum Zeilenende.  CR wird verworfen, damit es
 *  gleich ist, ob der Wirt \n oder \r\n schickt.  Leerlauf kostet nichts:
 *  ReadFile kehrt nach dem Zeitablauf mit null Zeichen zurueck, und wir
 *  fragen einfach weiter.
 * ==================================================================== */

static int ueberlauf;

/*  Von der Dienststeuerung gesetzt: aufhoeren, sobald es geht.  */
static volatile LONG anhalten;

static int
zeile_lesen (HANDLE h, char *puffer, int max)
{
    int n = 0;

    ueberlauf = 0;

    for (;;) {

        char  c;
        DWORD got = 0;

        if (!ReadFile(h, &c, 1, &got, NULL)) {
            return -1;              /* Leitung weg */
        }

        if (got == 0) {
            if (anhalten) {
                return -2;          /* Dienst soll anhalten */
            }
            continue;               /* Zeitablauf, weiter warten */
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            puffer[n] = '\0';
            return n;
        }

        if (n + 1 < max) {
            puffer[n] = c;
            n += 1;
        } else {
            ueberlauf = 1;          /* weiterlesen bis Zeilenende, dann melden */
        }
    }
}


/* ==================================================================== *
 *  Einen Befehl ausfuehren und die Ausgabe ueber die Leitung schicken
 *
 *  Der Befehl laeuft mit umgelenkter Ausgabe: statt auf eine Konsole
 *  schreibt er in ein Rohr, das dieser Agent leert und weiterreicht.
 *  Damit funktioniert jedes Konsolenprogramm ohne Zutun -- auch die
 *  beiden Werkzeuge, die ja selbst nur auf die Standardausgabe
 *  schreiben.
 * ==================================================================== */

static int
befehl_ausfuehren (HANDLE kanal, const char *befehl, int interaktiv)
{
    HANDLE              rohr_lese, rohr_schreib;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    static char         zeile[4200];
    DWORD               code = 0;

    memset(&sa, 0, sizeof(sa));
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&rohr_lese, &rohr_schreib, &sa, 0)) {
        sende_text(kanal, "AGENT: CreatePipe fehlgeschlagen\n");
        return -1;
    }

    /*  Nur das Schreibende erbt das Kind; das Leseende bleibt bei uns
     *  und darf nicht vererbt werden, sonst haelt das Kind es offen und
     *  ReadFile kehrt nie mit "Rohr zu" zurueck.  */

    SetHandleInformation(rohr_lese, 1 /* HANDLE_FLAG_INHERIT */, 0);

    memset(&si, 0, sizeof(si));
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = rohr_schreib;
    si.hStdError   = rohr_schreib;
    si.hStdInput   = INVALID_HANDLE_VALUE;

    /*  IA: auf den sichtbaren Desktop des Angemeldeten, mit eigenem
     *  Konsolenfenster.  Die Ausgabe geht trotzdem ins Rohr.  Nur als
     *  Dienst hat das Wirkung -- als Programm ist der Agent ohnehin dort.  */

    if (interaktiv) {
        si.lpDesktop = "WinSta0\\Default";
    }

    /*  "cmd /c" davor, damit auch eingebaute Befehle und Umlenkungen
     *  gehen -- dir, type, set.  CreateProcess veraendert die Zeile, sie
     *  muss also schreibbar liegen.  */

    strcpy(zeile, "cmd /c ");
    {
        int i = 7;
        const char *b = befehl;

        while (*b != '\0' && i + 1 < (int)sizeof(zeile)) {
            zeile[i] = *b;
            i += 1;
            b += 1;
        }

        zeile[i] = '\0';
    }

    if (!CreateProcessA(NULL, zeile, NULL, NULL, TRUE,
                        interaktiv ? CREATE_NEW_CONSOLE : 0, NULL, NULL,
                        &si, &pi)) {
        sende_text(kanal, "AGENT: CreateProcess fehlgeschlagen\n");
        CloseHandle(rohr_lese);
        CloseHandle(rohr_schreib);
        return -1;
    }

    /*  Unser Schreibende schliessen -- sonst zeigt das Rohr nie "zu",
     *  weil wir es selbst noch offen halten.  */

    CloseHandle(rohr_schreib);

    for (;;) {

        char  brocken[256];
        DWORD got = 0;

        if (!ReadFile(rohr_lese, brocken, sizeof(brocken), &got, NULL) ||
            got == 0) {
            break;
        }

        sende(kanal, brocken, got);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);

    CloseHandle(rohr_lese);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /*  Schlusszeile mit EOT davor -- daran erkennt der Wirt das Ende.  */

    zeile[0] = '\x04';
    strcpy(zeile + 1, "EXIT ");
    {
        char  z[12];
        int   n = 0;
        DWORD v = code;

        do {
            z[n] = (char)('0' + (v % 10));
            n += 1;
            v /= 10;
        } while (v != 0);

        {
            int i = 6;

            while (n != 0) {
                n -= 1;
                zeile[i] = z[n];
                i += 1;
            }

            zeile[i] = '\n';
            zeile[i + 1] = '\0';
        }
    }

    sende_text(kanal, zeile);
    return 0;
}


/* ==================================================================== *
 *  Hintergrund
 *
 *      BG <befehlszeile>\n   ->  PID <n>\n \x04EXIT 0\n
 *
 *  Startet  cmd /c <befehlszeile>  und kehrt SOFORT zurueck.  Das Kind
 *  erbt KEINEN Kanal (bInheritHandles = FALSE) und bekommt eine eigene,
 *  unsichtbare Konsole -- sonst hielte es das Rohr des Agenten offen, und
 *  der Agent wartete, bis es fertig ist.  Genau das war beim ersten
 *  Belastungstest passiert ("start" oeffnet eine Batchdatei unter NT 4.0
 *  mit cmd /k, und die Shell blieb offen).
 *
 *  Die Ausgabe leitet man in der Befehlszeile selbst um:
 *      BG C:\TEMP\LAST.BAT > C:\TEMP\last.txt 2>&1
 *  und holt sie spaeter mit GET.
 * ==================================================================== */

static void
bg (HANDLE kanal, const char *befehl)
{
    static char         zeile[4200];
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    char                z[40];

    strcpy(zeile, "cmd /c ");
    if (strlen(befehl) + 8 >= sizeof(zeile)) {
        sende_text(kanal, "AGENT: BG zu lang\n\x04""EXIT 1\n");
        return;
    }
    strcpy(zeile + 7, befehl);

    memset(&si, 0, sizeof(si));
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = 0;                         /* SW_HIDE */

    if (!CreateProcessA(NULL, zeile, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        sende_text(kanal, "AGENT: BG CreateProcess fehlgeschlagen\n\x04""EXIT 1\n");
        return;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    strcpy(z, "PID ");
    {
        char t[12]; int n = 0, i = 4; DWORD v = pi.dwProcessId;
        do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v != 0);
        while (n != 0) { z[i++] = t[--n]; }
        z[i++] = '\n'; z[i] = '\0';
    }
    sende_text(kanal, z);
    sende_text(kanal, "\x04""EXIT 0\n");
}


/* ==================================================================== *
 *  Dateien ueber die Leitung
 *
 *      PUT <pfad> <laenge>\n  <laenge Rohbytes>
 *          -> \x04EXIT 0\n
 *      GET <pfad>\n
 *          -> LEN <laenge>\n <laenge Rohbytes> \x04EXIT 0\n
 *
 *  Das ersetzt guest-file-read und guest-file-write des qemu-ga.  Damit
 *  laesst sich ein neues Werkzeug in den laufenden Gast legen, ohne das
 *  Abbild neu zu bauen, und ein Bericht, den ein Programm im Gast
 *  geschrieben hat, wieder herausholen.
 * ==================================================================== */

static unsigned long
zahl_lesen (const char *s)
{
    unsigned long v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned long)(*s - '0');
        s += 1;
    }

    return v;
}

static void
zahl_text (char *z, unsigned long v)
{
    char t[12];
    int  n = 0, i = 0;

    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v != 0);
    while (n != 0) { z[i++] = t[--n]; }
    z[i] = '\0';
}

static int
roh_lesen (HANDLE h, char *ziel, unsigned long n)
{
    /*  Genau n Byte, und wenn es dauert, dann dauert es.  Der
     *  Zeitablauf von fuenfzig Millisekunden liefert zwischendurch null
     *  Byte -- das ist Warten, kein Ende.  */

    while (n != 0) {

        DWORD got = 0;

        if (!ReadFile(h, ziel, (n > 4096) ? 4096 : n, &got, NULL)) {
            return 0;
        }

        ziel += got;
        n    -= got;
    }

    return 1;
}

static void
put (HANDLE kanal, char *arg)
{
    char         *pfad = arg;
    char         *leer = arg;
    unsigned long n;
    char         *daten;
    HANDLE        f;
    DWORD         hin = 0;

    while (*leer != '\0' && *leer != ' ') {
        leer += 1;
    }

    if (*leer == '\0') {
        sende_text(kanal, "AGENT: PUT <pfad> <laenge>\n\x04""EXIT 1\n");
        return;
    }

    *leer = '\0';
    n = zahl_lesen(leer + 1);

    daten = (char *)VirtualAlloc(NULL, n + 1, MEM_COMMIT, PAGE_READWRITE);

    if (daten == 0 || !roh_lesen(kanal, daten, n)) {
        sende_text(kanal, "AGENT: PUT lesen gescheitert\n\x04""EXIT 1\n");
        return;
    }

    /*  Schreibschutz am Ziel aufheben -- sonst scheitert das Ersetzen
     *  einer Datei, die schreibgeschuetzt kam (CD, Archiv).  */

    {
        DWORD a = GetFileAttributesA(pfad);
        if (a != 0xFFFFFFFFul && (a & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesA(pfad, a & ~FILE_ATTRIBUTE_READONLY);
        }
    }

    f = CreateFileA(pfad, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE) {
        VirtualFree(daten, 0, MEM_RELEASE);
        sende_text(kanal, "AGENT: PUT Datei nicht anlegbar\n\x04""EXIT 1\n");
        return;
    }

    WriteFile(f, daten, n, &hin, NULL);
    CloseHandle(f);
    VirtualFree(daten, 0, MEM_RELEASE);

    sende_text(kanal, (hin == n) ? "\x04""EXIT 0\n"
                                 : "AGENT: PUT unvollstaendig\n\x04""EXIT 1\n");
}

static void
get (HANDLE kanal, const char *pfad)
{
    HANDLE f;
    DWORD  n, got = 0;
    char  *daten;
    char   kopf[32];

    f = CreateFileA(pfad, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);

    if (f == INVALID_HANDLE_VALUE) {
        sende_text(kanal, "LEN 0\n\x04""EXIT 1\n");
        return;
    }

    n = GetFileSize(f, NULL);
    daten = (char *)VirtualAlloc(NULL, n + 1, MEM_COMMIT, PAGE_READWRITE);

    if (daten == 0) {
        CloseHandle(f);
        sende_text(kanal, "LEN 0\n\x04""EXIT 1\n");
        return;
    }

    ReadFile(f, daten, n, &got, NULL);
    CloseHandle(f);

    strcpy(kopf, "LEN ");
    zahl_text(kopf + 4, got);
    strcpy(kopf + strlen(kopf), "\n");
    sende_text(kanal, kopf);
    sende(kanal, daten, got);
    sende_text(kanal, "\x04""EXIT 0\n");

    VirtualFree(daten, 0, MEM_RELEASE);
}


/* ==================================================================== */

static int
agent_lauf (void)
{
    HANDLE kanal;
    static char befehl[4096];

    /*  Bis die Leitung da ist, warten.  serial.sys wird beim Systemstart
     *  geladen; startet der Agent frueher, ist COM2 noch nicht da.  */

    for (;;) {

        kanal = kanal_oeffnen();

        if (kanal != INVALID_HANDLE_VALUE) {
            break;
        }

        if (anhalten) {
            return 0;
        }

        Sleep(2000);
    }

    sende_text(kanal, "\x04""AGENT bereit\n");

    for (;;) {

        int n = zeile_lesen(kanal, befehl, sizeof(befehl));

        if (n == -2) {
            break;                  /* Dienst wird angehalten */
        }

        if (n < 0) {

            /*  Leitung verloren -- neu oeffnen, nicht aufgeben.  Der
             *  Wirt kann QEMU neu verbinden, ohne dass der Gast neu
             *  starten muss.  */

            CloseHandle(kanal);

            do {
                if (anhalten) {
                    return 0;
                }
                Sleep(2000);
                kanal = kanal_oeffnen();
            } while (kanal == INVALID_HANDLE_VALUE);

            continue;
        }

        if (n == 0) {
            continue;
        }

        /*  Eine zu lange Zeile nicht abgeschnitten ausfuehren -- ein
         *  halber Befehl tut womoeglich etwas ganz anderes als der ganze.  */

        if (ueberlauf) {
            sende_text(kanal, "AGENT: Befehlszeile zu lang, nicht "
                              "ausgefuehrt\n\x04""EXIT 1\n");
            continue;
        }

        if (strcmp(befehl, "PING") == 0) {
            sende_text(kanal, "PONG\n\x04""EXIT 0\n");
            continue;
        }

        if (befehl[0] == 'P' && befehl[1] == 'U' && befehl[2] == 'T' &&
            befehl[3] == ' ') {
            put(kanal, befehl + 4);
            continue;
        }

        if (befehl[0] == 'I' && befehl[1] == 'A' && befehl[2] == ' ') {
            befehl_ausfuehren(kanal, befehl + 3, 1);
            continue;
        }

        if (befehl[0] == 'B' && befehl[1] == 'G' && befehl[2] == ' ') {
            bg(kanal, befehl + 3);
            continue;
        }

        if (befehl[0] == 'G' && befehl[1] == 'E' && befehl[2] == 'T' &&
            befehl[3] == ' ') {
            get(kanal, befehl + 4);
            continue;
        }

        if (strcmp(befehl, "QUIT") == 0) {
            sende_text(kanal, "\x04""EXIT 0\n");
            break;
        }

        befehl_ausfuehren(kanal, befehl, 0);
    }

    CloseHandle(kanal);
    return 0;
}


/* ==================================================================== *
 *  Als Dienst
 *
 *  Die Dienstverwaltung ruft dienst_main in einem eigenen Faden.  Der
 *  Agent laeuft darin wie sonst; "Beenden" setzt  anhalten, und
 *  zeile_lesen kehrt beim naechsten Zeitablauf (50 ms) zurueck.  Ein
 *  gerade laufender Befehl wird zu Ende gefuehrt.
 * ==================================================================== */

static SERVICE_STATUS_HANDLE dienst_griff;
static SERVICE_STATUS        dienst_zustand;

static void
dienst_melden (DWORD zustand)
{
    dienst_zustand.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    dienst_zustand.dwCurrentState            = zustand;
    dienst_zustand.dwControlsAccepted        = (zustand == SERVICE_RUNNING)
        ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    dienst_zustand.dwWin32ExitCode           = 0;
    dienst_zustand.dwServiceSpecificExitCode = 0;
    dienst_zustand.dwCheckPoint              = 0;
    dienst_zustand.dwWaitHint                = (zustand == SERVICE_STOP_PENDING) ? 3000 : 0;
    SetServiceStatus(dienst_griff, &dienst_zustand);
}

static VOID WINAPI
dienst_steuer (DWORD was)
{
    switch (was) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            anhalten = 1;
            dienst_melden(SERVICE_STOP_PENDING);
            break;
        default:
            SetServiceStatus(dienst_griff, &dienst_zustand);
            break;
    }
}

static VOID WINAPI
dienst_main (DWORD argc, LPSTR *argv)
{
    dienst_griff = RegisterServiceCtrlHandlerA("ntagent", dienst_steuer);

    if (dienst_griff == 0) {
        return;
    }

    dienst_melden(SERVICE_RUNNING);
    agent_lauf();
    dienst_melden(SERVICE_STOPPED);
}


/* ==================================================================== *
 *  Einrichten und entfernen
 *
 *  Beim Einrichten wird der alte Autostart-Eintrag (Run-Schluessel) der
 *  ersten Fassung entfernt -- liefen beide, stritten sie sich um COM2.
 * ==================================================================== */

/*  Anhalten und WARTEN, bis der Dienst wirklich steht.  ControlService
 *  kehrt sofort zurueck; der Dienstprozess lebt danach noch einen Moment.
 *  Wer in diesem Moment die Datei ersetzen oder den Dienst loeschen will,
 *  verliert -- so geschehen im ersten Test von ENTFERNE.BAT + EINRICHT.BAT:
 *  "Der Prozess kann nicht auf die Datei zugreifen", und am Ende gab es
 *  keinen Dienst mehr.  */

static void
dienst_anhalten (SC_HANDLE d)
{
    SERVICE_STATUS st;
    int            k;

    if (!QueryServiceStatus(d, &st) || st.dwCurrentState == SERVICE_STOPPED) {
        return;
    }

    ControlService(d, SERVICE_CONTROL_STOP, &st);

    for (k = 0; k < 40; k += 1) {                       /* bis 10 s */
        if (QueryServiceStatus(d, &st) && st.dwCurrentState == SERVICE_STOPPED) {
            break;
        }
        Sleep(250);
    }

    Sleep(500);         /* "angehalten" gemeldet heisst noch nicht: Prozess weg */
}

/*  Sich selbst nach system32 kopieren, mit Wiederholung: die Datei kann
 *  noch einen Augenblick belegt sein.  */

static int
selbst_kopieren (char *ziel, int max)
{
    char quelle[MAX_PATH];
    int  n, k;

    GetModuleFileNameA(NULL, quelle, sizeof(quelle));
    n = (int)GetSystemDirectoryA(ziel, (UINT)max - 16);
    strcpy(ziel + n, "\\ntagent.exe");

    if (lstrcmpiA(quelle, ziel) == 0) {
        return 1;                       /* laeuft schon von dort */
    }

    for (k = 0; k < 20; k += 1) {       /* bis 10 s */
        if (CopyFileA(quelle, ziel, FALSE)) {
            printf("  %s -> %s\n", quelle, ziel);
            return 1;
        }
        Sleep(500);
    }

    printf("  Kopieren nach %s gescheitert (Fehler %lu)\n", ziel, GetLastError());
    return 0;
}

#define RUN_SCHLUESSEL "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"

static int
einrichten (void)
{
    SC_HANDLE scm, d;
    char      pfad[MAX_PATH];
    HKEY      k;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == 0) {
        printf("  OpenSCManager: Fehler %lu (als Administrator?)\n", GetLastError());
        return 1;
    }

    /*  Aktualisieren: eine laufende Fassung erst anhalten, sonst ist ihre
     *  Datei belegt.  */

    d = OpenServiceA(scm, "ntagent", SERVICE_ALL_ACCESS);
    if (d != 0) {
        printf("  laufende Fassung anhalten ...\n");
        dienst_anhalten(d);
        CloseServiceHandle(d);
    }

    if (!selbst_kopieren(pfad, sizeof(pfad))) {
        CloseServiceHandle(scm);
        return 1;
    }

    d = CreateServiceA(scm, "ntagent", "NTAGENT Befehlskanal (COM2)",
                       SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                       SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, pfad,
                       NULL, NULL, NULL, NULL, NULL);

    if (d == 0 && GetLastError() == 1073) {
        d = OpenServiceA(scm, "ntagent", SERVICE_ALL_ACCESS);
        if (d != 0) {
            ChangeServiceConfigA(d, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                 SERVICE_ERROR_NORMAL, pfad, NULL, NULL, NULL,
                                 NULL, NULL, NULL);
        }
    }

    if (d == 0) {
        printf("  CreateService: Fehler %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }

    printf("  Dienst ntagent eingerichtet: %s\n", pfad);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, RUN_SCHLUESSEL, 0, KEY_ALL_ACCESS, &k) == 0) {
        if (RegDeleteValueA(k, "ntagent") == 0) {
            printf("  alter Autostart-Eintrag entfernt\n");
        }
        RegCloseKey(k);
    }

    if (StartServiceA(d, 0, NULL)) {
        printf("  gestartet\n");
    } else if (GetLastError() == 1056) {
        printf("  laeuft bereits\n");
    } else if (GetLastError() == 1072) {
        printf("  starten: der alte Dienst ist zum Loeschen vorgemerkt -- erst\n"
               "  nach einem Neustart wieder einrichtbar\n");
    } else {
        printf("  starten: Fehler %lu -- laeuft noch eine Konsolenfassung auf COM2?\n",
               GetLastError());
    }

    CloseServiceHandle(d);
    CloseServiceHandle(scm);
    return 0;
}

static int
entfernen (void)
{
    SC_HANDLE      scm, d;
    SERVICE_STATUS st;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == 0) {
        printf("  OpenSCManager: Fehler %lu\n", GetLastError());
        return 1;
    }

    d = OpenServiceA(scm, "ntagent", SERVICE_ALL_ACCESS);
    if (d == 0) {
        printf("  kein Dienst ntagent eingerichtet\n");
        CloseServiceHandle(scm);
        return 1;
    }

    (void)st;
    dienst_anhalten(d);
    printf("  Dienst ntagent %s\n", DeleteService(d) ? "entfernt" : "nicht entfernbar");
    CloseServiceHandle(d);
    CloseServiceHandle(scm);
    return 0;
}


/* ==================================================================== */

int
main (int argc, char **argv)
{
    static SERVICE_TABLE_ENTRYA tabelle[] = {
        { "ntagent", dienst_main },
        { NULL, NULL }
    };

    if (argc > 1 && strcmp(argv[1], "-einrichten") == 0) {
        return einrichten();
    }

    if (argc > 1 && strcmp(argv[1], "-entfernen") == 0) {
        return entfernen();
    }

    if (argc > 1 && strcmp(argv[1], "-konsole") == 0) {
        return agent_lauf();
    }

    /*  Von der Dienstverwaltung gestartet?  Dann kehrt der Aufruf erst
     *  zurueck, wenn der Dienst endet.  Sonst (Fehler 1063) als
     *  gewoehnliches Programm laufen -- so bleibt der alte Autostart
     *  ueber den Run-Schluessel weiter moeglich.  */

    if (StartServiceCtrlDispatcherA(tabelle)) {
        return 0;
    }

    return agent_lauf();
}
