/*
 *  nt/ntnewbus.c -- der Geraetebaum.
 *
 *  Das ist der einzige groessere Nachbau der Unterlage.  Er ist noetig,
 *  weil der USB-Stapel den Baum wirklich benutzt: jeder Hub haengt seine
 *  Anschluesse als Kinder ein, jedes Kind traegt sein softc, und beim
 *  Abziehen eines Steckers wird ein Teilbaum abgeraeumt.
 *
 *  Was nicht nachgebaut wird, ist kobj.  Die Treiber rufen keine
 *  DEVICE_*- oder BUS_*-Makros; sie fuellen nur ihre Methodentabelle.
 *  Ein Nachschlagen nach Namen reicht also, und das spart die gesamte
 *  Maschinerie mit Klassen, Basisklassen und Sprungtabellen.
 *
 *  Kein NT-DEVICE_OBJECT je USB-Geraet.  NT 4.0 hat keinen USB-Bus, an
 *  dem eines haengen koennte -- nur der Wurzelknoten haelt eines.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <dev/pci/pcivar.h>
#include <dev/usb/usb_if.h>


/* ====================================================================
 *  Die Anmeldeliste
 *
 *  Der Binder legt NTUDRV$a, $m und $z alphabetisch hintereinander.
 *  Zwischen den beiden Marken liegen die Eintraege -- in unbestimmter
 *  Reihenfolge, was hier nichts macht, weil ueber die Gueteziffer des
 *  probe entschieden wird und nicht ueber die Reihenfolge.
 * ==================================================================== */

#pragma section("NTUDRV$a", read)
#pragma section("NTUDRV$z", read)

__declspec(allocate("NTUDRV$a")) __attribute__((used))
static const ntu_drvreg * const ntu_drv_start = 0;

__declspec(allocate("NTUDRV$z")) __attribute__((used))
static const ntu_drvreg * const ntu_drv_end = 0;


/* ====================================================================
 *  Methodensuche
 * ==================================================================== */

static void *
ntu_method (driver_t *d, const char *name)
{
    device_method_t *m;

    if (d == 0 || d->methods == 0) {
        return 0;
    }

    for (m = d->methods; m->name != 0; m += 1) {
        if (strcmp(m->name, name) == 0) {
            return m->func;
        }
    }

    return 0;
}

typedef int (*ntu_devfn)(device_t);

int
ntu_usb_take_controller (device_t dev)
{
    ntu_devfn f;

    if (dev == 0) {
        return 0;
    }

    f = (ntu_devfn)ntu_method(dev->Driver, "usb_take_controller");

    /*  Kein Eintrag heisst: dieser Baustein gibt nichts zurueck.  Das ist
     *  kein Fehler -- OHCI hat die Methode, UHCI auch, xHCI nicht.  */

    return (f != 0) ? f(dev) : 0;
}


/* ====================================================================
 *  Knoten
 * ==================================================================== */

static ntu_mtx  ntu_tree_lock;
static ULONG    ntu_unit[64];        /* Zaehler je Treibername, grob */
static device_t ntu_root;

void
bus_topo_lock (void)
{
    ntu_mtx_lock(&ntu_tree_lock);
}

void
bus_topo_unlock (void)
{
    ntu_mtx_unlock(&ntu_tree_lock);
}

static ULONG
ntu_name_slot (const char *name)
{
    ULONG h = 0;

    while (*name != '\0') {
        h = h * 31 + (UCHAR)*name;
        name += 1;
    }

    return h % 64;
}

device_t
device_add_child (device_t parent, const char *name, int unit)
{
    device_t d;

    d = (device_t)ntu_malloc(sizeof(*d), M_ZERO);

    if (d == 0) {
        return 0;
    }

    InitializeListHead(&d->Children);
    d->Parent = parent;

    if (name != 0) {
        strlcpy(d->Name, name, sizeof(d->Name));
    }

    /*  DEVICE_UNIT_ANY heisst: such dir eine Nummer.  Die Nummer ist
     *  nur Beschriftung -- nichts im Stapel haengt an ihrem Wert.  */

    if (unit == DEVICE_UNIT_ANY) {
        ULONG s = (name != 0) ? ntu_name_slot(name) : 0;

        d->Unit = ntu_unit[s];
        ntu_unit[s] += 1;
    } else {
        d->Unit = (ULONG)unit;
    }

    if (parent != 0) {
        InsertTailList(&parent->Children, &d->Siblings);
    }

    return d;
}

int
device_delete_child (device_t parent, device_t child)
{
    LIST_ENTRY *e;

    if (child == 0) {
        return 0;
    }

    /*  Wie FreeBSD (subr_bus.c): ERST das Kind abhaengen, DANN die
     *  Kinder loeschen, die es uebrig gelassen hat.
     *
     *  Bis 2026.11.17.66 stand es andersherum ("erst die Kinder, damit
     *  der Teilbaum eines abgezogenen Hubs nicht verloren geht").  Dann
     *  waren die Knoten hinter einem Hub schon frei, wenn uhub_detach
     *  lief -- und das loescht sie ueber usb_free_device ->
     *  usb_detach_device -> device_delete_child(hub, iface->subdev)
     *  noch einmal: Detach auf freiem Speicher, RemoveEntryList auf
     *  freiem Speicher, doppelte Freigabe von softc und Knoten.  Trat
     *  auf beim Abziehen eines Hubs mit Geraeten dahinter und beim
     *  Abkonfigurieren des Wurzelhubs (Herunterfahren).  Die Schleife
     *  danach nimmt mit, was der Treiber nicht selbst geloescht hat --
     *  der Teilbaum geht also weiterhin nicht verloren.  */

    if (child->Attached != 0) {
        ntu_devfn f = (ntu_devfn)ntu_method(child->Driver, "device_detach");

        if (f != 0) {
            f(child);
        }

        child->Attached = 0;
    }

    while (!IsListEmpty(&child->Children)) {
        e = child->Children.Flink;
        device_delete_child(child, CONTAINING_RECORD(e, struct ntu_device,
                                                     Siblings));
    }

    if (parent != 0) {
        RemoveEntryList(&child->Siblings);
    }

    if (child->SoftcOwned != 0) {
        ntu_free(child->Softc);
    }

    ntu_free(child);
    return 0;
}

int
bus_detach_children (device_t dev)
{
    while (!IsListEmpty(&dev->Children)) {
        LIST_ENTRY *e = dev->Children.Flink;

        device_delete_child(dev, CONTAINING_RECORD(e, struct ntu_device,
                                                   Siblings));
    }

    return 0;
}

int
bus_generic_detach (device_t dev)
{
    return bus_detach_children(dev);
}

int ntu_dev_method_call (device_t dev, const char *name);

/*  Wie FreeBSD: DEVICE_SHUTDOWN an alle angehaengten Kinder.  Bis
 *  2026.11.17.66 ein leerer Stumpf -- usb_shutdown (usb_controller.c)
 *  wurde dadurch nie erreicht.  */
int
bus_generic_shutdown (device_t dev)
{
    LIST_ENTRY *e;

    for (e = dev->Children.Flink; e != &dev->Children; e = e->Flink) {
        device_t c = CONTAINING_RECORD(e, struct ntu_device, Siblings);

        if (c->Attached != 0) {
            (void)ntu_dev_method_call(c, "device_shutdown");
        }
    }
    return 0;
}

int
bus_generic_suspend (device_t dev)
{
    return 0;
}

int
bus_generic_resume (device_t dev)
{
    return 0;
}

int
bus_generic_get_device_path (device_t bus, device_t child,
                             const char *locator, struct sbuf *sb)
{
    /*  Geraetepfade sind sysfs-artige Beschriftung; nichts im Stapel
     *  haengt am Ergebnis.  Leerer Rumpf, ausdruecklich.  */
    return 0;
}


/* ====================================================================
 *  Anhaengen
 *
 *  FreeBSDs Gueteziffer: probe liefert 0 oder negativ bei Eignung,
 *  positiv bei Fehler.  Der groesste nicht positive Wert gewinnt --
 *  BUS_PROBE_SPECIFIC (0) schlaegt BUS_PROBE_DEFAULT (-20).  Das ist
 *  genau andersherum als die Erwartung und deshalb hier ausgeschrieben.
 * ==================================================================== */

/*  Zusatztreiber: eigene .sys (z. B. USBXBOX.SYS), die sich beim Laden
 *  ueber ntu_treiber_anmelden (nt/ntzusatz.c) eintragen.  Sie stehen
 *  nicht im NTUDRV-Abschnitt dieses Abbilds, werden beim probe aber genau
 *  wie die eingebauten behandelt.  Eintraege werden nur angehaengt, nie
 *  entfernt: erst der Zeiger, dann die Anzahl -- ein gleichzeitiger Leser
 *  sieht entweder den alten oder den neuen Stand, nie einen halben.  */

#define NTU_ZUSATZ_MAX 8
static const ntu_drvreg *volatile ntu_zusatz[NTU_ZUSATZ_MAX];
static volatile LONG ntu_zusatz_n;

int
ntu_zusatz_hinzu (const ntu_drvreg *r)
{
    LONG i;

    for (i = 0; i < ntu_zusatz_n; i += 1) {
        if (ntu_zusatz[i] == r) {
            return 0;                  /* schon angemeldet */
        }
    }
    if (ntu_zusatz_n >= NTU_ZUSATZ_MAX) {
        return ENOMEM;
    }
    ntu_zusatz[ntu_zusatz_n] = r;
    InterlockedIncrement((PLONG)&ntu_zusatz_n);
    return 0;
}

/*  Ein Kandidat: passt der Bus, passt ein verlangter Name, und meldet
 *  sein probe Eignung, wird er mit dem bisher Besten verglichen.  */

static void
ntu_kandidat (device_t dev, const ntu_drvreg *reg, const char *busname,
              driver_t **best, int *bestv)
{
    ntu_devfn probe;
    int       r;

    if (reg == 0) {
        return;                        /* Ausrichtungsluecke des Binders */
    }

    if (strcmp(reg->parent, busname) != 0) {
        return;
    }

    /*  Hat das Kind schon einen Namen verlangt, muss er passen.
     *  device_add_child(self, "usbus", ...) meint genau usbus.  */

    if (dev->Name[0] != '\0' &&
        strcmp(reg->driver->name, dev->Name) != 0) {
        return;
    }

    probe = (ntu_devfn)ntu_method(reg->driver, "device_probe");

    if (probe == 0) {
        return;
    }

    dev->Driver = reg->driver;

    if (ntu_attach_softc(dev) != 0) {
        dev->Driver = 0;
        return;
    }

    r = probe(dev);

    if (r <= 0 && (*best == 0 || r > *bestv)) {
        *best  = reg->driver;
        *bestv = r;
    }

    ntu_detach_softc(dev);
    dev->Driver = 0;
}

int
device_probe_and_attach (device_t dev)
{
    const ntu_drvreg * const *p;
    driver_t   *best  = 0;
    int         bestv = 1;
    ntu_devfn   attach;
    const char *busname;
    LONG        i, n;
    int         r;

    if (dev == 0 || dev->Attached != 0) {
        return 0;
    }

    busname = (dev->Parent != 0 && dev->Parent->Driver != 0)
                ? dev->Parent->Driver->name
                : "pci";

    for (p = &ntu_drv_start + 1; p < &ntu_drv_end; p += 1) {
        ntu_kandidat(dev, *p, busname, &best, &bestv);
    }

    n = ntu_zusatz_n;
    for (i = 0; i < n; i += 1) {
        ntu_kandidat(dev, ntu_zusatz[i], busname, &best, &bestv);
    }

    if (best == 0) {
        return ENXIO;
    }

    dev->Driver = best;

    if (ntu_attach_softc(dev) != 0) {
        return ENOMEM;
    }

    attach = (ntu_devfn)ntu_method(best, "device_attach");

    if (attach == 0) {
        return ENXIO;
    }

    r = attach(dev);

    if (r != 0) {
        ntu_detach_softc(dev);
        dev->Driver = 0;
        return r;
    }

    dev->Attached = 1;

    if (dev->Quiet == 0) {
        ntu_printf("usb: %s%u angehaengt%s%s\n",
                   best->name, dev->Unit,
                   (dev->Desc[0] != '\0') ? ": " : "",
                   (dev->Desc[0] != '\0') ? dev->Desc : "");
    }

    return 0;
}


/* ====================================================================
 *  softc
 *
 *  FreeBSD legt es beim Anhaengen an und gibt es beim Abhaengen frei.
 *  Wichtig ist nur eines: es muss beim probe schon da sein, weil
 *  ohci_pci_probe, ehci_pci_probe und uhub_probe hineinschreiben.
 * ==================================================================== */

int
ntu_attach_softc (device_t d)
{
    if (d->Driver == 0 || d->Driver->size == 0) {
        return 0;
    }

    if (d->Softc != 0) {
        return 0;                  /* vom Treiber selbst gesetzt */
    }

    d->Softc = ntu_malloc(d->Driver->size, M_ZERO);

    if (d->Softc == 0) {
        return ENOMEM;
    }

    d->SoftcSize  = (ULONG)d->Driver->size;
    d->SoftcOwned = TRUE;
    return 0;
}

void
ntu_detach_softc (device_t d)
{
    if (d->SoftcOwned != 0) {
        ntu_free(d->Softc);
        d->Softc      = 0;
        d->SoftcOwned = FALSE;
    }
}


/* ====================================================================
 *  Zugriffe
 * ==================================================================== */

void       *device_get_softc   (device_t d) { return d->Softc; }
void        device_set_softc   (device_t d, void *s)
{
    if (d->SoftcOwned != 0) {
        ntu_free(d->Softc);
        d->SoftcOwned = FALSE;
    }

    d->Softc = s;
}
void       *device_get_ivars   (device_t d) { return d->Ivars; }
void        device_set_ivars   (device_t d, void *v) { d->Ivars = v; }
device_t    device_get_parent  (device_t d) { return d->Parent; }
int         device_get_unit    (device_t d) { return (int)d->Unit; }
int         device_is_attached (device_t d) { return d->Attached; }
int         device_is_alive    (device_t d) { return (d != 0); }
void        device_quiet       (device_t d) { d->Quiet = TRUE; }
const char *device_get_desc    (device_t d) { return d->Desc; }

void
device_set_desc (device_t d, const char *s)
{
    strlcpy(d->Desc, s, sizeof(d->Desc));
}

void
device_set_desc_copy (device_t d, const char *s)
{
    strlcpy(d->Desc, s, sizeof(d->Desc));
}

const char *
device_get_nameunit (device_t d)
{
    static CHAR nu[40];

    /*  FreeBSD liefert hier einen Zeiger in den Knoten.  Ein statischer
     *  Puffer waere ein Fehler, sobald zwei Ausgaben verschachtelt sind
     *  -- deshalb wird er beim ersten Ruf im Knoten selbst gebaut.  */

    if (d->Nameunit[0] == '\0') {
        _snprintf(d->Nameunit, sizeof(d->Nameunit) - 1, "%s%u",
                  (d->Driver != 0) ? d->Driver->name
                                   : (d->Name[0] != '\0' ? d->Name : "dev"),
                  d->Unit);
        d->Nameunit[sizeof(d->Nameunit) - 1] = '\0';
    }

    (void)nu;
    return d->Nameunit;
}

void
device_printf (device_t d, const char *fmt, ...)
{
    va_list ap;
    CHAR    line[256];

    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';

    ntu_printf("%s: %s", device_get_nameunit(d), line);
}


/* ====================================================================
 *  Wurzel
 *
 *  Der Wurzelknoten heisst "pci" und ist das, woran die vier
 *  Hostbaustein-Treiber sich angemeldet haben.  Fuer jeden Baustein aus
 *  dem Suchlauf entsteht ein Kind mit den ivars aus IopScanPciBus --
 *  demselben Suchlauf, der den p08-Bericht fuellt.
 * ==================================================================== */

typedef struct ntu_pci_driver_stub { const char *name; } ntu_pci_stub;
static driver_t ntu_pci_root_driver = { "pci", 0, 0, 0, 0, 0 };

device_t
ntu_root_create (PDEVICE_OBJECT pdo)
{
    ntu_mtx_init(&ntu_tree_lock, "ntu tree", "newbus", 0);

    ntu_root = (device_t)ntu_malloc(sizeof(*ntu_root), M_ZERO);

    if (ntu_root == 0) {
        return 0;
    }

    InitializeListHead(&ntu_root->Children);
    ntu_root->Driver = &ntu_pci_root_driver;
    ntu_root->Pdo    = pdo;
    strlcpy(ntu_root->Name, "pci", sizeof(ntu_root->Name));

    return ntu_root;
}

device_t
ntu_root_add_pci (ntu_pci_ivars *iv)
{
    device_t d = device_add_child(ntu_root, 0, DEVICE_UNIT_ANY);

    if (d == 0) {
        return 0;
    }

    d->Ivars = iv;
    return d;
}


/* ====================================================================
 *  devclass
 *
 *  FreeBSD fuehrt je Treibername eine Liste.  Der USB-Stapel braucht
 *  davon nur "zaehle alle usbus auf" -- beim Herunterfahren und beim
 *  Suchen eines Busses nach Nummer.  Ein Gang durch die Kinder der
 *  Wurzel tut dasselbe und haelt keine zweite Liste, die auseinander
 *  laufen koennte.
 *
 *  Der devclass_t ist deshalb nur der Name.  Das ist kein Kunstgriff,
 *  sondern genau so viel Struktur, wie gebraucht wird.
 * ==================================================================== */

devclass_t
devclass_find (const char *name)
{
    return (devclass_t)(void *)name;
}

static device_t
ntu_nth_of_class (const char *name, int unit, int *count)
{
    LIST_ENTRY *e;
    int         n = 0;

    if (ntu_root == 0) {
        return 0;
    }

    for (e = ntu_root->Children.Flink; e != &ntu_root->Children;
         e = e->Flink) {

        device_t    hc = CONTAINING_RECORD(e, struct ntu_device, Siblings);
        LIST_ENTRY *g;

        /*  Zwei Ebenen: der Hostbaustein haengt an der Wurzel, der
         *  usbus an ihm.  */

        for (g = hc->Children.Flink; g != &hc->Children; g = g->Flink) {

            device_t d = CONTAINING_RECORD(g, struct ntu_device, Siblings);

            if (d->Driver != 0 && strcmp(d->Driver->name, name) == 0) {

                if (count == 0 && n == unit) {
                    return d;
                }

                n += 1;
            }
        }

        if (hc->Driver != 0 && strcmp(hc->Driver->name, name) == 0) {

            if (count == 0 && n == unit) {
                return hc;
            }

            n += 1;
        }
    }

    if (count != 0) {
        *count = n;
    }

    return 0;
}

int
devclass_get_maxunit (devclass_t dc)
{
    int n = 0;

    ntu_nth_of_class((const char *)dc, 0, &n);
    return n;
}

device_t
devclass_get_device (devclass_t dc, int unit)
{
    return ntu_nth_of_class((const char *)dc, unit, 0);
}

void *
devclass_get_softc (devclass_t dc, int unit)
{
    device_t d = devclass_get_device(dc, unit);

    return (d != 0) ? d->Softc : 0;
}


/* ====================================================================
 *  bus_get_dma_tag
 *
 *  FreeBSD reicht den DMA-Rahmen des Elternbusses herunter; unter NT ist
 *  das der Adapter aus HalGetAdapter.  Jeder Hostbaustein bekommt genau
 *  einen, beim ersten Zugriff geholt und im PCI-Knoten abgelegt.
 *
 *  Das hier war die Luecke, die den ersten Absturz gebaut hat.  Die
 *  Routine gab NULL zurueck, weil DmaTag nie gesetzt wurde; damit ging
 *  NULL als Adapter in HalAllocateCommonBuffer, und die HAL greift
 *  ihren ersten Parameter ohne Pruefung an:
 *
 *      800650f3   mov 0x8(%ebp),%edi     ; AdapterObject
 *      800650f6   cmp %al,0x56(%edi)     ; hier knallt es
 *
 *  Genau die Stelle stand im Bluescreen.  Die HAL prueft nicht, und das
 *  ist richtig so -- ein Treiber, der ihr NULL reicht, hat einen Fehler,
 *  und den soll sie nicht verstecken.
 * ==================================================================== */

bus_dma_tag_t
bus_get_dma_tag (device_t dev)
{
    device_t        pci = dev;
    ntu_pci_ivars  *iv;
    ntu_dma_tag_t   t;
    DEVICE_DESCRIPTION dd;
    ULONG           karten = 0;

    /*  Hinauf bis zum PCI-Kind -- das ist der Knoten, dessen Eltern die
     *  Wurzel ist und dessen ivars die Busnummer tragen.  */

    while (pci != 0 && !(pci->Parent != 0 && pci->Parent->Parent == 0)) {
        pci = pci->Parent;
    }

    if (pci == 0 || pci->Ivars == 0) {
        ntu_printf("usb: bus_get_dma_tag ohne PCI-Knoten\n");
        return 0;
    }

    if (pci->DmaTag != 0) {
        return pci->DmaTag;
    }

    iv = (ntu_pci_ivars *)pci->Ivars;

    memset(&dd, 0, sizeof(dd));
    dd.Version           = DEVICE_DESCRIPTION_VERSION;
    dd.Master            = TRUE;        /* der Baustein holt selbst      */
    dd.ScatterGather     = TRUE;        /* kein einzelner Kanal          */
    dd.Dma32BitAddresses = TRUE;        /* 32 Bit, siehe sys/types.h     */
    dd.DemandMode        = FALSE;
    dd.AutoInitialize    = FALSE;
    dd.IgnoreCount       = FALSE;
    dd.InterfaceType     = PCIBus;
    dd.BusNumber         = iv->Bus;
    dd.DmaChannel        = 0;
    dd.MaximumLength     = 0x20000;     /* 128 kB am Stueck reicht dem
                                           groessten Ring                */

    t = (ntu_dma_tag_t)ntu_malloc(sizeof(*t), M_ZERO);

    if (t == 0) {
        return 0;
    }

    t->Adapter = HalGetAdapter(&dd, &karten);

    if (t->Adapter == 0) {

        /*  Kommt vor, wenn die HAL den Bus nicht kennt -- etwa weil die
         *  Busnummer falsch ist.  Ein ehrliches NULL nach oben ist
         *  besser als ein Adapter, der spaeter im Nichts zeigt.  */

        ntu_printf("usb: HalGetAdapter fuer Bus %u lieferte nichts\n",
                   iv->Bus);
        ntu_free(t);
        return 0;
    }

    t->Alignment = 1;
    t->MaxSize   = dd.MaximumLength;
    t->NSegments = 1;

    ntu_printf("usb: Adapter fuer Bus %u, %u Kartenregister\n",
               iv->Bus, karten);

    pci->DmaTag = t;
    return t;
}


/* ====================================================================
 *  DEVICE_SUSPEND und DEVICE_RESUME
 *
 *  Die einzigen beiden kobj-Aufrufe ausserhalb von usb_if.h.  Fehlt die
 *  Methode, ist das kein Fehler -- FreeBSD faellt dort auf
 *  bus_generic_suspend zurueck, und das tut nichts.
 * ==================================================================== */

int
ntu_dev_method_call (device_t dev, const char *name)
{
    ntu_devfn f;

    if (dev == 0) {
        return 0;
    }

    f = (ntu_devfn)ntu_method(dev->Driver, name);
    return (f != 0) ? f(dev) : 0;
}

int
ntu_usb_handle_request (device_t dev, const void *req, void **pptr,
                        unsigned short *plen, unsigned short offset,
                        unsigned char *pstate)
{
    /*  Geraetebetrieb -- der Rechner als USB-Ziel.  Unter NT 4.0 gibt es
     *  dafuer keinen Anwendungsfall, und ein halber Pfad waere
     *  schlimmer als keiner.  ENXIO ist die Antwort, die der Stapel
     *  auch von einem Baustein ohne Geraetebetrieb bekommt.  */

    return ENXIO;
}
