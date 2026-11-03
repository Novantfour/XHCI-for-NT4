/*
 *  werkzeug/sonderset.c -- SONDERSET: Belegung einer Sondertaste lesen
 *  oder setzen, fuer Skripte.
 *      sonderset                 alle Tasten mit Belegung
 *      sonderset <Kennung>       eine lesen   (z. B. Rechner, F13, Play)
 *      sonderset <Kennung> <Text...>  setzen (sofort + Registry)
 *  kernel32.  Kein CRT (ntbasis).
 */
#include <windows.h>
#include "ntbasis.h"
#define NTU_SONDER_TABELLE
#include "../ober/ntusbioctl.h"
#include "../ober/ntsonder.h"

int
main (int argc, char **argv)
{
    static const char *namen[] = { "\\\\.\\UsbNt", "\\\\.\\UhciNt", "\\\\.\\OhciNt",
                                   "\\\\.\\EhciNt", "\\\\.\\XhciNt" };
    HANDLE              g = INVALID_HANDLE_VALUE;
    NTU_SONDER_BELEGUNG b;
    DWORD               got;
    int                 i, k, von = 0, bis = NTU_SONDER_ANZAHL;

    for (i = 0; i < 5 && g == INVALID_HANDLE_VALUE; i += 1) {
        g = CreateFileA(namen[i], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    }
    if (g == INVALID_HANDLE_VALUE) { printf("USB-Treiber nicht erreichbar\n"); return 1; }
    if (argc >= 2) {
        for (von = 0; von < NTU_SONDER_ANZAHL; von += 1) {
            if (lstrcmpiA(argv[1], ntu_sonder_tabelle[von].kennung) == 0) break;
        }
        if (von == NTU_SONDER_ANZAHL) { printf("unbekannte Taste: %s\n", argv[1]); return 1; }
        bis = von + 1;
    }
    if (argc >= 3) {
        memset(&b, 0, sizeof(b));
        b.Index = (unsigned long)von;
        for (k = 2; k < argc; k += 1) {                   /* Rest der Zeile, mit Leerzeichen */
            if (k > 2) lstrcatA(b.Text, " ");
            if (lstrlenA(b.Text) + lstrlenA(argv[k]) < (int)sizeof(b.Text) - 2) lstrcatA(b.Text, argv[k]);
        }
        if (!DeviceIoControl(g, IOCTL_NTU_SONDER_SETZEN, &b, sizeof(b), NULL, 0, &got, NULL)) {
            printf("%s: \"%s\" ist keine gueltige Belegung\n", ntu_sonder_tabelle[von].kennung, b.Text);
            return 1;
        }
    }
    for (i = von; i < bis; i += 1) {
        memset(&b, 0, sizeof(b)); b.Index = (unsigned long)i;
        DeviceIoControl(g, IOCTL_NTU_SONDER_HOLEN, &b, sizeof(b), &b, sizeof(b), &got, NULL);
        printf("  %-14s %s\n", ntu_sonder_tabelle[i].kennung, b.Text);
    }
    return 0;
}
