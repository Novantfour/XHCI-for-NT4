/*
 *  werkzeug/usbdruck.c -- USB-Drucker ansehen und pruefen.
 *
 *      usbdruck                    alle USBPRNn: Kennung, Status, Info
 *      usbdruck reset n            SOFT_RESET an USBPRNn
 *      usbdruck schreibe n datei   in 64-kB-Stuecken schreiben; bei einem
 *                                  Fehler GetLastError und Byte melden
 *      usbdruck storno n datei ms  ueberlappt schreiben, nach ms CancelIo
 *      usbdruck lies n             Rueckkanal lesen, bis nichts mehr kommt
 *      usbdruck pjl n              @PJL INFO STATUS schicken, Antwort lesen
 *      usbdruck lesestorno n ms    ueberlappt lesen, nach ms CancelIo
 *
 *  Kennung, Status und Info gehen mit einem Griff OHNE Schreibzugriff --
 *  der eine schreibende Griff bleibt dem Druck.  Nur kernel32.
 */
#include <windows.h>
#include "ntbasis.h"
#include "../ober/ntuprn.h"

static void
pfad (char *p, int n)
{
    lstrcpyA(p, "\\\\.\\USBPRN0");
    p[10] = (char)('0' + n);
}

static HANDLE
oeffne (int n, DWORD zugriff, DWORD flags)
{
    char p[20];
    pfad(p, n);
    return CreateFileA(p, zugriff, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, flags, NULL);
}

static void
zeige (int n, HANDLE h)
{
    char        k[256];
    NTUP_STATUS s;
    NTUP_INFO   i;
    DWORD       got;

    printf("\n  USBPRN%d\n", n);
    if (DeviceIoControl(h, IOCTL_NTUP_KENNUNG, NULL, 0, k, sizeof(k), &got, NULL)) {

        /*  Am Semikolon umbrechen -- ntbasis' printf hat einen kleinen
         *  Puffer, und die Kennung ist oft lang.  */

        char *a = k;
        printf("  Kennung:");
        while (*a != '\0') {
            char stueck[80];
            int  j = 0;
            while (*a != '\0' && j < 78) {
                stueck[j++] = *a;
                if (*a++ == ';') break;
            }
            stueck[j] = '\0';
            printf("  %s\n         ", stueck);
        }
        printf("\n");
    }
    if (DeviceIoControl(h, IOCTL_NTUP_INFO, NULL, 0, &i, sizeof(i), &got, NULL)) {
        printf("  Geraet:   %04x:%04x  bcdDevice %04x  Seriennummer \"%s\"\n",
               i.Vid, i.Pid, i.BcdDevice, i.Seriennummer);
        printf("  Pfad:     %s   Schnittstelle %u, Einstellung %u, Protokoll %02u\n",
               i.Pfad, i.Schnittstelle, i.Alternative, i.Protokoll);
        if (i.Wartezeit) printf("  Wartezeit: %lu s je 64 kB", i.Wartezeit);
        else             printf("  Wartezeit: unbegrenzt");
        printf("   Fassung %lu, Aenderungen %lu\n", i.Fassung, i.Aenderungen);
        if (i.Fassung >= 3) {
            printf("  Rueckkanal: %s\n", i.Rueckkanal ? "ja (ReadFile)" : "nein (Protokoll 01)");
        }
    }
    if (DeviceIoControl(h, IOCTL_NTUP_STATUS, NULL, 0, &s, sizeof(s), &got, NULL)) {
        if (s.Gelesen) {
            printf("  Status:   %s, %s, %s  (%02lx)\n",
                   s.PapierLeer ? "PAPIER LEER" : "Papier da",
                   s.Ausgewaehlt ? "ausgewaehlt" : "NICHT ausgewaehlt",
                   s.KeinFehler ? "kein Fehler" : "FEHLER", s.Roh);
        } else {
            printf("  Status:   nicht abfragbar\n");
        }
        printf("  gedruckt: %lu Byte seit dem Einstecken\n", s.Bytes);
    }
}

static char puffer[65536];

static int
schreibe (int n, const char *datei)
{
    HANDLE h, d;
    DWORD  gelesen, geschrieben, summe = 0, t0 = GetTickCount();

    d = CreateFileA(datei, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (d == INVALID_HANDLE_VALUE) {
        printf("  %s: Fehler %lu\n", datei, GetLastError());
        return 1;
    }
    h = oeffne(n, GENERIC_WRITE, 0);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  CreateFile USBPRN%d (schreiben): Fehler %lu\n", n, GetLastError());
        CloseHandle(d);
        return 1;
    }
    while (ReadFile(d, puffer, sizeof(puffer), &gelesen, NULL) && gelesen > 0) {
        geschrieben = 0;
        if (!WriteFile(h, puffer, gelesen, &geschrieben, NULL)) {
            printf("  WriteFile: Fehler %lu nach %lu ms; dieser Aufruf: %lu von %lu Byte;"
                   " insgesamt %lu Byte\n", GetLastError(), GetTickCount() - t0,
                   geschrieben, gelesen, summe + geschrieben);
            CloseHandle(h); CloseHandle(d);
            return 1;
        }
        summe += geschrieben;
    }
    printf("  geschrieben: %lu Byte in %lu ms\n", summe, GetTickCount() - t0);
    CloseHandle(h); CloseHandle(d);
    return 0;
}

static char gross[1048576];

static int
storno (int n, const char *datei, int ms)
{
    HANDLE     h, d;
    DWORD      gelesen = 0, geschrieben = 0;
    OVERLAPPED o;

    d = CreateFileA(datei, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (d == INVALID_HANDLE_VALUE || !ReadFile(d, gross, sizeof(gross), &gelesen, NULL)) {
        printf("  %s: Fehler %lu\n", datei, GetLastError());
        return 1;
    }
    CloseHandle(d);
    h = oeffne(n, GENERIC_WRITE, FILE_FLAG_OVERLAPPED);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  CreateFile USBPRN%d (schreiben): Fehler %lu\n", n, GetLastError());
        return 1;
    }
    memset(&o, 0, sizeof(o));
    o.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!WriteFile(h, gross, gelesen, NULL, &o) && GetLastError() != ERROR_IO_PENDING) {
        printf("  WriteFile: sofort Fehler %lu\n", GetLastError());
        return 1;
    }
    Sleep((DWORD)ms);
    printf("  CancelIo: %s\n", CancelIo(h) ? "ok" : "Fehler");
    if (GetOverlappedResult(h, &o, &geschrieben, TRUE)) {
        printf("  Schreiben fertig trotz CancelIo: %lu Byte\n", geschrieben);
    } else {
        printf("  Schreiben: Fehler %lu, %lu von %lu Byte\n", GetLastError(), geschrieben, gelesen);
    }
    CloseHandle(h);
    return 0;
}

/*  Lesen, bis ein ReadFile 0 Byte liefert (Lesezeit abgelaufen).  Den
 *  Text sichtbar machen: \r \n \f als solche.  */

static int
lies_alles (HANDLE h)
{
    static char b[4097];
    DWORD n, summe = 0, t0 = GetTickCount();

    for (;;) {
        if (!ReadFile(h, b, 4096, &n, NULL)) {
            printf("  ReadFile: Fehler %lu nach %lu ms\n", GetLastError(), GetTickCount() - t0);
            return 1;
        }
        if (n == 0) {
            break;
        }
        {
            DWORD k;
            printf("  ");
            for (k = 0; k < n; k += 1) {
                char c = b[k];
                if (c == '\r')      printf("\\r");
                else if (c == '\n') printf("\\n\n  ");
                else if (c == '\f') printf("\\f");
                else                printf("%c", c);
            }
            printf("\n");
        }
        summe += n;
    }
    printf("  gelesen: %lu Byte; das letzte ReadFile kam nach %lu ms mit 0 Byte zurueck\n",
           summe, GetTickCount() - t0);
    return 0;
}

int
main (int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "lies") == 0) {
        HANDLE h = oeffne(atoi(argv[2]), GENERIC_READ, 0);
        int    r;
        if (h == INVALID_HANDLE_VALUE) { printf("  CreateFile: Fehler %lu\n", GetLastError()); return 1; }
        r = lies_alles(h);
        CloseHandle(h);
        return r;
    }
    if (argc > 2 && strcmp(argv[1], "pjl") == 0) {
        static const char anfrage[] = "\x1b%-12345X@PJL INFO STATUS\r\n\x1b%-12345X";
        HANDLE h = oeffne(atoi(argv[2]), GENERIC_READ | GENERIC_WRITE, 0);
        DWORD  n;
        int    r;
        if (h == INVALID_HANDLE_VALUE) { printf("  CreateFile: Fehler %lu\n", GetLastError()); return 1; }
        if (!WriteFile(h, anfrage, sizeof(anfrage) - 1, &n, NULL)) {
            printf("  WriteFile: Fehler %lu\n", GetLastError()); CloseHandle(h); return 1;
        }
        printf("  @PJL INFO STATUS geschickt (%lu Byte), Antwort:\n", n);
        r = lies_alles(h);
        CloseHandle(h);
        return r;
    }
    if (argc > 3 && strcmp(argv[1], "lesestorno") == 0) {
        HANDLE     h = oeffne(atoi(argv[2]), GENERIC_READ, FILE_FLAG_OVERLAPPED);
        OVERLAPPED o;
        char       b[256];
        DWORD      n = 0;
        if (h == INVALID_HANDLE_VALUE) { printf("  CreateFile: Fehler %lu\n", GetLastError()); return 1; }
        memset(&o, 0, sizeof(o));
        o.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ReadFile(h, b, sizeof(b), NULL, &o) && GetLastError() != ERROR_IO_PENDING) {
            printf("  ReadFile: sofort Fehler %lu\n", GetLastError()); return 1;
        }
        Sleep((DWORD)atoi(argv[3]));
        printf("  CancelIo: %s\n", CancelIo(h) ? "ok" : "Fehler");
        if (GetOverlappedResult(h, &o, &n, TRUE)) printf("  Lesen fertig trotz CancelIo: %lu Byte\n", n);
        else                                      printf("  Lesen: Fehler %lu, %lu Byte\n", GetLastError(), n);
        CloseHandle(h);
        return 0;
    }

    int    n, gefunden = 0;
    HANDLE h;

    if (argc > 2 && strcmp(argv[1], "reset") == 0) {
        DWORD got;
        n = atoi(argv[2]);
        h = oeffne(n, GENERIC_READ, 0);
        if (h == INVALID_HANDLE_VALUE) { printf("  USBPRN%d nicht da\n", n); return 1; }
        printf("  SOFT_RESET an USBPRN%d: %s\n", n,
               DeviceIoControl(h, IOCTL_NTUP_RESET, NULL, 0, NULL, 0, &got, NULL) ? "ok" : "Fehler");
        CloseHandle(h);
        return 0;
    }
    if (argc > 3 && strcmp(argv[1], "schreibe") == 0) {
        return schreibe(atoi(argv[2]), argv[3]);
    }
    if (argc > 4 && strcmp(argv[1], "storno") == 0) {
        return storno(atoi(argv[2]), argv[3], atoi(argv[4]));
    }

    for (n = 1; n <= 9; n += 1) {
        h = oeffne(n, GENERIC_READ, 0);
        if (h != INVALID_HANDLE_VALUE) {
            zeige(n, h);
            CloseHandle(h);
            gefunden += 1;
        }
    }
    if (!gefunden) {
        printf("\n  Kein USB-Drucker.\n");
    }
    printf("\n");
    return gefunden ? 0 : 1;
}
