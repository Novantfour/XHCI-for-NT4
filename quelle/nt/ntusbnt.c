/*
 *  nt/ntusbnt.c -- der Einstieg.
 *
 *  Was NT vom Treiber verlangt, und was der FreeBSD-Stapel vom Kern
 *  verlangt, treffen sich hier und sonst nirgends.
 *
 *  Ablauf:
 *
 *      DriverEntry
 *        Uhr und Arbeitsfaden starten
 *        PCI durchsuchen: Klasse 0x0C, Unterklasse 0x03
 *        Wurzel anlegen, je Baustein ein Kind mit ivars
 *        device_probe_and_attach je Kind
 *
 *  Die Suche ist dieselbe wie in IopScanPciBus im Kern -- nur dass sie
 *  hier nach Klasse geht statt nach Hersteller.  Wer den Kernpatch
 *  eingespielt hat, sieht dieselben Bausteine im p08-Bericht und kann
 *  beide Listen vergleichen.  Stimmen sie nicht ueberein, liegt es an
 *  der Slotrechnung, und die ist unten ausgeschrieben.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <ober/ntkbdmou.h>
#include <nt/ntpaged.h>
#include <ober/ntusbioctl.h>

VOID ntu_port_init(PDRIVER_OBJECT, PUNICODE_STRING);


#define NTU_MAX_HC  16

/*  Nicht static: das Steuergeraet in ober/ntsteuer.c liest dieselbe
 *  Tabelle, damit das Werkzeug genau das sieht, was der Treiber sieht,
 *  und nicht eine zweite Meinung.  */

ntu_pci_ivars ntu_hc[NTU_MAX_HC];
ULONG         ntu_hc_count;
device_t      ntu_hc_dev[NTU_MAX_HC];
ULONG         ntu_hc_status[NTU_MAX_HC];   /* 0 nein, 1 ja, 2 uebergangen */

static device_t ntu_bus_root;


/* ====================================================================
 *  PCI durchsuchen
 *
 *  Die Slotnummer ist nicht die Geraetenummer.  NT packt Geraet und
 *  Funktion in ein Wort:
 *
 *      slot = (dev & 0x1F) | ((fn & 0x07) << 5)
 *
 *  Wer das uebersieht, findet auf SB700 nur den OHCI auf Funktion 0 und
 *  nie den EHCI auf Funktion 2 -- also USB 1.1 statt USB 2.0, ohne
 *  einen Fehler zu sehen.
 *
 *  Mehrfunktionsbausteine erkennt man an Bit 0x80 im HeaderType, und
 *  zwar nur an Funktion 0.  Ohne das Bit bricht die Schleife nach
 *  Funktion 0 ab; mit ihm laeuft sie bis 7.
 * ==================================================================== */

static ULONG
ntu_cfg_read (ULONG bus, ULONG slot, ULONG off, ULONG len)
{
    ULONG v = 0;

    if (HalGetBusDataByOffset(PCIConfiguration, bus, slot, &v, off, len) < len) {
        return 0xFFFFFFFFu;
    }

    return v;
}

static ULONG ntu_letzter_bus;

static VOID
ntu_scan_pci (VOID)
{
    ULONG bus;

    for (bus = 0; bus < 256; bus += 1) {

        ULONG dev;
        ULONG onbus = 0;

        for (dev = 0; dev < 32; dev += 1) {

            ULONG fn;

            for (fn = 0; fn < 8; fn += 1) {

                ULONG slot = (dev & 0x1F) | ((fn & 0x07) << 5);
                ULONG id   = ntu_cfg_read(bus, slot, 0x00, 4);
                ULONG cls;
                ULONG hdr;

                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) {
                    if (fn == 0) {
                        break;
                    }
                    continue;
                }

                onbus += 1;
                hdr = (ntu_cfg_read(bus, slot, 0x0C, 4) >> 16) & 0xFF;
                cls = ntu_cfg_read(bus, slot, 0x08, 4);      /* rev|if|sub|base */

                if (((cls >> 24) & 0xFF) == PCIC_SERIALBUS &&
                    ((cls >> 16) & 0xFF) == PCIS_SERIALBUS_USB &&
                    ntu_hc_count >= NTU_MAX_HC) {

                    /*  Mehr als NTU_MAX_HC: bisher still uebergangen.  Die
                     *  Grenze haengt an der IOCTL-Struktur fuer USBTEST und
                     *  den Geraetemanager (NTU_MAX_HC_IOCTL); hier wenigstens
                     *  sagen, welcher fehlt.  */
                    ntu_printf("usb: %02x:%02x.%x  %04x:%04x  uebergangen -- "
                               "mehr als %u Controller\n", bus, dev, fn,
                               id & 0xFFFF, id >> 16, NTU_MAX_HC);

                } else if (((cls >> 24) & 0xFF) == PCIC_SERIALBUS &&
                    ((cls >> 16) & 0xFF) == PCIS_SERIALBUS_USB) {

                    ntu_pci_ivars *iv = &ntu_hc[ntu_hc_count];
                    ULONG          sub;

                    ntu_hc_count += 1;

                    iv->Bus      = (uint8_t)bus;
                    iv->Slot     = (uint8_t)dev;
                    iv->Func     = (uint8_t)fn;
                    iv->Vendor   = (uint16_t)(id & 0xFFFF);
                    iv->Device   = (uint16_t)(id >> 16);
                    iv->Revision = (uint8_t)(cls & 0xFF);
                    iv->ProgIf   = (uint8_t)((cls >> 8) & 0xFF);
                    iv->SubClass = (uint8_t)((cls >> 16) & 0xFF);
                    iv->Class    = (uint8_t)((cls >> 24) & 0xFF);
                    iv->Irq      = (uint8_t)(ntu_cfg_read(bus, slot,
                                                          0x3C, 4) & 0xFF);

                    sub = ntu_cfg_read(bus, slot, 0x2C, 4);
                    iv->SubVendor = (uint16_t)(sub & 0xFFFF);
                    iv->SubDevice = (uint16_t)(sub >> 16);

                    ntu_printf("usb: %02x:%02x.%x  %04x:%04x  %s  irq %u\n",
                               bus, dev, fn, iv->Vendor, iv->Device,
                               (iv->ProgIf == 0x00) ? "uhci" :
                               (iv->ProgIf == 0x10) ? "ohci" :
                               (iv->ProgIf == 0x20) ? "ehci" :
                               (iv->ProgIf == 0x30) ? "xhci" : "eigen",
                               iv->Irq);
                }

                if (fn == 0 && (hdr & 0x80) == 0) {
                    break;
                }
            }
        }

        if (onbus != 0) {
            ntu_letzter_bus = bus;
        }

        /*  Frueher: hinter Bus 8 ohne Fund Schluss ("ein Brett mit
         *  Bussen jenseits 8 und einer Luecke davor gibt es nicht").
         *  Gibt es doch: Ryzen-APUs haengen ihre eigenen xHCI hinter
         *  00:08.1 an einen hohen Bus (oft 0x0a), mit leeren Bussen
         *  davor.  HP EliteDesk 705 G5: nur Chipsatz-xHCI und Realtek-
         *  EHCI gefunden, die Front-Buchsen fehlten.  Alle 256 Busse
         *  kosten 8192 Konfigurationszugriffe -- vernachlaessigbar.  */
    }

    ntu_printf("usb: PCI-Suche: %u Controller, hoechster belegter Bus %02x\n",
               ntu_hc_count, ntu_letzter_bus);
}


/* ====================================================================
 *  Anhaengen
 *
 *  Die Reihenfolge ist nicht beliebig.  Auf jedem Brett mit EHCI teilen
 *  sich EHCI und die begleitenden UHCI/OHCI dieselben Buchsen: der EHCI
 *  besitzt sie, und was er nicht bedienen kann -- alles unter 480 MBit
 *  -- gibt er an den Begleiter ab.
 *
 *  Haengt der Begleiter zuerst, nimmt er Buchsen an sich, die der EHCI
 *  gleich darauf beansprucht.  Ergebnis: schnelle Geraete laufen langsam
 *  oder gar nicht, und es sieht nach einem Kabelproblem aus.
 *
 *  Deshalb erst alle EHCI, dann der Rest.
 * ==================================================================== */

static VOID
ntu_attach_all (VOID)
{
    ULONG runde;
    ULONG i;

    for (runde = 0; runde < 2; runde += 1) {

        for (i = 0; i < ntu_hc_count; i += 1) {

            ntu_pci_ivars *iv = &ntu_hc[i];
            device_t       d;
            int            r;

            if (runde == 0 && iv->ProgIf != PCIP_SERIALBUS_USB_EHCI) {
                continue;
            }

            if (runde == 1 && iv->ProgIf == PCIP_SERIALBUS_USB_EHCI) {
                continue;
            }

            /*  Alle vier Bauarten werden angehaengt.  xHCI hat dafuer
             *  die genaue Maskierung ueber IMAN.IE bekommen (ntintr.c)
             *  statt des groben USBCMD.INTE -- letzteres haette bei
             *  einem Baustein mit mehreren Unterbrechern zuviel
             *  abgeschaltet.
             *
             *  Bausteine mit herstellereigener Schnittstelle (0x80)
             *  bleiben draussen: fuer sie gibt es keinen Treiber, und
             *  ein probe waere ein Ratespiel am fremden Registerblock.  */

#ifdef NTU_EINE_BAUART

            /*  Getrennter Bau fuer genau eine Bauart.  Damit laesst sie
             *  sich einzeln laden und entladen, ohne dass die anderen
             *  Bausteine derselben Suedbruecke mitgehen -- und ohne dass
             *  ein Fehler dort wie ein Fehler hier aussieht.  */

            if (iv->ProgIf != NTU_EINE_BAUART) {
                ntu_hc_status[i] = 2;
                continue;
            }
#endif

            if (iv->ProgIf != PCIP_SERIALBUS_USB_UHCI &&
                iv->ProgIf != PCIP_SERIALBUS_USB_OHCI &&
                iv->ProgIf != PCIP_SERIALBUS_USB_EHCI &&
                iv->ProgIf != PCIP_SERIALBUS_USB_XHCI) {

                ntu_printf("usb: %02x:%02x.%x hat Schnittstelle %02x -- "
                           "uebergangen\n",
                           iv->Bus, iv->Slot, iv->Func, iv->ProgIf);
                ntu_hc_status[i] = 2;
                continue;
            }

            d = ntu_root_add_pci(iv);

            if (d == 0) {
                continue;
            }

            ntu_hc_dev[i] = d;
            r = device_probe_and_attach(d);

            ntu_hc_status[i] = (r == 0) ? 1 : 0;

            /*  xHCI: Intel-Sonderbehandlung zeigen, ggf. umschalten --
             *  siehe ober/ntxhcisonder.c.  */

            if (r == 0 && iv->ProgIf == 0x30) {
                ntu_xhci_sonder(d, iv->Vendor, iv->Device);
            }

            if (r != 0) {
                ntu_printf("usb: %02x:%02x.%x haengt nicht an: %d\n",
                           iv->Bus, iv->Slot, iv->Func, r);
            }
        }
    }
}


/* ====================================================================
 *  DriverEntry
 * ==================================================================== */

/* ====================================================================
 *  Herzschlag
 *
 *  Nur mit -DNTU_HERZSCHLAG.  Alle fuenf Sekunden eine Zeile auf COM1:
 *  Unterbrechungen, Fadenlaeufe, Pool, und bei xHCI jeder PORTSC.
 *
 *  Gebaut fuer den Pruefstand unter QEMU, wo kein Werkzeug laeuft,
 *  solange niemand angemeldet ist -- und wo genau die Frage offen ist,
 *  ob der Baustein etwas meldet und ob der Treiber es abholt.  Im
 *  normalen Bau aus, weil er den Bericht zuschuettet.
 * ==================================================================== */

#ifdef NTU_HERZSCHLAG

#include <ober/ntusbioctl.h>
extern NTU_ZAEHLER ntu_zaehler;

static ntu_thread ntu_herz;

static VOID
ntu_herz_body (PVOID ctx)
{
    ULONG runde = 0;

    while (!ntu_herz.Stop) {

        LARGE_INTEGER t;
        ULONG         i;

        t.QuadPart = -5LL * 10000000LL;
        KeWaitForSingleObject(&ntu_herz.Wake, Executive, KernelMode,
                              FALSE, &t);

        if (ntu_herz.Stop) {
            break;
        }

        runde += 1;
        ntu_printf("herz %u: pcd-nachgeholt %u amdpll-aus %u  ", runde,
                   ntu_pcd_nachgeholt, ntu_amdpll_aus);
        ntu_printf("maus %ld/%lu/%ld  ", (long)ntu_maus_berichte,
                   (unsigned long)ntu_zaehler.MausMeldungen, (long)ntu_maus_leer);
        ntu_printf("unt %u fremd %u faden %u verl %u "
                   "pool %u/%u/%u\n",
                   ntu_zaehler.Unterbrechungen, ntu_zaehler.Fremdunterbr,
                   ntu_zaehler.Fadenlaeufe, ntu_zaehler.ArbeitVerloren,
                   ntu_pool_vergaben(), ntu_pool_offen(),
                   ntu_pool_gemeckert());

        {
            int k;

            for (k = 0; k < NTU_WART_MAX; k += 1) {

                ntu_wart *w = &ntu_wart_tab[k];
                int       her;

                if (w->Faden == 0 || w->Art == 0) {
                    continue;
                }

                /*  Der Leerlauf eines usb_process-Fadens ist cv_wait auf
                 *  up_cv, und die heisst in usb_process.c "-".  Das ist
                 *  kein Haengen, sondern Warten auf Arbeit.  */

                if (w->Worauf != 0 && w->Worauf[0] == '-' &&
                    w->Worauf[1] == '\0') {
                    continue;
                }

                her = ticks - w->Seit;

                if (her < 3 * hz) {
                    continue;
                }

                ntu_printf("  HAENGT: %s (%p) seit %u s auf %s \"%s\"",
                           w->FadenName ? w->FadenName : "?", w->Faden,
                           her / hz, w->Art, w->Worauf ? w->Worauf : "?");

                if (w->Halter != 0) {

                    const char *hn = "?";
                    int         j;

                    for (j = 0; j < NTU_WART_MAX; j += 1) {
                        if (ntu_wart_tab[j].Faden == w->Halter) {
                            hn = ntu_wart_tab[j].FadenName
                               ? ntu_wart_tab[j].FadenName : "?";
                        }
                    }

                    ntu_printf(", gehalten von %s (%p)", hn, w->Halter);
                }

                ntu_printf("\n");
            }
        }

        for (i = 0; i < ntu_hc_count; i += 1) {

            device_t  d = ntu_hc_dev[i];
            ntu_intr *q = 0;
            ULONG     sts = 0, ena = 0;
            UCHAR     pi  = ntu_hc[i].ProgIf;

            if (d == 0) {
                continue;
            }

            /*  Das Unterbrechungsobjekt haengt am PCI-Knoten; bei
             *  EHCI/OHCI hat FreeBSD es aber am selben Knoten
             *  eingerichtet, bei dem auch die Betriebsmittel liegen.  */

            q = (ntu_intr *)d->Intr;

            if (d->Res != 0) {
                switch (pi) {
                    case 0x00:
                        sts = ntu_r2(d->Res->r_tag, d->Res->r_handle, 0x02);
                        ena = ntu_r2(d->Res->r_tag, d->Res->r_handle, 0x04);
                        break;
                    case 0x10:
                        sts = ntu_r4(d->Res->r_tag, d->Res->r_handle, 0x0C);
                        ena = ntu_r4(d->Res->r_tag, d->Res->r_handle, 0x10);
                        break;
                    case 0x20: {
                        ULONG op = ntu_r1(d->Res->r_tag, d->Res->r_handle, 0);
                        sts = ntu_r4(d->Res->r_tag, d->Res->r_handle, op + 4);
                        ena = ntu_r4(d->Res->r_tag, d->Res->r_handle, op + 8);
                        break;
                    }
                    case 0x30: {
                        ULONG op = ntu_r1(d->Res->r_tag, d->Res->r_handle, 0);
                        sts = ntu_r4(d->Res->r_tag, d->Res->r_handle, op + 4);
                        ena = ntu_r4(d->Res->r_tag, d->Res->r_handle, op + 0);
                        break;
                    }
                }
            }

            ntu_printf("  hc%u %s irq %u  unt %u fremd %u (gesperrt %u)  "
                       "sts %08x ena %08x%s\n", i,
                       (pi == 0x00) ? "uhci" : (pi == 0x10) ? "ohci" :
                       (pi == 0x20) ? "ehci" : "xhci",
                       ntu_hc[i].Irq,
                       q ? q->Unt : 0, q ? q->Fremd : 0,
                       q ? q->FremdGesp : 0, sts, ena,
                       q ? "" : "  (kein ISR)");
        }
    }
}

#endif


/* ====================================================================
 *  Entladen
 *
 *  Die Reihenfolge ist der ganze Inhalt dieser Routine.  Ein Treiber,
 *  der sich einmal laden laesst, laesst sich fast immer laden; einer,
 *  der sich sauber entladen laesst, ist selten.  Was hier falsch steht,
 *  faellt beim ersten Mal nicht auf und beim zehnten.
 *
 *      1  Steuergeraet weg          das Werkzeug soll nicht mehr
 *                                   hineinrufen, waehrend abgebaut wird
 *      2  Geraete abhaengen         xhci_pci_detach klemmt die
 *                                   Unterbrechung ab, haelt den
 *                                   Baustein an und gibt die
 *                                   Betriebsmittel zurueck
 *      3  Uhr anhalten              danach kein callout mehr
 *      4  Arbeitsfaden anhalten     danach laeuft kein Rueckruf mehr
 *
 *  Zwischen 2 und 3 darf nichts vertauscht werden: solange ein Geraet
 *  haengt, stellt es Zeitgeber ein.  Und 4 muss nach 3 kommen, weil der
 *  Faden genau die Zeitgeber abarbeitet.
 * ==================================================================== */

/*  Unbenanntes Geraet nur fuer IoRegisterShutdownNotification.  */
static PDEVICE_OBJECT ntu_abschalt_dev;

static VOID NtuUnload (PDRIVER_OBJECT drv);
NTU_PAGED(NtuUnload)

static VOID
NtuUnload (PDRIVER_OBJECT drv)
{
    extern VOID ntu_ressourcen_abmelden(VOID);
    ULONG i;

    NTU_PAGED_CODE();           /* Entladen laeuft immer passiv */
    ntu_printf("usb: entladen\n");

    ntu_steuer_delete();

    if (ntu_abschalt_dev != 0) {
        IoUnregisterShutdownNotification(ntu_abschalt_dev);
        IoDeleteDevice(ntu_abschalt_dev);
        ntu_abschalt_dev = 0;
    }

#ifdef NTU_HERZSCHLAG
    ntu_thread_stop(&ntu_herz);
#endif

    for (i = 0; i < ntu_hc_count; i += 1) {

        if (ntu_hc_dev[i] != 0) {
            device_delete_child(ntu_bus_root, ntu_hc_dev[i]);
            ntu_hc_dev[i] = 0;
            ntu_hc_status[i] = 0;
        }
    }

    ntu_ressourcen_abmelden();      /* HKLM\HARDWARE\RESOURCEMAP\USB */

    ntu_clock_stop();
    ntu_work_stop();

    /*  Die festen Tastatur- und Mausanschluesse zuletzt: bis hierher
     *  koennte ein Geraet noch hineinmelden.  */

    ntu_port_ende();

    ntu_printf("usb: entladen fertig -- %u Vergaben, %u Bloecke stehen "
               "noch, %u Freigaben abgewiesen\n",
               ntu_pool_vergaben(), ntu_pool_offen(), ntu_pool_gemeckert());

#if !defined(NTU_KISERIAL) && !defined(NTU_OHNE_SERIELL)
    ntu_ring_anhalten();            /* erst alles hinaus, dann Faden beenden */
#endif
}

/*  DriverEntry in den Abschnitt INIT: NT gibt ihn nach dem Start wieder
 *  frei.  So macht es auch SIVNT4.SYS (Ray Hinchliffe), dessen Hinweis
 *  auf die Abschnittsattribute diese Aenderung ausgeloest hat.  Alles
 *  andere bleibt groesstenteils resident, weil der USB-Treiber im ISR-
 *  und DPC-Pfad arbeitet.  Was nachweislich NUR passiv laeuft (Entladen,
 *  Einrichten, Registry), liegt im auslagerbaren Abschnitt PAGE
 *  (nt/ntpaged.h, NTU_PAGED).  */
/*  Laeuft das grafische Setup?  HKLM\SYSTEM\Setup, SystemSetupInProgress.  */
static BOOLEAN ntu_setup_laeuft (VOID);
#pragma alloc_text(INIT, ntu_setup_laeuft)

static BOOLEAN
ntu_setup_laeuft (VOID)
{
    RTL_QUERY_REGISTRY_TABLE t[2];
    ULONG wert = 0, vorgabe = 0;

    memset(t, 0, sizeof(t));
    t[0].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    t[0].Name          = L"SystemSetupInProgress";
    t[0].EntryContext  = &wert;
    t[0].DefaultType   = REG_DWORD;
    t[0].DefaultData   = &vorgabe;
    t[0].DefaultLength = sizeof(ULONG);

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                        L"\\Registry\\Machine\\System\\Setup", t, 0, 0))) {
        return FALSE;
    }
    return wert != 0;
}

#ifndef NTU_SETUP_WARTEN
#define NTU_SETUP_WARTEN  30        /* Sekunden, nur im Textmodus-Setup */
#endif
NTSTATUS DriverEntry (PDRIVER_OBJECT drv, PUNICODE_STRING reg);
#pragma alloc_text(INIT, DriverEntry)

/* ====================================================================
 *  Herunterfahren
 *
 *  Beim Neustart laufen die Bausteine sonst weiter: sie lesen ihre
 *  Listen per DMA aus dem Speicher, den BIOS und naechstes System schon
 *  neu belegen, und manche BIOSse haengen beim Warmstart an einem
 *  laufenden EHCI/xHCI (Linux haelt sie deshalb an, ehci_shutdown /
 *  xhci_shutdown).
 *
 *  Aber: NT 4.0 schickt IRP_MJ_SHUTDOWN an die Angemeldeten in Phase 0,
 *  die Dateisysteme werden erst in Phase 1 geleert.  Ein USB-Stick mit
 *  FAT verloere dann seine letzten Daten.  Bausteine, an denen (auch
 *  hinter Hubs) ein Massenspeicher haengt, bleiben deshalb laufen; das
 *  ist der bisherige Zustand und schadet nicht mehr als bisher.
 * ==================================================================== */

static BOOLEAN
ntu_hat_massenspeicher (device_t d)
{
    LIST_ENTRY *e;

    /*  Der Massenspeichertreiber heisst hier "ntumass" (ober/ntumass.c),
     *  nicht "umass".  */
    if (d->Driver != 0 && d->Driver->name != 0 &&
        (strcmp(d->Driver->name, "ntumass") == 0 ||
         strcmp(d->Driver->name, "umass") == 0)) {
        return TRUE;
    }
    for (e = d->Children.Flink; e != &d->Children; e = e->Flink) {
        if (ntu_hat_massenspeicher(CONTAINING_RECORD(e, struct ntu_device,
                                                     Siblings))) {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID
ntu_baustein_anhalten (device_t d, UCHAR pi)
{
    ntu_space_tag    t = d->Res->r_tag;
    ntu_space_handle h = d->Res->r_handle;
    ULONG            op, n;

    switch (pi) {

        case 0x00:                                  /* UHCI */
            ntu_w2(t, h, 0x04, 0);                  /* USBINTR */
            ntu_w2(t, h, 0x00, 0);                  /* USBCMD: RS aus */
            break;

        case 0x10:                                  /* OHCI */
            ntu_w4(t, h, 0x14, 0xFFFFFFFFul);       /* alle Quellen aus */
            ntu_w4(t, h, 0x04, ntu_r4(t, h, 0x04) & 0x200);  /* HCFS=Reset, RWC behalten */
            break;

        case 0x20:                                  /* EHCI */
            op = ntu_r1(t, h, 0);
            ntu_w4(t, h, op + 0x08, 0);             /* USBINTR */
            ntu_w4(t, h, op + 0x00, ntu_r4(t, h, op + 0x00) & ~0x31ul); /* RS, PSE, ASE */
            for (n = 0; n < 20 && (ntu_r4(t, h, op + 0x04) & 0x1000) == 0; n += 1) {
                KeStallExecutionProcessor(125);     /* HCHalted, max. 2,5 ms */
            }
            ntu_w4(t, h, op + 0x40, 0);             /* CONFIGFLAG: Anschluesse an die Begleiter */
            break;

        case 0x30:                                  /* xHCI */
            op = ntu_r1(t, h, 0);
            ntu_w4(t, h, op + 0x00, ntu_r4(t, h, op + 0x00) & ~0x0Dul); /* RS, INTE, HSEE */
            for (n = 0; n < 160 && (ntu_r4(t, h, op + 0x04) & 0x01) == 0; n += 1) {
                KeStallExecutionProcessor(100);     /* HCH, Spez.: max. 16 ms */
            }
            break;
    }
}

static NTSTATUS
ntu_herunterfahren (PDEVICE_OBJECT dev, PIRP irp)
{
    ULONG i;

    if (dev == ntu_abschalt_dev) {
        for (i = 0; i < ntu_hc_count; i += 1) {
            device_t d = ntu_hc_dev[i];

            if (d == 0 || d->Res == 0) {
                continue;
            }
            if (ntu_hat_massenspeicher(d)) {
                ntu_printf("usb: Herunterfahren: hc%u bleibt an (Massenspeicher)\n", i);
                continue;
            }
            /*  1. Der FreeBSD-Weg: DEVICE_SHUTDOWN -> usb_shutdown ->
             *     usb_bus_shutdown im Erkundungsfaden: Kinder abschalten,
             *     Wurzelhub abkonfigurieren, no_explore, dann
             *     set_hw_power_sleep(SHUTDOWN) -- der Baustein haelt an.
             *     Braucht den Unterbrechungsfaden noch.
             *  2. Dann Faden und Sicherungstakt beenden.  Ein nur von
             *     aussen angehaltener Baustein unter laufendem Stapel
             *     fuehrt sonst zu Zeitueberschreitungen, Neuaufzaehlung
             *     und "host controller halted" alle 50 ms (am Pruefstand
             *     so gesehen).
             *  3. Zuletzt die Register selbst, falls FreeBSD etwas
             *     laufen liess (EHCI: CONFIGFLAG).  */
            {
                extern int ntu_dev_method_call (device_t, const char *);
                ntu_intr *q;

                (void)ntu_dev_method_call(d, "device_shutdown");

                q = (ntu_intr *)d->Intr;
                if (q != 0) {
                    KeCancelTimer(&q->Takt);
                    q->Stop = 1;
                    KeSetEvent(&q->Wake, 0, FALSE);
                }
            }
            ntu_baustein_anhalten(d, ntu_hc[i].ProgIf);
            ntu_printf("usb: Herunterfahren: hc%u angehalten\n", i);
        }
    }

    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry (PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    extern PDRIVER_OBJECT ntu_treiberobjekt;       /* ResourceMap (ntgeraet.c) */

    NTU_PAGED_CODE();           /* DriverEntry laeuft passiv (Abschnitt INIT) */
#if !defined(NTU_KISERIAL) && !defined(NTU_OHNE_SERIELL)

    /*  Die Ausgabe einstellen, BEVOR das erste Zeichen geschrieben wird --
     *  sonst ist die serielle Maus schon verstellt.  */

    {
        extern ULONG ntu_uart_basis;
        RTL_QUERY_REGISTRY_TABLE t[2];
        ULONG wert = 1, vorgabe = 1;
        WCHAR pfad[260];
        ULONG n = reg->Length / sizeof(WCHAR);

        if (n < 240) {
            memcpy(pfad, reg->Buffer, n * sizeof(WCHAR));
            memcpy(pfad + n, L"\\Parameters", 12 * sizeof(WCHAR));
            pfad[n + 11] = 0;

            memset(t, 0, sizeof(t));
            t[0].Flags         = RTL_QUERY_REGISTRY_DIRECT;
            t[0].Name          = L"Ausgabe";
            t[0].EntryContext  = &wert;
            t[0].DefaultType   = REG_DWORD;
            t[0].DefaultData   = &vorgabe;
            t[0].DefaultLength = sizeof(ULONG);

            if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                                                   pfad, t, 0, 0))) {
                wert = 1;
            }
        }

#ifdef NTU_AUSGABE_IMMER
        wert = 1;                   /* Diagnosefassung: immer COM1 */
#endif
        ntu_uart_basis = (wert == 0) ? 0 :
                         (wert == 1) ? 0x3F8 :
                         (wert == 2) ? 0x2F8 : wert;
    }
#endif

#ifdef NTU_SETUP_BILDSCHIRM
    {
        extern ULONG ntu_bildschirm;
        if (NT_SUCCESS(RtlCheckRegistryKey(RTL_REGISTRY_SERVICES, L"setupdd"))) {
            ntu_bildschirm = 1;
        }
    }
#endif
    ntu_printf("\r\n%s: FreeBSD-USB auf NT 4.0, Fassung %s\n", NTU_BAUART, NTU_DATEIFASSUNG);
    ntu_treiberobjekt = drv;

#if !defined(NTU_KISERIAL) && !defined(NTU_OHNE_SERIELL)
    /*  Ab hier geht die Ausgabe ueber den Ringpuffer -- nichts im
     *  Treiber wartet mehr auf den seriellen Anschluss.  */
    ntu_ring_starten();
#endif

    /*  Belegung der Maustasten 4..8 (Parameters\Taste4..8).  */

    ntu_maustasten_lesen(reg);
    ntu_sondertasten_lesen(reg);
    ntup_parameter_lesen(reg);
    ntu_sysctl_lesen(reg);          /* FreeBSD-Schalter: Parameters\Sysctl */

    /*  Reihenfolge: Uhr und Arbeitsfaden zuerst.  Der erste attach ruft
     *  schon callout_reset, und ein callout ohne Arbeitsfaden landet im
     *  Ring und wird nie abgeholt -- ein Aufzaehlungsvorgang, der still
     *  stehenbleibt.  */

    /*  DriverUnload als allererstes.  Scheitert irgendetwas weiter
     *  unten und wir kehren mit einem Fehler zurueck, muss NT den
     *  Treiber trotzdem wieder loswerden koennen -- ein Kerntreiber
     *  ohne DriverUnload laesst sich nicht anhalten, und dann hilft
     *  nur noch ein Neustart.  */

    drv->DriverUnload = NtuUnload;

    /*  Das Treiberobjekt zuerst hinterlegen: der erste attach einer
     *  Tastatur legt ein Anschlussgeraet an, und IoCreateDevice braucht
     *  es.  */

    ntu_printf("usb: Anschluesse anlegen\n");
    ntu_port_init(drv, reg);

    ntu_clock_start();
    ntu_work_start();

    /*  Nach dem PCI-Suchlauf: ob die AMD-PLL-Abhilfe gebraucht wird.
     *  Sie greift erst, wenn ein isochroner Strom beginnt.  */

    ntu_printf("usb: Uhr/Arbeitsfaden laufen, AMD-PLL pruefen\n");
    ntu_amdpll_erkennen();

#if defined(NTU_USB_DEBUG) && NTU_USB_DEBUG > 0

    /*  Die Gespraechigkeit des FreeBSD-Stapels.  usb_debug ist eine
     *  gewoehnliche Variable in usb_debug.c; unter FreeBSD setzt man sie
     *  per sysctl, hier beim Bauen.  Stufe 1 bis 2 zeigt Aufzaehlung und
     *  Anforderungen, ab 5 wird es ein Strom.  */

    {
        extern int usb_debug;
        usb_debug = NTU_USB_DEBUG;
    }
#endif

    /*  Erst nachsehen, ob die andere Bauform schon liegt.
     *
     *  usbnt.sys und xhcint.sys sind derselbe Treiber, einmal mit allen
     *  vier Bauarten und einmal nur mit xHCI.  Sie haben verschiedene
     *  Dienst- und Geraetenamen, also laesst NT beide laden -- und dann
     *  durchsuchen beide den PCI-Bus, finden denselben Baustein, haengen
     *  beide an, rufen beide IoConnectInterrupt und machen beide die
     *  BIOS-Uebergabe.  Der zweite bekommt einen Baustein, den der erste
     *  gerade zurueckgesetzt hat.
     *
     *  Ein Griff auf das Geraet der Gegenseite genuegt zum Erkennen.
     *  Liegt sie, brechen wir ab -- mit einer Meldung, die sagt warum,
     *  statt eines Absturzes, der es nicht sagt.  */

    {
        static PCWSTR namen[] = {
            L"\\Device\\UsbNt",
            L"\\Device\\UhciNt",
            L"\\Device\\OhciNt",
            L"\\Device\\EhciNt",
            L"\\Device\\XhciNt"
        };

        ULONG k;

        for (k = 0; k < 5; k += 1) {

            UNICODE_STRING andere;
            PFILE_OBJECT   fo   = 0;
            PDEVICE_OBJECT dobj = 0;

            /*  Das eigene Geraet ueberspringen -- es gibt es zu diesem
             *  Zeitpunkt ohnehin noch nicht, aber die Absicht gehoert
             *  hingeschrieben.  */

            if (k == NTU_EIGENER_NAME) {
                continue;
            }

            RtlInitUnicodeString(&andere, namen[k]);

            if (NT_SUCCESS(IoGetDeviceObjectPointer(&andere, FILE_READ_DATA,
                                                    &fo, &dobj))) {
                ObDereferenceObject(fo);
                ntu_printf("usb: eine andere Bauform liegt bereits -- "
                           "es darf immer nur eine sein.\n"
                           "usb: erst den anderen Dienst anhalten.\n");
                return STATUS_DEVICE_ALREADY_ATTACHED;
            }
        }
    }

    ntu_bus_root = ntu_root_create(0);

    if (ntu_bus_root == 0) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ntu_printf("usb: PCI-Suche\n");
    ntu_scan_pci();

    if (ntu_hc_count == 0) {

        /*  Kein Fehler zurueckgeben: der Dienst soll trotzdem stehen,
         *  damit das Werkzeug das Steuergeraet oeffnen und die leere
         *  Tabelle sehen kann.  Ein Dienst, der beim Start abbricht,
         *  sagt dem Benutzer nur "Fehler 31" und sonst nichts.  */

        ntu_printf("usb: kein Hostbaustein gefunden\n");
    }

    ntu_printf("usb: %u Hostbausteine\n", ntu_hc_count);
    ntu_attach_all();

    /*  Textmodus-Setup vom USB-DVD-Laufwerk.
     *
     *  Gleich nach den Starttreibern legt NT die ARC-Namen an
     *  (IopCreateArcNames).  Fuer eine gestartete CD sucht es dabei unter
     *  den vorhandenen \Device\CdRomN das Laufwerk mit der Startscheibe;
     *  Setup oeffnet danach \ArcName\multi(0)disk(0)cdrom(N).  Unsere
     *  Aufzaehlung laeuft aber im Hintergrund -- kehren wir sofort zurueck,
     *  gibt es das Laufwerk noch nicht, und Setup endet mit Stop 0x7B
     *  (0xC0000034).  Also im Setup warten, bis ein USB-CD-Laufwerk
     *  angelegt ist, hoechstens NTU_SETUP_WARTEN Sekunden.
     *
     *  Setup erkennt man am Dienst setupdd: den gibt es nur in der
     *  Registry des Textmodus-Setups.
     *
     *  Ebenso im grafischen Setup (System\Setup\SystemSetupInProgress):
     *  setup.exe sucht gleich zu Beginn sein Quelllaufwerk.  Ist das
     *  USB-DVD-Laufwerk dann noch nicht angelegt (es laeuft nach dem
     *  Neustart erst an), endet Setup mit "Interne Setup-Datenstrukturen
     *  sind beschaedigt (Phase 0)" -- HP EliteDesk 705 G5, mal ja, mal nein.
     *  Im fertig installierten System wird nie gewartet.  */
    if (ntu_hc_count != 0 &&
        (NT_SUCCESS(RtlCheckRegistryKey(RTL_REGISTRY_SERVICES, L"setupdd")) ||
         ntu_setup_laeuft())) {
        extern volatile LONG ntum_cd_angelegt;
        NTHALAPI VOID NTAPI HalDisplayString (PUCHAR s);
        LARGE_INTEGER t;
        ULONG z, grenze;
        BOOLEAN grafisch;
        ntu_printf("usb: Setup -- warte auf USB-CD/DVD\n");
        t.QuadPart = -2500000;                      /* 250 ms */
        /*  Grafisches Setup: bis zu 3 Minuten, mit Anzeige auf dem
         *  Startbildschirm.  Ohne Quelllaufwerk bricht Setup ohnehin ab
         *  ("Phase 0"; in QEMU nachgestellt: Start ohne USB-DVD-Laufwerk),
         *  und auf dem HP 705 G5 reichen 30 s nicht.  */
        grafisch = !NT_SUCCESS(RtlCheckRegistryKey(RTL_REGISTRY_SERVICES, L"setupdd"));
        grenze   = (grafisch ? 180 : NTU_SETUP_WARTEN) * 4;
        if (grafisch) {
            HalDisplayString((PUCHAR)"usbnt: Setup -- warte auf das USB-CD/DVD-Laufwerk\n");
        }
        for (z = 0; z < grenze && ntum_cd_angelegt == 0; z += 1) {
            extern volatile LONG ntum_angehaengt;

            /*  Grafisches Setup ohne jeden USB-Massenspeicher: dann kommt
             *  die Quelle nicht von USB (IDE/SATA-CD, Festplatte nach
             *  winnt /b).  Ein USB-Laufwerk haengt sich in wenigen
             *  Sekunden als umass an; erst danach dauert das Anlaufen der
             *  Scheibe.  Nach 45 s ohne umass nicht drei Minuten weiter
             *  warten.  */
            if (grafisch && z >= 45 * 4 && ntum_angehaengt == 0) {
                HalDisplayString((PUCHAR)"usbnt: kein USB-Massenspeicher -- Quelle nicht von USB, weiter\n");
                break;
            }
            KeDelayExecutionThread(KernelMode, FALSE, &t);
            if (grafisch && z % 40 == 39) {
                HalDisplayString((PUCHAR)"usbnt: ... noch kein USB-CD/DVD (weitere 10 s)\n");
            }
            if (z % 20 == 19) {
                ntu_printf("usb: Setup: warte seit %u s, noch kein USB-CD/DVD\n",
                           (z + 1) / 4);
            }
        }
        if (ntum_cd_angelegt != 0) {
            /*  Etwas Nachlauf: ein zweites Laufwerk oder eine Tastatur am
             *  selben Hub haengt meist gleich danach an.  */
            t.QuadPart = -10000000;                 /* 1 s */
            KeDelayExecutionThread(KernelMode, FALSE, &t);
        }
        ntu_printf("usb: Setup: %d USB-CD/DVD nach %u ms\n",
                   (int)ntum_cd_angelegt, z * 250);
        if (grafisch) {
            static CHAR zl[100];
            extern NTU_ZAEHLER ntu_zaehler;
            _snprintf(zl, sizeof(zl) - 1,
                      "usbnt: USB-CD/DVD %s nach %lu s (%lu echte Unterbrechungen, %lu Fadenlaeufe)\n",
                      ntum_cd_angelegt != 0 ? "gefunden" : "FEHLT",
                      (ULONG)(z / 4), (ULONG)ntu_zaehler.Unterbrechungen,
                      (ULONG)ntu_zaehler.Fadenlaeufe);
            HalDisplayString((PUCHAR)zl);
        }
#ifdef NTU_SETUP_BILDSCHIRM
        if (ntum_cd_angelegt == 0) {
            extern VOID ntu_bildschirm_zusammenfassung (ULONG sekunden);
            ntu_bildschirm_zusammenfassung(90);
        }
#endif
    }

    /*  Das Steuergeraet zuletzt: erst wenn die Tabelle steht, hat das
     *  Werkzeug etwas zu lesen.  Scheitert es, laeuft der Treiber
     *  trotzdem -- nur ohne Diagnose.  */

    /*  Rueckruf beim Herunterfahren (siehe ntu_herunterfahren).  Der
     *  Abfertiger fuer IRP_MJ_SHUTDOWN gilt fuer alle Geraete dieses
     *  Treibers; er handelt nur beim eigenen Geraet.  */
    drv->MajorFunction[IRP_MJ_SHUTDOWN] = ntu_herunterfahren;
    if (NT_SUCCESS(IoCreateDevice(drv, 0, NULL, FILE_DEVICE_UNKNOWN, 0,
                                  FALSE, &ntu_abschalt_dev))) {
        ntu_abschalt_dev->Flags &= ~DO_DEVICE_INITIALIZING;
        if (!NT_SUCCESS(IoRegisterShutdownNotification(ntu_abschalt_dev))) {
            IoDeleteDevice(ntu_abschalt_dev);
            ntu_abschalt_dev = 0;
        }
    } else {
        ntu_abschalt_dev = 0;
    }

    ntu_printf("usb: DriverEntry fertig\n");
#ifdef NTU_SETUP_BILDSCHIRM
    {
        /*  Ab hier gehoert der Bildschirm dem Setup (setupdd bzw. dem
         *  Grafiktreiber).  Ein spaeteres HalDisplayString holt ihn sich
         *  zurueck und zerstoert dessen Anzeige.  */
        extern ULONG ntu_bildschirm;
        ntu_bildschirm = 0;
    }
#endif
    ntu_steuer_create();

#ifdef NTU_HERZSCHLAG
    ntu_thread_start(&ntu_herz, ntu_herz_body, 0, "usb herz");
#endif

    return STATUS_SUCCESS;
}
