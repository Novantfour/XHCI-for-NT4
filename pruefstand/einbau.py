#!/usr/bin/env python3
"""
einbau.py <treiber.sys> [start] -- einen Treiber offline in das NT-Abbild legen.

Arbeitet immer auf einer frischen Kopie von nt4_orig.img.  Ein Abbild,
das QEMU hart beendet hat, wird nie wiederverwendet: der SYSTEM-Hive
koennte schmutzig sein, und NT spielt dann beim naechsten Start sein
.LOG-Journal ein und ueberschreibt, was hier eingetragen wurde.

    start  1 = beim Systemstart (Vorgabe) -- die serielle Ausgabe
               zeigt dann alles ab DriverEntry
           3 = von Hand, ueber usbtest/xhcitest
"""
import sys, os, struct, subprocess, shutil, hivex

treiber = sys.argv[1]
ohne_treiber = (treiber == 'keiner')   # nur Agent, Anmeldung, Werkzeuge
start   = int(sys.argv[2]) if len(sys.argv) > 2 else 1
name    = os.path.basename(treiber)[:-4].lower()

os.chdir('/home/claude/vm')
# Grundlage: NT4_BASIS=neu nimmt das Abbild aus der VirtualBox-VM des
# Anwenders (SP6a, IE 6 SP1), sonst das alte (IE4-Shell mit IE3-WININET,
# an der die Systemsteuerung abstuerzt).
import os as _os
basis = 'nt4_neu_orig.img' if _os.environ.get('NT4_BASIS') == 'neu' else 'nt4_orig.img'
shutil.copy(basis, 'nt4_smal.img')
print('Grundlage:', basis)
subprocess.run('dd if=nt4_smal.img of=part.ntfs bs=512 skip=63 count=1048257 status=none',
               shell=True, check=True)

def sz(s):  return (s + '\0').encode('utf-16-le')
def dw(v):  return struct.pack('<I', v)

# --- Registrierung ---------------------------------------------------
subprocess.run('ntfscat part.ntfs /WINNT/system32/config/system > /tmp/SYSTEM',
               shell=True, check=True)
h = hivex.Hivex('/tmp/SYSTEM', write=True)
root = h.root()
cur = struct.unpack('<I', h.value_value(h.node_get_value(
          h.node_get_child(root, 'Select'), 'Current'))[1])[0]
cs  = h.node_get_child(root, 'ControlSet%03d' % cur)
svc = h.node_get_child(cs, 'Services')

if not ohne_treiber:
  knoten = h.node_get_child(svc, name) or h.node_add_child(svc, name)
  h.node_set_values(knoten, [
      {'key': 'Type',         't': 4, 'value': dw(1)},
      {'key': 'Start',        't': 4, 'value': dw(start)},
      {'key': 'ErrorControl', 't': 4, 'value': dw(1)},
      {'key': 'ImagePath',    't': 2, 'value': sz('System32\\drivers\\%s.sys' % name)},
      # Gruppe "Keyboard Port": laedt vor "Pointer Class" und "Keyboard
      # Class".  Ohne Gruppe laedt der Treiber nach kbdclass, und der hat
      # DEVICEMAP dann schon gezaehlt -- die USB-Tastatur bleibt tot.
      {'key': 'Group',        't': 1, 'value': sz('Keyboard Port')},
  ])

  # SYSCTL="hw.usb.xhci.ctlstep=1;..." -> Parameters\Sysctl (FreeBSD-Schalter,
  # nt/ntsysctl.c).  Ohne die Variable wird ein alter Wert entfernt, damit
  # nichts aus einem frueheren Lauf stehen bleibt.  Uebrige Parameter bleiben.
  import os as _os2
  par = h.node_get_child(knoten, 'Parameters') or h.node_add_child(knoten, 'Parameters')
  werte = [{'key': h.value_key(v), 't': h.value_type(v)[0], 'value': h.value_value(v)[1]}
           for v in h.node_values(par) if h.value_key(v) not in ('Sysctl', 'XhciSchritt')]
  if _os2.environ.get('XHCISCHRITT') is not None:
      werte.append({'key': 'XhciSchritt', 't': 4, 'value': dw(int(_os2.environ['XHCISCHRITT']))})
      print('Parameters\\XhciSchritt =', _os2.environ['XHCISCHRITT'])
  if _os2.environ.get('SYSCTL'):
      werte.append({'key': 'Sysctl', 't': 1, 'value': sz(_os2.environ['SYSCTL'])})
      print('Parameters\\Sysctl =', _os2.environ['SYSCTL'])
  h.node_set_values(par, werte)

# ConnectMultiplePorts fuer die Tastatur- und Mausklasse -- ohne das
# landet eine USB-Tastatur auf KeyboardClass1 und USER liest sie nie.
for klasse in (() if ohne_treiber else ('Kbdclass', 'Mouclass')):
    k = h.node_get_child(svc, klasse)
    if k:
        p = h.node_get_child(k, 'Parameters') or h.node_add_child(k, 'Parameters')
        alt = [{'key': h.value_key(v), 't': h.value_type(v)[0],
                'value': h.value_value(v)[1]} for v in h.node_values(p)]
        alt = [v for v in alt if v['key'] not in ('ConnectMultiplePorts',
                                                  'MaximumPortsServiced')]
        alt += [{'key': 'ConnectMultiplePorts', 't': 4, 'value': dw(1)},
                {'key': 'MaximumPortsServiced', 't': 4, 'value': dw(4)}]
        h.node_set_values(p, alt)

# --- automatische Anmeldung + Agent-Autostart ---------------------
# Ohne Anmeldung greift kein Run-Schluessel.  Es gibt kein Kennwort,
# also traegt AutoAdminLogon den leeren Administrator ein.  Der Agent
# startet aus dem Run-Schluessel, sobald der Desktop steht.
sw_pfad = 'Microsoft\\Windows NT\\CurrentVersion\\Winlogon'
try:
    import subprocess as _sp
    _sp.run('ntfscat part.ntfs /WINNT/system32/config/software > /tmp/SOFTWARE',
            shell=True, check=True)
    hs = hivex.Hivex('/tmp/SOFTWARE', write=True)
    n = hs.root()
    for teil in sw_pfad.split('\\'):
        k = hs.node_get_child(n, teil)
        n = k if k else hs.node_add_child(n, teil)
    vorhanden = {hs.value_key(v): v for v in hs.node_values(n)}
    neu = [{'key': hs.value_key(v), 't': hs.value_type(v)[0],
            'value': hs.value_value(v)[1]} for v in hs.node_values(n)
           if hs.value_key(v) not in ('AutoAdminLogon','DefaultUserName',
                                      'DefaultPassword','ForceAutoLogon')]
    neu += [
        {'key': 'AutoAdminLogon', 't': 1,
         'value': sz('0' if os.environ.get('OHNE_ANMELDUNG') else '1')},
        {'key': 'DefaultUserName', 't': 1, 'value': sz('Administrator')},
        {'key': 'DefaultPassword', 't': 1, 'value': sz('')},
    ]
    hs.node_set_values(n, neu)
    # Run-Schluessel
    r = hs.root()
    for teil in 'Microsoft\\Windows\\CurrentVersion\\Run'.split('\\'):
        k = hs.node_get_child(r, teil)
        r = k if k else hs.node_add_child(r, teil)
    rl = [{'key': hs.value_key(v), 't': hs.value_type(v)[0],
           'value': hs.value_value(v)[1]} for v in hs.node_values(r)
          if hs.value_key(v) not in ('ntagent', 'VBoxTray')]
    # kein Run-Eintrag mehr: der Agent ist ein Dienst.  VBoxTray stuerzt
    # in QEMU ab (SSE) -- nur in der Arbeitskopie heraus.
    hs.node_set_values(r, rl)
    hs.commit(None)
    subprocess.run(['ntfscp','-f','part.ntfs','/tmp/SOFTWARE',
                    '/WINNT/system32/config/software'], check=True)
    print('AutoAdminLogon + Agent-Autostart eingetragen')
except Exception as e:
    print('SOFTWARE-Hive:', e)

# NTAGENT als Dienst: startet mit dem System, vor jeder Anmeldung, ohne
# Fenster.  Ersetzt den Autostart ueber den Run-Schluessel.
ag = h.node_get_child(svc, 'ntagent') or h.node_add_child(svc, 'ntagent')
h.node_set_values(ag, [
    {'key': 'Type',         't': 4, 'value': dw(0x10)},
    {'key': 'Start',        't': 4, 'value': dw(2)},
    {'key': 'ErrorControl', 't': 4, 'value': dw(1)},
    {'key': 'ImagePath',    't': 2, 'value': sz('%SystemRoot%\\system32\\ntagent.exe')},
    {'key': 'ObjectName',   't': 1, 'value': sz('LocalSystem')},
    {'key': 'DisplayName',  't': 1, 'value': sz('NTAGENT Befehlskanal (COM2)')},
])

# Pruefaufbau: zwei Starttreiber zeigen auf Dateien, die es im Abbild
# nicht gibt, und loesen bei jedem Start den Dialog "Dienstkontroll-
# Manager" aus.  Der haelt den Fokus und verfaelschte Messungen.
# Im Abbild aus der VirtualBox-VM zusaetzlich: was es nur in VirtualBox
# gibt und in QEMU scheitert (PCnet-Netzkarte, VBoxGuest-Geraet) oder
# abstuerzt (VBoxService, VBoxTray: SSE-Befehle, die NT 4.0 nie
# freischaltet), dazu der Computer-Suchdienst, der am Server haengt.
# Andere Dienste unserer Treiberfamilie abschalten.  Das Abbild aus der
# VirtualBox-VM hatte schon "usbnt"; baute ich usbnt_diag.sys ein, entstand
# ein ZWEITER Dienst -- zwei Treiber programmierten denselben xHCI, mit
# Zeitueberschreitungen, Controller-Resets und "host controller error".
familie = ('usbnt', 'uhcint', 'ohcint', 'ehcint', 'xhcint', 'usbnt_diag', 'usbnt_herz')
andere = tuple(d for d in familie if d != name)
# AUS_DIENSTE="i8042prt,...": weitere Dienste abschalten (z. B. ohne
# PS/2, damit der USB-Treiber Anschluss 0 von kbdclass/mouclass wird --
# so wie auf Brettern ohne PS/2-Maus).
andere = andere + tuple(x for x in os.environ.get('AUS_DIENSTE', '').split(',') if x)
for tot in ('PCIDump', 'pnpisa', 'ati', 'Sermouse',
            'AMDPCN', 'Browser', 'VBoxGuest', 'VBoxService') + andere:
    k = h.node_get_child(svc, tot)
    if k:
        rest = [{'key': h.value_key(v), 't': h.value_type(v)[0],
                 'value': h.value_value(v)[1]} for v in h.node_values(k)
                if h.value_key(v) != 'Start']
        rest.append({'key': 'Start', 't': 4, 'value': dw(4)})
        h.node_set_values(k, rest)

h.commit(None)
print(('Dienst %s eingetragen' % name) if not ohne_treiber else 'ohne Treiber', 'ControlSet%03d' % cur)
print('abgeschaltet, falls vorhanden:', ', '.join(d for d in andere if h.node_get_child(svc, d)) or '-')

# --- Dateien ---------------------------------------------------------
def rein(quelle, ziel):
    r = subprocess.run(['ntfscp', '-f', 'part.ntfs', quelle, ziel],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('ntfscp', ziel, r.stderr.strip()); sys.exit(1)

rein('/tmp/SYSTEM', '/WINNT/system32/config/system')

# MP=1: Mehrprozessor-Start aus SP6a (sp6i386.exe): ntkrnlmp.exe und
# halmps.dll unter eigenem Namen nach system32, dazu ein erster boot.ini-
# Eintrag mit /KERNEL= /HAL= -- NTLDR nimmt bei gleichem ARC-Pfad den
# ersten.  Der Einprozessor-Eintrag bleibt als zweiter stehen.  QEMU dazu
# mit -smp N starten.
# DATEIEN="wirt1=gast1;wirt2=gast2": Testdaten gleich ins Abbild legen.
# Unter dem MP-HAL bleiben grosse Uebertragungen ueber COM2 in QEMU haengen
# (1 MB: nie fertig; 64 kB: geht) -- so muss nichts Grosses ueber den Agenten.
for paar in [x for x in os.environ.get('DATEIEN', '').split(';') if '=' in x]:
    wirt, gast = paar.split('=', 1)
    rein(wirt, gast)
    print('Datei ins Abbild:', wirt, '->', gast)

if os.environ.get('MP') == '1':
    rein('mp/ntkrnlmp.exe', '/WINNT/system32/ntkrnlmp.exe')
    rein('mp/halmps.dll',   '/WINNT/system32/halmps.dll')
    # Einprozessor-Installationen ersetzen in drei Dateien die LOCK-Praefixe
    # (F0) durch NOP (90) und rechnen die Pruefsumme nach; der MP-Kern haelt
    # dann mit STOP c0000249 (UP/MP-Image-Fehler, "ntdll.dll ... fuer Uni-
    # Prozessor geaendert").  Im Abbild durch Vergleich mit sp6i386.exe
    # gefunden: sonst byte-gleich.  Die unveraenderten aus dem Paket rein.
    for f, ziel in (('ntdll.dll', 'NTDLL.DLL'), ('kernel32.dll', 'KERNEL32.DLL'),
                    ('win32k.sys', 'WIN32K.SYS')):
        rein('mp/' + f, '/WINNT/system32/' + ziel)
    arc = 'multi(0)disk(0)rdisk(0)partition(1)\\WINNT'
    ini = ('[boot loader]\r\ntimeout=3\r\ndefault=' + arc + '\r\n[operating systems]\r\n' +
           arc + '="NT 4.0 Mehrprozessor (ntkrnlmp, halmps)" /KERNEL=ntkrnlmp.exe /HAL=halmps.dll\r\n' +
           arc + '="Windows NT Workstation, Version 4.0"\r\n')
    open('/tmp/boot.ini', 'w', newline='').write(ini)
    rein('/tmp/boot.ini', '/boot.ini')
    print('Mehrprozessor-Start eingerichtet (ntkrnlmp.exe, halmps.dll)')
if not ohne_treiber:
    rein(treiber, '/WINNT/system32/drivers/%s.sys' % name)
for w in ('usbtest.exe', 'xhcitest.exe', 'ntagent.exe', 'usbgeraet.exe'):
    q = '/home/claude/paket/usb-nt4/bin/' + w
    if os.path.exists(q):
        rein(q, '/WINNT/system32/' + w)

subprocess.run('dd if=part.ntfs of=nt4_smal.img bs=512 seek=63 conv=notrunc status=none',
               shell=True, check=True)
print('Abbild fertig:', treiber, '-> \\WINNT\\system32\\drivers')
