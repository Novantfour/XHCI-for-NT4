/*
 *  ober/ntsteuer.c -- das Steuergeraet fuer das Werkzeug.
 *
 *  \Device\UsbNt mit Verweis \DosDevices\UsbNt, damit ein Win32-Programm
 *  es mit CreateFile("\\\\.\\UsbNt") oeffnen kann.
 *
 *  Warum der Treiber die Arbeit tut und nicht das Werkzeug: ein
 *  Benutzerprozess unter NT 4.0 kommt weder an den PCI-Konfigraum noch
 *  an die Register des Bausteins.  Es gibt keinen Weg, der ohne
 *  Kernbeteiligung auskommt -- und ein zweiter Treiber nur zum Lesen
 *  waere ein zweiter Treiber, der abstuerzen kann.
 *
 *  Alle Aufrufe sind METHOD_BUFFERED.  NT kopiert dann selbst, und wir
 *  fassen nie eine Benutzeradresse an.  Bei einem Diagnosewerkzeug ist
 *  das die richtige Wahl: die Puffer sind klein, und ein falsch
 *  geprobter Zeiger waere ein Bugcheck, ausgeloest aus dem
 *  Benutzermodus.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <machine/bus.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <ober/ntkbdmou.h>
#include <ober/ntusbioctl.h>

#ifndef NTU_DATEIFASSUNG
#define NTU_DATEIFASSUNG "unbekannt"
#endif

NTU_ZAEHLER ntu_zaehler;

extern ntu_pci_ivars ntu_hc[];
extern ULONG         ntu_hc_count;
extern device_t      ntu_hc_dev[];
extern ULONG         ntu_ioctl_usbliste(NTU_USB_ANTWORT *a, ULONG platz);   /* ntusbliste.c */
extern ULONG         ntu_hc_status[];

static PDEVICE_OBJECT ntu_steuer_dev;


/* ====================================================================
 *  Hostbausteine
 * ==================================================================== */

static ULONG
ntu_ioctl_hc (NTU_HC_ANTWORT *a, ULONG platz)
{
    ULONG i;

    if (platz < sizeof(*a)) {
        return 0;
    }

    memset(a, 0, sizeof(*a));
    a->Anzahl = (ntu_hc_count < NTU_MAX_HC_IOCTL)
                    ? ntu_hc_count : NTU_MAX_HC_IOCTL;

    for (i = 0; i < a->Anzahl; i += 1) {

        ntu_pci_ivars  *iv = &ntu_hc[i];
        NTU_HC_EINTRAG *e  = &a->Eintrag[i];
        device_t        d  = ntu_hc_dev[i];

        e->Bus       = iv->Bus;
        e->Slot      = iv->Slot;
        e->Func      = iv->Func;
        e->ProgIf    = iv->ProgIf;
        e->Vendor    = iv->Vendor;
        e->Device    = iv->Device;
        e->SubVendor = iv->SubVendor;
        e->SubDevice = iv->SubDevice;
        e->Irq       = iv->Irq;
        e->Revision  = iv->Revision;
        e->Angehaengt = ntu_hc_status[i];

        /*  Konfigraum jetzt lesen, nicht den Stand vom Start.  Genau
         *  darin liegt der Wert: das Command-Register sagt erst nach
         *  dem Anhaengen, ob der Busmeister wirklich an ist.  */

        if (d != 0) {
            e->Command = (unsigned short)pci_read_config(d, PCIR_COMMAND, 2);
            e->Bar     = pci_read_config(d,
                            (iv->ProgIf == PCIP_SERIALBUS_USB_UHCI)
                                ? PCIR_BAR(4) : PCIR_BAR(0), 4);
            e->Sbrn    = (unsigned char)pci_read_config(d, PCI_USB_SBRN, 1);

            if (iv->ProgIf == PCIP_SERIALBUS_USB_UHCI) {
                e->LegSup = (unsigned short)pci_read_config(d,
                                PCI_UHCI_LEGSUP, 2);
            }

            if (d->Res != 0 &&
                (iv->ProgIf == PCIP_SERIALBUS_USB_EHCI ||
                 iv->ProgIf == PCIP_SERIALBUS_USB_XHCI)) {

                e->OpBase = ntu_r1(d->Res->r_tag, d->Res->r_handle, 0);

                if (iv->ProgIf == PCIP_SERIALBUS_USB_XHCI) {
                    e->RtsOff = ntu_r4(d->Res->r_tag, d->Res->r_handle,
                                       0x18) & ~0x1Fu;
                }
            }

            strlcpy(e->Name, device_get_nameunit(d), sizeof(e->Name));
        }
    }

    return sizeof(*a);
}


/* ====================================================================
 *  Geraetebaum
 *
 *  Gelaufen wird der Baum, den ntnewbus.c fuehrt -- derselbe, den der
 *  USB-Kern beim Aufzaehlen fuellt.  Damit sieht das Werkzeug genau das,
 *  was der Stapel sieht, und nicht eine zweite Meinung.
 * ==================================================================== */

static ULONG ntu_dev_n;

static VOID
ntu_dev_sammle (NTU_DEV_ANTWORT *a, device_t d, ULONG tiefe)
{
    LIST_ENTRY *e;

    if (d == 0 || ntu_dev_n >= NTU_MAX_DEV_IOCTL) {
        return;
    }

    {
        NTU_DEV_EINTRAG *x = &a->Eintrag[ntu_dev_n];

        ntu_dev_n += 1;

        x->Tiefe  = (unsigned char)tiefe;
        x->Belegt = (unsigned char)(d->Attached != 0);

        if (d->Driver != 0) {
            strlcpy(x->Treiber, d->Driver->name, sizeof(x->Treiber));
        }

        strlcpy(x->Text, (d->Desc[0] != '\0') ? d->Desc : "-",
                sizeof(x->Text));

        /*  Die USB-Angaben stehen in den ivars, aber nur bei Knoten, die
         *  der USB-Kern angelegt hat.  Bei einem PCI-Kind zeigen die
         *  ivars auf ntu_pci_ivars -- deshalb wird hier nach der Tiefe
         *  unterschieden und nicht geraten.  */

        if (tiefe == 0 && d->Ivars != 0) {
            ntu_pci_ivars *iv = (ntu_pci_ivars *)d->Ivars;

            x->Vendor  = iv->Vendor;
            x->Product = iv->Device;
            x->Klasse  = iv->Class;
            x->Unterklasse = iv->SubClass;
            x->Protokoll   = iv->ProgIf;
        }
    }

    for (e = d->Children.Flink; e != &d->Children; e = e->Flink) {
        ntu_dev_sammle(a, CONTAINING_RECORD(e, struct ntu_device, Siblings),
                       tiefe + 1);
    }
}

static ULONG
ntu_ioctl_dev (NTU_DEV_ANTWORT *a, ULONG platz)
{
    ULONG i;

    if (platz < sizeof(*a)) {
        return 0;
    }

    memset(a, 0, sizeof(*a));
    ntu_dev_n = 0;

    for (i = 0; i < ntu_hc_count; i += 1) {
        ntu_dev_sammle(a, ntu_hc_dev[i], 0);
    }

    a->Anzahl = ntu_dev_n;
    return sizeof(*a);
}


/* ====================================================================
 *  Registerabzug
 *
 *  Nur lesen, nie schreiben.  Ein Schreibpfad aus dem Benutzermodus in
 *  die Register eines Busmeisters waere ein Werkzeug, mit dem man den
 *  Rechner aus einer Eingabeaufforderung heraus abschiessen kann --
 *  und zwar aus Versehen.
 * ==================================================================== */

static ULONG
ntu_ioctl_reg (NTU_REG_ANFRAGE *q, NTU_REG_ANTWORT *a, ULONG platz)
{
    device_t d;
    ULONG    i;
    ULONG    n;
    ULONG    breite;

    if (platz < sizeof(*a) || q->Nummer >= ntu_hc_count) {
        return 0;
    }

    d = ntu_hc_dev[q->Nummer];

    if (d == 0 || d->Res == 0) {
        return 0;
    }

    /*  UHCI liegt im E/A-Raum und hat 16-Bit-Register; alles andere
     *  liegt im Speicher und hat 32-Bit-Register.  Wer einen UHCI mit
     *  32-Bit-Zugriffen liest, bekommt zwei Register in einem Wort und
     *  wundert sich ueber die Werte.  */

    breite = (ntu_hc[q->Nummer].ProgIf == PCIP_SERIALBUS_USB_UHCI) ? 2 : 4;

    n = q->Anzahl;

    if (n > 64) {
        n = 64;
    }

    memset(a, 0, sizeof(*a));
    a->Anzahl = n;
    a->Breite = breite;

    for (i = 0; i < n; i += 1) {

        ULONG off = q->Versatz + i * breite;

        if (off + breite > d->Res->r_len) {
            a->Anzahl = i;
            break;
        }

        a->Wert[i] = (breite == 2)
                        ? ntu_r2(d->Res->r_tag, d->Res->r_handle, off)
                        : ntu_r4(d->Res->r_tag, d->Res->r_handle, off);
    }

    return sizeof(*a);
}


/* ====================================================================
 *  Aufrufverteilung
 * ==================================================================== */

NTSTATUS
ntu_steuer_ioctl (PIRP irp, ULONG code, PVOID puffer, ULONG ein, ULONG aus,
                  PULONG len)
{
    *len = 0;

    switch (code) {

        case IOCTL_NTU_HOSTBAUSTEINE:
            *len = ntu_ioctl_hc((NTU_HC_ANTWORT *)puffer, aus);
            break;

        case IOCTL_NTU_USBGERAETE:
            *len = ntu_ioctl_usbliste((NTU_USB_ANTWORT *)puffer, aus);
            break;

        case IOCTL_NTU_GERAETE:
            *len = ntu_ioctl_dev((NTU_DEV_ANTWORT *)puffer, aus);
            break;

        case IOCTL_NTU_REGISTER:

            if (ein < sizeof(NTU_REG_ANFRAGE)) {
                return STATUS_INVALID_PARAMETER;
            }

            {
                /*  METHOD_BUFFERED benutzt denselben Puffer fuer Ein-
                 *  und Ausgabe.  Die Anfrage muss deshalb vor dem
                 *  Ueberschreiben herausgerettet werden -- sonst liest
                 *  man Register, die die eigene Antwort schon
                 *  ueberschrieben hat.  */

                NTU_REG_ANFRAGE q = *(NTU_REG_ANFRAGE *)puffer;

                *len = ntu_ioctl_reg(&q, (NTU_REG_ANTWORT *)puffer, aus);
            }

            break;

        case IOCTL_NTU_ZAEHLER:

            if (aus < sizeof(NTU_ZAEHLER)) {
                return STATUS_BUFFER_TOO_SMALL;
            }

            ntu_zaehler.Ticks         = (unsigned long)ticks;
            ntu_zaehler.PoolOffen     = ntu_pool_offen();
            ntu_zaehler.PoolVergaben  = ntu_pool_vergaben();
            ntu_zaehler.PoolGemeckert = ntu_pool_gemeckert();
            memcpy(puffer, &ntu_zaehler, sizeof(NTU_ZAEHLER));
            *len = sizeof(NTU_ZAEHLER);
            break;

        case IOCTL_NTU_MAUS_ZUSTAND: {
            NTU_MAUS_ZUSTAND *z = (NTU_MAUS_ZUSTAND *)puffer;
            if (aus < sizeof(NTU_MAUS_ZUSTAND)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            z->Fassung  = NTU_MAUS_FASSUNG;
            z->Maeuse   = (unsigned long)ntu_maus_anzahl;
            z->Knoepfe  = (unsigned long)ntu_maus_knoepfe;
            z->Roh      = (unsigned long)ntu_maus_roh;
            z->Berichte = (unsigned long)ntu_maus_berichte;
            *len = sizeof(NTU_MAUS_ZUSTAND);
            break;
        }

        case IOCTL_NTU_MAUS_HOLEN: {
            NTU_MAUS_BELEGUNG *b = (NTU_MAUS_BELEGUNG *)puffer;
            ULONG              t;
            if (ein < sizeof(unsigned long) || aus < sizeof(NTU_MAUS_BELEGUNG)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            t = b->Taste;
            if (t < 1 || t > NTU_MAUS_TASTEN) {
                return STATUS_INVALID_PARAMETER;
            }
            ntu_maustaste_holen((int)t, b->Text, sizeof(b->Text));
            *len = sizeof(NTU_MAUS_BELEGUNG);
            break;
        }

        case IOCTL_NTU_MAUS_PROTOKOLL:
            if (aus < sizeof(NTU_MAUS_PROTOKOLL)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            ntu_maus_protokoll_holen((NTU_MAUS_PROTOKOLL *)puffer);
            *len = sizeof(NTU_MAUS_PROTOKOLL);
            break;

        case IOCTL_NTU_MAUS_SETZEN: {
            NTU_MAUS_BELEGUNG *b = (NTU_MAUS_BELEGUNG *)puffer;
            if (ein < sizeof(NTU_MAUS_BELEGUNG)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            b->Text[sizeof(b->Text) - 1] = '\0';
            return ntu_maustaste_setzen((int)b->Taste, b->Text);
        }

        case IOCTL_NTU_SPUR:

            if (aus < sizeof(NTU_SPUR_ANTWORT)) {
                return STATUS_BUFFER_TOO_SMALL;
            }

            {
                NTU_SPUR_ANTWORT *s = (NTU_SPUR_ANTWORT *)puffer;
                ULONG             k;

                memset(s, 0, sizeof(*s));
                s->Anzahl = NTU_SPUR_TIEFE;
                s->Kopf   = ntu_spur_kopf;

                for (k = 0; k < NTU_SPUR_TIEFE; k += 1) {
                    s->Eintrag[k].Iman   = ntu_spur_ring[k].Iman;
                    s->Eintrag[k].Usbsts = ntu_spur_ring[k].Usbsts;
                    s->Eintrag[k].Tick   = ntu_spur_ring[k].Tick;
                }

                *len = sizeof(*s);
            }

            break;

        case IOCTL_NTU_SONDER_ZUSTAND:
            if (aus < sizeof(NTU_SONDER_ZUSTAND)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            ntu_sonder_zustand((NTU_SONDER_ZUSTAND *)puffer);
            *len = sizeof(NTU_SONDER_ZUSTAND);
            break;

        case IOCTL_NTU_SONDER_HOLEN: {
            NTU_SONDER_BELEGUNG *b = (NTU_SONDER_BELEGUNG *)puffer;
            if (ein < sizeof(unsigned long) || aus < sizeof(NTU_SONDER_BELEGUNG)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            if (b->Index >= NTU_SONDER_ANZAHL) {
                return STATUS_INVALID_PARAMETER;
            }
            ntu_sonder_holen((int)b->Index, b->Text, sizeof(b->Text));
            *len = sizeof(NTU_SONDER_BELEGUNG);
            break;
        }

        case IOCTL_NTU_SONDER_SETZEN: {
            NTU_SONDER_BELEGUNG *b = (NTU_SONDER_BELEGUNG *)puffer;
            if (ein < sizeof(NTU_SONDER_BELEGUNG)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            b->Text[sizeof(b->Text) - 1] = '\0';
            return ntu_sonder_setzen((int)b->Index, b->Text);
        }

        case IOCTL_NTU_SONDER_ABHOLEN:
            if (aus < sizeof(NTU_SONDER_AUFTRAG)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            *len = ntu_sonder_abholen((NTU_SONDER_AUFTRAG *)puffer) ? sizeof(NTU_SONDER_AUFTRAG) : 0;
            return STATUS_SUCCESS;

        case IOCTL_NTU_VERSION:

            if (aus < sizeof(NTU_VERSION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }

            {
                NTU_VERSION *v = (NTU_VERSION *)puffer;

                memset(v, 0, sizeof(*v));
                v->Fassung = NTU_ABI_FASSUNG;
                /*  "2026.9.26.1, Sep 26 2026" -- Dateifassung (wie in der
                 *  Versionsressource) und Baudatum.  */
                strlcpy(v->Bau, NTU_DATEIFASSUNG ", " __DATE__, sizeof(v->Bau));
                *len = sizeof(*v);
            }

            break;

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }

    return (*len != 0) ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}


/* ====================================================================
 *  Anlegen
 * ==================================================================== */

int
ntu_steuer_create (VOID)
{
    UNICODE_STRING name;
    UNICODE_STRING link;
    NTSTATUS       st;

    memset(&ntu_zaehler, 0, sizeof(ntu_zaehler));

    RtlInitUnicodeString(&name, NTU_DEV_NAME_W);

    st = IoCreateDevice(NtuDriverObject, sizeof(ntu_port), &name,
                        0x00000022 /* FILE_DEVICE_UNKNOWN */, 0, FALSE,
                        &ntu_steuer_dev);

    if (!NT_SUCCESS(st)) {
        ntu_printf("usb: Steuergeraet nicht angelegt (%08x)\n", st);
        return ENXIO;
    }

    ((ntu_port *)ntu_steuer_dev->DeviceExtension)->Kind = NTU_PORT_STEUER;

    RtlInitUnicodeString(&link, NTU_LINK_NAME_W);
    st = IoCreateSymbolicLink(&link, &name);

    if (!NT_SUCCESS(st)) {

        /*  Ohne Verweis findet CreateFile("\\\\.\\UsbNt") nichts.  Das
         *  Geraet bleibt trotzdem stehen -- es ist nur aus Win32 nicht
         *  erreichbar, und das gehoert gesagt statt verschwiegen.  */

        ntu_printf("usb: \\DosDevices\\UsbNt nicht angelegt (%08x) -- "
                   "das Werkzeug wird den Treiber nicht finden\n", st);
    }

    ntu_steuer_dev->Flags |= DO_BUFFERED_IO;
    ntu_steuer_dev->Flags &= ~DO_DEVICE_INITIALIZING;

    ntu_printf("usb: Steuergeraet %S bereit\n", NTU_DEV_NAME_W);
    return 0;
}

VOID
ntu_steuer_delete (VOID)
{
    UNICODE_STRING link;

    if (ntu_steuer_dev == 0) {
        return;
    }

    RtlInitUnicodeString(&link, NTU_LINK_NAME_W);
    IoDeleteSymbolicLink(&link);
    IoDeleteDevice(ntu_steuer_dev);
    ntu_steuer_dev = 0;
}
