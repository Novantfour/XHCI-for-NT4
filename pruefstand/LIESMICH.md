# Gastagent fuer NT 4.0 unter QEMU

Der qemu-guest-agent laeuft auf NT 4.0 nicht -- er haengt an GLib, das
sich seit Jahren nicht mehr auf NT 4.0 bauen laesst, und spricht ueber
virtio-serial, wofuer es keinen NT-4.0-Treiber gibt.

`ntagent.exe` ist der kleine Ersatz: er liest Befehle von COM2, fuehrt
sie ueber `cmd /c` aus und schickt die Ausgabe zurueck.  Nur kernel32,
keine Laufzeitbibliothek, 7 kB -- dieselbe Machart wie die Werkzeuge.

## Kanaele

```
COM1   serieller Treiberbericht (der Treiber schreibt selbst darauf)
COM2   Befehlskanal des Agenten
```

Die beiden duerfen sich nicht ins Wort fallen; deshalb getrennt.

## Auf dem Wirt

```
./start.sh <usb-geraete>     VM starten (COM1 -> com1.log, COM2 -> Socket)
python3 agent.py PING        Lebenszeichen
python3 agent.py xhcitest    ein Programm im Gast laufen lassen
python3 agent.py "xhcitest zaehler"
./stop.sh                    VM ueber die PID beenden (NICHT pkill -f!)
```

## Einbau ins Abbild

`einbau.py <treiber.sys>` legt offline ins NT-Abbild:

```
den Treiber            \WINNT\system32\drivers\<name>.sys
Dienst + Gruppe        ControlSet, Start=1, Group="Keyboard Port"
usbtest, xhcitest      \WINNT\system32
ntagent                \WINNT\system32, Autostart im Run-Schluessel
AutoAdminLogon         damit der Run-Schluessel ohne Eingabe greift
ConnectMultiplePorts   Kbdclass und Mouclass, damit USB neben PS/2 gilt
```

Immer auf einer frischen Kopie von nt4_orig.img -- ein hart beendetes
Abbild koennte einen schmutzigen Hive haben, dessen .LOG-Journal die
Eintraege beim naechsten Start ueberschreibt.

## Am NEC uPD720200 nachgewiesen

Genau die Kartenbauart aus dem Projekt (1033:0194 rev 03).  Stick,
Tastatur und Maus zaehlen vollstaendig auf, PED steht an allen dreien,
78 Unterbrechungen zu 78 Fadenlaeufen, keine fremde, keine verlorene,
keine abgewiesene Freigabe.
