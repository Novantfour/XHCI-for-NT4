# usbnt – Antwort auf die Rückmeldung von CUPS4NT

Stand: 26.09.2026, `USBNT.SYS` Dateifassung **2026.9.26.1**, Druckerschnittstelle
Fassung 4 (unverändert gegenüber der vorigen Antwort).

## 1. Verträglichkeit

Danke. An der Druckerschnittstelle hat sich seit der geprüften Fassung nichts
geändert.

## 2. Versionsressource – umgesetzt

`USBNT.SYS` (und jede andere Bauart: `uhcint`, `ohcint`, `ehcint`, `xhcint`)
trägt jetzt eine Versionsressource, bei jedem Bau neu erzeugt:

| Feld | Beispiel |
|---|---|
| `FileVersion` / `ProductVersion` | `2026.9.26.1` – Jahr.Monat.Tag.Baunummer |
| `FILEVERSION` (fest, 4 × 16 Bit) | 2026, 9, 26, 1 |
| `FileDescription` | `USB-Treiber (usbnt, Freigabe)` – Bauart und Bauform (Freigabe, Diagnose, Herzschlag) |
| `OriginalFilename` / `InternalName` | `usbnt.sys` / `usbnt` |
| Dateityp | `VFT_DRV` / `VFT2_DRV_SYSTEM` |

Auslesen wie bei jeder Datei: `GetFileVersionInfo` / `VerQueryValue`
(`\StringFileInfo\040704E4\FileVersion` oder `VS_FIXEDFILEINFO`), im Explorer
unter Eigenschaften → Version.

Dieselbe Angabe steht außerdem

- in der Startmeldung auf COM1: `usb: FreeBSD-USB auf NT 4.0, Fassung 2026.9.26.1`,
- in `IOCTL_NTU_VERSION` am Steuergerät (`\\.\UsbNt`), Feld `Bau`:
  `"2026.9.26.1, Sep 26 2026"` – ohne die Datei suchen zu müssen, gilt für den
  **geladenen** Treiber (nach einem Austausch ohne Neustart kann die Datei
  schon neuer sein als der laufende Treiber).

## 3. USBPRN (02) und USBIPP (04) desselben Geräts gleichzeitig

**Treiberseite:** Die beiden sind verschiedene Schnittstellen mit eigenen
Endpunkten, eigenen Warteschlangen und eigenen Fäden. usbnt serialisiert sie
nicht und sperrt keine gegen die andere. Gleichzeitiger Betrieb ist im Nachbau
geprüft; das beweist nur, dass der **Treiber** es kann.

**Geräteseite:** Wie sich echte Drucker verhalten, weiß ich nicht verlässlich.
Die IPP-USB-Spezifikation lässt klassische Schnittstellen neben den IPP-USB-
Schnittstellen zu, legt aber nach meinem Kenntnisstand nicht fest, dass ein Gerät
beide gleichzeitig bedienen muss. Dass einzelne Geräte einen Weg sperren oder
Aufträge verlieren, solange der andere benutzt wird, halte ich für möglich; einen
konkreten Beleg dafür habe ich nicht, und kein echtes IPP-USB-Gerät geprüft.

**Empfehlung für cupsd4nt:** je Gerät **einen** Weg zur Zeit.

- Hat ein Gerät `USBIPPn` (gleiche Seriennummer/VID/PID in `NTUP_INFO`): dort
  drucken und dort den Status holen; `USBPRNn` desselben Geräts nicht parallel
  zum Drucken benutzen.
- Sonst `USBPRNn` wie bisher.
- Wer es an einem echten Gerät wissen will: während eines IPP-Auftrags über
  `USBIPP1` auf `USBPRN1` nur `IOCTL_NTUP_STATUS` abfragen (liest nur, druckt
  nichts) und beobachten, ob der IPP-Auftrag weiterläuft.

Über Befunde an echten Geräten würde ich mich freuen; dann kann usbnt einzelne
Geräte gezielt behandeln (z. B. IPP-Schnittstellen nicht anbieten, solange
`USBPRN` offen ist, oder umgekehrt).

## 4. NTUP_INFO.Aenderungen und IPP-USB

**Ja.** Der Zähler ist gemeinsam für `USBPRNn` und `USBIPPn` und wird beim
Anmelden **und** beim Abmelden **jeder Schnittstelle** erhöht. Ein IPP-USB-Drucker
mit einer klassischen und zwei IPP-Schnittstellen erhöht ihn beim Anstecken um
3 und beim Abziehen um weitere 3. Für cupsd4nt heißt das: Ändert sich der Wert,
die Geräteliste neu einlesen – beide Namensräume (`USBPRN1..8`, `USBIPP1..8`).
