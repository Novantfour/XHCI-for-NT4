/*
 *  ober/ntusbliste.c -- IOCTL_NTU_USBGERAETE: jedes USB-Geraet einmal,
 *  wie lsusb.  Fuer den Geraetemanager (DEVMGR.EXE / DEVMGR.CPL).
 *
 *  Weg: je Hostbaustein das Kind "usbus"; dessen ivars sind der struct
 *  usb_bus (so legt ihn usb_controller.c an).  bus->devices[] ist nach
 *  Adresse geordnet, [USB_ROOT_HUB_ADDR] ist der Wurzelhub.
 *
 *  Sperre: die Aufzaehlungssperre des Wurzelhubs.  Genau die haelt der
 *  Erkundungsfaden, waehrend er Geraete an- und abmeldet -- solange wir
 *  lesen, verschwindet also keins.  Nur auf PASSIVE_LEVEL (IOCTL).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/callout.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/usb_device.h>
#include <ober/ntkbdmou.h>
#include <ober/ntusbioctl.h>

extern ULONG    ntu_hc_count;
extern device_t ntu_hc_dev[];

static void
kopie (char *z, const char *q, int n)
{
    int i = 0;
    if (q != 0) {
        for (; i < n - 1 && q[i] != '\0'; i += 1) {
            z[i] = q[i];
        }
    }
    z[i] = '\0';
}

/*  Das Kind "usbus" eines Hostbausteins.  */
static struct usb_bus *
bus_von (device_t hc)
{
    LIST_ENTRY *e;
    for (e = hc->Children.Flink; e != &hc->Children; e = e->Flink) {
        device_t k = CONTAINING_RECORD(e, struct ntu_device, Siblings);
        const char *n = device_get_nameunit(k);
        if (n != 0 && n[0] == 'u' && n[1] == 's' && n[2] == 'b' && n[3] == 'u' && n[4] == 's') {
            return (struct usb_bus *)device_get_ivars(k);
        }
    }
    return 0;
}

static void
ein_geraet (NTU_USB_GERAET *g, struct usb_device *u, ULONG busnr, const char *art)
{
    ULONG k;

    g->Bus             = (unsigned char)busnr;
    g->Adresse         = u->address;
    g->ElternAdresse   = (u->parent_hub != 0) ? u->parent_hub->address : 0;
    g->Anschluss       = u->port_no;
    g->Tiefe           = u->depth;
    g->Geschwindigkeit = (unsigned char)u->speed;
    g->Vendor          = UGETW(u->ddesc.idVendor);
    g->Product         = UGETW(u->ddesc.idProduct);
    g->Geraetefassung  = UGETW(u->ddesc.bcdDevice);
    g->UsbFassung      = UGETW(u->ddesc.bcdUSB);
    g->Klasse          = u->ddesc.bDeviceClass;
    g->Unterklasse     = u->ddesc.bDeviceSubClass;
    g->Protokoll       = u->ddesc.bDeviceProtocol;
    g->MaxPaket0       = u->ddesc.bMaxPacketSize;
    g->Konfigurationen = u->ddesc.bNumConfigurations;
    g->Hub             = (u->hub != 0) ? 1 : 0;
    if (u->cdesc != 0) {
        /*  bMaxPower: Einheiten zu 2 mA, bei SuperSpeed zu 8 mA.  */
        g->StromMa = (unsigned short)(u->cdesc->bMaxPower * (u->speed == USB_SPEED_SUPER ? 8 : 2));
    }
    kopie(g->Hersteller,   u->manufacturer, sizeof(g->Hersteller));
    kopie(g->Produkt,      u->product,      sizeof(g->Produkt));
    kopie(g->Seriennummer, u->serial,       sizeof(g->Seriennummer));
    if (u->parent_hub == 0) {
        kopie(g->HostArt, art, sizeof(g->HostArt));
    }

    for (k = 0; k < u->ifaces_max && g->AnzahlIf < NTU_USB_MAX_IF; k += 1) {
        struct usb_interface *ifc = usbd_get_iface(u, (uint8_t)k);
        NTU_USB_IF           *x;
        if (ifc == 0 || ifc->idesc == 0) {
            continue;
        }
        x = &g->If[g->AnzahlIf++];
        x->Nummer      = ifc->idesc->bInterfaceNumber;
        x->Klasse      = ifc->idesc->bInterfaceClass;
        x->Unterklasse = ifc->idesc->bInterfaceSubClass;
        x->Protokoll   = ifc->idesc->bInterfaceProtocol;
        if (ifc->subdev != 0 && device_is_attached(ifc->subdev)) {
            kopie(x->Treiber, device_get_nameunit(ifc->subdev), sizeof(x->Treiber));
        }
    }
}

ULONG
ntu_ioctl_usbliste (NTU_USB_ANTWORT *a, ULONG platz)
{
    ULONG i, adr;

    if (platz < sizeof(*a) || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return 0;
    }
    memset(a, 0, sizeof(*a));
    a->Fassung = 1;

    for (i = 0; i < ntu_hc_count; i += 1) {
        struct usb_bus    *bus = bus_von(ntu_hc_dev[i]);
        struct usb_device *wurzel;
        uint8_t            entsperren;
        const char        *art = device_get_nameunit(ntu_hc_dev[i]);
        char               grossart[8];
        int                j;

        if (bus == 0 || bus->devices == 0 || bus->devices_max <= USB_ROOT_HUB_ADDR) {
            continue;
        }
        wurzel = bus->devices[USB_ROOT_HUB_ADDR];
        if (wurzel == 0) {
            continue;
        }
        /*  "ohci0" -> "OHCI"  */
        for (j = 0; j < 4 && art != 0 && art[j] != '\0'; j += 1) {
            grossart[j] = (char)((art[j] >= 'a' && art[j] <= 'z') ? art[j] - 32 : art[j]);
        }
        grossart[j] = '\0';

        entsperren = usbd_enum_lock(wurzel);
        for (adr = USB_ROOT_HUB_ADDR; adr < bus->devices_max && a->Anzahl < NTU_USB_MAX; adr += 1) {
            struct usb_device *u = bus->devices[adr];
            if (u != 0) {
                ein_geraet(&a->Geraet[a->Anzahl++], u, i, grossart);
            }
        }
        if (entsperren) {
            usbd_enum_unlock(wurzel);
        }
    }
    return sizeof(*a);
}
