/*
 *  auswurf.h -- USB-Laufwerk sicher entfernen.
 *  Gemeinsam fuer USBTASTEN.EXE (Menue im Infobereich) und usbgeraet
 *  ("usbgeraet auswerfen X:").  Ohne C-Laufzeit.
 */

#ifndef AUSWURF_H
#define AUSWURF_H

#define AUSWURF_OK        0   /* sicher entfernt; der Treiber meldet es an USBTASTEN */
#define AUSWURF_OFFEN     1   /* noch eine Datei offen, nichts ausgeworfen */
#define AUSWURF_FEHLER    2   /* Oeffnen/Abloesen/Auswerfen gescheitert */

/*  Ist X: ein USB-Laufwerk dieses Treibers?  Dann 1 und das NT-Geraet
 *  (\Device\UsbPlatteN) in ziel.  CDs zaehlen hier nicht (die wirft der
 *  Explorer aus; die Bestaetigung kommt trotzdem).  */
int usb_laufwerk (char lw, char *ziel, int max);

/*  Ausschreiben, sperren, Dateisystem abloesen, dem Treiber den Auswurf
 *  schicken.  Der Treiber fuehrt ihn erst aus, wenn alle vorher gestellten
 *  Lese- und Schreibauftraege fertig sind, schreibt den Puffer des Sticks
 *  aus und laesst danach nichts mehr durch.  fortschritt darf 0 sein.
 *  meldung bekommt bei Fehlern den Grund.  */
int usb_auswerfen (char lw, char *meldung, int max,
                   void (*fortschritt)(const char *text));

#endif
