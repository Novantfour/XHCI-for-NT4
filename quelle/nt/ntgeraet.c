/*
 *  nt/ntgeraet.c -- PCI, Betriebsmittel, Unterbrechung, Uhr, Faden.
 *
 *  Die geraetenahe Haelfte der Unterlage, Schritt 4 des Skills.  Alles
 *  hier ist gegen tools/ntkrnlmp.def geprueft: was dort fehlt, liegt in
 *  der HAL, und der Treiber bindet gegen beide Bibliotheken.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/callout.h>
#include <sys/proc.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>


/* ====================================================================
 *  PCI-Konfigurationsraum
 *
 *  HalGetBusDataByOffset / HalSetBusDataByOffset, PCIConfiguration.  Die
 *  Slotnummer ist nicht die Geraetenummer: NT packt Geraet und Funktion
 *  in ein Wort, genau wie IopScanPciBus es tut --
 *
 *      slot = (dev & 0x1F) | ((fn & 0x07) << 5)
 *
 *  Wer das uebersieht, liest von Funktion 0 statt von Funktion 2, und
 *  bei USB ist Funktion 2 fast immer der EHCI.  Das ist kein
 *  theoretischer Fehler: auf SB700 sitzen OHCI auf .0 und .1, EHCI
 *  auf .2 desselben Geraets.
 * ==================================================================== */

static ntu_pci_ivars *
ntu_iv (device_t dev)
{
    /*  pci_read_config wird immer am PCI-Kind gerufen, nie tiefer.  Wenn
     *  doch, gehen wir nach oben, bis wir ivars mit Bus/Slot finden --
     *  billiger als eine falsche Antwort.  */

    while (dev != 0) {
        if (dev->Parent != 0 && dev->Parent->Parent == 0) {
            return (ntu_pci_ivars *)dev->Ivars;
        }

        dev = dev->Parent;
    }

    return 0;
}

uint32_t
pci_read_config (device_t dev, int reg, int width)
{
    ntu_pci_ivars *iv = ntu_iv(dev);
    ULONG          v  = 0;
    ULONG          got;

    if (iv == 0) {
        return 0xFFFFFFFFu;
    }

    got = HalGetBusDataByOffset(PCIConfiguration, iv->Bus,
                                (iv->Slot & 0x1F) | ((iv->Func & 7) << 5),
                                &v, (ULONG)reg, (ULONG)width);

    if (got < (ULONG)width) {
        return 0xFFFFFFFFu;
    }

    switch (width) {
        case 1:  return (uint32_t)(v & 0xFF);
        case 2:  return (uint32_t)(v & 0xFFFF);
        default: return (uint32_t)v;
    }
}

void
pci_write_config (device_t dev, int reg, uint32_t val, int width)
{
    ntu_pci_ivars *iv = ntu_iv(dev);
    ULONG          v  = val;

    if (iv == 0) {
        return;
    }

    HalSetBusDataByOffset(PCIConfiguration, iv->Bus,
                          (iv->Slot & 0x1F) | ((iv->Func & 7) << 5),
                          &v, (ULONG)reg, (ULONG)width);
}

uint16_t pci_get_vendor  (device_t d) { return ntu_iv(d)->Vendor; }
uint16_t pci_get_device  (device_t d) { return ntu_iv(d)->Device; }
uint8_t  pci_get_class   (device_t d) { return ntu_iv(d)->Class; }
uint8_t  pci_get_subclass(device_t d) { return ntu_iv(d)->SubClass; }
uint8_t  pci_get_progif  (device_t d) { return ntu_iv(d)->ProgIf; }
uint8_t  pci_get_revid   (device_t d) { return ntu_iv(d)->Revision; }

uint32_t
pci_get_devid (device_t d)
{
    ntu_pci_ivars *iv = ntu_iv(d);

    /*  FreeBSD legt Geraet nach oben, Hersteller nach unten.  Die
     *  Sondertabellen der Treiber vergleichen gegen genau diese Form;
     *  eine vertauschte Haelfte faellt erst auf, wenn ein Quirk nicht
     *  greift, und das kostet einen Tag.  */

    return ((uint32_t)iv->Device << 16) | (uint32_t)iv->Vendor;
}

void
pci_enable_busmaster (device_t d)
{
    uint32_t c = pci_read_config(d, PCIR_COMMAND, 2);

    pci_write_config(d, PCIR_COMMAND,
                     c | PCIM_CMD_BUSMASTEREN | PCIM_CMD_MEMEN
                       | PCIM_CMD_PORTEN, 2);
}

void
pci_disable_busmaster (device_t d)
{
    uint32_t c = pci_read_config(d, PCIR_COMMAND, 2);

    pci_write_config(d, PCIR_COMMAND, c & ~PCIM_CMD_BUSMASTEREN, 2);
}

int
pci_get_powerstate (device_t d)
{
    return PCI_POWERSTATE_D0;       /* NT 4.0 laesst alles in D0 */
}

int
pci_set_powerstate (device_t d, int state)
{
    return PCI_POWERSTATE_D0;
}

device_t
pci_find_device (uint16_t vendor, uint16_t device)
{
    return 0;                       /* kein Rufer im USB-Stapel */
}


/* ====================================================================
 *  Betriebsmittel
 *
 *  BAR0 im Speicher, BAR4 im E/A-Raum, dazu die Leitungsnummer.  Welcher
 *  BAR es ist, sagt der Treiber ueber die rid -- und das ist genau das,
 *  was der erweiterte Suchlauf im Bericht schon ausgibt.
 *
 *  HalTranslateBusAddress zuerst: auf einem PCI-Brett ist die
 *  Bus-Adresse zwar meist gleich der physischen, aber "meist" ist keine
 *  Grundlage, und der Ruf kostet nichts.
 * ==================================================================== */

struct resource *
bus_alloc_resource_any (device_t dev, int type, int *rid, unsigned int flags)
{
    struct resource  *r;
    ntu_pci_ivars    *iv = ntu_iv(dev);
    uint32_t          bar;
    PHYSICAL_ADDRESS  bus;
    PHYSICAL_ADDRESS  phys;
    ULONG             space;

    if (iv == 0) {
        return 0;
    }

    r = (struct resource *)ntu_malloc(sizeof(*r), M_ZERO);

    if (r == 0) {
        return 0;
    }

    r->r_type = type;
    r->r_rid  = (rid != 0) ? *rid : 0;

    if (type == SYS_RES_IRQ) {
        r->r_irq = iv->Irq;
        return r;
    }

    bar = pci_read_config(dev, r->r_rid, 4);

    if (type == SYS_RES_IOPORT) {

        /*  Bit 0 gesetzt heisst E/A-Raum; die unteren zwei Bit gehoeren
         *  nicht zur Adresse.  */

        r->r_start = bar & ~0x3u;
        r->r_tag   = NTU_SPACE_IO;
        space      = 1;

        bus.LowPart  = r->r_start;
        bus.HighPart = 0;

        if (!HalTranslateBusAddress(PCIBus, iv->Bus, bus, &space, &phys)) {
            ntu_free(r);
            return 0;
        }

        /*  Bleibt es E/A-Raum, ist der Griff die Portnummer selbst.  */

        r->r_handle = (bus_space_handle_t)(ULONG_PTR)phys.LowPart;
        r->r_len    = 0x20;          /* UHCI: 32 Byte Registerblock */
        dev->Res    = r;
        return r;
    }

    /*  Speicher.  Bit 0 ist frei, Bit 1..2 sagen die Breite, Bit 3 ob
     *  vorablesbar -- alles nicht Teil der Adresse.  */

    if ((bar & 0x1u) != 0) {
        ntu_printf("usb: BAR %02x ist ein E/A-Bereich, erwartet war "
                   "Speicher (%08x)\n", r->r_rid, bar);
        ntu_free(r);
        return 0;
    }

    /*  64-Bit-Registerblock (Bit 1..2 = 10b): die obere Haelfte der
     *  Adresse steht im NAECHSTEN Register.  xHCI-Bausteine haben fast
     *  immer einen solchen, und neuere Hauptplatinen legen ihn mit
     *  "Above 4G Decoding" oberhalb von 4 GB.  NT 4.0 kann dort nichts
     *  abbilden.  Frueher wurde die obere Haelfte gar nicht gelesen:
     *  der Treiber bildete dann die untere Haelfte ab -- FREMDEN Speicher
     *  -- und las dort vermeintliche Register.  Das ist kein Fall zum
     *  Weitermachen, sondern einer fuer eine klare Meldung.  */

    if ((bar & 0x6u) == 0x4u) {

        uint32_t oben = pci_read_config(dev, r->r_rid + 4, 4);

        if (oben != 0) {
            ntu_printf("usb: Registerblock liegt bei %08x%08x, oberhalb "
                       "von 4 GB -- fuer NT 4.0 unerreichbar.  Im BIOS "
                       "\"Above 4G Decoding\" abschalten.\n",
                       oben, bar & ~0xFu);
            ntu_free(r);
            return 0;
        }
    }

    r->r_start = bar & ~0xFu;

    if (r->r_start == 0) {
        ntu_printf("usb: BAR %02x ist leer -- das BIOS hat dem Baustein "
                   "keine Adresse gegeben\n", r->r_rid);
        ntu_free(r);
        return 0;
    }

    space        = 0;
    bus.LowPart  = r->r_start;
    bus.HighPart = 0;

    if (!HalTranslateBusAddress(PCIBus, iv->Bus, bus, &space, &phys)) {
        ntu_free(r);
        return 0;
    }

    /*  Eine Seite reicht fuer OHCI (0x100) und EHCI (selten ueber
     *  0x100).  Bei xHCI reicht sie nicht, und eine geratene Zahl auch
     *  nicht: der Registerblock haengt an der Anzahl der Anschluesse,
     *  der Unterbrecher und der Plaetze, und die erweiterten
     *  Faehigkeiten koennen weit hinten liegen.
     *
     *  Deshalb zweistufig: erst eine Seite abbilden, den Baustein
     *  fragen, dann neu abbilden.  Mehr abzubilden, als der BAR
     *  hergibt, waere ein Zugriff auf fremden Speicher -- und ihn
     *  ueber die uebliche Groessenermittlung (alles Einsen schreiben
     *  und zuruecklesen) zu erfragen, wuerde den Baustein mitten im
     *  Betrieb abmelden.  */

    r->r_len = 0x1000;

    r->r_handle = (bus_space_handle_t)MmMapIoSpace(phys, r->r_len,
                                                   MmNonCached);

    if (r->r_handle == 0) {
        ntu_free(r);
        return 0;
    }

    if (iv->ProgIf == PCIP_SERIALBUS_USB_XHCI) {

        ULONG caplen = ntu_r1(NTU_SPACE_MEM, r->r_handle, 0x00);
        ULONG hcs1   = ntu_r4(NTU_SPACE_MEM, r->r_handle, 0x04);
        ULONG hcc1   = ntu_r4(NTU_SPACE_MEM, r->r_handle, 0x10);
        ULONG dboff  = ntu_r4(NTU_SPACE_MEM, r->r_handle, 0x14) & ~0x3u;
        ULONG rtsoff = ntu_r4(NTU_SPACE_MEM, r->r_handle, 0x18) & ~0x1Fu;

        ULONG plaetze = hcs1 & 0xFF;              /* MaxSlots        */
        ULONG unterbr = (hcs1 >> 8) & 0x7FF;      /* MaxIntrs        */
        ULONG anschl  = (hcs1 >> 24) & 0xFF;      /* MaxPorts        */
        ULONG xecp    = ((hcc1 >> 16) & 0xFFFF) * 4;

        ULONG noetig = caplen + 0x400 + anschl * 0x10;

        if (rtsoff + 0x20 + unterbr * 0x20 > noetig) {
            noetig = rtsoff + 0x20 + unterbr * 0x20;
        }

        if (dboff + (plaetze + 1) * 4 > noetig) {
            noetig = dboff + (plaetze + 1) * 4;
        }

        if (xecp != 0 && xecp + 0x100 > noetig) {
            noetig = xecp + 0x100;
        }

        noetig = (noetig + PAGE_MASK) & ~((ULONG)PAGE_MASK);

        if (noetig > 0x40000) {
            noetig = 0x40000;       /* 256 kB, mehr gibt es nicht */
        }

        ntu_printf("xhci: caplen %02x  plaetze %u  unterbr %u  anschl %u"
                   "  xecp %05x  -> %u kB\n",
                   caplen, plaetze, unterbr, anschl, xecp, noetig / 1024);

        if (noetig > r->r_len) {

            MmUnmapIoSpace((PVOID)r->r_handle, r->r_len);
            r->r_len    = noetig;
            r->r_handle = (bus_space_handle_t)MmMapIoSpace(phys, r->r_len,
                                                           MmNonCached);

            if (r->r_handle == 0) {
                ntu_printf("xhci: %u kB liessen sich nicht abbilden\n",
                           noetig / 1024);
                ntu_free(r);
                return 0;
            }
        }
    }

    r->r_tag = NTU_SPACE_MEM;
    dev->Res = r;
    return r;
}

int
bus_release_resource (device_t dev, int type, int rid, struct resource *r)
{
    if (r == 0) {
        return 0;
    }

    if (type == SYS_RES_MEMORY && r->r_handle != 0) {
        MmUnmapIoSpace((PVOID)r->r_handle, r->r_len);
    }

    /*  dev->Res zeigt auf genau diesen Block (bus_setup_intr liest ihn);
     *  nach der Freigabe darf er nicht stehen bleiben.  */

    if (dev != 0 && dev->Res == r) {
        dev->Res = 0;
    }

    ntu_free(r);
    return 0;
}


/* ====================================================================
 *  Unterbrechung
 *
 *  Der Umbau steht in ntunterlage.c beschrieben.  Hier wird er
 *  eingehaengt.  Zwei Dinge sind nicht verhandelbar:
 *
 *    ShareVector = TRUE, LevelSensitive.  USB-Bausteine teilen sich die
 *    Leitung fast immer mit der Nachbarfunktion -- auf SB700 haengen
 *    OHCI.0, OHCI.1 und EHCI.2 an derselben.
 *
 *    Der Maskierer.  Ohne ihn wird nicht angehaengt.  Ein ISR, der TRUE
 *    zurueckgibt, ohne die Quelle am Baustein abzustellen, laesst die
 *    pegelgesteuerte Leitung stehen, und der Rechner kommt nie beim
 *    Faden an.
 * ==================================================================== */

/* ==================================================================== *
 *  HKLM\HARDWARE\RESOURCEMAP\USB\usbnt
 *
 *  Jeder Hostbaustein meldet seinen Registerbereich (Speicher exklusiv,
 *  bei UHCI E/A-Ports) und seine Unterbrechung (geteilt) -- so, wie es
 *  WinMSD ("Ressourcen") und andere Treiber sehen.  Die Bausteine haben
 *  keine eigenen NT-Geraeteobjekte; gemeldet wird deshalb EINE Liste auf
 *  Treiberebene, ein Eintrag je Baustein, bei jedem neuen Baustein
 *  vollstaendig neu.  Beim Entladen: leere Liste.
 * ==================================================================== */

PDRIVER_OBJECT ntu_treiberobjekt;

#define NTU_MELDE_MAX 16
static struct {
    ULONG Bus, Start, Len, Irq;
    int   Io;
} ntu_melde[NTU_MELDE_MAX];
static int ntu_melde_n;

static void
ntu_ressourcen_melden (void)
{
    UNICODE_STRING                   klasse;
    PCM_RESOURCE_LIST                l;
    PCM_FULL_RESOURCE_DESCRIPTOR     f;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR  p;
    ULONG                            je, groesse;
    BOOLEAN                          konflikt = FALSE;
    NTSTATUS                         st;
    int                              k;

    if (ntu_treiberobjekt == 0 || ntu_melde_n == 0) {
        return;
    }
    je      = sizeof(CM_FULL_RESOURCE_DESCRIPTOR) + sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    groesse = FIELD_OFFSET(CM_RESOURCE_LIST, List) + ntu_melde_n * je;
    l = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(PagedPool, groesse, 'MuN ');
    if (l == 0) {
        return;
    }
    RtlZeroMemory(l, groesse);
    l->Count = (ULONG)ntu_melde_n;
    f = &l->List[0];
    for (k = 0; k < ntu_melde_n; k += 1) {
        f->InterfaceType                  = PCIBus;
        f->BusNumber                      = ntu_melde[k].Bus;
        f->PartialResourceList.Version    = 1;
        f->PartialResourceList.Revision   = 1;
        f->PartialResourceList.Count      = ntu_melde[k].Irq ? 2 : 1;
        p = &f->PartialResourceList.PartialDescriptors[0];
        if (ntu_melde[k].Io) {
            p->Type             = CmResourceTypePort;
            p->ShareDisposition = CmResourceShareDeviceExclusive;
            p->Flags            = CM_RESOURCE_PORT_IO;
            p->u.Port.Start.LowPart = ntu_melde[k].Start;
            p->u.Port.Length    = ntu_melde[k].Len;
        } else {
            p->Type             = CmResourceTypeMemory;
            p->ShareDisposition = CmResourceShareDeviceExclusive;
            p->Flags            = CM_RESOURCE_MEMORY_READ_WRITE;
            p->u.Memory.Start.LowPart = ntu_melde[k].Start;
            p->u.Memory.Length  = ntu_melde[k].Len;
        }
        p += 1;
        p->Type                 = CmResourceTypeInterrupt;
        p->ShareDisposition     = CmResourceShareShared;
        p->Flags                = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
        p->u.Interrupt.Level    = ntu_melde[k].Irq;
        p->u.Interrupt.Vector   = ntu_melde[k].Irq;
        p->u.Interrupt.Affinity = (KAFFINITY)-1;
        f = (PCM_FULL_RESOURCE_DESCRIPTOR)((PUCHAR)f + je);
    }
    RtlInitUnicodeString(&klasse, L"USB");
    st = IoReportResourceUsage(&klasse, ntu_treiberobjekt, l, groesse, NULL, NULL, 0,
                               FALSE, &konflikt);
    if (!NT_SUCCESS(st) || konflikt) {
        ntu_printf("usb: ResourceMap: %08x%s\n", st, konflikt ? ", Konflikt gemeldet" : "");
    }
    ExFreePool(l);
}

static void
ntu_ressourcen_eintragen (ntu_pci_ivars *iv, struct resource *r, ULONG irq)
{
    if (r == 0 || ntu_melde_n >= NTU_MELDE_MAX) {
        return;
    }
    ntu_melde[ntu_melde_n].Bus   = iv->Bus;
    ntu_melde[ntu_melde_n].Start = (ULONG)r->r_start;
    ntu_melde[ntu_melde_n].Len   = (ULONG)r->r_len;
    ntu_melde[ntu_melde_n].Io    = (r->r_tag == NTU_SPACE_IO);
    ntu_melde[ntu_melde_n].Irq   = irq;
    ntu_melde_n += 1;
    ntu_ressourcen_melden();
}

/*  Beim Entladen: Eintrag aus der ResourceMap nehmen.  */
VOID
ntu_ressourcen_abmelden (VOID)
{
    UNICODE_STRING klasse;
    BOOLEAN        konflikt = FALSE;
    CM_RESOURCE_LIST leer;

    if (ntu_treiberobjekt == 0 || ntu_melde_n == 0) {
        return;
    }
    RtlZeroMemory(&leer, sizeof(leer));
    RtlInitUnicodeString(&klasse, L"USB");
    IoReportResourceUsage(&klasse, ntu_treiberobjekt, &leer,
                          FIELD_OFFSET(CM_RESOURCE_LIST, List), NULL, NULL, 0, FALSE, &konflikt);
    ntu_melde_n = 0;
}

int
bus_setup_intr (device_t dev, struct resource *r, int flags,
                int (*filter)(void *), void (*handler)(void *),
                void *arg, void **cookiep)
{
    ntu_intr      *i;
    ntu_pci_ivars *iv = ntu_iv(dev);
    NTSTATUS       st;
    KIRQL          dirql;
    KAFFINITY      aff;
    ULONG          vector;

    if (iv == 0 || handler == 0) {
        return EINVAL;
    }

    i = (ntu_intr *)ntu_malloc(sizeof(*i), M_ZERO);

    if (i == 0) {
        return ENOMEM;
    }

    i->Handler = handler;
    i->Filter  = filter;
    i->Arg     = arg;
    i->ProgIf  = iv->ProgIf;

    /*  Der ISR darf nichts nachschlagen.  Alles, was er zum Abstellen
     *  braucht, wird hier einmal hineingelegt -- der Registerblock aus
     *  bus_alloc_resource_any, nicht die IRQ-Ressource, die uns der
     *  Treiber gerade gibt.  */

    if (dev->Res == 0) {
        ntu_printf("usb: %s hat noch keinen Registerblock -- "
                   "bus_setup_intr zu frueh\n", device_get_nameunit(dev));
        ntu_free(i);
        return EINVAL;
    }

    i->Tag    = dev->Res->r_tag;
    i->Handle = dev->Res->r_handle;
    i->OpBase = 0;

    /*  EHCI und xHCI trennen Faehigkeits- und Betriebsregister.  Die
     *  Laenge des ersten Blocks steht im ersten Byte -- CAPLENGTH.  Sie
     *  ist nicht fest: auf manchen Bausteinen 0x20, auf anderen 0x10.  */

    if (iv->ProgIf == PCIP_SERIALBUS_USB_EHCI ||
        iv->ProgIf == PCIP_SERIALBUS_USB_XHCI) {

        i->OpBase = ntu_r1(i->Tag, i->Handle, 0);

        if (i->OpBase == 0 || i->OpBase > 0x80) {
            ntu_printf("usb: CAPLENGTH ist %02x -- der Registerblock "
                       "antwortet nicht\n", i->OpBase);
            ntu_free(i);
            return ENXIO;
        }
    }

    /*  xHCI sperrt ueber IMAN im Laufzeitblock, nicht ueber USBCMD.  Wo
     *  der Laufzeitblock liegt, sagt RTSOFF; die unteren fuenf Bit
     *  gehoeren nicht zur Adresse.  Unterbrecher 0 liegt bei +0x20.  */

    if (iv->ProgIf == PCIP_SERIALBUS_USB_XHCI) {

        ULONG rts = ntu_r4(i->Tag, i->Handle, 0x18) & ~0x1Fu;

        if (rts == 0 || rts + 0x20 + 4 > dev->Res->r_len) {
            ntu_printf("usb: RTSOFF ist %08x, passt nicht in den "
                       "Registerblock (%x)\n", rts, dev->Res->r_len);
            ntu_free(i);
            return ENXIO;
        }

        i->Iman = rts + 0x20;
    }

    i->Thread = (ntu_thread *)ntu_malloc(sizeof(ntu_thread), M_ZERO);

    if (i->Thread == 0) {
        ntu_free(i);
        return ENOMEM;
    }

    KeInitializeEvent(&i->Wake, SynchronizationEvent, FALSE);
    KeInitializeDpc(&i->Dpc, ntu_dpc, i);

    st = ntu_thread_start(i->Thread, ntu_ithread_body, i, "usb intr");

    if (!NT_SUCCESS(st)) {
        ntu_free(i->Thread);
        ntu_free(i);
        return ENOMEM;
    }

    /*  Leitung 0 und 255 heissen "nicht zugewiesen".  Auf neueren
     *  Brettern im CSM-Betrieb haeufig: das BIOS traegt nur noch fuer den
     *  APIC-Betrieb ein (Leitung 16..55) oder gar nichts.  Unter der
     *  PIC-HAL ist das nicht anschliessbar.  Dann nicht aufgeben,
     *  sondern abfragen.  */

    st = STATUS_INVALID_PARAMETER;

#ifndef NTU_ABFRAGE_ERZWINGEN
#define NTU_ABFRAGE_ERZWINGEN 0     /* Pruefstand: 1 = nie anschliessen */
#endif
    if (r->r_irq != 0 && r->r_irq != 255 && !NTU_ABFRAGE_ERZWINGEN) {

        vector = HalGetInterruptVector(PCIBus, iv->Bus, r->r_irq, r->r_irq,
                                       &dirql, &aff);

        if (vector != 0) {
            st = IoConnectInterrupt(&i->Object, ntu_isr, i, NULL, vector,
                                    dirql, dirql, LevelSensitive, TRUE, aff,
                                    FALSE);
        }
    }

    if (!NT_SUCCESS(st)) {

        LARGE_INTEGER erst;

        ntu_printf("usb: %s: Leitung %u nicht nutzbar (%08x) -- "
                   "Abfragebetrieb im Systemtakt\n",
                   device_get_nameunit(dev), r->r_irq, st);
        i->Object  = NULL;
        i->Abfrage = 1;

        /*  INTx abschalten (PCI 2.3, Befehlsregister Bit 10).  Der
         *  Baustein zieht seine Leitung sonst weiter -- und war sie nur
         *  nicht anschliessbar, weil ein anderer Treiber sie exklusiv
         *  haelt, bliebe dessen Leitung dauerhaft oben.  Aeltere Bausteine
         *  kennen das Bit nicht (fest 0); dort aendert der Schreibzugriff
         *  nichts.  */
        pci_write_config(dev, PCIR_COMMAND,
                         pci_read_config(dev, PCIR_COMMAND, 2) | 0x0400, 2);
        KeInitializeTimer(&i->Takt);
        KeInitializeDpc(&i->TaktDpc, ntu_dpc, i);
        /*  1 ms angefordert; NT 4.0 rundet auf seinen Takt (10-15 ms) --
         *  ExSetTimerResolution gibt es dort noch nicht.  Fuer Tastatur,
         *  Maus und Laufwerk reicht das.  */
        erst.QuadPart = -10000;
        KeSetTimerEx(&i->Takt, erst, 1, &i->TaktDpc);

        *cookiep = i;
        dev->Intr = i;
        ntu_ressourcen_eintragen(iv, dev->Res, 0);
        return 0;
    }

    /*  Leitung angeschlossen: INTx-Sperre (Befehlsregister Bit 10)
     *  loesen.  Sie kann stehen geblieben sein -- vom BIOS, von einem
     *  System, das den Baustein mit MSI betrieb, oder von unserem eigenen
     *  Abfragebetrieb beim vorigen Start, wenn der Warmstart den Baustein
     *  nicht zuruecksetzt.  Dann kaeme nie eine Unterbrechung, und USB
     *  liefe still nur im 50-ms-Sicherungstakt.  */
    {
        uint32_t cmd = pci_read_config(dev, PCIR_COMMAND, 2);

        if (cmd & 0x0400) {
            pci_write_config(dev, PCIR_COMMAND, cmd & ~0x0400u, 2);
            ntu_printf("usb: %s: INTx war abgeschaltet -- eingeschaltet\n",
                       device_get_nameunit(dev));
        }
    }

    /*  Sicherungstakt: auch mit angeschlossener Leitung alle 50 ms
     *  nachsehen.  Unter der MPS-HAL mit aus ACPI erzeugter Tabelle (HP
     *  EliteDesk 705 G5) bekam der xHCI eine Leitung, auf der nichts ankam:
     *  USB stand, das Setup fand sein DVD-Laufwerk nicht.  Kommt die
     *  Unterbrechung, kostet der Takt nichts als einen leeren Lauf des
     *  Handlers; kommt sie nicht, laeuft USB trotzdem.  */
    {
        LARGE_INTEGER erst;
        KeInitializeTimer(&i->Takt);
        KeInitializeDpc(&i->TaktDpc, ntu_dpc, i);
        erst.QuadPart = -500000;
        KeSetTimerEx(&i->Takt, erst, 50, &i->TaktDpc);
    }

    *cookiep = i;
    dev->Intr = i;
    ntu_ressourcen_eintragen(iv, dev->Res, r->r_irq);
    return 0;
}

int
bus_teardown_intr (device_t dev, struct resource *r, void *cookie)
{
    ntu_intr *i = (ntu_intr *)cookie;

    if (i == 0) {
        return 0;
    }

    /*  Reihenfolge: erst abklemmen, dann den Faden beenden.  Andersherum
     *  kann ein DPC noch ein Ereignis setzen, auf das niemand mehr
     *  wartet, und der naechste Ruf laeuft in freigegebenen Speicher.  */

    KeCancelTimer(&i->Takt);               /* Abfrage- oder Sicherungstakt */
    KeRemoveQueueDpc(&i->TaktDpc);
    if (!i->Abfrage) {
        IoDisconnectInterrupt(i->Object);
    }

    /*  Der ISR kann unmittelbar vor dem Abklemmen noch einen DPC
     *  eingereiht haben.  Der liefe sonst nach ntu_free(i) und setzte ein
     *  Ereignis in freigegebenem Speicher.  */

    KeRemoveQueueDpc(&i->Dpc);

    if (dev != 0 && dev->Intr == i) {
        dev->Intr = 0;
    }

    i->Stop = 1;
    KeSetEvent(&i->Wake, 0, FALSE);
    ntu_thread_stop(i->Thread);
    ntu_free(i->Thread);
    ntu_free(i);
    return 0;
}


/* ====================================================================
 *  Uhr
 *
 *  ticks laeuft in einem eigenen Taktgeber hoch, nicht aus KeTickCount:
 *  KeTickCount zaehlt in NT-Takten, und deren Laenge haengt von HAL und
 *  Maschine ab.  Ein eigener Zaehler mit bekanntem hz ist das, was der
 *  Treiber erwartet.
 * ==================================================================== */

static KTIMER ntu_clock;
static KDPC   ntu_clock_dpc;

static VOID
ntu_clock_tick (PKDPC d, PVOID c, PVOID a1, PVOID a2)
{
    ticks += 1;
}

VOID
ntu_clock_start (VOID)
{
    LARGE_INTEGER due;

    due.QuadPart = -NTU_TICK_100NS;

    KeInitializeDpc(&ntu_clock_dpc, ntu_clock_tick, 0);
    KeInitializeTimer(&ntu_clock);
    KeSetTimerEx(&ntu_clock, due, NTU_TICK_100NS / 10000, &ntu_clock_dpc);
}

VOID
ntu_clock_stop (VOID)
{
    KeCancelTimer(&ntu_clock);

    /*  KeCancelTimer nimmt den Zeitgeber aus der Liste, aber ein DPC,
     *  der schon eingestellt ist, laeuft noch.  Unter Windows XP gaebe
     *  es dafuer KeFlushQueuedDpcs -- NT 4.0 hat es nicht (gegen
     *  ntkrnlmp.def geprueft).  Bleibt: warten, bis er durch ist.
     *
     *  Dreissig Millisekunden sind drei Taktschritte.  Ein DPC, der
     *  laenger braucht, haengt ohnehin.  Ohne dieses Warten laeuft der
     *  DPC in Speicher, den das Entladen gleich freigibt -- und das ist
     *  genau der Absturz, den man beim zehnten Laden und Entladen
     *  bekommt und beim ersten nicht.  */

    ntu_pause_ticks(3);
}

VOID
ntu_delay_us (ULONG us)
{
    /*  KeStallExecutionProcessor soll nicht ueber fuenfzig Mikrosekunden
     *  am Stueck gerufen werden -- auf einer Maschine mit Cool'n'Quiet
     *  ist die Schleife sonst nicht mehr verlaesslich.  Gestueckelt.  */

    while (us > 50) {
        KeStallExecutionProcessor(50);
        us -= 50;
    }

    if (us != 0) {
        KeStallExecutionProcessor(us);
    }
}

VOID
ntu_pause_ticks (ULONG n)
{
    LARGE_INTEGER to;

    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {

        /*  Auf DISPATCH_LEVEL darf nicht gewartet werden.  Das darf hier
         *  eigentlich nie vorkommen -- der Fadenumbau ist genau dafuer
         *  da.  Kommt es doch vor, ist das eine Fehlstelle im Umbau und
         *  keine, die man stillschweigend durch Stallen ueberbrueckt.  */

        ntu_printf("usb: pause auf IRQL %u -- Fehlstelle im Fadenumbau\n",
                   KeGetCurrentIrql());
        ntu_delay_us(n * (1000000 / NTU_HZ));
        return;
    }

    to.QuadPart = -((LONGLONG)n * NTU_TICK_100NS);
    KeDelayExecutionThread(KernelMode, FALSE, &to);
}


/* ====================================================================
 *  callout
 *
 *  KTIMER plus KDPC.  Der Rueckruf des USB-Stapels nimmt einen Mutex,
 *  darf also nicht im DPC laufen.  Er wird deshalb ueber denselben Weg
 *  geschickt wie die Unterbrechung -- ein Serialisierungspunkt je Bus
 *  statt zwei.
 * ==================================================================== */

static VOID
ntu_callout_dpc (PKDPC d, PVOID ctx, PVOID a1, PVOID a2)
{
    struct callout *c = (struct callout *)ctx;

    ntu_defer_callout(c);
}

void
callout_init_mtx (struct callout *c, struct mtx *m, int flags)
{
    memset(c, 0, sizeof(*c));
    c->Mtx = m;
    KeInitializeTimer(&c->Timer);
    KeInitializeDpc(&c->Dpc, ntu_callout_dpc, c);
}

void
callout_init (struct callout *c, int mpsafe)
{
    callout_init_mtx(c, 0, 0);
}

int
callout_reset (struct callout *c, int timo, void (*fn)(void *), void *arg)
{
    LARGE_INTEGER due;

    if (timo < 1) {
        timo = 1;
    }

    c->Fn      = fn;
    c->Arg     = arg;
    c->Pending = 1;

    due.QuadPart = -((LONGLONG)timo * NTU_TICK_100NS);
    KeSetTimer(&c->Timer, due, &c->Dpc);
    return 0;
}

int
callout_stop (struct callout *c)
{
    c->Pending = 0;
    return KeCancelTimer(&c->Timer) ? 1 : 0;
}

int
callout_drain (struct callout *c)
{
    return callout_stop(c);
}

int
callout_pending (struct callout *c)
{
    return c->Pending;
}


/* ====================================================================
 *  Faden
 *
 *  usb_process legt je Bus mehrere an -- Steuerung, Explore, Giant,
 *  Non-Giant.  Alle laufen auf PASSIVE_LEVEL, alle duerfen schlafen.
 * ==================================================================== */

static VOID
ntu_thread_trampoline (PVOID ctx)
{
    ntu_thread *t = (ntu_thread *)ctx;

    ntu_wart_name(t->Name);
    t->Body(t->Arg);
    KeSetEvent(&t->Gone, 0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
ntu_thread_start (ntu_thread *t, VOID (*body)(PVOID), PVOID arg,
                  const char *name)
{
    NTSTATUS st;

    memset(t, 0, sizeof(*t));
    t->Body = body;
    t->Arg  = arg;
    t->Name = name;
    KeInitializeEvent(&t->Wake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&t->Gone, NotificationEvent, FALSE);

    st = PsCreateSystemThread(&t->Handle, THREAD_ALL_ACCESS, NULL, NULL,
                              NULL, ntu_thread_trampoline, t);

    if (!NT_SUCCESS(st)) {
        return st;
    }

    ObReferenceObjectByHandle(t->Handle, THREAD_ALL_ACCESS, NULL,
                              KernelMode, (PVOID *)&t->Thread, NULL);
    return st;
}

VOID
ntu_thread_stop (ntu_thread *t)
{
    t->Stop = TRUE;
    KeSetEvent(&t->Wake, 0, FALSE);
    KeWaitForSingleObject(&t->Gone, Executive, KernelMode, FALSE, NULL);

    if (t->Handle != 0) {
        ZwClose(t->Handle);
        t->Handle = 0;
    }
}

int
kproc_kthread_add (void (*fn)(void *), void *arg, struct proc **p,
                   struct thread **td, int flags, int pages,
                   const char *procname, const char *fmt, ...)
{
    ntu_thread *t = (ntu_thread *)ntu_malloc(sizeof(*t), M_ZERO);
    NTSTATUS    st;

    if (t == 0) {
        return ENOMEM;
    }

    st = ntu_thread_start(t, (VOID (*)(PVOID))fn, arg, procname);

    if (!NT_SUCCESS(st)) {
        ntu_free(t);
        return ENOMEM;
    }

    if (td != 0) {
        *td = (struct thread *)t;
    }

    if (p != 0) {
        *p = (struct proc *)t;
    }

    return 0;
}

void
kthread_exit (void)
{
    PsTerminateSystemThread(STATUS_SUCCESS);
}

int
kthread_suspend_check (void)
{
    return 0;
}

int
kthread_suspend (struct thread *td, int timo)
{
    return 0;
}


/* ====================================================================
 *  Kleinkram
 * ==================================================================== */

size_t
strlcpy (char *dst, const char *src, size_t n)
{
    size_t i = 0;

    if (n == 0) {
        return 0;
    }

    while (i + 1 < n && src[i] != '\0') {
        dst[i] = src[i];
        i += 1;
    }

    dst[i] = '\0';

    while (src[i] != '\0') {
        i += 1;
    }

    return i;
}

int
copyin (const void *user, void *kern, size_t n)
{
    /*  Kein Benutzermodus in diesem Aufbau: der Stapel wird nur vom
     *  SCSI-Miniport und den Klassentreibern gerufen, nie aus einem
     *  Benutzerprozess.  Ein ehrliches EFAULT statt eines Kopiervorgangs
     *  aus einer Adresse, die niemand geprueft hat.  */

    return EFAULT;
}

int
copyout (const void *kern, void *user, size_t n)
{
    return EFAULT;
}

int
uiomove (void *buf, int n, struct uio *uio)
{
    return EFAULT;
}
