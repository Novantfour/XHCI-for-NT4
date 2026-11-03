/*
 *  werkzeug/ipptest.c -- IPPTEST.EXE: die Wuensche von CUPS4NT pruefen.
 *
 *    W1  \\.\USBIPP1 und \\.\USBIPP2 gleichzeitig: schreiben, lesen,
 *        NTUP_INFO (Protokoll 4, Schnittstelle)
 *    W3  IOCTL_NTUP_ZEIT: Lesegrenze 300 ms, Lesen ohne Daten
 *    W5  auf \\.\USBPRN1: IOCTL_USBPRINT_GET_1284_ID, _GET_LPT_STATUS,
 *        _SOFT_RESET
 */
#include <windows.h>
#include "ntbasis.h"
#include "../ober/ntuprn.h"

static HANDLE
oeffnen (const char *n, DWORD zugriff)
{
    HANDLE h = CreateFileA(n, zugriff, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) printf("  %s: CreateFile Fehler %lu\n", n, GetLastError());
    return h;
}

static void
ipp_kanal (HANDLE h, const char *name)
{
    static const char anfrage[] = "POST /ipp/print HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
    char        puffer[512];
    DWORD       n = 0, got;
    NTUP_INFO   i;
    int         k;

    if (DeviceIoControl(h, IOCTL_NTUP_INFO, NULL, 0, &i, sizeof(i), &got, NULL)) {
        printf("  %s: Fassung %lu, Protokoll %u, Schnittstelle %u, Einstellung %u, Rueckkanal %u\n",
               name, i.Fassung, i.Protokoll, i.Schnittstelle, i.Alternative, i.Rueckkanal);
    }
    if (!WriteFile(h, anfrage, sizeof(anfrage) - 1, &n, NULL)) {
        printf("  %s: WriteFile Fehler %lu\n", name, GetLastError());
        return;
    }
    printf("  %s: %lu Byte geschrieben\n", name, n);
    if (!ReadFile(h, puffer, sizeof(puffer) - 1, &n, NULL)) {
        printf("  %s: ReadFile Fehler %lu\n", name, GetLastError());
        return;
    }
    puffer[n] = '\0';
    for (k = 0; k < (int)n; k += 1) if (puffer[k] == '\r' || puffer[k] == '\n') puffer[k] = '|';
    printf("  %s: %lu Byte gelesen: %s\n", name, n, puffer);
}

int
main (int argc, char **argv)
{
    HANDLE    a, b, p;
    NTUP_ZEIT z, zr;
    DWORD     got, n, t0, t1;
    char      puffer[300];
    unsigned char s;

    printf("\nW1 -- IPP over USB, zwei Kanaele gleichzeitig\n");
    a = oeffnen("\\\\.\\USBIPP1", GENERIC_READ | GENERIC_WRITE);
    b = oeffnen("\\\\.\\USBIPP2", GENERIC_READ | GENERIC_WRITE);
    if (a != INVALID_HANDLE_VALUE) ipp_kanal(a, "USBIPP1");
    if (b != INVALID_HANDLE_VALUE) ipp_kanal(b, "USBIPP2");

    printf("\nW3 -- Zeitgrenze je Griff\n");
    if (a != INVALID_HANDLE_VALUE) {
        if (DeviceIoControl(a, IOCTL_NTUP_ZEIT, NULL, 0, &zr, sizeof(zr), &got, NULL)) {
            printf("  vorher:  Lesen %lu ms, Schreiben %lu ms\n", zr.LesenMs, zr.SchreibenMs);
        }
        z.LesenMs = 300; z.SchreibenMs = NTUP_ZEIT_BLEIBT;
        if (DeviceIoControl(a, IOCTL_NTUP_ZEIT, &z, sizeof(z), &zr, sizeof(zr), &got, NULL)) {
            printf("  gesetzt: Lesen %lu ms, Schreiben %lu ms\n", zr.LesenMs, zr.SchreibenMs);
        } else {
            printf("  IOCTL_NTUP_ZEIT Fehler %lu\n", GetLastError());
        }
        t0 = GetTickCount();
        n = 99;
        if (ReadFile(a, puffer, sizeof(puffer), &n, NULL)) {
            t1 = GetTickCount();
            printf("  Lesen ohne Daten: %lu Byte nach %lu ms\n", n, t1 - t0);
        } else {
            printf("  Lesen ohne Daten: Fehler %lu\n", GetLastError());
        }
        if (b != INVALID_HANDLE_VALUE &&
            DeviceIoControl(b, IOCTL_NTUP_ZEIT, NULL, 0, &zr, sizeof(zr), &got, NULL)) {
            printf("  anderer Griff (USBIPP2) unveraendert: Lesen %lu ms\n", zr.LesenMs);
        }
    }

    printf("\nW5 -- usbprint.sys-Codes auf USBPRN1\n");
    p = oeffnen("\\\\.\\USBPRN1", 0);
    if (p != INVALID_HANDLE_VALUE) {
        if (DeviceIoControl(p, IOCTL_USBPRINT_GET_1284_ID, NULL, 0, puffer, sizeof(puffer) - 1, &got, NULL)) {
            unsigned lang = ((unsigned char)puffer[0] << 8) | (unsigned char)puffer[1];
            puffer[got] = '\0';
            printf("  GET_1284_ID: %lu Byte, Laengenfeld %u (big endian), Kennung: %.50s...\n",
                   got, lang, puffer + 2);
        } else {
            printf("  GET_1284_ID Fehler %lu\n", GetLastError());
        }
        if (DeviceIoControl(p, IOCTL_USBPRINT_GET_LPT_STATUS, NULL, 0, &s, 1, &got, NULL)) {
            printf("  GET_LPT_STATUS: %02x  (%s, %s, %s)\n", s, (s & 0x20) ? "Papier leer" : "Papier da",
                   (s & 0x10) ? "ausgewaehlt" : "nicht ausgewaehlt", (s & 0x08) ? "kein Fehler" : "Fehler");
        } else {
            printf("  GET_LPT_STATUS Fehler %lu\n", GetLastError());
        }
        printf("  SOFT_RESET: %s\n", DeviceIoControl(p, IOCTL_USBPRINT_SOFT_RESET, NULL, 0, NULL, 0, &got, NULL)
                                     ? "ok" : "Fehler");
    }
    printf("\n");
    return 0;
}
