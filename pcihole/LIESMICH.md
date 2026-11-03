# PCIHOLE -- HTTPS fuer den Geraetemanager (wolfSSL 5.9.1, TLS 1.2)

`PCIHOLE <https://...> <zieldatei>` -- vom Geraetemanager unsichtbar
gestartet; Ausgabe zeilenweise `KB n`, `OK n`, `FEHLER text`.

- Prueft die Zertifikatskette gegen `CACERT.PEM` (neben dem Programm;
  Mozilla-Liste ueber certifi) **und den Rechnernamen**
  (`wolfSSL_check_domain_name`), SNI.  Ohne `CACERT.PEM` wird **nicht**
  geladen -- das mitgelieferte Beispiel `https_get.c` schaltet dann die
  Pruefung ab (`SSL_VERIFY_NONE`) und prueft den Namen gar nicht.
- Umleitungen nur auf https://.  HTTP/1.1 mit `chunked` (HTTP/1.0 lehnen
  manche Server mit 426 ab -- gemessen).
- **Wichtig zu wolfssl_nt4_build.zip:** die Bibliothek importiert
  `strcpy_s`/`strtok_s` aus msvcrt.dll -- die gibt es erst ab VC 2005,
  nicht in NT 4.0 SP6a.  Die beigelegten `https_get.exe`,
  `test_client.exe`, `test_server.exe` starten deshalb unter NT 4.0 nicht.
  `pcihole.c` stellt eigene Fassungen ueber die Importzeiger bereit.

## Geprueft
- Wine 9.0, lokale Server (pruefstand/): gueltig, chunked, HTTPS-Umleitung
  -> byte-gleich; chunked kaputt, Umleitung auf http, falscher Name,
  abgelaufen, selbst ausgestellt, 404 -> abgelehnt mit deutscher Meldung;
  ohne CACERT.PEM -> kein Laden.
- Echtes Netz: raw.githubusercontent.com, 1,7 MB, byte-gleich mit curl
  (in der Bauumgebung ueber einen abfangenden Proxy -- mit der reinen
  Mozilla-Liste wurde der korrekt ABGELEHNT).
- NT 4.0 SP6a (QEMU, ohne Netz): startet, laedt 121 Zertifikate,
  "Rechnername unbekannt"; aus dem Geraetemanager ueber die Pipe.
