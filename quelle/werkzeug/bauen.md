# usbtest.exe und xhcitest.exe

```
cl      /W3 /O1 /MT usbtest.c  advapi32.lib
cl      /W3 /O1     xhcitest.c

clang-cl --target=i386-pc-windows-msvc /O1 xhcitest.c
```

Die beiliegenden Fassungen sind **ohne Laufzeitbibliothek** gebunden:

```
i686-w64-mingw32-gcc -Os -fno-builtin -o xhcitest.exe xhcitest.c ntbasis.c \
    -nostdlib -Wl,-e,_ntb_start \
    -Wl,--major-subsystem-version=4 -Wl,--minor-subsystem-version=0 \
    -Wl,--major-os-version=4 -Wl,--minor-os-version=0 -s \
    -lkernel32 -ladvapi32
```

Der Grund steht im Kopf von `ntbasis.c`: welche `msvcrt.dll` auf einer
NT-4.0-Maschine liegt, haengt davon ab, was dort einmal installiert
wurde. Fehlt ein Eintrag, startet das Programm nicht -- und die Meldung
nennt den fehlenden Namen nicht. Bei einem Werkzeug, das gerade dann
helfen soll, wenn der Rechner nicht rundlaeuft, ist das die falsche
Abhaengigkeit.

Geprueft am Ergebnis:

```
PE32, Subsystem 4.0, OS-Fassung 4.0     laeuft auf NT 4.0
13 kB

xhcitest.exe importiert NUR aus kernel32:
    CloseHandle CreateFileA DeviceIoControl ExitProcess
    GetCommandLineA GetStdHandle WriteFile

usbtest.exe zusaetzlich aus advapi32 (Dienst- und Registrierungsteil):
    OpenSCManagerA OpenServiceA CreateServiceA StartServiceA
    ControlService DeleteService QueryServiceStatus CloseServiceHandle
    RegOpenKeyExA RegQueryValueExA RegEnumValueA RegCloseKey
und  FormatMessageA GetFileAttributesA GetModuleFileNameA
     GetLastError Sleep
```

Jeder dieser Namen ist seit NT 3.1 da. Kein `msvcrt`, kein
`SetUnhandledExceptionFilter`, kein `InitializeCriticalSection`.

Das eigene `printf` in `ntbasis.c` kann `%s %c %d %u %x %X %p %%`,
das `l` wird gelesen und ignoriert (unter Win32 ist `long` so breit wie
`int`), dazu Breite, fuehrende Nullen und Linksbuendigkeit. Kein
Gleitkomma, keine Genauigkeit mit Punkt.

Die grossen Antwortpuffer sind `static`, nicht auf dem Stapel -- ueber
vier Kilobyte Rahmen laesst der Uebersetzer `__chkstk` rufen, und das
gibt es ohne Laufzeitbibliothek nicht. Dieselbe Falle wie im Treiber.

## Treiber bauen

```
./bau.sh ../bau/src              usbnt.sys   -- alle vier Bauarten
NUR=xhci ./bau.sh ../bau/src     xhcint.sys  -- nur xHCI
```

Der getrennte xHCI-Bau bekommt einen eigenen Geraetenamen
(`\Device\XhciNt`) und einen eigenen Dienst (`xhcint`). Damit laesst er
sich laden und entladen, ohne dass die OHCI und EHCI derselben
Suedbruecke mitgehen -- und ohne dass ein Fehler dort einen Fehler hier
vortaeuscht.

`xhcint.sys` neben `xhcitest.exe` legen; das Werkzeug sucht ihn dort.

# Fensterprogramme und Systemsteuerung

Ressourcen (Symbol, Versionsangabe, Texte) mit `windres`, dann binden.
**Nicht mit `-s` binden**, sondern danach `strip`: mit `-s` meldet ld
bei Ressourcen "(.rsrc) is too large", und die Datei ist nicht zu trauen.

```
cd res
i686-w64-mingw32-windres usbsetup.rc -O coff -o usbsetup_res.o
i686-w64-mingw32-windres usbmaus.rc  -O coff -o usbmaus_res.o
cd ..

# USBSETUP.EXE -- Fenster, Einstieg Einstieg@0
i686-w64-mingw32-gcc -Os -Wall -fno-builtin -nostdlib -o usbsetup.exe \
    usbsetup.c res/usbsetup_res.o -Wl,-e,_Einstieg@0 -Wl,--subsystem,windows \
    -Wl,--major-subsystem-version=4 -Wl,--minor-subsystem-version=0 \
    -Wl,--major-os-version=4 -Wl,--minor-os-version=0 \
    -lkernel32 -luser32 -lgdi32 -ladvapi32
i686-w64-mingw32-strip usbsetup.exe

# USBMAUS.CPL -- DLL, Einstieg DllEntry@12, Ausfuhr CPlApplet
i686-w64-mingw32-gcc -Os -Wall -fno-builtin -shared -nostdlib -o usbmaus.cpl \
    usbmaus.c usbmaus.def res/usbmaus_res.o -Wl,-e,_DllEntry@12 \
    -Wl,--major-subsystem-version=4 -Wl,--minor-subsystem-version=0 \
    -Wl,--major-os-version=4 -Wl,--minor-os-version=0 \
    -lkernel32 -luser32 -lgdi32
i686-w64-mingw32-strip usbmaus.cpl
```

`usbjoy.dll` und `usbjoy.cpl` gehen genauso (`-lwinmm` dazu).  Ohne CRT
muss jedes Programm, das `ZeroMemory` oder Strukturzuweisungen nutzt,
ein eigenes `memset` mitbringen (steht jeweils im Quelltext).

Nach jedem Bau die Einfuhren gegen die DLLs eines echten NT 4.0 SP6a
pruefen -- was dort fehlt, laesst das Programm beim Start scheitern.
