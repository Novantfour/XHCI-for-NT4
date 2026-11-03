/*
 *  werkzeug/joytest.c -- Gamepads pruefen.
 *
 *      joytest              ueber winmm, wie ein Spiel: joyGetNumDevs,
 *                           joyGetDevCaps, joyGetPosEx (braucht usbjoy.dll)
 *      joytest folge [s]    s Sekunden lang Joystick 1 abfragen, in EINEM
 *                           Prozess (wie ein Spiel) -- meldet jeden Wechsel
 *                           von Verbindung, Knoepfen, Steuerkreuz
 *      joytest roh [n]      \\.\UsbJoyN direkt: Info und Zustand (Kerntreiber)
 *
 *  kernel32 und winmm.
 */

#include <windows.h>
#include "ntbasis.h"
#include <mmsystem.h>
#include "../ober/ntujoy.h"

static const char *achsname (unsigned short a)
{
    switch (a) {
        case NTUJ_X: return "X";   case NTUJ_Y: return "Y";   case NTUJ_Z: return "Z";
        case NTUJ_RX: return "Rx"; case NTUJ_RY: return "Ry"; case NTUJ_RZ: return "Rz";
        case NTUJ_SLIDER: return "Schieber"; case NTUJ_DIAL: return "Drehknopf";
        case NTUJ_WHEEL: return "Rad";
    }
    return "?";
}

static int
roh (int n)
{
    char         pfad[24] = "\\\\.\\UsbJoy0";
    HANDLE       h;
    NTUJ_INFO    i;
    NTUJ_ZUSTAND z;
    DWORD        got;
    unsigned long k;

    if (n > 9) { pfad[10] = '1'; pfad[11] = (char)('0' + n - 10); pfad[12] = 0; }
    else       { pfad[10] = (char)('0' + n); }

    h = CreateFileA(pfad, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  %s: nicht da (Fehler %lu)\n", pfad, GetLastError());
        return 1;
    }

    if (!DeviceIoControl(h, IOCTL_NTUJ_INFO, NULL, 0, &i, sizeof(i), &got, NULL) ||
        !DeviceIoControl(h, IOCTL_NTUJ_ZUSTAND, NULL, 0, &z, sizeof(z), &got, NULL)) {
        printf("  %s: Abfrage gescheitert (Fehler %lu)\n", pfad, GetLastError());
        CloseHandle(h);
        return 1;
    }

    printf("\n  %s  %s  %04x:%04x%s\n", pfad, i.Name, i.Vid, i.Pid,
           (i.Xbox == 2) ? "  (Xbox One, GIP)" : i.Xbox ? "  (Xbox 360)" : "");
    printf("  Berichte: %lu\n", z.Zaehler);
    for (k = 0; k < i.AnzahlAchsen; k += 1) {
        printf("  Achse %-10s %5lu\n", achsname(i.AchsenArt[k]), z.Achse[k]);
    }
    printf("  Knoepfe (%lu) gedrueckt:", i.AnzahlKnoepfe);
    for (k = 0; k < i.AnzahlKnoepfe; k += 1) {
        if (z.Knoepfe & (1ul << k)) {
            printf(" %lu", k + 1);
        }
    }
    printf("   Maske %08lx\n", z.Knoepfe);
    if (i.HatPov) {
        if (z.Pov == NTUJ_POV_MITTE) printf("  Steuerkreuz: Mitte\n");
        else                         printf("  Steuerkreuz: %lu Grad\n", z.Pov / 100);
    }
    printf("\n");
    CloseHandle(h);
    return 0;
}

static int
ueber_winmm (void)
{
    UINT       n = joyGetNumDevs(), id, gefunden = 0;
    JOYCAPSA   c;
    JOYINFOEX  j;
    MMRESULT   r;

    printf("\n  winmm: joyGetNumDevs = %u\n", n);

    for (id = 0; id < n && id < 16; id += 1) {

        memset(&c, 0, sizeof(c));
        r = joyGetDevCapsA(id, &c, sizeof(c));
        if (r != JOYERR_NOERROR) {
            continue;
        }

        memset(&j, 0, sizeof(j));
        j.dwSize  = sizeof(j);
        j.dwFlags = JOY_RETURNALL;
        r = joyGetPosEx(id, &j);

        printf("\n  Joystick %u: \"%s\"  %u Achsen, %u Knoepfe%s%s%s%s%s\n", id + 1,
               c.szPname, c.wNumAxes, c.wNumButtons,
               (c.wCaps & JOYCAPS_HASZ) ? " Z" : "", (c.wCaps & JOYCAPS_HASR) ? " R" : "",
               (c.wCaps & JOYCAPS_HASU) ? " U" : "", (c.wCaps & JOYCAPS_HASV) ? " V" : "",
               (c.wCaps & JOYCAPS_HASPOV) ? " POV" : "");

        if (r != JOYERR_NOERROR) {
            printf("  joyGetPosEx: Fehler %u%s\n", r,
                   (r == JOYERR_UNPLUGGED) ? " (nicht angeschlossen)" : "");
            continue;
        }

        printf("  X %5lu  Y %5lu  Z %5lu  R %5lu  U %5lu  V %5lu\n",
               j.dwXpos, j.dwYpos, j.dwZpos, j.dwRpos, j.dwUpos, j.dwVpos);
        printf("  Knoepfe %08lx (niedrigster %lu)  POV %lu\n\n",
               j.dwButtons, j.dwButtonNumber, j.dwPOV);
        gefunden += 1;
    }

    if (gefunden == 0) {
        printf("  kein Joystick antwortet.\n\n");
    }
    return gefunden ? 0 : 1;
}

static int
folge (int sekunden)
{
    DWORD     ende = GetTickCount() + (DWORD)sekunden * 1000, t0 = GetTickCount();
    JOYINFOEX j;
    MMRESULT  r, r_alt = 0xFFFF;
    DWORD     k_alt = 0xFFFFFFFF, p_alt = 0xFFFFFFFF;

    while (GetTickCount() < ende) {

        memset(&j, 0, sizeof(j));
        j.dwSize  = sizeof(j);
        j.dwFlags = JOY_RETURNALL;
        r = joyGetPosEx(0, &j);

        if (r != r_alt || (r == JOYERR_NOERROR &&
                           (j.dwButtons != k_alt || j.dwPOV != p_alt))) {
            if (r == JOYERR_NOERROR) {
                printf("  %6lu ms  da     Knoepfe %08lx  POV %5lu  X %5lu\n",
                       GetTickCount() - t0, j.dwButtons, j.dwPOV, j.dwXpos);
            } else {
                printf("  %6lu ms  Fehler %u%s\n", GetTickCount() - t0, r,
                       (r == JOYERR_UNPLUGGED) ? " (nicht angeschlossen)" : "");
            }
            r_alt = r; k_alt = j.dwButtons; p_alt = j.dwPOV;
        }
        Sleep(100);
    }
    return 0;
}

static int
ruetteln (int stark, int schwach, int n)
{
    char          pfad[24] = "\\\\.\\UsbJoy0";
    HANDLE        h;
    NTUJ_RUETTELN r;
    DWORD         got;

    pfad[10] = (char)('0' + (n % 10));
    h = CreateFileA(pfad, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  %s: nicht da (Fehler %lu)\n", pfad, GetLastError());
        return 1;
    }
    memset(&r, 0, sizeof(r));
    r.Stark   = (unsigned char)stark;
    r.Schwach = (unsigned char)schwach;
    if (!DeviceIoControl(h, IOCTL_NTUJ_RUETTELN, &r, sizeof(r), NULL, 0, &got, NULL)) {
        printf("  Vibration: Fehler %lu%s\n", GetLastError(),
               GetLastError() == ERROR_NOT_SUPPORTED ? " (Controller kann es nicht)" : "");
        CloseHandle(h);
        return 1;
    }
    printf("  Vibration: stark %d, schwach %d\n", stark, schwach);
    CloseHandle(h);
    return 0;
}

int
main (int argc, char **argv)
{
    if (argc > 3 && strcmp(argv[1], "ruetteln") == 0) {
        return ruetteln(atoi(argv[2]), atoi(argv[3]), argc > 4 ? atoi(argv[4]) : 0);
    }
    if (argc > 1 && strcmp(argv[1], "folge") == 0) {
        return folge(argc > 2 ? atoi(argv[2]) : 20);
    }
    if (argc > 1 && strcmp(argv[1], "roh") == 0) {
        return roh(argc > 2 ? atoi(argv[2]) : 0);
    }
    return ueber_winmm();
}
