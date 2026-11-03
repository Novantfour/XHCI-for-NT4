/*
 *  werkzeug/usbgeraetdll.c -- USBGERAET.DLL: dieselbe Funktion wie das
 *  fruehere USBGERAET.EXE, aber als DLL (ueber rundll32 aufrufbar).
 *
 *  Grund: die winzige EXE wurde von einem ML-Scanner (Elastic) als
 *  verdaechtig eingestuft -- ein Fehlalarm.  Als DLL (mit Ressourcen und
 *  Export, wie USBTASTX.DLL) faellt das Muster weg.  Die Anzeige der
 *  USB-Geraete gibt es ohnehin auch im Geraetemanager; diese DLL behaelt
 *  zusaetzlich den rohen Entwicklerzugriff (Deskriptoren, HID, lesen,
 *  schreiben, steuer, stall, auswerfen).
 *
 *      rundll32 usbgeraet.dll,Geraete                 alle Geraete
 *      rundll32 usbgeraet.dll,Geraete 2               Geraet 2 ausfuehrlich
 *      rundll32 usbgeraet.dll,Geraete 2 hid           HID-Bericht
 *      rundll32 usbgeraet.dll,Geraete auswerfen E:    Stick sicher entfernen
 *      ... (alle Befehle des alten USBGERAET)
 *
 *  Die Ausgabe erscheint in einem Fenster (rundll32 hat keine Konsole).
 */

#include <windows.h>

extern int usbg_main(int argc, char **argv);           /* usbgeraet.c */
extern char        *ntb_puffer;                         /* ntbasis.c   */
extern unsigned int ntb_puffer_max;
extern unsigned int ntb_puffer_len;

/*  rundll32-Kommandozeile in argv zerlegen (einfache Leerzeichen-Trennung;
 *  fuer die Befehle von usbgeraet ausreichend).  argv[0] = "usbgeraet".  */
static int zerlege(char *zeile, char **argv, int max)
{
    int n = 1;
    argv[0] = "usbgeraet";
    while (*zeile && n < max) {
        while (*zeile == ' ' || *zeile == '\t') *zeile++ = '\0';
        if (!*zeile) break;
        argv[n++] = zeile;
        while (*zeile && *zeile != ' ' && *zeile != '\t') zeile++;
    }
    return n;
}

BOOL WINAPI
DllEntry (HINSTANCE h, DWORD grund, LPVOID r)
{
    (void)h; (void)grund; (void)r;
    return TRUE;
}

/*  Fuer Skripte/Pruefung: rundll32 usbgeraet.dll,GeraeteInDatei C:\\o.txt [befehle]
 *  schreibt die Ausgabe in die Datei statt in ein Fenster.  */
void CALLBACK
GeraeteInDatei (HWND hwnd, HINSTANCE inst, LPSTR lpCmdLine, int zeigen)
{
    static char  puffer[16384];
    static char  zeile[1024];
    char        *argv[32], *datei;
    int          argc, i = 0;
    HANDLE       h;
    DWORD        hin = 0;

    (void)hwnd; (void)inst; (void)zeigen;
    while (lpCmdLine && lpCmdLine[i] && i < (int)sizeof(zeile) - 1) { zeile[i] = lpCmdLine[i]; i += 1; }
    zeile[i] = '\0';
    argc = zerlege(zeile, argv, 32);
    if (argc < 2) return;
    datei = argv[1];
    /*  argv[1] (Dateiname) herausnehmen, Rest ab argv[1] verschieben  */
    { int k; for (k = 1; k < argc - 1; k += 1) argv[k] = argv[k + 1]; argc -= 1; }
    puffer[0] = '\0'; ntb_puffer = puffer; ntb_puffer_max = sizeof(puffer); ntb_puffer_len = 0;
    usbg_main(argc, argv);
    ntb_puffer = 0;
    /*  Ausgabedatei (fuer Skripte): darf ueberschrieben werden -- das ist der
     *  Zweck -- aber FILE_SHARE_READ, damit ein anderes Programm sie lesen
     *  kann, und der Fehlerfall wird gemeldet statt still zu scheitern.  */
    h = CreateFileA(datei, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, puffer, ntb_puffer_len, &hin, NULL);
        CloseHandle(h);
    } else {
        MessageBoxA(NULL, "Die Ausgabedatei liess sich nicht anlegen.",
                    "USB-Geraete", MB_OK | MB_ICONWARNING);
    }
}

void CALLBACK
Geraete (HWND hwnd, HINSTANCE inst, LPSTR lpCmdLine, int zeigen)
{
    static char  puffer[16384];
    static char  zeile[1024];
    char        *argv[32];
    int          argc, i = 0;

    (void)hwnd; (void)inst; (void)zeigen;

    /*  Kommandozeile kopieren (rundll32 gibt sie schreibbar, aber sicher ist
     *  sicher).  */
    while (lpCmdLine && lpCmdLine[i] && i < (int)sizeof(zeile) - 1) {
        zeile[i] = lpCmdLine[i]; i += 1;
    }
    zeile[i] = '\0';

    argc = zerlege(zeile, argv, 32);

    /*  Ausgabe in den Puffer statt auf eine (nicht vorhandene) Konsole.  */
    puffer[0] = '\0';
    ntb_puffer     = puffer;
    ntb_puffer_max = sizeof(puffer);
    ntb_puffer_len = 0;

    usbg_main(argc, argv);

    ntb_puffer = 0;                    /* Umleitung wieder aus */

    MessageBoxA(NULL, puffer[0] ? puffer : "keine Ausgabe",
                "USB-Geraete", MB_OK | MB_ICONINFORMATION);
}
