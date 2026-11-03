/*
 *  nt/ntzusatz.c -- Zusatztreiber beim USB-Stapel anmelden.
 *
 *  Ein Zusatztreiber ist eine eigene .sys (zuerst USBXBOX.SYS fuer
 *  Gamepads), die einen FreeBSD-Schnittstellentreiber enthaelt und ihn
 *  beim Laden hier eintraegt.  Sie importiert dazu die Stapelfunktionen,
 *  die usbnt.sys exportiert (siehe bau-clangcl.sh: die Exportliste wird
 *  aus den offenen Bezuegen des Zusatztreibers erzeugt).
 *
 *  Das eigentliche Problem ist die Reihenfolge.  Laedt der Zusatztreiber
 *  nach dem ersten Absuchen der Busse, hat ntugen (BUS_PROBE_HOOVER, der
 *  "Staubsauger") die Schnittstellen schon genommen, die sonst niemand
 *  wollte -- also gerade die des Gamepads.  Ein blosses Neu-Absuchen
 *  aendert daran nichts, weil belegte Schnittstellen uebersprungen
 *  werden.  Deshalb wird jedes Geraet, an dem ntugen haengt, mit
 *  FreeBSDs eigenem Mittel neu aufgezaehlt (usbd_start_re_enumerate):
 *  alle Treiber los, Anschluss zuruecksetzen, neu auswaehlen -- jetzt mit
 *  dem Zusatztreiber in der Liste.  Geraete ohne ntugen bleiben unberuehrt.
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
#include <sys/callout.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/usb_dynamic.h>

int  ntu_zusatz_hinzu (const ntu_drvreg *r);           /* nt/ntnewbus.c */
void ntu_printf (const char *fmt, ...);

#define NTU_NEU_MAX 16

static int
haengt_ntugen (struct usb_device *udev)
{
    struct usb_interface *iface;
    uint8_t i;

    for (i = 0; i < USB_IFACE_MAX; i += 1) {
        iface = usbd_get_iface(udev, i);
        if (iface == NULL) {
            break;
        }
        if (iface->subdev != NULL && iface->subdev->Driver != NULL &&
            strcmp(iface->subdev->Driver->name, "ntugen") == 0) {
            return 1;
        }
    }
    return 0;
}

/*  Von aussen gerufen (Export).  0 = angemeldet.  */

int __stdcall
ntu_treiber_anmelden (const ntu_drvreg *r)
{
    struct usb_device *neu[NTU_NEU_MAX];
    struct usb_bus    *bus;
    devclass_t         dc;
    device_t           dev;
    int                max, n = 0, k;
    uint8_t            a;

    if (r == NULL || r->driver == NULL || r->parent == NULL) {
        return EINVAL;
    }
    if (ntu_zusatz_hinzu(r) != 0) {
        ntu_printf("usb: Zusatztreiber %s: kein Platz mehr\n", r->name);
        return ENOMEM;
    }
    ntu_printf("usb: Zusatztreiber %s angemeldet (an %s)\n", r->name, r->parent);

    /*  Geraete sammeln, an denen ntugen haengt -- unter der Bussperre;
     *  neu aufgezaehlt wird erst danach, weil usb_needs_explore die
     *  Sperre selbst nimmt.  */

    dc = usb_devclass_ptr;
    if (dc != NULL) {
        for (max = devclass_get_maxunit(dc); max >= 0; max -= 1) {
            dev = devclass_get_device(dc, max);
            bus = dev ? device_get_softc(dev) : NULL;
            if (bus == NULL || bus->devices == NULL) {
                continue;
            }
            USB_BUS_LOCK(bus);
            for (a = USB_ROOT_HUB_ADDR; a < bus->devices_max && n < NTU_NEU_MAX; a += 1) {
                struct usb_device *udev = bus->devices[a];
                if (udev != NULL && udev->parent_hub != NULL && haengt_ntugen(udev)) {
                    neu[n++] = udev;
                }
            }
            USB_BUS_UNLOCK(bus);
        }
    }

    for (k = 0; k < n; k += 1) {
        ntu_printf("usb: %04x:%04x wird neu aufgezaehlt (fuer %s)\n",
                   UGETW(neu[k]->ddesc.idVendor), UGETW(neu[k]->ddesc.idProduct), r->name);
        usbd_start_re_enumerate(neu[k]);
    }

    /*  Freie Schnittstellen aller Busse mit der neuen Liste pruefen.  */

    usb_needs_explore_all();
    return 0;
}

/*  hz als Funktion: Daten ueber Modulgrenzen zu importieren verlangt
 *  dllimport im Quelltext des Zusatztreibers -- der bleibt so unberuehrt.  */

int __stdcall
ntu_hz (void)
{
    return hz;
}
