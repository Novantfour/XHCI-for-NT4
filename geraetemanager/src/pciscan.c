/*++

    pciscan.c -- PCI-Bestandsaufnahme fuer Windows NT 4.0

    Ein klassischer Treiber, kein PnP.  Er liest den Konfigurationsraum
    aller PCI-Geraete und gibt das Ergebnis als Text zurueck.  Nichts
    wird geschrieben.

    Das Verfahren stammt aus private/ntos/dd/pcidump/pcidump.c, Zeile 196:
    ueber alle Busse und Steckplaetze HalGetBusData mit PCIConfiguration
    rufen.  Meldet ein Steckplatz VendorID 0xFFFF, ist dort nichts.

    Der beständige Schluessel eines Geraets ist VendorID, DeviceID und
    Subsystem -- die stehen fest, solange die Karte dieselbe ist.  Bus,
    Steckplatz, IRQ und die Basisadressen aendern sich, sobald man
    umsteckt oder das BIOS anders vergibt; sie werden deshalb als
    veraenderliche Merkmale gefuehrt, nicht als Kennung.

--*/

#include "ntos.h"
#include "pci.h"

int __cdecl sprintf(char*, const char*, ...);

#define DEVNAME     L"\\Device\\PciScan"
#define LINKNAME    L"\\DosDevices\\PciScan"

#define IOCTL_PCI_GET_REPORT   0x00226000
#define IOCTL_PCI_GET_LIST     0x00226004

/*
    Die Liste in Binaerform, fuer HWSCAN.

    Der Text ist zum Lesen da; wer damit rechnen will, soll nicht
    zurueckparsen muessen.  Aufbau bewusst flach und ohne Zeiger, damit
    beide Seiten dieselbe Sicht haben.
*/

#pragma pack(1)

typedef struct _PCI_ENTRY {
    ULONG  Bus;
    ULONG  Device;
    ULONG  Function;
    USHORT VendorID;
    USHORT DeviceID;
    USHORT SubVendorID;
    USHORT SubSystemID;
    UCHAR  BaseClass;
    UCHAR  SubClass;
    UCHAR  RevisionID;
    UCHAR  HeaderType;
    ULONG  Bar[6];
    ULONG  InterruptLine;
} PCI_ENTRY;

#pragma pack()

#define MAX_ENTRIES 128

static PCI_ENTRY gList[MAX_ENTRIES];
static ULONG     gListCount;

#define REPORT_MAX  60000

static char  gReport[REPORT_MAX];
static ULONG gUsed;

/* ------------------------------------------------------------------ */

static void Put(const char* s)
{
    while (*s && gUsed < REPORT_MAX - 1) {
        gReport[gUsed++] = *s++;
    }
    gReport[gUsed] = 0;
}

static void Line(const char* s) { Put(s); Put("\r\n"); }

/*
    Immer vier Argumente, immer Zahlen.

    Der erste Bau ist hier abgestuerzt: zweimal stand ein %s in der
    Formatzeichenkette und bekam eine 0 oder 1 als Argument.  sprintf hat
    die 1 als Zeiger aufgefasst und von Adresse 1 gelesen -- eine
    Zugriffsverletzung mitten in ntoskrnl, wo sprintf wohnt, ausgeloest
    von diesem Treiber.

    Deshalb: durch diese Funktion gehen ausschliesslich Zahlen.  Text
    wird mit Put() angehaengt, nie ueber einen Platzhalter.
*/

static void PutF(const char* fmt, ULONG a, ULONG b, ULONG c, ULONG d)
{
    char tmp[256];
    sprintf(tmp, fmt, a, b, c, d);
    Put(tmp);
}

/* ------------------------------------------------------------------ */
/*  Geraeteklassen im Klartext                                         */
/* ------------------------------------------------------------------ */

/*
    Nach der PCI-Festlegung, Anhang D.  Nur die Hauptklasse -- die
    Unterklassen waeren eine lange Tabelle fuer wenig Gewinn.
*/

static const char* ClassName(UCHAR base, UCHAR sub)
{
    switch (base) {
    case 0x00: return "vor der Klassifizierung";
    case 0x01:
        switch (sub) {
        case 0x00: return "Massenspeicher, SCSI";
        case 0x01: return "Massenspeicher, IDE";
        case 0x05: return "Massenspeicher, ATA";
        case 0x06: return "Massenspeicher, SATA";
        case 0x08: return "Massenspeicher, NVMe";
        }
        return "Massenspeicher";
    case 0x02:
        if (sub == 0x00) return "Netzwerk, Ethernet";
        return "Netzwerk";
    case 0x03:
        if (sub == 0x00) return "Anzeige, VGA";
        return "Anzeige";
    case 0x04:
        if (sub == 0x01) return "Multimedia, Audio";
        if (sub == 0x03) return "Multimedia, HD Audio";
        return "Multimedia";
    case 0x05: return "Speichercontroller";
    case 0x06:
        if (sub == 0x00) return "Bruecke, Host";
        if (sub == 0x01) return "Bruecke, ISA";
        if (sub == 0x04) return "Bruecke, PCI-PCI";
        return "Bruecke";
    case 0x07: return "Kommunikation";
    case 0x08: return "Systembaustein";
    case 0x09: return "Eingabegeraet";
    case 0x0A: return "Dockingstation";
    case 0x0B: return "Prozessor";
    case 0x0C:
        if (sub == 0x03) return "Serieller Bus, USB";
        if (sub == 0x05) return "Serieller Bus, SMBus";
        return "Serieller Bus";
    case 0x0D: return "Funk";
    case 0x0E: return "Intelligente E/A";
    case 0x0F: return "Satellit";
    case 0x10: return "Verschluesselung";
    case 0x11: return "Signalverarbeitung";
    }
    return "unbekannt";
}

/*
    Ein paar Hersteller, damit die Liste lesbar ist.  Bewusst kurz --
    eine vollstaendige Liste waere hundert Kilobyte, und der Zweck ist
    Wiedererkennung, nicht Vollstaendigkeit.
*/

static const char* VendorName(USHORT v)
{
    switch (v) {
    case 0x1002: return "ATI/AMD";
    case 0x1022: return "AMD";
    case 0x10DE: return "nVidia";
    case 0x10EC: return "Realtek";
    case 0x1102: return "Creative";
    case 0x1106: return "VIA";
    case 0x1179: return "Toshiba";
    case 0x11AB: return "Marvell";
    case 0x1234: return "Bochs/QEMU";
    case 0x1283: return "ITE";
    case 0x1969: return "Atheros";
    case 0x197B: return "JMicron";
    case 0x8086: return "Intel";
    case 0x9004:
    case 0x9005: return "Adaptec";
    case 0x104B: return "BusLogic";
    case 0x1AF4: return "Virtio";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    }
    return "";
}

/* ------------------------------------------------------------------ */
/*  Der Durchlauf                                                      */
/* ------------------------------------------------------------------ */

static void ReportDevice(ULONG bus, ULONG dev, ULONG fn,
                         PPCI_COMMON_CONFIG c)
{
    ULONG i;
    int   nbar = 0;

    Line("");

    PutF("PCI %02x:%02x.%x   VEN_%04x", bus, dev, fn, c->VendorID);
    PutF("&DEV_%04x", c->DeviceID, 0, 0, 0);

    if ((c->HeaderType & 0x7F) == 0) {
        PutF("&SUBSYS_%04x%04x",
             c->u.type0.SubSystemID, c->u.type0.SubVendorID, 0, 0);
    }
    Line("");

    Put("  Hersteller     ");
    Put(VendorName(c->VendorID));
    Line("");

    PutF("  Klasse         %02x%02x  ", c->BaseClass, c->SubClass, 0, 0);
    Put(ClassName(c->BaseClass, c->SubClass));
    Line("");

    PutF("  Revision       %02x\r\n", c->RevisionID, 0, 0, 0);
    PutF("  Kopftyp        %02x", c->HeaderType & 0x7F, 0, 0, 0);
    if (c->HeaderType & 0x80) {
        Put("   mehrere Funktionen");
    }
    Line("");

    if ((c->HeaderType & 0x7F) != 0) {
        Line("  (Bruecke, keine Basisadressen aufgefuehrt)");
        return;
    }

    /*
        Die Basisadressen.  Bit 0 unterscheidet E/A-Port von Speicher.
        Diese Bereiche sind es, die sich spaeter mit HKLM\HARDWARE\
        ResourceMap vergleichen lassen -- dort steht, welcher Treiber
        welchen Bereich belegt hat, und daraus ergibt sich die Zuordnung
        Geraet zu Treiber ohne jede Tabelle von Hand.
    */

    for (i = 0; i < PCI_TYPE0_ADDRESSES; i += 1) {

        ULONG b = c->u.type0.BaseAddresses[i];

        if (b == 0) {
            continue;
        }

        if (nbar == 0) {
            Line("  Basisadressen");
        }
        nbar += 1;

        if (b & 1) {
            PutF("    BAR%u  Port    %04x\r\n", i, b & 0xFFFFFFFC, 0, 0);
        } else {
            PutF("    BAR%u  Speicher %08x", i, b & 0xFFFFFFF0, 0, 0);
            if (b & 0x08) {
                Put("  vorablesbar");
            }
            Line("");
        }
    }

    /*
        Die Interruptzeile ist Beiwerk, keine Aussage.

        Auf dieser Maschine melden mehrere Geraete 128 und der
        Tastaturcontroller meldet 0xFFFFFFFF -- das sind Platzhalter des
        BIOS, keine Unterbrechungsnummern.  NT 4.0 holt seine Zuordnung
        ohnehin aus der PCI-Routingtabelle, nicht von hier.  Fuer den
        Abgleich mit der ResourceMap taugen deshalb nur Port- und
        Speicherbereiche.
    */

    if (c->u.type0.InterruptLine != 0 && c->u.type0.InterruptLine != 0xFF) {
        PutF("  Interrupt      %u   Pin %u   (BIOS-Angabe, unverbindlich)\r\n",
             c->u.type0.InterruptLine, c->u.type0.InterruptPin, 0, 0);
    } else if (c->u.type0.InterruptPin != 0) {
        PutF("  Interrupt      nicht zugewiesen   Pin %u\r\n",
             c->u.type0.InterruptPin, 0, 0, 0);
    }
}

static void Scan(void)
{
    ULONG bus, dev, fn;
    ULONG found = 0;
    ULONG buses = 0;

    gUsed = 0;
    gReport[0] = 0;

    Line("PCI-Bestandsaufnahme fuer Windows NT 4.0");
    Line("");
    Line("Der bestaendige Schluessel eines Geraets ist VEN, DEV und SUBSYS.");
    Line("Bus, Steckplatz, Interrupt und die Basisadressen aendern sich beim");
    Line("Umstecken -- sie sind Merkmale, keine Kennung.");
    Line("");

    for (bus = 0; bus < 256; bus += 1) {

        ULONG onThisBus = 0;

        for (dev = 0; dev < 32; dev += 1) {

            for (fn = 0; fn < 8; fn += 1) {

                PCI_COMMON_CONFIG cfg;
                ULONG             got;
                ULONG             slot;

                /*
                    SlotNumber ist bei HalGetBusData in Geraet und
                    Funktion geteilt: die unteren fuenf Bit das Geraet,
                    die oberen drei die Funktion.
                */

                slot = (dev & 0x1F) | ((fn & 0x07) << 5);

                got = HalGetBusData(PCIConfiguration, bus, slot,
                                    &cfg, sizeof(PCI_COMMON_CONFIG));

                if (got < 2 || cfg.VendorID == 0xFFFF || cfg.VendorID == 0) {

                    /*
                        Ist Funktion 0 leer, ist der ganze Steckplatz
                        leer -- so steht es auch in pci_scan_slot des
                        Linux-Kerns.
                    */

                    if (fn == 0) {
                        break;
                    }
                    continue;
                }

                ReportDevice(bus, dev, fn, &cfg);
                found += 1;
                onThisBus += 1;

                if (gListCount < MAX_ENTRIES) {

                    PCI_ENTRY* e = &gList[gListCount];
                    ULONG      q;

                    gListCount += 1;

                    e->Bus        = bus;
                    e->Device     = dev;
                    e->Function   = fn;
                    e->VendorID   = cfg.VendorID;
                    e->DeviceID   = cfg.DeviceID;
                    e->BaseClass  = cfg.BaseClass;
                    e->SubClass   = cfg.SubClass;
                    e->RevisionID = cfg.RevisionID;
                    e->HeaderType = cfg.HeaderType;

                    for (q = 0; q < 6; q += 1) {
                        e->Bar[q] = 0;
                    }
                    e->SubVendorID  = 0;
                    e->SubSystemID  = 0;
                    e->InterruptLine = 0;

                    if ((cfg.HeaderType & 0x7F) == 0) {
                        for (q = 0; q < PCI_TYPE0_ADDRESSES && q < 6; q += 1) {
                            e->Bar[q] = cfg.u.type0.BaseAddresses[q];
                        }
                        e->SubVendorID   = cfg.u.type0.SubVendorID;
                        e->SubSystemID   = cfg.u.type0.SubSystemID;
                        e->InterruptLine = cfg.u.type0.InterruptLine;
                    }
                }

                /*
                    Ist Bit 7 des Kopftyps nicht gesetzt, hat das Geraet
                    nur eine Funktion.
                */

                if (fn == 0 && (cfg.HeaderType & 0x80) == 0) {
                    break;
                }
            }
        }

        if (onThisBus != 0) {
            buses += 1;
        }

        /*
            Ohne Bruecken gibt es keine hohen Busnummern.  Nach dem
            ersten leeren Bus jenseits von Bus 8 wird abgebrochen, damit
            der Durchlauf nicht 256 mal ins Leere greift.
        */

        if (bus > 8 && onThisBus == 0) {
            break;
        }
    }

    Line("");
    PutF("Zusammen %u Geraete auf %u Bussen.\r\n", found, buses, 0, 0);
    Line("Es wurde nichts geschrieben, nur gelesen.");
}

/* ------------------------------------------------------------------ */
/*  Treibergeruest                                                     */
/* ------------------------------------------------------------------ */

static NTSTATUS DispatchOpenClose(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);

    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchControl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG    n  = 0;

    UNREFERENCED_PARAMETER(dev);

    if (s->Parameters.DeviceIoControl.IoControlCode == IOCTL_PCI_GET_LIST) {

        ULONG want = sizeof(ULONG) + gListCount * sizeof(PCI_ENTRY);
        ULONG have = s->Parameters.DeviceIoControl.OutputBufferLength;

        if (have < want) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            PUCHAR out = (PUCHAR)irp->AssociatedIrp.SystemBuffer;

            *(PULONG)out = gListCount;
            RtlCopyMemory(out + sizeof(ULONG), gList,
                          gListCount * sizeof(PCI_ENTRY));
            st = STATUS_SUCCESS;
            n  = want;
        }

    } else if (s->Parameters.DeviceIoControl.IoControlCode == IOCTL_PCI_GET_REPORT) {

        ULONG want = gUsed + 1;
        ULONG have = s->Parameters.DeviceIoControl.OutputBufferLength;

        if (have < want) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, gReport, want);
            st = STATUS_SUCCESS;
            n  = want;
        }
    }

    irp->IoStatus.Status      = st;
    irp->IoStatus.Information = n;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static VOID DriverUnload(PDRIVER_OBJECT drv)
{
    UNICODE_STRING link;

    RtlInitUnicodeString(&link, LINKNAME);
    IoDeleteSymbolicLink(&link);

    if (drv->DeviceObject) {
        IoDeleteDevice(drv->DeviceObject);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNICODE_STRING name, link;
    PDEVICE_OBJECT dev;
    NTSTATUS       st;

    UNREFERENCED_PARAMETER(reg);

    RtlInitUnicodeString(&name, DEVNAME);
    RtlInitUnicodeString(&link, LINKNAME);

    st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if (!NT_SUCCESS(st)) return st;

    st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(dev);
        return st;
    }

    dev->Flags |= DO_BUFFERED_IO;

    drv->MajorFunction[IRP_MJ_CREATE]         = DispatchOpenClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = DispatchOpenClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchControl;
    drv->DriverUnload                         = DriverUnload;

    Scan();

    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
