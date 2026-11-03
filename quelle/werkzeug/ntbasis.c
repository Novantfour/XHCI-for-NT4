/*
 *  werkzeug/ntbasis.c -- alles, was sonst die Laufzeitbibliothek taete.
 *
 *  Warum ueberhaupt:
 *
 *  Ein mit der ueblichen Laufzeitbibliothek gebundenes Programm holt
 *  sich printf und Freunde aus msvcrt.dll.  Welche msvcrt.dll auf einer
 *  NT-4.0-Maschine liegt, haengt davon ab, was dort einmal installiert
 *  wurde -- die von SP6a, die eines Office-Pakets, gar keine.  Fehlt
 *  ein Eintrag, startet das Programm nicht, und die Meldung nennt den
 *  fehlenden Namen nicht.  Bei einem Werkzeug, das gerade dann helfen
 *  soll, wenn der Rechner nicht rundlaeuft, ist das die falsche
 *  Abhaengigkeit.
 *
 *  Hier gibt es deshalb keine.  Gebunden wird gegen kernel32 und
 *  advapi32, sonst nichts -- beide sind seit NT 3.1 da.  Die Ausgabe
 *  geht per WriteFile auf den Griff der Standardausgabe.
 *
 *  Der Preis ist ein eigenes printf.  Es kann genau das, was die beiden
 *  Werkzeuge benutzen, und nichts darueber hinaus:
 *
 *      %s  %c  %d  %u  %x  %X  %p  %%
 *      %lu %ld %lx   (l wird gelesen und ignoriert -- unter Win32 ist
 *                     long genau so breit wie int)
 *      Breite und fuehrende Nullen:  %8x  %08lx  %-12s  %2lu
 *
 *  Was es NICHT kann: Gleitkomma, %e, %g, Genauigkeit mit Punkt.  Wer
 *  das braucht, hat die falsche Datei erwischt.
 */

#include <windows.h>

static HANDLE ntb_aus;


/* ==================================================================== *
 *  Zeichenketten -- der Uebersetzer erzeugt Rufe auf memset und memcpy
 *  auch dann, wenn im Quelltext keine stehen.  Sie muessen also da sein.
 * ==================================================================== */

void *
memset (void *z, int c, unsigned int n)
{
    unsigned char *p = (unsigned char *)z;

    while (n-- != 0) {
        *p++ = (unsigned char)c;
    }

    return z;
}

void *
memcpy (void *z, const void *q, unsigned int n)
{
    unsigned char       *d = (unsigned char *)z;
    const unsigned char *s = (const unsigned char *)q;

    while (n-- != 0) {
        *d++ = *s++;
    }

    return z;
}

unsigned int
strlen (const char *s)
{
    const char *a = s;

    while (*s != '\0') {
        s += 1;
    }

    return (unsigned int)(s - a);
}

int
strcmp (const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a += 1;
        b += 1;
    }

    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *
strcpy (char *z, const char *q)
{
    char *a = z;

    while ((*z++ = *q++) != '\0') {
        ;
    }

    return a;
}

char *
strchr (const char *s, int c)
{
    for (; *s != '\0'; s += 1) {
        if (*s == (char)c) {
            return (char *)s;
        }
    }
    return (c == '\0') ? (char *)s : 0;
}

int
strncmp (const char *a, const char *b, unsigned int n)
{
    for (; n != 0; n -= 1, a += 1, b += 1) {
        if (*a != *b || *a == '\0') {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}

char *
strrchr (const char *s, int c)
{
    const char *f = 0;

    for (;;) {

        if (*s == (char)c) {
            f = s;
        }

        if (*s == '\0') {
            break;
        }

        s += 1;
    }

    return (char *)f;
}

int
atoi (const char *s)
{
    int v = 0;
    int m = 1;

    while (*s == ' ' || *s == '\t') {
        s += 1;
    }

    if (*s == '-') {
        m = -1;
        s += 1;
    }

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s += 1;
    }

    return v * m;
}


/* ==================================================================== *
 *  Ausgabe
 * ==================================================================== */

/*  Umleitung: setzt eine DLL-Huelle ntb_puffer, sammelt die Ausgabe dort
 *  (fuer rundll32, das keine Konsole hat) statt sie auf die Standardausgabe
 *  zu schreiben.  Ohne Umleitung unveraendert.  */
char        *ntb_puffer = 0;
unsigned int ntb_puffer_max = 0;
unsigned int ntb_puffer_len = 0;

static void
schreib (const char *p, unsigned int n)
{
    DWORD hin = 0;

    if (ntb_puffer != 0) {
        unsigned int i;
        for (i = 0; i < n && ntb_puffer_len < ntb_puffer_max - 1; i += 1) {
            ntb_puffer[ntb_puffer_len++] = p[i];
        }
        ntb_puffer[ntb_puffer_len] = '\0';
        return;
    }
    if (ntb_aus == 0) {
        ntb_aus = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    WriteFile(ntb_aus, p, n, &hin, NULL);
}

static char *
zahl (char *ziel, unsigned long v, unsigned int basis, int gross)
{
    const char *z = gross ? "0123456789ABCDEF" : "0123456789abcdef";
    char        um[24];
    int         n = 0;

    do {
        um[n++] = z[v % basis];
        v /= basis;
    } while (v != 0);

    while (n != 0) {
        *ziel++ = um[--n];
    }

    return ziel;
}

int
printf (const char *fmt, ...)
{
    char         puffer[1024];
    char        *p = puffer;
    va_list      ap;

    va_start(ap, fmt);

    while (*fmt != '\0') {

        char  feld[64];
        char *f;
        int   breite = 0;
        int   links  = 0;
        int   nullen = 0;
        int   n;

        /*  Rand lassen: das laengste Einzelfeld ist eine Zeichenkette,
         *  und die wird unten auf 64 gekuerzt.  Der Puffer wird
         *  ausgeschrieben, sobald weniger als 80 Byte frei sind --
         *  einmal mehr schreiben ist billiger als ein Ueberlauf.  */

        if ((unsigned int)(p - puffer) > sizeof(puffer) - 80) {
            schreib(puffer, (unsigned int)(p - puffer));
            p = puffer;
        }

        if (*fmt != '%') {

            if (*fmt == '\n') {
                *p++ = '\r';        /* NT-Konsole will CR LF */
            }

            *p++ = *fmt++;
            continue;
        }

        fmt += 1;

        if (*fmt == '-') {
            links = 1;
            fmt += 1;
        }

        if (*fmt == '0') {
            nullen = 1;
            fmt += 1;
        }

        while (*fmt >= '0' && *fmt <= '9') {
            breite = breite * 10 + (*fmt - '0');
            fmt += 1;
        }

        while (*fmt == 'l' || *fmt == 'h') {
            fmt += 1;               /* unter Win32 gleich breit */
        }

        f = feld;

        switch (*fmt) {

            case 'd': {

                long v = va_arg(ap, long);

                if (v < 0) {
                    *f++ = '-';
                    v = -v;
                }

                f = zahl(f, (unsigned long)v, 10, 0);
                break;
            }

            case 'u':
                f = zahl(f, va_arg(ap, unsigned long), 10, 0);
                break;

            case 'x':
                f = zahl(f, va_arg(ap, unsigned long), 16, 0);
                break;

            case 'X':
                f = zahl(f, va_arg(ap, unsigned long), 16, 1);
                break;

            case 'p':
                f = zahl(f, (unsigned long)va_arg(ap, void *), 16, 0);
                break;

            case 'c':
                *f++ = (char)va_arg(ap, int);
                break;

            case 's': {

                const char *s = va_arg(ap, const char *);
                int         k = 0;

                if (s == 0) {
                    s = "(null)";
                }

                /*  Lange Zeichenkette ohne Breitenangabe: Puffer leeren
                 *  und DIREKT schreiben, statt auf 63 Zeichen zu kuerzen
                 *  (usbinst pruefen gibt Pfade und Erklaerungen aus).
                 *  LF wird CR LF, wie im Formattext.  */

                if (breite == 0 && lstrlenA(s) > 63) {
                    schreib(puffer, (unsigned int)(p - puffer));
                    p = puffer;
                    while (*s != '\0') {
                        const char *a = s;
                        while (*s != '\0' && *s != '\n') s += 1;
                        schreib(a, (unsigned int)(s - a));
                        if (*s == '\n') {
                            schreib("\r\n", 2);
                            s += 1;
                        }
                    }
                    break;
                }

                while (*s != '\0' && k < 63) {
                    *f++ = *s++;
                    k += 1;
                }

                break;
            }

            case '%':
                *f++ = '%';
                break;

            default:
                *f++ = '%';
                *f++ = *fmt;
                break;
        }

        if (*fmt != '\0') {
            fmt += 1;
        }

        n = (int)(f - feld);

        if (links) {

            for (f = feld; n != 0; n -= 1) {
                *p++ = *f++;
            }

            for (n = (int)(f - feld); n < breite; n += 1) {
                *p++ = ' ';
            }

        } else {

            int k;

            for (k = n; k < breite; k += 1) {
                *p++ = (char)(nullen ? '0' : ' ');
            }

            for (f = feld, k = 0; k < n; k += 1) {
                *p++ = *f++;
            }
        }
    }

    va_end(ap);
    schreib(puffer, (unsigned int)(p - puffer));
    return 0;
}

int
sprintf (char *z, const char *fmt, const char *a1)
{
    /*  Nur der eine Fall, den usbtest braucht: eine Zeichenkette in ein
     *  Muster mit genau einem %s.  Mehr waere ein zweites printf, und
     *  eines reicht.  */

    char *p = z;

    while (*fmt != '\0') {

        if (fmt[0] == '%' && fmt[1] == 's') {

            while (*a1 != '\0') {
                *p++ = *a1++;
            }

            fmt += 2;
            continue;
        }

        *p++ = *fmt++;
    }

    *p = '\0';
    return (int)(p - z);
}


/* ==================================================================== *
 *  Einsprung
 *
 *  Ohne Laufzeitbibliothek gibt es kein mainCRTStartup, das die
 *  Befehlszeile zerlegt.  Das hier tut genau so viel, wie die beiden
 *  Werkzeuge brauchen: an Leerzeichen trennen, Anfuehrungszeichen
 *  beachten.
 * ==================================================================== */

/*  gcc ruft __main am Anfang von main, um Konstruktoren zu starten.  Es
 *  gibt keine; die Funktion muss trotzdem da sein.  */
void __main (void) { }

#define NTB_MAX_ARG 256

#ifndef NTB_DLL
extern int main(int argc, char **argv);

void
ntb_start (void)
{
    static char  zeile[4096];
    static char *argv[NTB_MAX_ARG + 1];
    int          argc = 0;
    char        *s    = GetCommandLineA();
    char        *d    = zeile;

    ntb_aus = GetStdHandle(STD_OUTPUT_HANDLE);

    /*  Die erste Fassung hatte Platz fuer 23 Argumente und schnitt den
     *  Rest STILLSCHWEIGEND ab.  Aufgefallen ist es an einem
     *  Bulk-Only-Befehlsblock: 31 Byte als Hexzahlen auf der
     *  Befehlszeile, davon kamen 18 an, und das Speichergeraet sperrte
     *  den Endpunkt.  Jetzt 256, und wer darueber kommt, bekommt eine
     *  Meldung statt eines verstuemmelten Aufrufs.  */

    while (*s != '\0' && d < zeile + sizeof(zeile) - 2) {

        if (argc == NTB_MAX_ARG) {
            printf("\n  Zu viele Argumente -- mehr als %d.  Abbruch, statt\n"
                   "  mit abgeschnittener Befehlszeile weiterzumachen.\n\n",
                   NTB_MAX_ARG);
            ExitProcess(2);
        }


        int anf = 0;

        while (*s == ' ' || *s == '\t') {
            s += 1;
        }

        if (*s == '\0') {
            break;
        }

        argv[argc++] = d;

        while (*s != '\0' && (anf || (*s != ' ' && *s != '\t'))) {

            if (*s == '"') {
                anf = !anf;
                s += 1;
                continue;
            }

            *d++ = *s++;
        }

        *d++ = '\0';
    }

    argv[argc] = 0;
    ExitProcess((UINT)main(argc, argv));
}
#endif /* NTB_DLL */
