# Treiber bauen für Windows NT 4.0 — Leitfaden (Stand 2026.10.10)

Dieses Dokument fasst zusammen, wie ein Kernel-Treiber (.sys) für NT 4.0
gebaut wird, der den Qualitätsansprüchen eines kritischen Reviewers
genügt. Grundlage sind zwei konkrete Rückmeldungen von Ray Hinchliffe
(Autor von SIV, SIVNT4.SYS) zu diesem Projekt.

## Werkzeugkette
- **Übersetzen:** clang-cl (LLVM), Ziel `--target=i386-pc-windows-msvc`.
- **Binden:** lld-link, `/subsystem:native,4.00 /driver /entry:DriverEntry@8`.
- **Einfuhrbibliotheken:** die DDK-eigenen (ntoskrnl.lib/hal.lib) lassen
  sich mit lld-link teils nicht binden ("section larger than 4 GiB").
  Deshalb aus den Original-Exportnamen selbst erzeugen: eine .def mit
  `LIBRARY ntoskrnl.exe` (bzw. hal.dll, ntvdm.exe) + `llvm-dlltool -k`.
  Bei NTVDM-Diensten: Namen sind dekoriert (`_VDDInstallIOHook@16`,
  `getAX@0`), also __stdcall.
- **Abschnittsattribute nachbearbeiten:** lld-link setzt das
  NOT_PAGED-Bit nicht. Ein Nachbearbeiten des PE-Kopfs (abschnitte.py)
  ist nötig, danach die Prüfsumme neu (pruefsumme.py).

## Zwei Qualitätsregeln von Ray Hinchliffe

### 1. Abschnittsattribute (erste Rückmeldung)
".text + .data are marked as paged, they should not be, further paged
code should be in PAGE, it should be linked with -MERGE:.rdata=.text and
DriverEntry() should be in INIT."

Das heißt konkret:
- **`.text`, `.rdata`, `.data` müssen resident sein** (NOT_PAGED, Bit
  0x08000000). Sonst lagert NT Treibercode aus; wird er dann bei
  erhöhtem IRQL (ISR/DPC) gebraucht, folgt ein Bugcheck
  (`IRQL_NOT_LESS_OR_EQUAL` o. ä.).
- **`DriverEntry()` gehört in den Abschnitt INIT.** NT gibt INIT nach dem
  Start frei (DISCARDABLE) — der Startcode belegt dann keinen Speicher mehr.
- **Auslagerbarer Code gehört in den Abschnitt PAGE** (siehe Regel 2).

Umsetzung hier:
- `#pragma alloc_text(INIT, DriverEntry)` legt DriverEntry in INIT.
- abschnitte.py setzt nach dem Binden: .text/.rdata/.data = NOT_PAGED,
  INIT = DISCARDABLE, PAGE = auslagerbar (kein NOT_PAGED).
- Prüfen: `pefile`, Bit 0x08000000 je Abschnitt.

### 2. Auslagerbarer Code und PAGED_CODE() (zweite Rückmeldung)
"apart from DriverEntry() none of the code is pageable. ... Your drivers
must have similar and all those routines are pageable assuming they stay
at passive level. Note any routine which is pageable should have
PAGED_CODE() as the first statement, that includes DriverEntry()."

Vorbild aus SIVNT4.SYS:
```
#pragma alloc_text( INIT, DriverEntry           )
#pragma alloc_text( PAGE, DriverUnload          )
#pragma alloc_text( PAGE, sik_dispatch_create   )
#pragma alloc_text( PAGE, sik_dispatch_close    )
#pragma alloc_text( PAGE, sik_dispatch_ioctl    )
...
NTSTATUS DriverEntry(...) { ... PAGED_CODE() ... }
```

Das heißt:
- **Jede Routine, die GARANTIERT nur auf PASSIVE_LEVEL läuft, gehört in
  PAGE** (spart Arbeitsspeicher, solange sie nicht gebraucht wird).
- **Jede auslagerbare Routine trägt `PAGED_CODE()` als erste Anweisung.**
  Im Debug-Build prüft das, dass die Routine nicht über APC_LEVEL läuft
  (sonst wäre sie fälschlich ausgelagert); im Free-Build ist es leer.
- Das schließt DriverEntry ein (läuft passiv, liegt in INIT).

Umsetzung hier (nt/ntpaged.h):
- `NTU_PAGED(fn)` = `#pragma alloc_text(PAGE, fn)` vor der Routine.
- `NTU_PAGED_CODE()` als erste Anweisung (Debug: IRQL-Prüfung).

**WICHTIGE Einschränkung bei portiertem Code (FreeBSD-Stapel):** Beim
USB-Treiber ist die Grenze passiv/DISPATCH nicht überall sauber zu
ziehen — IRP-Dispatch-Pfade können ein Interrupt-Transfer im DPC
abschließen. Deshalb kommen NUR die nachweislich immer passiven Routinen
nach PAGE: DriverEntry (INIT), DriverUnload/NtuUnload, die newbus-
Routinen probe/attach/detach (laufen beim An-/Abstecken passiv). Die
eigentlichen IRP- und Strompfade bleiben resident. Das ist ehrlicher und
sicherer als pauschales Auslagern — ein falsch ausgelagerter ISR-naher
Pfad gibt sonst denselben Bugcheck wie ein nicht-residentes .text.

## Prüf-Rezept (nach jedem Bau)
1. `pefile`: .text/.rdata/.data haben Bit 0x08000000 (resident),
   INIT/PAGE nicht.
2. Importe gegen die Ziel-Systemdateien prüfen (NT 4.0!): jedes
   importierte Symbol muss in ntoskrnl.exe/hal.dll der NT-4.0-Maschine
   existieren. Spätere Exporte (Win2000+) brechen den Start.
3. Prüfsumme gesetzt (sonst STATUS_IMAGE_CHECKSUM_MISMATCH 0xC0000221
   beim Laden, ohne weitere Meldung).
4. Im Gast laden und entladen, Funktion prüfen.

## Weitere, früher erkaufte Regeln
- `-Zg`-/MASM-Kompatibilität ist hier nicht nötig (kein MASM); die
  Assembly ist klein und in eigenen .obj.
- clang-cl-Codegen-Divergenzen zu cl.exe traten beim USB-Stapel nicht
  auf; wo sie aufträten, hilft ReactOS' `/fallback`-Muster (betroffene
  Datei mit cl.exe).
- Struktur-Offsets zwischen C und Assembly müssen bitgenau stimmen
  (hier kaum Assembly, deshalb unkritisch).
