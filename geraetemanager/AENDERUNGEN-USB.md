# Geraetemanager: USB-Geraete, Symbole, Korrekturen (Fassung 1.8)

- USB wie lsusb: Klasse "USB-Geraete", je Bus der Wurzelhub, Geraete unter
  ihrem Hub (lsusb -t).  Quelle: IOCTL_NTU_USBGERAETE des USB-Treibers
  (usbnt 2026.9.27.4+, src/ntusbioctl.h).  Eigenschaften: "Allgemein"
  (Hersteller, Produkt, Seriennummer, VID/PID, bcdDevice, Klasse, USB-
  Fassung, Geschwindigkeit, Bus/Adresse/Hub-Anschluss, Strom) und
  "Schnittstellen" (Klasse, Treiber).  Diagnose: lsusb-Abschnitt.
- Symbole (res/, erzeugt von tools/mkicons.py): Programmsymbol fuer EXE und
  Systemsteuerung, eigene Baumsymbole; klassisches ICO (BMP) mit 256 und
  16 Farben -- PNG-Symbole kann NT 4.0 nicht lesen.  Bildliste ILC_COLOR8.
- Korrektur: PROPSHEETPAGEA-Feld mit dwSize = V1_SIZE -- mit neueren
  Kopfdateien ist die Struktur groesser, comctl32 fand ab der zweiten
  Seite nichts mehr (Reiter fehlten).  Seiten jetzt dicht mit V1-Abstand.
- Korrektur: Fenster 580 x 520 ragte bei 640 x 480 ueber den Rand
  (Knoepfe unsichtbar) -> an den Arbeitsbereich angepasst, Knoepfe 110.
- Textfelder SS_NOPREFIX ("&" in PCI\VEN_..&DEV_.. war Tastenkuerzel).
- Bauen ohne NT-Quellbaum: bau-hier.sh (DDK + mingw + windres);
  kompat/ntos.h, pci.h verweisen auf ntddk.h.  build.sh ist das Original.

# Fassung 1.9: PCI.IDS aktualisieren

- Knopf "PCI.IDS...": Stand der Datei; Ja = aus dem Internet (src/pcidl.h,
  HTTP/1.0 ueber wsock32.dll, erst bei Bedarf geladen -- ohne TCP/IP
  startet der Geraetemanager trotzdem), Nein = aus einer Datei.
  Adresse: HKLM\SOFTWARE\Geraetemanager, PciIdsUrl (Vorgabe
  http://pci-ids.ucw.cz/v2.2/pci.ids).  Nur http:// -- eine Umleitung auf
  https:// wird klar gemeldet (NT 4.0 kann kein TLS 1.2).
- Pruefung vor dem Ersetzen (DlPruefe: Groesse, Kopf, 8086 Intel, Klasse 01);
  neue Datei als .NEU schreiben, alte zu .ALT, umbenennen; dahin, wo die
  benutzte lag, sonst system32.  Danach sofort neu geladen, Baum neu,
  Titel "PCI.IDS vom <Stand>, N Hersteller".
- Fokus nach dem Dialog zurueck in den Baum (das Sperren waehrend des
  Ladens nahm ihn weg; Tastenkuerzel liefen ins Leere).
- Stapel unter 4 KB (grosse Puffer statisch).
- Pruefstand (pruefstand/): dltest.c + server.py pruefen dieselbe
  pcidl.h unter Linux: 200, Umleitung relativ/absolut, auf https, 404,
  zu klein, Schleife, Port zu, Unsinn.

# Fassung 2.0: Deutsch, Englisch, Franzoesisch

src/sprache.h waehlt die Sprache ueber GetUserDefaultLangID() --
also nach Systemsteuerung -> Laendereinstellungen.  Deutsch (0x07) und
Franzoesisch (0x0C, auch Schweiz/Kanada, da nur die Hauptsprache zaehlt)
haben eigene Tabellen, alles andere bekommt Englisch.  Ein fehlender
Eintrag faellt auf Englisch zurueck.
Benutzung im Quelltext:  S(T_SCHLIESSEN)
Umgesetzt: Knoepfe, Geraeteklassen, Eigenschaftenseiten, USB-Texte,
Geschwindigkeiten (45 Stellen).  Meldungstexte und die Diagnose sind
noch deutsch.
Kirgisisch wurde bewusst weggelassen: NT 4.0 kennt die Sprache nicht und
hat auf einem westlichen System keine kyrillische Codepage.
