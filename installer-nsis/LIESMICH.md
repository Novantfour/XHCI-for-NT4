# NSIS-Installer für FreeBSD-USB unter Windows NT 4.0

`usbnt-setup-<Fassung>.exe` ist ein gewöhnlicher Windows-Installer (NSIS 3,
ANSI) und läuft auf Windows NT 4.0. Er bringt den Treiber in allen fünf
Fassungen mit (USBNT, UHCINT, OHCINT, EHCINT, XHCINT), dazu USBINST/USBSETUP,
Gerätemanager, die Reiter für Maus- und Sondertasten und auf Wunsch die
Zusatzprogramme.

## Seiten

1. Willkommen
2. Komponenten: USB-Treiber (immer), Zusatzprogramme (USBTASTEN, USBTEST,
   USBGERAET, USBDRUCK)
3. Einstellungen: Controllerart, Diagnoseausgabe (Aus/COM1/COM2),
   Maustasten 4 und 5, VirtualBox mit USB 3.0
4. Unverträgliche Treiber: Ausgabe von `usbinst pruefen`. Vorgewählt wie in
   USBSETUP: alles außer PS/2. Gibt es keine Funde, entfällt die Seite.
5. Zielordner (Vorgabe `C:\Programme\FreeBSD-USB`)
6. Installation, danach Neustart

## Was er tut

Er trägt selbst nichts in die Registry ein, außer dem Eintrag unter
„Software“ und `XhciSchritt` für VirtualBox. Alles andere macht USBINST.EXE,
dieselbe geprüfte Logik wie bei USBSETUP:

    usbinst abschalten <fund>        je angehakter Fund (alter Zustand gesichert)
    usbinst ein <art> -seriell <n>   Treiber, Dienst, Klassenwerte, Werkzeuge
    usbinst taste 4|5 <belegung>

Die Deinstallation (Systemsteuerung → Software, oder `Deinstallieren.exe`)
ruft `usbinst aus`. Das nimmt Dienst, Klassenwerte und Werkzeuge zurück und
stellt alles wieder her, was beim Einrichten abgeschaltet wurde.

Bei einer erneuten Installation ist die zuletzt gewählte Controllerart
vorgewählt.

## Unbeaufsichtigt

`usbnt-setup-<Fassung>.exe /S` richtet USBNT mit Vorgaben ein (Bericht aus,
Maustasten Zurück/Vorwärts). Unverträgliche Treiber werden dabei **nicht**
abgeschaltet; das geht nur mit Zustimmung auf Seite 4.

## Bauen

    FASSUNG=2026.11.17.66 sh installer-nsis/bau-nsis.sh

Erwartet die fünf Treiber in `bin/<Fassung>/` und die Werkzeuge in
`installer-mit-werkzeugen/`. Braucht `makensis` (NSIS 3; unter Debian/Ubuntu
Paket `nsis`). Das Skript ist in CP1252 gespeichert.
