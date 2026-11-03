# Bauen mit clang-cl und lld-link

```
pruef/bau-clangcl.sh                 usbnt.sys   alle vier Bauarten
NUR=xhci pruef/bau-clangcl.sh        xhcint.sys  nur xHCI
```

Gebaut wird gegen das **echte NT-4.0-DDK** -- seine Kopfdateien, seine
Bibliotheken. Drei Stellen mussten dafuer angefasst werden, und jede
einzelne ist hier begruendet.

## 1. Vier Kopfdateien aus dem Win32-SDK fehlen

Das DDK setzt das SDK voraus; auf dieser Maschine liegt es nicht. Ersatz
in `pruef/sdk-ersatz/`:

```
excpt.h     Rumpf.  Der Treiber benutzt keine strukturierte
            Ausnahmebehandlung -- try/except gibt es im ganzen Baum
            nicht.  Wer spaeter __try schreibt, ersetzt diese Datei
            durch die echte, statt sie zu erweitern.
ctype.h     Rumpf, plus wchar_t.  MSVC definiert wchar_t hier und nicht
            in stddef.h; ntdef.h verlaesst sich darauf.
string.h    Die Prototypen, die ntddk.h selbst braucht.
pshpack1/2/4/8.h, poppack.h
            NICHT leer.  Diese steuern die Strukturpackung; ein leerer
            Rumpf wuerde jede Lage im DDK verschieben, ohne dass
            irgendwo ein Fehler steht.  Inhalt ist genau
            #pragma pack(push, N) bzw. #pragma pack(pop).
```

## 2. Eine Zeile in ntddk.h

Die einzige Stelle im ganzen DDK, die clang-cl nicht uebersetzt:

```c
__inline ULONG KeGetCurrentProcessorNumber(VOID)
{
    __asm {  movzx eax, _PCR KPCR.Number  }     /* _PCR ist fs:[0] */
}
```

clang kann dieser Form die Operandenbreite nicht ansehen und meldet
*ambiguous operand size*. Ersetzt durch

```c
return (ULONG)__readfsbyte(FIELD_OFFSET(KPCR, Number));
```

Die Lage wird also weiterhin vom Uebersetzer aus `KPCR` gerechnet und
nicht von Hand eingesetzt. Das ist der Punkt, an dem eine eingetippte
Zahl still falsch waere. Die geaenderte Fassung liegt in
`inc-patched/`; das Original bleibt unberuehrt.

## 3. Die Einfuhrbibliotheken mussten neu

`NTOSKRNL.LIB` und `HAL.LIB` des DDK lassen sich mit `lld-link` nicht
binden:

```
lld-link: error: section larger than 4 GiB: .data
```

Sie legen `.idata$4` und `.idata$5` als COMMON-Symbole mit der Groesse
`c0000040` an. Das ist in Wahrheit der Abschnittskennwert, den MSVC 4 an
dieser Stelle missbraucht hat; lld nimmt die Zahl woertlich.

`einfuhrlib.py` liest die Namen aus denselben Bibliotheken und bindet
sie mit `llvm-dlltool --kill-at` neu:

```
  ntoskrnl  1011 Namen, davon 32 fastcall
  hal         90 Namen, davon 11 fastcall
```

Fastcall zaehlt mit. NT 4.0 hat `@KfAcquireSpinLock@4` und Verwandte
eingefuehrt; sie tragen ein fuehrendes `@` und wuerden von einem
Ausdruck, der nur auf `__imp__` passt, uebersehen -- und dann fehlen
beim Binden genau die Sperren.

Dazu `kiserial.lib` aus `pruef/kiserial.def`: `KiSerialPutString` ist der
1017. Export des selbstgebauten Kernels und steht in keiner
`NTOSKRNL.LIB`, die Microsoft ausgeliefert hat. Wer den Treiber auf
einem unveraenderten NT 4.0 SP6a laden will, baut mit
`-DNTU_OHNE_KISERIAL` und bekommt `DbgPrint` -- ohne angehaengten
Kerndebugger geht die Ausgabe dann ins Leere.

## Schalter, die nicht verhandelbar sind

```
/Gz          stdcall als Vorgabe.  Ohne das heisst der Einsprung
             _DriverEntry statt _DriverEntry@8, und der Binder findet
             ihn nicht.
__cdecl      an den NTOSKRNL-Zeichenkettenfunktionen.  Mit /Gz wuerde
             sonst __vsnprintf@16 verlangt, und die Bibliothek hat
             _vsnprintf.
/nodefaultlib Keine Laufzeitbibliothek.  Daher auch keine 64-Bit-
             Division: __aulldiv gibt es nicht.
/merge:NTUDRV=.rdata
             Sonst kostet die Treiberanmeldeliste eine eigene Seite.
/subsystem:native,4.00
/entry:DriverEntry@8
```

## Ergebnis

```
usbnt.sys    155 136 Byte   32 Objekte
xhcint.sys   103 424 Byte   26 Objekte
beide        PE32, Subsystem native 4.0, Basis 00010000
             Einfuhren ausschliesslich ntoskrnl.exe und hal.dll
```

## Zwei Fehler, die erst die Maschine gezeigt hat

Beide Treiber liessen sich zunaechst nicht starten:

```
0xC0000221   STATUS_IMAGE_CHECKSUM_MISMATCH
```

### Die Pruefsumme fehlte

NT 4.0 prueft bei Kerntreibern die Pruefsumme im PE-Kopf. `lld-link`
laesst sie ohne `/release` auf Null, und eine Null ist keine gueltige
Pruefsumme. Der Treiber war dabei vollkommen in Ordnung -- er wurde nur
nicht angesehen, und die Meldung sagt dazu nichts.

`/release` setzt sie. Nachgerechnet wird trotzdem, mit
`pruefsumme.py`: sich auf einen Schalter zu verlassen ist billiger, als
ihn zu pruefen, aber nicht sicherer.

Das Verfahren ist seit NT 3.1 unveraendert -- 16-Bit-Einerkomplement
ueber die ganze Datei plus Dateigroesse, wobei das Pruefsummenfeld
selbst als Null zaehlt. Es ist **vier** Byte breit, also **zwei**
Woerter; eines davon zu ueberspringen ergibt eine Summe, die genau um
den oberen Halbteil danebenliegt. Der erste Anlauf hatte exakt diesen
Fehler und wich um 2 ab.

### /align:32 war falsch

Der zweite Fehler wurde erst beim Nachsehen sichtbar und haette auch mit
richtiger Pruefsumme zugeschlagen:

```
SectionAlignment    32
FileAlignment      512
```

Das ist ein ungueltiges Abbild. Ist die Abschnittsausrichtung kleiner
als eine Seite, muss die Dateiausrichtung denselben Wert haben -- so
funktioniert der alte `/ALIGN:0x20`-Kniff aus der NT-3.x-Zeit. Halb
angewandt ergibt er ein Abbild, das der Lader nicht abbilden kann.

Ohne den Schalter nimmt lld die ueblichen 4096 und 512. Der Treiber
wird dadurch nicht spuerbar groesser, und das Abbild stimmt.

Nachgesehen wurden bei der Gelegenheit gleich alle Kopffelder, die der
Lader prueft:

```
Merkmale 0102     EXECUTABLE_IMAGE | 32BIT_MACHINE
                  RELOCS_STRIPPED NICHT gesetzt -- ein Treiber ohne
                  Verschiebungen laesst sich nicht laden
Subsystem  1      native
Fassung  4.0
Einfuhren         nur ntoskrnl.exe und hal.dll
.reloc            vorhanden
```

### KiSerialPutString wurde nicht gefunden

```
Der Treiber laedt nicht, der Lader vermisst KiSerialPutString@4.
```

Der Kern fuehrt ihn **undekoriert**. Nachgesehen im Exportverzeichnis
von `ntoskrpn.exe` selbst: 1017 Namen, darunter genau
`KiSerialPutString` -- ohne `@4`. So halten es alle NT-Exporte; die
Dekoration entsteht nur auf der Rufseite, weil der Ruf stdcall ist.

`llvm-dlltool` braucht deshalb `-k` (`--kill-at`), genau wie bei
ntoskrnl und hal. Bei den beiden war es von Anfang an dran, bei
kiserial.lib fehlte es -- und dann sucht der Lader einen Namen, den es
nicht gibt, und nennt in der Meldung nur den verzierten.

### Gegengeprueft: jeder Import gegen den echten Kern

```
usbnt.sys    ntoskrnl.exe  48 Namen   im Kern nicht gefunden: 0
             hal.dll       18 Namen
xhcint.sys   ntoskrnl.exe  47 Namen   im Kern nicht gefunden: 0
             hal.dll       18 Namen
```

Geprueft wurde gegen das Exportverzeichnis von `ntoskrpn.exe`, nicht
gegen eine Liste. Die achtzehn HAL-Namen stehen in der `HAL.LIB` des
DDK und damit in jeder NT-4.0-x86-HAL; sie sollten trotzdem einmal
gegen die tatsaechlich eingespielte `hal.dll` gehalten werden, falls
dort eine Fremd-HAL liegt.

### Zum Modulnamen

Der interne Modulname im Exportverzeichnis lautet `ntoskrpn.exe`, weil
der Dateiname hoechstens acht Zeichen haben darf. Die Treiber fuehren
trotzdem `ntoskrnl.exe` in ihrer Einfuhrtabelle -- so macht es jeder
NT-Treiber, und deshalb laeuft NT auch mit `/KERNEL=ntkrnlmp.exe`.
Sollte der Lader den Namen wider Erwarten woertlich nehmen, steht die
Stellschraube in `einfuhrlib.py` und in `pruef/kiserial.def`: dort den
Namen hinter `LIBRARY` und hinter `-D` auf `ntoskrpn.exe` setzen.

### Bluescreen in HAL.DLL bei 800650f6

Die Adresse loest sich sauber auf. HAL-Basis 80062000, also RVA 30f6,
und das liegt in

```
000030de  HalAllocateCommonBuffer   + 0x18
```

Die Stelle selbst, aus der hochgeladenen HAL herausgeschnitten:

```
800650f3   mov 0x8(%ebp),%edi     ; der erste Parameter, AdapterObject
800650f6   cmp %al,0x56(%edi)     ; hier knallt es
```

`HalAllocateCommonBuffer` greift den Adapter im vierten Befehl an, ohne
ihn zu pruefen. Wir hatten NULL uebergeben.

**Die Ursache war `bus_get_dma_tag`.** Sie stand seit Schritt 4 als
leerer Rumpf in der Liste des Offenen und gab NULL zurueck. Von dort
lief NULL durch `bus_dma_tag_create` in `t->Adapter` und weiter in die
HAL. Der USB-Stapel ruft sie ueber `USB_GET_DMA_TAG(self)` in jedem der
vier Hostbaustein-Treiber.

Jetzt holt sie einen echten Adapter:

```c
dd.Master            = TRUE;     der Baustein holt selbst
dd.ScatterGather     = TRUE;     kein einzelner Kanal
dd.Dma32BitAddresses = TRUE;     32 Bit, siehe sys/types.h
dd.InterfaceType     = PCIBus;
dd.BusNumber         = iv->Bus;
dd.MaximumLength     = 0x20000;
HalGetAdapter(&dd, &karten)
```

Einer je Hostbaustein, im PCI-Knoten abgelegt, beim ersten Zugriff
geholt.

Dazu zwei Sperren, damit derselbe Fehler nicht noch einmal in der HAL
landet statt im Treiber: `bus_dma_tag_create` weist einen Rahmen ohne
Adapter ab, und `bus_dmamem_alloc` ruft die HAL gar nicht erst, wenn
der Adapter fehlt. Beide schreiben stattdessen eine Zeile in den
Bericht. Die HAL prueft nicht, und das ist richtig so -- ein Treiber,
der ihr NULL reicht, hat einen Fehler, und den soll sie nicht
verstecken.

### Einfuhren jetzt gegen die echten Binaerdateien

```
                ntoskrnl.exe        hal.dll
usbnt.sys       48 Namen, 0 fehlen  19 Namen, 0 fehlen
xhcint.sys      47 Namen, 0 fehlen  19 Namen, 0 fehlen
```

Geprueft gegen das Exportverzeichnis von `ntoskrpn.exe` (1017 Namen) und
gegen die eingespielte `HAL.DLL` (90 Namen, Zeitstempel 36c49ac6) --
nicht gegen eine Liste und nicht gegen das DDK.
