/*
 *  werkzeug16/joy16.c -- Joystick aus Sicht eines Win16-Programms.
 *
 *  Win16-Spiele (z. B. SinkSub Pro) laufen unter NT 4.0 in WOW und rufen
 *  die 16-Bit-Fassung von joyGetDevCaps/joyGetPos auf.  Deren Strukturen
 *  sind kleiner als die 32-Bit-Fassungen (JOYCAPS 54 Byte, JOYINFO 8
 *  Byte), und WOW reicht sie an winmm -> usbjoy.dll weiter.  Dieses
 *  Programm macht genau diese Aufrufe und schreibt das Ergebnis nach
 *  C:\TEMP\JOY16.TXT (Win16 hat keine Konsole).
 *
 *      JOY16            joyGetNumDevs / joyGetDevCaps / joyGetPos (+ Rohbytes)
 *      JOY16 fang <s>   s Sekunden joySetCapture auf beide Joysticks, wie es
 *                       viele Win16-Spiele tun: jede MM_JOYn-Nachricht
 *                       (Bewegung, Z, Knopf ab/auf) wird protokolliert,
 *                       dazu joySetThreshold/joyGetThreshold
 *
 *  Bauen (Open Watcom):  wcl -bt=windows -l=windows joy16.c mmsystem.lib
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

static HFILE f;

static void zeile (const char *s)
{
    _lwrite(f, s, strlen(s));
    _lwrite(f, "\r\n", 2);
}

/* ---------------------------------------------------------------- *
 *  joySetCapture
 * ---------------------------------------------------------------- */

static int  n_nachr[2][4];               /* je Joystick: Bewegung, Z, ab, auf */
static int  geloggt;

static const char *nachr_name (UINT m)
{
    switch (m) {
        case MM_JOY1MOVE:  case MM_JOY2MOVE:  return "MOVE ";
        case MM_JOY1ZMOVE: case MM_JOY2ZMOVE: return "ZMOVE";
        case MM_JOY1BUTTONDOWN: case MM_JOY2BUTTONDOWN: return "AB   ";
        case MM_JOY1BUTTONUP:   case MM_JOY2BUTTONUP:   return "AUF  ";
    }
    return "?";
}

LRESULT CALLBACK _export FensterProc (HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    char z[120];
    int  j = -1, art = 0;

    switch (m) {
        case MM_JOY1MOVE:       j = 0; art = 0; break;
        case MM_JOY2MOVE:       j = 1; art = 0; break;
        case MM_JOY1ZMOVE:      j = 0; art = 1; break;
        case MM_JOY2ZMOVE:      j = 1; art = 1; break;
        case MM_JOY1BUTTONDOWN: j = 0; art = 2; break;
        case MM_JOY2BUTTONDOWN: j = 1; art = 2; break;
        case MM_JOY1BUTTONUP:   j = 0; art = 3; break;
        case MM_JOY2BUTTONUP:   j = 1; art = 3; break;
        case WM_TIMER:
            KillTimer(w, 1);
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    if (j >= 0) {
        n_nachr[j][art]++;
        if (geloggt < 120) {
            geloggt++;
            sprintf(z, "  J%d %s  wParam %04x  X %5u  Y %5u  (t=%lu ms)", j + 1, nachr_name(m),
                    (unsigned)wp, LOWORD(lp), HIWORD(lp), GetTickCount());
            zeile(z);
        }
        return 0;
    }
    return DefWindowProc(w, m, wp, lp);
}

static int fangen (HINSTANCE hi, int sekunden)
{
    WNDCLASS wc;
    HWND     w;
    MSG      msg;
    UINT     r, schwelle;
    char     z[120];
    int      j;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = FensterProc;
    wc.hInstance     = hi;
    wc.lpszClassName = "JOY16";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);
    w = CreateWindow("JOY16", "JOY16 - joySetCapture", WS_OVERLAPPEDWINDOW,
                     10, 10, 300, 80, NULL, NULL, hi, NULL);
    ShowWindow(w, SW_SHOWNORMAL);

    for (j = 0; j < 2; j++) {
        r = joySetThreshold(j, 1000);
        joyGetThreshold(j, &schwelle);
        sprintf(z, "Joystick %d: joySetThreshold(1000) -> %u, joyGetThreshold = %u", j + 1, r, schwelle);
        zeile(z);
        r = joySetCapture(w, j, 20, TRUE);
        sprintf(z, "Joystick %d: joySetCapture(20 ms, nur Aenderungen) -> %u", j + 1, r);
        zeile(z);
    }
    SetTimer(w, 1, (UINT)sekunden * 1000, NULL);
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    for (j = 0; j < 2; j++) {
        r = joyReleaseCapture(j);
        sprintf(z, "Joystick %d: joyReleaseCapture -> %u;  MOVE %d  ZMOVE %d  AB %d  AUF %d", j + 1, r,
                n_nachr[j][0], n_nachr[j][1], n_nachr[j][2], n_nachr[j][3]);
        zeile(z);
    }
    return 0;
}

int PASCAL WinMain (HINSTANCE hi, HINSTANCE hv, LPSTR cmd, int show)
{
    JOYCAPS  c;
    JOYINFO  j;
    UINT     n, id, r;
    char     z[160];

    f = _lcreat("C:\\TEMP\\JOY16.TXT", 0);
    if (f == HFILE_ERROR) return 1;

    sprintf(z, "Win16: sizeof(JOYCAPS)=%u sizeof(JOYINFO)=%u", sizeof(c), sizeof(j));
    zeile(z);
    if (cmd[0] == 'f' && cmd[1] == 'a' && cmd[2] == 'n' && cmd[3] == 'g') {
        int s = 0;
        LPSTR q = cmd + 4;
        while (*q == ' ') q++;
        while (*q >= '0' && *q <= '9') s = s * 10 + (*q++ - '0');
        fangen(hi, s > 0 ? s : 10);
        _lclose(f);
        return 0;
    }
    n = joyGetNumDevs();
    sprintf(z, "joyGetNumDevs = %u", n);
    zeile(z);

    for (id = 0; id < 2; id++) {
        memset(&c, 0, sizeof(c));
        r = joyGetDevCaps(id, &c, sizeof(c));
        sprintf(z, "Joystick %u: joyGetDevCaps -> %u  \"%s\"  Knoepfe %u  X %u..%u  Y %u..%u",
                id + 1, r, c.szPname, c.wNumButtons, c.wXmin, c.wXmax, c.wYmin, c.wYmax);
        zeile(z);
        {   unsigned char *b = (unsigned char *)&c; unsigned k; char *p = z;
            for (k = 0; k < sizeof(c); k++) p += sprintf(p, "%02x%s", b[k], (k % 2) ? " " : "");
            zeile(z); }
        memset(&j, 0, sizeof(j));
        r = joyGetPos(id, &j);
        sprintf(z, "Joystick %u: joyGetPos -> %u  X %u  Y %u  Z %u  Knoepfe %04x",
                id + 1, r, j.wXpos, j.wYpos, j.wZpos, j.wButtons);
        zeile(z);
    }
    _lclose(f);
    return 0;
}
