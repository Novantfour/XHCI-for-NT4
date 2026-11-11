# HID für Windows NT 4.0

NT 4.0 hat keinen HID-Stapel: `hidclass.sys`, `hidusb.sys` und `hid.dll`
kamen erst mit Windows 98/2000. Programme, die HID erwarten — allen voran
DirectInput ab Fassung 5 — finden nichts vor.

`HID.DLL` legt die **originalen** Schnittstellen darüber. Ein eigener
Treiber ist nicht nötig: unser USB-Stapel kann schon alles und bietet es
über `\\.\UsbGenN` an.

## Stand

Die Quellen liegen in `quelle/werkzeug/` (`hiddll.c`, `hid.def`,
`hidtest.c`) und werden von `bau-werkzeuge.sh` mitgebaut -- hier liegt
bewusst keine zweite Kopie.  Gebaute Dateien:
`bin/ungeprueft-2026.11.11.2/programme/hid.dll` und `hidtest.exe`
(beide ohne C-Laufzeit, mit Versionsangabe).  **Noch nicht auf NT 4.0
gelaufen.**

## Was HID.DLL bietet

Alle Funktionen tragen die Originalnamen, mit und ohne Dekoration:

- `HidD_GetHidGuid`, `HidD_GetAttributes`
- `HidD_GetManufacturerString`, `HidD_GetProductString`, `HidD_GetSerialNumberString`
- `HidD_GetPreparsedData`, `HidD_FreePreparsedData`
- `HidD_GetFeature`, `HidD_SetFeature`, `HidD_SetOutputReport`
- `HidD_SetNumInputBuffers`, `HidD_FlushQueue`
- `HidP_GetCaps`, `HidP_GetButtonCaps`, `HidP_GetValueCaps`
- `HidP_GetUsages`, `HidP_GetUsageValue`, `HidP_MaxUsageListLength`
- `HidD_GetInputReport`
- `HidP_GetData`, `HidP_SetData`
- `HidP_GetUsagesEx`, `HidP_GetScaledUsageValue`
- `HidP_GetSpecificButtonCaps`, `HidP_GetLinkCollectionNodes`
- `HidP_MaxDataListLength`

Die zweite Gruppe kam auf Rueckmeldung aus dem DirectInput-Zweig dazu.
`HidP_GetData` ist dabei die wichtigste: darueber liest DirectInput alle
Eingabeberichte, ohne sie geht nichts.  Knoepfe erscheinen dort nur, wenn
sie gedrueckt sind, Werte immer mit ihrem Rohwert -- wie im Original.

Zu `HidP_GetLinkCollectionNodes`: wir fuehren bisher **eine** oberste
Collection und melden genau die.  Fuer Geraete mit mehreren Collections
(etwa ein Gamepad mit eigenem Kopfhoererteil) muesste der Deskriptorleser
den Collection-Baum mitfuehren -- noch offen, bisher nicht gebraucht.

Der Berichtsdeskriptor wird in `hiddll.c` selbst ausgewertet (Haupt-,
globale und lokale Posten), weil genau das die `HidP_*`-Funktionen nach
außen sichtbar machen.

## Die eine Abweichung vom Original

Unter Windows 98/2000 findet man HID-Geräte über `SetupDiGetClassDevs`
mit `GUID_DEVINTERFACE_HID`. Das sind Geräteschnittstellen aus Plug and
Play, die es auf NT 4.0 nicht gibt; die `setupapi.dll` von NT 4.0 kann
das nicht. Stattdessen:

    HidNt_Enumerate(char *puffer, ULONG *n)

liefert die Pfade aller HID-Schnittstellen, durch Null getrennt, am Ende
eine zweite Null — dieselbe Form, die SetupAPI-Aufrufer ohnehin
verarbeiten. **Genau ein Aufruf Unterschied; ab `CreateFile` ist alles
original.**

Dasselbe Verfahren nutzt der Gerätemanager, um PCI- und USB-Geräte ohne
Plug and Play aufzulisten (siehe PCISCAN).

## HIDTEST.EXE

    hidtest                 gefundene Geräte auflisten
    hidtest N               Gerät N ausführlich (Caps, Knöpfe, Werte)
    hidtest N lesen [S]     S Sekunden Berichte mitlesen
    
Benutzt ausschließlich `HID.DLL`, also genau den Weg, den auch
DirectInput nehmen würde. Scheitert etwas hier, scheitert es dort genauso.

## Noch zu tun

1. ~~`hidtest.c` ohne C-Laufzeit bauen~~ — erledigt (2026.11.11).
2. ~~In `bau-werkzeuge.sh` aufnehmen~~ — erledigt.
3. Am echten Headset prüfen: Schnittstelle 3 (HID, Endpunkt 0x83) muss
   auftauchen, und `hidtest N lesen` muss beim Drehen am Lautstärkerad
   Berichte zeigen. Damit wäre belegt, wo das Rad meldet.
4. Erst danach: HID-Klassenschicht im Treiber, falls fremde Programme
   Geräte unter originalen Namen (`\\.\HID#VID_...`) öffnen sollen.
5. Collection-Baum im Deskriptorleser, falls ein Gerät mehrere
   Collections hat.

## Zur Geräteaufzählung — offener Punkt aus dem DirectInput-Zweig

DirectInput zählt Geräte über SetupAPI auf: `SetupDiGetClassDevs`,
`SetupDiEnumDeviceInterfaces`, `SetupDiGetDeviceInterfaceDetail`,
`SetupDiOpenDevRegKey`, dazu `CM_Get_*`. Auf NT 4.0 gibt es davon nur
Bruchstücke, und Geräteschnittstellen kennt es gar nicht.

`HidNt_Enumerate` ersetzt die ersten drei Aufrufe. Offen bleibt
`SetupDiOpenDevRegKey`, über das DirectInput je Gerät einen
Registrierungsschlüssel öffnet (Kalibrierung, Typname). Wenn das
gebraucht wird, wäre der saubere Weg ein `HidNt_OpenDevRegKey`, das
einen Schlüssel unter einem eigenen Pfad liefert, etwa
`HKLM\SYSTEM\CurrentControlSet\Services\usbnt\HID\VID_xxxx&PID_xxxx`.
Sag Bescheid, dann baue ich es dazu.
