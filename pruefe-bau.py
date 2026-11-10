#!/usr/bin/env python3
"""
pruefe-bau.py -- gebaute Dateien pruefen, bevor sie ausgeliefert werden.

    pruefe-bau.py <verzeichnis> [<verzeichnis> ...]

Prueft jede .sys/.exe/.dll/.cpl:

  Treiber (.sys)   .text/.rdata/.data resident (NOT_PAGED), INIT und PAGE
                   nicht; PE-Pruefsumme stimmt (sonst lehnt NT 4.0 den
                   Treiber wortlos mit 0xC0000221 ab)
  alle             Versionsangabe vorhanden; kein Import, der in NT 4.0
                   fehlt (Liste unten); Programme ohne msvcrt.dll

Die Liste der in NT 4.0 fehlenden Funktionen ist NICHT vollstaendig --
sie faengt die ueblichen Verdaechtigen.  Die sichere Pruefung ist der
Vergleich gegen die Exporttabellen der echten NT-4.0-Dateien.

Entstanden am 2026.11.11: vierzehn Programme, darunter die Installer,
waren monatelang ohne Versionsangabe ausgeliefert worden, weil
werkzeug.ico im Projekt fehlte und windres deshalb still scheiterte.
Niemand hatte es gemerkt, weil nichts es pruefte.
"""
import glob, os, sys
import pefile

FEHLT_IN_NT4 = {
    # kernel32
    'GetModuleHandleExA', 'GetModuleHandleExW', 'IsProcessorFeaturePresent',
    'TryEnterCriticalSection', 'InitializeCriticalSectionAndSpinCount',
    'SetFilePointerEx', 'GetFileSizeEx', 'CreateTimerQueueTimer',
    'HeapSetInformation', 'EncodePointer', 'DecodePointer', 'GetTickCount64',
    'GetNativeSystemInfo', 'IsWow64Process', 'AddVectoredExceptionHandler',
    # msvcrt
    '_strtoi64', '_strtoui64', '_ftol2', '_ftol2_sse', '_aligned_malloc',
    '_aligned_free', '_set_invalid_parameter_handler', '_crt_debugger_hook',
    '__iob_func', 'vsnprintf', 'snprintf', 'strtoll', 'strtoull',
    # user32
    'GetRawInputData', 'RegisterRawInputDevices',
}
NOT_PAGED, DISCARD = 0x08000000, 0x02000000


def pruefe(pfad):
    maengel = []
    try:
        p = pefile.PE(pfad)
    except pefile.PEFormatError:
        return None                       # Win16 (NE) u. ae.: nicht unser Fall
    imp = [(i.dll.decode().lower(), x.name.decode())
           for i in getattr(p, 'DIRECTORY_ENTRY_IMPORT', [])
           for x in i.imports if x.name]
    fremd = sorted({n for _, n in imp if n in FEHLT_IN_NT4})
    if fremd:
        maengel.append('Importe, die NT 4.0 fehlen: ' + ', '.join(fremd))
    res = getattr(p, 'DIRECTORY_ENTRY_RESOURCE', None)
    if res is None or not any(e.id == 16 for e in res.entries):
        maengel.append('keine Versionsangabe')
    if pfad.lower().endswith('.sys'):
        ab = {s.Name.rstrip(b'\0').decode(): s.Characteristics for s in p.sections}
        for n in ('.text', '.rdata', '.data'):
            if n in ab and not ab[n] & NOT_PAGED:
                maengel.append('%s ist auslagerbar' % n)
        for n in ('INIT', 'PAGE'):
            if n in ab and ab[n] & NOT_PAGED:
                maengel.append('%s ist resident' % n)
        if 'INIT' in ab and not ab['INIT'] & DISCARD:
            maengel.append('INIT ist nicht verwerfbar')
        if p.OPTIONAL_HEADER.CheckSum != p.generate_checksum():
            maengel.append('PE-Pruefsumme falsch')
    elif any(d == 'msvcrt.dll' for d, _ in imp):
        maengel.append('braucht msvcrt.dll (Werkzeuge werden ohne C-Laufzeit gebaut)')
    return maengel


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    schlecht = gesamt = 0
    for d in sys.argv[1:]:
        for f in sorted(glob.glob(os.path.join(d, '*'))):
            if not f.lower().endswith(('.sys', '.exe', '.dll', '.cpl')):
                continue
            m = pruefe(f)
            if m is None:
                continue
            gesamt += 1
            if m:
                schlecht += 1
                print('%-44s %s' % (f, '; '.join(m)))
    print('%d Dateien geprueft, %d mit Maengeln' % (gesamt, schlecht))
    sys.exit(1 if schlecht else 0)


if __name__ == '__main__':
    main()
