# Gerätemanager für Windows NT 4.0 — Quelltext

```
src/devmgr.c      Oberflaeche und Datenauswertung
src/pciids.h      Hersteller- und Geraetenamen aus PCI.IDS
src/pciscan.c     der Hilfstreiber, liest den PCI-Bus
def/              Importdefinitionen
tools/mkpayload.py    bettet den Treiber in die EXE/CPL ein
tools/trimpciids.py   verkleinert PCI.IDS, wenn gewuenscht
build.sh          baut alles
binaries/         Stand VOR dieser Korrektur, siehe unten
```

**Die Dateien in `binaries/` sind der alte Stand.** Dort stuerzt das
Oeffnen der Eigenschaften ab. Sie liegen nur zum Vergleich bei; nach
`./build.sh` stehen die neuen in `obj/`.

---

## Was seit dem letzten Stand geaendert wurde

### Absturz beim Oeffnen der Eigenschaften

`ShowProperties` setzte `PSH_PROPSHEETPAGE` und uebergab gleichzeitig
das Feld der Seitenhandles aus `CreatePropertySheetPageA`. Das Flag
sagt comctl32 aber, dass dort `PROPSHEETPAGE`-**Strukturen** stehen.
comctl32 las die drei Handles als `dwSize`, `dwFlags` und `hInstance`
der ersten Seite und holte `pResource` und `pfnDlgProc` aus dem Stapel
dahinter -- der Sprung auf diesen Zufallszeiger war der Absturz.

Im alten `DEVMGR.EXE` steht es woertlich bei `0x401f84`
(`mov [eax+4],0x88`) und `0x401faa` (`mov [eax+0x20],esp`).

Behoben durch `psh.ppsp = psp`. `CreatePropertySheetPageA` entfaellt
damit ganz -- `PropertySheetA` legt die Seiten selbst an und kopiert
die Strukturen beim Eintritt.

### Unvollstaendige Dialogvorlage

`gTpl` setzte `DS_SETFONT`, lieferte hinter dem Titel aber weder
Punktgroesse noch Schriftnamen. Der Dialogverwalter las beides aus der
Strukturfuellung: Groesse 0, leerer Name. Jetzt steht MS Sans Serif 8
in der Vorlage, und sie ist auf 300 x 200 Dialogeinheiten verbreitert,
damit die Klartextnamen hineinpassen.

### Namen statt Zahlen

`src/pciids.h` liest die uebliche `PCI.IDS` von pci-ids.ucw.cz.
Gesucht wird in dieser Reihenfolge:

```
1. neben der ausfuehrenden Datei
2. system32
3. das Windows-Verzeichnis
```

Die Datei wird einmal ganz eingelesen, Zeilenenden auf Null gesetzt,
UTF-8 auf Latin-1 heruntergerechnet, dann ein Index ueber die
Hersteller- und Klassenzeilen angelegt. Alle zurueckgegebenen Zeiger
zeigen in diesen Puffer und bleiben gueltig -- noetig, weil `BuildTree`
Klassennamen ueber Aufrufgrenzen hinweg vergleicht.

Fehlt die Datei, liefern alle Abfragen NULL und es bleibt bei der
eingebauten Notliste, die von 14 auf rund 50 Hersteller erweitert
wurde. Die Titelzeile zeigt, welche Quelle greift.

Im Baum steht dann `Intel 82801FB/FBM USB2 EHCI Controller` statt
`Intel 8086:265C`. Die Zahlen bleiben als Rueckfall und stehen weiter
auf der Kennungszeile. Neu auf der Karte "Allgemein": Bezeichnung
und, falls vorhanden, das Untersystem.

Die eingebauten deutschen Klassennamen behalten Vorrang -- sie fassen
groeber zusammen und geben einen aufgeraeumteren Baum. Nur wo dort
nichts steht, kommt der Klassenteil aus PCI.IDS.

### Fehlende hal.def

`PCISCAN.SYS` ruft `HalGetBusData`. Das liegt in `hal.dll`, nicht im
Kernel. Das vorige Paket linkte nur `obj/ntoskrnl.lib`, also blieb
`_HalGetBusData@20` offen. `def/hal.def` ist nachgereicht, `build.sh`
erzeugt und linkt `obj/hal.lib`.

### kernel32c.def

Sechs Eintraege dazu: `GetModuleFileNameA`, `GetWindowsDirectoryA`,
`ReadFile`, `GetFileSize`, `VirtualAlloc`, `VirtualFree` -- alles
Serienausstattung jedes NT seit 3.1.

---

## PCI.IDS einrichten

```
https://pci-ids.ucw.cz/v2.2/pci.ids
```

herunterladen und als `PCI.IDS` nach `C:\WINNT\system32\` legen, oder
neben `DEVMGR.EXE`. Rund 1,3 MB. Wer das zu viel findet:

```
python3 tools/trimpciids.py pci.ids PCI.IDS --no-subsys
```

Das laesst die Untersystemzeilen weg -- sie werden nur fuer die Zeile
"Untersystem" auf der Karte "Allgemein" gebraucht -- und bringt die
Datei auf etwa 400 KB.

Die Datei darf reiner Text bleiben; Zeilenenden sind egal, UTF-8 wird
beim Einlesen umgerechnet.

---

## Was er anzeigt

Links ein Baum nach Geraeteklassen, unten rechts die Schaltflaechen.
Doppelklick oder "Eigenschaften" oeffnet ein Blatt mit drei
Registerkarten:

```
Allgemein    Geraetetyp, Hersteller, Bezeichnung, Untersystem, Ort,
             Klassencode, Revision, Kennung, Zustand
Treiber      Dienst, letzter Status, Ladedauer, Fehlschlaege
Ressourcen   Port- und Speicherbereiche mit BAR-Nummer, Unterbrechung
```

Geraete ohne Treiber bekommen ein Warnsymbol.

## Treiber einrichten

Die Schaltflaeche "Treiber einrichten" fragt nach einer `.SYS`-Datei,
kopiert sie nach `system32\drivers` und legt den Dienst an:

```
Type           1     SERVICE_KERNEL_DRIVER
Start          3     SERVICE_DEMAND_START   -- immer, ohne Ausnahme
ErrorControl   1     SERVICE_ERROR_NORMAL
ImagePath            system32\drivers\<datei>.sys
```

Der Dienstname entsteht aus dem Dateinamen ohne Erweiterung, wie es
ueblich ist -- `ATAPI.SYS` wird zu `atapi`.

**Start steht immer auf manuell, und das ist Absicht.** Ein Treiber mit
`Start = 0` oder `1`, der beim Laden bugcheckt, nimmt das System mit
und laesst sich danach nicht mehr abschalten -- man braucht eine
zweite Installation oder eine Reparaturdiskette, um den Eintrag wieder
herauszunehmen. Bei `3` bootet die Maschine in jedem Fall wieder hoch,
und der Eintrag laesst sich in Ruhe aendern oder loeschen.

Nach dem Eintragen wird gefragt, ob der Treiber gleich geladen werden
soll. Klappt das, wird der Baum neu aufgebaut -- ein Treiber, der beim
Laden Bereiche belegt, taucht dann sofort in der ResourceMap auf und
wird seinem Geraet zugeordnet. Klappt es nicht, bleibt der Eintrag
bestehen; er steht auf manuell und wird beim naechsten Start nicht
angefasst.

Wer einen Treiber wirklich beim Start geladen haben will, setzt `Start`
von Hand herunter -- nachdem er einmal von Hand geladen hat und weiss,
dass es gutgeht.

Am Dienstschluessel wird ausserdem notiert, fuer welches Geraet der
Treiber gedacht war:

```
HKLM\SYSTEM\CurrentControlSet\Services\<dienst>
    DevMgrGeraet  REG_SZ  PCI\VEN_10EC&DEV_8168&SUBSYS_...
```

Der Dienstverwalter sieht ueber unbekannte Werte hinweg; am Verhalten
aendert das nichts.

Entfernt wird ein Eintrag noch nicht aus dem Programm heraus. Bis
dahin:

```
net stop <dienst>
```

und den Schluessel unter `Services` loeschen.

## Woher die Angaben kommen

```
PCISCAN.SYS                  der PCI-Bus selbst, ueber HalGetBusData
HKLM\HARDWARE\RESOURCEMAP    welcher Treiber welchen Bereich belegt
Control\BootProfile          ob der Treiber sein Geraet gefunden hat
PCI.IDS                      Namen im Klartext
```

Die ersten beiden sind Serienausstattung. **`Control\BootProfile`
schreibt nur der erweiterte Kernel** (`io/ioinit.c`, Startprofil) --
auf einem unveraenderten SP6a steht auf der Karte "Treiber" dann
"keine Angaben", alles uebrige arbeitet unveraendert. Ein Kernelpatch
ist fuer den Gerätemanager also nicht noetig.

**Die Zuordnung Geraet zu Treiber** entsteht durch Vergleich: der
Treiber hat in der ResourceMap einen Bereich eingetragen, dessen
Anfangsadresse **und** Busnummer mit einer Basisadresse des Geraets
uebereinstimmen. Auf der Vergleichsmaschine passte das fuenfmal von
fuenf, ohne Toleranz.

Die Richtung ist Geraet zu Treiber, nie umgekehrt -- ein Treiber kann
mehrere Geraete bedienen. `Usbd` hing im Test an sechs Controllern,
`UniATA` an zweien.

**Unterbrechungsnummern werden nicht verglichen.** Sie sind auf dieser
Hardware unbrauchbar: mehrere Geraete melden 128, der Tastaturcontroller
meldet 0xFFFFFFFF. Das sind BIOS-Platzhalter; die tatsaechliche
Zuordnung macht die PCI-Routingtabelle.

## Wenn ein geladener Treiber nicht auftaucht

Die Zuordnung hatte bis p06 nur **eine** Beweisquelle: der Treiber
muss in `HARDWARE\RESOURCEMAP` einen Bereich eingetragen haben, dessen
Anfangsadresse zu einer Basisadresse des Geraets passt. Ein Treiber,
der geladen ist, aber nichts eintraegt, war damit unsichtbar -- und
das Programm konnte nicht einmal sagen, welcher der beiden Faelle
vorlag.

Jetzt gibt es drei Quellen, in dieser Rangfolge:

```
Eintrag am Dienstschluessel   DevMgrGeraet mit der PCI-Kennung
ResourceMap, Adresse und Bus  wie bisher, der genaue Fall
ResourceMap, nur Adresse      Busnummer weicht ab
```

Der zweite Durchgang ist neu: manche Treiber tragen ihre Bereiche mit
Busnummer 0 ein, gleichgueltig auf welchem Bus das Geraet sitzt. Das
galt vorher als Fehlschlag. Welcher Weg gegriffen hat, steht auf der
Karte "Treiber" unter "Zuordnung".

Ebenfalls neu auf dieser Karte: **Zustand** (aus dem Dienstverwalter,
`QueryServiceStatus`) und **Start** (aus der Registry). Damit laesst
sich unterscheiden zwischen "kein Treiber da", "eingetragen, aber
nicht geladen" und "geladen, meldet nur nichts an".

## Diagnose

Die Schaltflaeche unten links schreibt `system32\DEVMGR.TXT` mit allem,
was gelesen wurde:

```
jedes Geraet mit Ort, Kennung, Klasse und allen Basisadressen
jeder Bereich aus der ResourceMap mit Treibername, Bus und Anfang
jeder Dienst, der einen DevMgrGeraet-Vermerk traegt, mit Start
  und Zustand
```

Damit laesst sich die Frage "der Treiber ist doch geladen, warum
steht er nicht da" beantworten statt vermuten. Taucht der Treiber
unter den Bereichen gar nicht auf, traegt er nichts ein -- dann hilft
nur der `DevMgrGeraet`-Vermerk. Taucht er auf, vergleicht man seinen
Anfang mit den Basisadressen des Geraets und sieht sofort, woran es
scheitert.

## Grenzen, die der Manager selbst nennt

Geraete **ausserhalb** des PCI-Busses erscheinen nicht -- Tastatur,
serielle Schnittstelle, Diskettenlaufwerk. Das ist keine Schwaeche der
Anzeige, sondern des Verfahrens: ohne laufenden Plug-and-Play-Manager
gibt es keine vollstaendige Geraeteliste, nur die Busse, die man selbst
absuchen kann.

Steht bei einem Geraet kein Treiber, heisst das entweder "keiner
installiert" oder "hat keine Bereiche belegt". Beides steht so auf der
Karte "Treiber".

## Einrichten

```
DEVMGR.CPL  nach  C:\WINNT\system32\
PCI.IDS     nach  C:\WINNT\system32\
```

Danach steht "Geraetemanager" in der Systemsteuerung. Zum Entfernen
genuegt Loeschen -- `sysdm.cpl` wird nicht angefasst.

`DEVMGR.EXE` ist dieselbe Uebersetzungseinheit als Programm.
`PCISCAN.SYS` **nicht** kopieren: er steckt in beiden Dateien und wird
beim Aufruf selbst abgelegt und wieder entfernt.

Administratorrechte sind noetig, sonst laesst sich der Hilfstreiber
nicht laden.

## Bauen

```
NT=/pfad/zum/nt4-baum ./build.sh
```

Gebraucht werden clang-18, lld-18, llvm-18 und der NT-4.0-Quellbaum
fuer die Kopfdateien.

### Stellen, an denen es leicht schiefgeht

**Der Treiber darf keine 0x40-Ausrichtung bekommen.** `/align:0x40`
gilt nur fuer den Kernel. Treiber brauchen die Vorgaben 0x1000 / 0x200
-- sonst bildet der Speichermanager das Abbild als eine einzige
Untersektion ab, und eine `.data` ohne Rohdaten hat dort keine
Entsprechung. Ergebnis: `STATUS_INVALID_IMAGE_FORMAT`
(`mm/creasect.c`, Zeile 1615).

**Der Treiber braucht hal.lib.** `HalGetBusData` liegt in `hal.dll`.
Wer nur `ntoskrnl.lib` linkt, bekommt ein offenes Symbol.

**Der Export der CPL.** `/export:CPlApplet@16` wird abgewiesen, weil
clang das Symbol als `_CPlApplet@16` ablegt. Richtig ist

```
/export:CPlApplet=_CPlApplet@16
```

Dann steht der Name unverziert in der Tabelle, und genau so sucht ihn
die Systemsteuerung.

**`src/drvpayload.h` liegt nicht im Paket.** Sie wird von
`tools/mkpayload.py` aus dem frisch gebauten `PCISCAN.SYS` erzeugt;
`build.sh` macht das in Schritt 2.

**Die Importdefinitionen sind kein eigener kernel32.** `kernel32c.def`
beschreibt das stocknormale `KERNEL32.DLL`; das `c` ist reine
Dateizaehlung, weil im Gesamtbaum drei verschiedene Ausschnitte
nebeneinanderliegen. Im fertigen Abbild steht `KERNEL32.dll`.

## Aufbau des Quelltexts

```
  1- 60   Kopf, Konstanten
 60-120   Ersatz fuer die C-Laufzeit (slen, scpy, scat, scatlim, hexinto)
120-190   PCI_ENTRY, die Geraeteliste
190-260   ResourceMap lesen, CM_RESOURCE_LIST zerlegen
260-300   Match: Geraet zu Treiber
300-420   Klassen-, Hersteller- und Geraetenamen
420-490   Hilfstreiber laden, befragen, entfernen
490-520   Startprofil eines Dienstes lesen
520-620   Baum aufbauen
620-840   Eigenschaftenblatt, drei Registerkarten
840-910   Hauptfenster
910-1050  Systemsteuerungsschnittstelle (CPlApplet) und WinMain
```

Ohne C-Laufzeit uebersetzt, damit es auf einem NT 4.0 ohne
Laufzeitbibliotheken laeuft. Die Dialogseiten kommen ohne
Ressourcendatei aus: eine `DLGTEMPLATE` im Speicher, Steuerelemente zur
Laufzeit erzeugt.

## Offen

```
Treiber wieder entfernen -- geht bisher nur ueber net stop und Regedit
eigene Symbole statt der Standardsymbole des Systems
Geraete ausserhalb des PCI-Busses -- braucht PnP
Aktualisieren liest nur die ResourceMap neu, nicht die Geraeteliste
auf echter Hardware ist dieser Stand noch nicht gelaufen
```

---

## Anmerkung zu diesem Bau

`DEVMGR.EXE` und `DEVMGR.CPL` in `binaries/` sind mit clang 18 und
lld 18 gebaut, aber **gegen die mingw-w64-Kopfdateien** statt gegen den
NT-4.0-SDK-Baum -- der lag beim Bau nicht vor. Uebersetzt wurde mit

```
clang --target=i686-w64-mingw32 -Os -fno-builtin -std=gnu89 \
  -DWIN32=100 -D_WIN32_WINNT=0x0400 -DWINVER=0x0400 -D_WIN32_IE=0x0300
```

und gelinkt mit demselben `lld-link` und denselben Schaltern wie in
`build.sh`. Das Ergebnis ist eine reine Win32-Binaerdatei ohne
C-Laufzeit, Subsystemversion 4.00, ohne einen einzigen Import
ausserhalb der fuenf gewohnten DLLs.

Damit das zulaessig ist, wurden die Strukturlayouts vorher gegen die
NT-4.0-Werte geprueft -- als Uebersetzungszeitbedingungen, nicht durch
Augenschein:

```
TV_ITEM            40 Bytes, lParam bei 36
TV_INSERTSTRUCT    item bei 8
NM_TREEVIEW        itemOld bei 16, itemNew bei 56
PROPSHEETPAGEA     V1 40 Bytes, pszTitle 20, pfnDlgProc 24
PROPSHEETHEADERA   V1 40 Bytes, pszCaption 20, nPages 24, ppsp 32
INITCOMMONCONTROLSEX  8, WNDCLASSA 40, MSG 28, SERVICE_STATUS 28
TVN_SELCHANGED     0u-402u   (ANSI-Fassung)
```

Dazu kommt `OPENFILENAME_SIZE_VERSION_400` = 76 fuer den
Dateiauswahldialog -- neuere Kopfdateien haengen auch dort Felder an,
und NT4s comdlg32 weist eine unbekannte Groesse mit `CDERR_STRUCTSIZE`
ab. Im Abbild steht `mov DWORD PTR [eax],0x4c`, also 76.

Alle stimmen. Die drei `_V1_SIZE`-Makros stehen im Quelltext, weil
neuere Kopfdateien an beide Strukturen Felder anhaengen; der
NT-4.0-Baum kennt die Makros nicht, dort faellt der Ausdruck auf
`sizeof` zurueck.

Wer den NT-4.0-Baum zur Hand hat, sollte trotzdem mit `build.sh` neu
bauen -- das ist der gemeinte Weg, und nur dort wird auch
`PCISCAN.SYS` frisch erzeugt.
