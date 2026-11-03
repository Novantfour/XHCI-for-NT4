# Soundkartentreiber für Windows NT 4.0 — Wiedergabe und Lautstärkeregler

Stand 2026.10.11. Zusammengetragen aus zwei Projekten, die beide auf
dieselben Fallen gelaufen sind: dem EMU10K1-Treiber (`pawave.dll`,
`wavekern.c`, `emumixer.c`) und dem USB-Audio-Treiber (`usbaudio.sys`,
`usbaudio.dll`, UAC1). Alles hier ist **gemessen**, nicht vermutet.

---

## Teil 1 — Wiedergabe

### 1.1 Mehrere Tonquellen: mischen muss der Kerntreiber

Ein Treiber, der nur einen Öffner zulässt, spielt entweder die Musik
oder den Systemklang, nie beides. Typisches Bild beim Anwender:
„es geht nur eine Tonquelle“.

**Mischen im Benutzermodus ist unmöglich.** Die winmm-Treiber-DLL wird
**je Prozess** geladen. Der Media Player und der Explorer haben jeweils
ihre eigene Instanz und können sich nicht absprechen. Das Mischen muss
deshalb in den Kerntreiber.

Muster (EMU10K1 `wavekern.c`, übernommen im USB-Treiber):

- Der Treiber führt ein Feld von Klienten, `gKlient[STROEME]` bzw.
  `sc_kl[NTUA_KLIENTEN]`. Jeder Öffner bekommt einen **eigenen Ring**,
  eigenen Zustand, eigene Zähler.
- Beim Füllen des Hardware- oder USB-Puffers addiert der Treiber alle
  aktiven Klienten zu einer Summe, in 16 Bit, **mit Begrenzung** gegen
  Übersteuerung (nicht umschlagen lassen).
- Der Strom läuft, sobald der erste Klient spielt, und hört auf, wenn
  der letzte geht.
- `IRP_MJ_CLEANUP` gibt **nur den einen** Klienten frei, nie den ganzen
  Strom. Sonst reißt ein beendetes Programm allen anderen den Ton ab.
- Die Benutzermodus-DLL öffnet mit
  `FILE_SHARE_READ | FILE_SHARE_WRITE` und gibt bei belegtem Gerät
  **nicht** auf.

### 1.2 Die Anlaufschwelle — kurze Klänge bleiben sonst stumm

Das ist die heimtückischste Falle, und beide Projekte sind darauf
hereingefallen. Wörtlich aus `wavekern.c`:

> Die Schwelle lag beim halben Ring — 32768 Byte, also
> hunderteinundsiebzig Millisekunden. Alles Kürzere kam nie über sie
> hinweg und blieb stumm: der Klang war zu Ende, bevor genug im Puffer
> stand. Mit laufender Musik fiel das nicht auf, weil der Strom dann
> ohnehin schon lief und der Klang nur dazugemischt wurde. Genau
> deshalb ging es „mit Musik immer, allein nicht“.

**Das Erkennungszeichen:** Systemklänge gehen, solange Musik läuft, und
fehlen, wenn nichts läuft. Wer nur mit Musik testet, sieht den Fehler
nie.

Die Lösung sind **zwei Bedingungen, von denen eine genügt**:

1. Es steht ein Vorlauf von einem Abschnitt bereit (dort 85 ms), **oder**
2. es kommt ohnehin nichts mehr nach (kein Auftrag mehr offen).

Die zweite ist die entscheidende: Liegt kein weiterer Puffer an, ist
das Vorhandene alles, und länger zu warten wäre sinnlos.

    if (!gLaeuft && WerSpielt() > 0 && gRahmenInRing > 0) {
        if (gRahmenInRing * RAHMEN >= gVorlauf || !AuftraegeOffen())
            StromStarten();
    }

### 1.3 Das Ende nicht abschneiden

Zwei Fehler, die beide das Ende verschlucken:

**(a) Zustand je Klient melden.** Sobald jeder Klient einen eigenen
Ring hat, muss die Zustandsabfrage (`Fuellstand`, `Platz`, gespielte
Bytes) die Werte **des anfragenden Klienten** liefern. Wird weiter der
alte gemeinsame Ring gemeldet, steht dort immer null; die DLL fragt vor
dem Schließen „ist noch Ton da?“, hört „nein“ und bricht ab. Gemessen:
`ding.wav` mit 0,524 s Inhalt war nach 198 ms zu Ende. Nach der
Korrektur läuft der Mitschnitt exakt bis 0,524 s.

**(b) Beim Schließen auslaufen lassen, aber nie hängen.** Hängen beim
`waveOutClose` noch Puffer, ist `WAVERR_STILLPLAYING` zurückzugeben
formal richtig und praktisch verhängnisvoll: Der **Explorer** spielt
die Systemklänge, behandelt den Fehler nicht und behält den Griff. Da
er nie endet, bleibt das Gerät bis zum Abmelden belegt, und es kommt
überhaupt kein Ton mehr (Symptom: `START` scheitert mit 170,
ERROR_BUSY, direkt nach dem Anmelden).

Richtig ist beides zusammen:

1. erst auf das Auslaufen warten (Puffer werden vom Abspielfaden selbst
   zurückgegeben), mit Obergrenze, etwa drei Sekunden,
2. was dann noch hängt, zurückgeben und **trotzdem schließen**.

### 1.4 Griffe nicht dauerhaft halten

Aus `emumixer.c`, wörtlich sinngemäß: Solange irgendein Prozess ein
Handle auf das Gerät hält, lässt NT den Treiber nicht entladen.
`net stop` meldet dann 2186 („reagiert nicht auf die
Steuerungsfunktion“), und es hilft nur ein Neustart.

Der erste Entwurf dort öffnete in `MXDM_GETNUMDEVS` und schloss nie
wieder — und winmm ruft das **beim Laden**. Das Handle hing damit für
immer in dem Prozess, der winmm geladen hatte.

**Regel:** Offen bleibt der Zugang nur zwischen `MXDM_OPEN` und
`MXDM_CLOSE`. Alles andere — zählen, einen Regler lesen oder setzen —
öffnet, tut sein Werk und lässt wieder los.

---

## Teil 2 — Lautstärkeregler

### 2.1 Das Gerät ist die einzige Wahrheit

Es gibt mehrere Stellen, die eine Lautstärke zu kennen glauben: die
waveOut-Lautstärke (`waveOutSetVolume` / `GetVolume`), der Mixer
(`sndvol32`) und das Gerät selbst. Hält jede Stelle ihren eigenen
zwischengespeicherten Wert, laufen sie auseinander, und der Anwender
meldet „die Regler sind nicht synchron“.

**Regel:** Nie einen eigenen Wert als Wahrheit führen. Jede Abfrage
liest frisch aus dem Gerät, jedes Setzen schreibt ins Gerät. Der
zwischengespeicherte Wert dient höchstens als Rückfall, wenn das Lesen
scheitert.

### 2.2 Beim Öffnen nicht die Lautstärke verstellen

Viele Geräte kommen stumm oder auf Minimum hoch. Wer deshalb beim
Öffnen pauschal auf 100 % setzt, erzeugt den Fehler „die Lautstärke
springt hoch, sobald ich auf Start klicke“.

Richtig ist eine Fallunterscheidung: beim Öffnen den Gerätewert
**lesen** und nur dann anheben, wenn er praktisch am Minimum steht
(Schwelle etwa 2 %). Steht er darüber, ist das die bewusste Einstellung
des Anwenders und bleibt unangetastet.

**Reihenfolge beachten:** Das muss **vor** dem Öffnen des Tonstroms
passieren. Danach hält der Strom das Gerät, und der Steuerzugang lässt
sich nicht mehr öffnen — dann bliebe alles stumm.

### 2.3 Die Skala: gleichmäßig in dB, nicht in Amplitude

Rechnet man physikalisch (`Regler = 65535 · 10^(dB/20)`), landet bei
einem Gerätebereich von −64 bis 0 dB ein angenehmer Pegel von −19 dB
bei **11 %**. Alles Brauchbare drängt sich ins oberste Zehntel, und der
Anwender sagt zu Recht „die Skala haut nicht hin“.

Mischpulte und die Windows-Lautstärkeregelung legen den dB-Bereich
**gleichmäßig** auf den Reglerweg:

    Regler = (dB − min) · 65535 / (max − min)
    dB     = min + Regler · (max − min) / 65535

min und max sind die Grenzen, die das Gerät selbst meldet. Probe:
−6 dB bei Bereich −64..0 ergibt 91 % statt vorher 50 %.

### 2.4 Benachrichtigung über Prozessgrenzen

Ändert ein Programm die Lautstärke, muss `sndvol32` das mitbekommen.
`MM_MIXM_CONTROL_CHANGE` über `DriverCallback` erreicht aber nur Mixer
**im eigenen Prozess**, weil die Mixer-DLL je Prozess geladen wird.

Daraus folgt:

- `WODM_SETVOLUME` muss die Benachrichtigung auslösen (sonst zeigt die
  Lautstärkeregelung weiter den alten Wert) — das deckt den eigenen
  Prozess ab.
- Für andere Prozesse bleibt nur, dass **jede Instanz selbst nachsieht**.
  Ein Wächterfaden, der läuft, solange ein Mixer offen ist, liest den
  Gerätewert zyklisch (etwa alle 400 ms) und meldet Änderungen.
- Der Wächter darf den Tonstrom nicht stören: nicht lesen, solange ein
  Strom dieses Prozesses läuft, und alle Steuerzugriffe durch **eine
  Sperre** schicken. Sonst blockiert der Wächter genau dann den
  Steuerzugang, wenn ein Klang startet — und es bleibt still.
- Sauberer wäre ein Signal aus dem Kerntreiber (benanntes Ereignis oder
  ein wartender IRP), das alle Instanzen weckt. Noch nicht umgesetzt.

### 2.5 Reglerbewegungen am Gerät selbst

Ein Lautstärkerad am Gerät meldet sich **nicht** zwangsläufig.

- **USB-Audio 1.0** sieht dafür einen optionalen Status-Interrupt-
  Endpunkt an der AudioControl-Schnittstelle vor (Abschnitt 3.7.1.2).
  Er liefert `bStatusType` und `bOriginator`; der Rechner fragt daraufhin
  den betroffenen Regler ab. **Viele Geräte haben ihn nicht.** Prüfen:
  hat die AC-Schnittstelle null Endpunkte, gibt es keine Meldung.
- Viele Headsets melden das Rad stattdessen über eine **eigene
  HID-Schnittstelle** als Consumer Control (Volume Increment 0xE9,
  Decrement 0xEA, Mute 0xE2). Im Deskriptor erkennbar an einer
  Schnittstelle der Klasse 03 mit Interrupt-IN-Endpunkt.
- Manche Geräte verarbeiten das Rad rein intern und melden gar nichts.
  Dann hilft nur zyklisches Abfragen.

Beispiel (HyperX Cloud Stinger Core Wireless, 0951:170b): AC-
Schnittstelle mit **null** Endpunkten, dafür Schnittstelle 3 als HID mit
Endpunkt 0x83. Das Rad verstellt messbar die Feature Unit, Abfragen
sieht die Änderung also.

---

## Prüfliste vor dem Ausliefern

1. **Kurzer Klang allein**, ohne laufende Musik. Das ist der Test, den
   fast jeder vergisst, und genau der, der die Anlaufschwelle entlarvt.
2. **Tatsächliche Tondauer messen**, nicht die Rückmeldezeit. Viele
   Treiber melden einen Puffer als fertig, sobald er im Ring liegt; das
   sagt nichts darüber, ob er gespielt wurde.
3. **Zwei und drei Quellen gleichzeitig**, Frequenzen getrennt
   nachweisen (eine dritte als Gegenprobe muss nahe null sein).
4. **Regler von drei Seiten lesen:** waveOut, Mixer, Gerät. Alle drei
   müssen denselben Wert zeigen.
5. **Lautstärke leise stellen, dann Ton starten.** Sie darf nicht
   springen.
6. **Gerät am Minimum, dann Ton starten.** Es muss hörbar werden.
7. **Nach dem Abspielen:** lässt sich der Treiber noch entladen? Wenn
   nicht, hält jemand einen Griff.
8. **Versionsnummer** in jede Datei und ein Werkzeug, das sie anzeigt.
   Ohne das prüft man früher oder später die falsche Datei — das ist
   hier zweimal passiert.
