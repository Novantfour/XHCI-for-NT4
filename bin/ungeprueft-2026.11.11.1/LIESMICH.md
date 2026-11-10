# Neubau 2026.11.11.1 — ungeprüft

Gebaut aus dem Quellstand dieses Repos (clang-cl/lld-link gegen das
NT-4.0-DDK für die Treiber, mingw ohne C-Laufzeit für die Programme).
`pruefe-bau.py` meldet für alle 32 Dateien keine Mängel: Abschnitte,
PE-Prüfsummen, Versionsangaben, keine NT-4.0-fremden Importe.

**Nicht gelaufen.** Weder im Prüfstand noch an einer echten NT-4.0-
Maschine. Die Installer-Ordner enthalten deshalb weiter die Fassung
2026.10.11.4.

Gegenüber 2026.10.11.4 geändert ist im Code nur der Audio-Teil
(`usbaudio.sys`, `usbaudio.dll`) — siehe `AENDERUNGEN.md`. Alle anderen
Dateien sind derselbe Code, neu gebaut; die Programme haben jetzt
durchgehend eine Versionsangabe. Neu sind `hid.dll`, `hidtest.exe` und
`diprobe.exe` aus dem Bauskript.

Vor der Übernahme in die Installer: Audio am echten Gerät prüfen
(kurzer Klang allein, zwei Quellen zugleich, Klang nach mehr als zwei
Sekunden Pause, Gerät während der Wiedergabe abziehen).
