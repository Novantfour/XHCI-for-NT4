#!/usr/bin/env python3
"""Erzeugt ARCHITEKTUR.svg -- das Schema des Treibers.  Aendern, dann:
   python3 schema/architektur.py   (schreibt ARCHITEKTUR.svg daneben)"""
import os
W, H = 1280, 1010
o = []
def esc(s): return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
def box(x, y, w, h, titel, zeilen=(), fill="#fff", rand="#555", gr=13, zg=11):
    o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="{fill}" stroke="{rand}" stroke-width="1.2"/>')
    o.append(f'<text x="{x+w/2}" y="{y+18}" text-anchor="middle" font-size="{gr}" font-weight="bold">{esc(titel)}</text>')
    for i, z in enumerate(zeilen):
        o.append(f'<text x="{x+w/2}" y="{y+34+i*14}" text-anchor="middle" font-size="{zg}">{esc(z)}</text>')
def band(y, h, titel, fill):
    o.append(f'<rect x="10" y="{y}" width="900" height="{h}" rx="10" fill="{fill}"/>')
    o.append(f'<text x="22" y="{y+17}" font-size="13" font-weight="bold" fill="#333">{esc(titel)}</text>')
def pfeil(y1, y2, text=""):
    o.append(f'<line x1="460" y1="{y1}" x2="460" y2="{y2}" stroke="#333" stroke-width="1.6" marker-end="url(#spitze)"/>')
    if text: o.append(f'<text x="470" y="{(y1+y2)/2+4}" font-size="11" fill="#333" font-style="italic">{esc(text)}</text>')
def textkasten(x, y, w, h, titel, zeilen, fill, rand):
    box(x, y, w, h, titel, (), fill, rand)
    for i, z in enumerate(zeilen):
        o.append(f'<text x="{x+12}" y="{y+40+i*15}" font-size="11" xml:space="preserve" '
                 f'font-family="Consolas, Courier New, monospace">{esc(z)}</text>')

o.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="Arial, Helvetica, sans-serif">')
o.append('<defs><marker id="spitze" markerWidth="10" markerHeight="8" refX="9" refY="4" orient="auto">'
         '<path d="M0,0 L10,4 L0,8 z" fill="#333"/></marker></defs>')
o.append(f'<rect width="{W}" height="{H}" fill="#fafafa"/>')
o.append('<text x="640" y="30" text-anchor="middle" font-size="20" font-weight="bold">'
         'FreeBSD-USB auf Windows NT 4.0 — Aufbau und Zugriffswege</text>')

band(45, 105, "1  Programme (Benutzermodus)", "#e8f1fb")
for i, (a, b) in enumerate([("Anwendungen", ["Editor, Explorer", "IE, Spiele"]),
        ("Druckspooler", ["lokaler Anschluss", "\\\\.\\USBPRNn"]),
        ("Spiele / winmm", ["joyGetPosEx", "usbjoy.dll"]),
        ("Systemsteuerung", ["USB-Maus", "USB-Gamepads"]),
        ("Werkzeuge", ["usbgeraet · usbdruck", "usbtest · joytest"]),
        ("Einrichtung", ["USBSETUP.EXE", "→ USBINST.EXE"]),
        ("eigene Programme", ["Scanner, Audio, …", "über \\\\.\\UsbDevN"])]):
    box(20 + i*126, 70, 118, 70, a, b, "#fff", "#4a78a8")
pfeil(152, 178, "CreateFile · ReadFile · WriteFile · DeviceIoControl")

band(180, 95, "2  Windows NT 4.0 (Kern)", "#eef6e8")
box(20, 205, 150, 60, "USER / win32k", ["liest Tastatur, Maus"], "#fff", "#5b8a3c")
box(180, 205, 160, 60, "kbdclass · mouclass", ["ConnectMultiplePorts=1"], "#fff", "#5b8a3c")
box(350, 205, 150, 60, "Dateisysteme", ["FAT16 · NTFS → X:"], "#fff", "#5b8a3c")
box(510, 205, 200, 60, "E/A-Manager", ["IRP_MJ_CREATE/READ/WRITE", "IRP_MJ_DEVICE_CONTROL"], "#fff", "#5b8a3c")
box(720, 205, 180, 60, "Registry", ["Services\\usbnt\\Parameters", "DEVICEMAP\\USB"], "#fff", "#5b8a3c")
pfeil(277, 303, "IRP  /  ClassService-Rückruf (Tastatur, Maus)")

band(305, 150, "3  usbnt.sys — obere Treiber   (quelle/ober)      je Schnittstelle genau einer", "#fdf3e3")
for i, (a, b) in enumerate([
        ("Tastatur", ["ntukbd · ntkbdport", "\\Device\\", "KeyboardPortN", "Wiederholung, LEDs"]),
        ("Maus", ["ntums · ntmaustasten", "\\Device\\", "PointerPortN", "Tasten 1–8, Rad"]),
        ("Speicher", ["ntumass", "\\Device\\", "UsbPlatteN", "+ Laufwerk X:"]),
        ("Drucker", ["ntuprn", "\\Device\\", "UsbDruckN", "= USBPRNn"]),
        ("Gamepads", ["ntujoy", "\\Device\\", "UsbJoyN", "HID, Xbox 360/One"]),
        ("alles andere", ["ntugen", "\\Device\\", "UsbDevN", "Steuer/Bulk/Int/Iso"]),
        ("Steuergerät", ["ntsteuer", "\\\\.\\UsbNt", "Zähler, Spur,", "Maustasten"])]):
    box(20 + i*127, 330, 124, 112, a, b, "#fff", "#b07d2b")
pfeil(457, 483, "usbd_transfer_setup / _start  ·  newbus probe / attach")

band(485, 120, "4  FreeBSD-USB-Stapel — unverändert   (freebsd/)", "#f3e9f7")
box(20, 510, 420, 85, "USB-Kern", ["usb_device · usb_hub · usb_transfer · usb_request",
    "usb_busdma · usb_process · usb_hid · hid.c (HID-Parser)",
    "Hub-Erkundung, Einrichtung, Zeitüberschreitung"], "#fff", "#7d4a99")
for i, (a, b) in enumerate([("UHCI", ["uhci.c", "USB 1.1"]), ("OHCI", ["ohci.c", "USB 1.1"]),
                            ("EHCI", ["ehci.c", "USB 2.0"]), ("xHCI", ["xhci.c", "USB 2/3", "+ ntxhcisonder"])]):
    box(450 + i*113, 510, 107, 85, a, b, "#fff", "#7d4a99")
pfeil(607, 633, "mtx · callout · bus_dma · bus_space · malloc · kthread")

band(635, 150, "5  Unterlage — FreeBSD-Grundfunktionen auf NT   (quelle/nt, quelle/sys)", "#fbe9ea")
for i, (a, b) in enumerate([("mtx", ["→ KMUTEX (nur PASSIVE)"]), ("ithread", ["→ ISR → DPC → Systemfaden"]),
        ("callout", ["→ KTIMER + KDPC"]), ("kthread", ["→ PsCreateSystemThread"]),
        ("bus_dma", ["→ HalAllocateCommonBuffer"]), ("PCI, BAR", ["→ HalGetBusDataByOffset,", "MmMapIoSpace"]),
        ("malloc", ["→ ExAllocatePoolWithTag", "+ Poolwache"]), ("newbus · sysctl", ["→ Linker-Sektionen", "NTUDRV / NTUSYS"])]):
    box(20 + (i % 4)*222, 660 + (i // 4)*60, 214, 54, a, b, "#fff", "#b04a55", gr=12.5)
pfeil(787, 813)

band(815, 60, "6  NT-Kern und HAL", "#eeeeee")
box(250, 828, 200, 38, "ntoskrnl / ntkrnlmp", [], "#fff", "#666")
box(470, 828, 200, 38, "hal / halapic / halmps", [], "#fff", "#666")
pfeil(877, 903, "Register, DMA, Unterbrechung (INTx)")
band(905, 95, "7  Hardware (PCI)", "#e9eef3")
for i, (a, b) in enumerate([("UHCI", ["Intel, VIA"]), ("OHCI", ["AMD SB7x0, NEC, SiS"]), ("EHCI", ["USB 2.0"]),
                            ("xHCI", ["NEC µPD720200, Intel"]), ("USB-Geräte", ["Low/Full/High/Super"])]):
    box(20 + i*178, 925, 170, 60, a, b, "#fff", "#46637d")

textkasten(925, 45, 345, 395, "Wie ein Gerät seinen Treiber findet", [
    "Das Gerät meldet VID:PID und je",
    "Schnittstelle ein Klassen-Tripel",
    "(Klasse / Unterklasse / Protokoll).",
    "Jede Schnittstelle wird allen oberen",
    "Treibern angeboten (newbus probe):",
    "",
    "03 HID Boot/Bericht  → Tastatur, Maus",
    "03 HID Gamepad       → Gamepad",
    "FF/5D/01, FF/47/D0   → Xbox 360 / One",
    "08/06,05,02/50       → Speicher",
    "07/01/01 oder 02     → Drucker",
    "alles andere         → ntugen (UsbDevN)",
    "",
    "ntugen hat die niedrigste Güte",
    "(BUS_PROBE_HOOVER): es bekommt nur, was",
    "sonst keiner nimmt — nichts geht verloren.",
    "",
    "HKLM\\HARDWARE\\DEVICEMAP\\USB:",
    "  UsbDev0 = VID_0627&PID_0001&MI_00 03/00/00",
    "",
    "Neue Geräteart: Programm gegen UsbDevN",
    "(ntusbgen.h) oder oberer Treiber mit",
    "probe auf Klasse oder VID:PID."], "#fffbe6", "#a08a2a")

textkasten(925, 455, 345, 330, "Unterbrechungsweg (nt/ntintr.c)", [
    "FreeBSDs Handler ist ein ithread: er",
    "nimmt Mutexe und wartet. Auf DIRQL",
    "wäre das ein Bluescreen. Deshalb:",
    "",
    "ntu_isr          DIRQL",
    "  unser Baustein? Quelle abstellen,",
    "  DPC einreihen — sonst nichts",
    "ntu_dpc          DISPATCH_LEVEL",
    "  Ereignis setzen",
    "ntu_ithread_body PASSIVE_LEVEL",
    "  FreeBSD-Handler rufen,",
    "  Quelle wieder freigeben",
    "",
    "Die PCI-Leitung ist pegelgesteuert:",
    "ohne Abstellen im ISR käme sofort die",
    "nächste — ein Sturm wie ein Hänger.",
    "",
    "Herzschlag: unt = faden, verl 0"], "#f0f7ff", "#4a78a8")

textkasten(925, 800, 345, 200, "Bauformen", [
    "usbnt.sys   alle vier Bauarten (empf.)",
    "uhcint.sys  nur UHCI",
    "ohcint.sys  nur OHCI",
    "ehcint.sys  nur EHCI",
    "xhcint.sys  nur xHCI",
    "",
    "Diagnose:   Eingabe-Protokoll auf COM1",
    "Herzschlag: alle 5 s Zähler, Hänger"], "#f4f4f4", "#666")
o.append('</svg>')
ziel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ARCHITEKTUR.svg')
open(ziel, 'w', encoding='utf-8').write("\n".join(o))
print("geschrieben:", os.path.normpath(ziel))
