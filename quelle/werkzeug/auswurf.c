/*
 *  auswurf.c -- USB-Laufwerk sicher entfernen (siehe auswurf.h).
 *
 *  NT 4.0 kennt kein "Hardware sicher entfernen".  Das Dateisystem haelt
 *  Geschriebenes im Cache; wer den Stick zu frueh zieht, verliert Daten
 *  oder beschaedigt die FAT.  Die Schritte:
 *
 *    FlushFileBuffers          alles Gepufferte auf den Stick
 *    FSCTL_LOCK_VOLUME         niemand hat mehr etwas offen
 *    FSCTL_DISMOUNT_VOLUME     das Dateisystem gibt den Datentraeger frei
 *    IOCTL_DISK_EJECT_MEDIA    an den Treiber (umass): wartet, bis alle
 *                              Auftraege davor fertig sind, SYNCHRONIZE
 *                              CACHE, anhalten, danach kein Befehl mehr --
 *                              und erst dann die Meldung "kann gezogen
 *                              werden" an USBTASTEN.
 *
 *  Scheitert das Sperren, ist noch eine Datei offen -- dann ist zwar
 *  ausgeschrieben, aber nichts ausgeworfen.
 */

#include <windows.h>
#include "auswurf.h"

#define FSCTL_LOCK_VOLUME_NT       0x00090018
#define FSCTL_DISMOUNT_VOLUME_NT   0x00090020
#define IOCTL_DISK_EJECT_MEDIA_NT  0x00074808

static void
melden (void (*f)(const char *), const char *t)
{
    if (f != 0) {
        f(t);
    }
}

int
usb_laufwerk (char lw, char *ziel, int max)
{
    char name[3];
    char z[260];

    name[0] = lw; name[1] = ':'; name[2] = '\0';
    if (QueryDosDeviceA(name, z, sizeof(z)) == 0) {
        return 0;
    }
    /*  "\Device\UsbPlatte3" -- so legt umass Platten und Sticks an.  */
    if (CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, z, 17,
                       "\\Device\\UsbPlatte", 17) != 2) {
        return 0;
    }
    if (ziel != 0 && max > 0) {
        lstrcpynA(ziel, z, max);
    }
    return 1;
}

int
usb_auswerfen (char lw, char *meldung, int max,
               void (*fortschritt)(const char *text))
{
    char   pfad[8], t[200];
    HANDLE h;
    DWORD  n;
    int    k;

    if (meldung != 0 && max > 0) {
        meldung[0] = '\0';
    }

    pfad[0] = '\\'; pfad[1] = '\\'; pfad[2] = '.'; pfad[3] = '\\';
    pfad[4] = lw;   pfad[5] = ':';  pfad[6] = '\0';

    h = CreateFileA(pfad, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wsprintfA(t, "%c: laesst sich nicht oeffnen (Fehler %lu).", lw, GetLastError());
        if (meldung) lstrcpynA(meldung, t, max);
        return AUSWURF_FEHLER;
    }

    wsprintfA(t, "%c: ausschreiben ... %s", lw, FlushFileBuffers(h) ? "ok" : "Fehler");
    melden(fortschritt, t);

    /*  Sperren bis zu zehnmal im Sekundentakt.  Nach dem Schliessen einer
     *  Datei haelt der Cache-Manager noch einige Sekunden einen Verweis,
     *  bis sein verzoegertes Schreiben durch ist; so lange meldet FastFat
     *  "in Benutzung".  Im Test: nach sechs Sekunden ging es.  */
    for (k = 0; k < 10; k += 1) {
        if (DeviceIoControl(h, FSCTL_LOCK_VOLUME_NT, NULL, 0, NULL, 0, &n, NULL)) {
            break;
        }
        if (k == 0) {
            wsprintfA(t, "%c: sperren ... warte auf den Cache", lw);
            melden(fortschritt, t);
        }
        Sleep(1000);
        FlushFileBuffers(h);
    }
    if (k == 10) {
        wsprintfA(t, "%c: ist noch in Benutzung (Fehler %lu).\r\n\r\n"
                     "Alles Geschriebene ist auf dem Laufwerk, aber eine Datei oder ein "
                     "Fenster ist noch offen. Bitte schliessen und noch einmal "
                     "versuchen. Noch NICHT herausziehen.", lw, GetLastError());
        if (meldung) lstrcpynA(meldung, t, max);
        CloseHandle(h);
        return AUSWURF_OFFEN;
    }
    wsprintfA(t, "%c: sperren ... ok", lw);
    melden(fortschritt, t);

    if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME_NT, NULL, 0, NULL, 0, &n, NULL)) {
        wsprintfA(t, "%c: Dateisystem laesst sich nicht abmelden (Fehler %lu).\r\n\r\n"
                     "Noch NICHT herausziehen.", lw, GetLastError());
        if (meldung) lstrcpynA(meldung, t, max);
        CloseHandle(h);
        return AUSWURF_FEHLER;
    }
    wsprintfA(t, "%c: Dateisystem abmelden ... ok", lw);
    melden(fortschritt, t);

    /*  Der Treiber wartet, bis alles davor Gestellte durch ist, und
     *  antwortet erst dann.  Scheitert es (alter Treiber: kennt den
     *  Auswurf fuer Platten nicht), ist das Dateisystem trotzdem
     *  abgemeldet -- dann sagt das Werkzeug das selbst.  */
    if (!DeviceIoControl(h, IOCTL_DISK_EJECT_MEDIA_NT, NULL, 0, NULL, 0, &n, NULL)) {
        DWORD f = GetLastError();
        wsprintfA(t, "%c: Auswurf beim Treiber: Fehler %lu (alter Treiber?). "
                     "Das Dateisystem ist abgemeldet.", lw, f);
        melden(fortschritt, t);
    } else {
        wsprintfA(t, "%c: Treiber: alle Auftraege fertig, Geraet angehalten", lw);
        melden(fortschritt, t);
    }

    CloseHandle(h);
    return AUSWURF_OK;
}
