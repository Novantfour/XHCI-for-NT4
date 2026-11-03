/*
 *  ober/ntxhcisonder.c -- xHCI: Intel-Sonderbehandlung anzeigen und
 *  schalten.
 *
 *  FreeBSDs xhci_pci_attach schaltet fuer Intel-Kennungen (Panther Point
 *  8086:1e31, Lynx Point, ...) zwei Dinge ein:
 *    - sc_ctlstep: Steuertransfers Schritt fuer Schritt -- der Status-
 *      Schritt wird erst freigegeben, wenn Setup und Daten fertig sind;
 *    - sc_port_route: Anschluesse von EHCI auf xHCI umschalten, und bei
 *      einem Adressfehler alle wieder weg.
 *
 *  8086:1e31 ist auch die Kennung, die VirtualBox fuer seinen USB-3.0-
 *  Controller nachbildet.  QEMUs xHCI fasst eine Steuerübertragung von
 *  SETUP bis STATUS zusammen und beginnt erst, wenn alle Teile gueltig
 *  sind ("hack to bundle together the two/three TDs that make a setup
 *  transfer", hw/usb/hcd-xhci.c) -- mit dem Schrittmodus wartet dort
 *  jeder auf den anderen.  Verhaelt sich VirtualBox ebenso, haengt jede
 *  Geraeteeinrichtung.  Echte Intel-Bausteine arbeiten Schritt fuer
 *  Schritt und brauchen den Modus.
 *
 *  Parameters\XhciSchritt (REG_DWORD): 0 = aus, 1 = ein, fehlt =
 *  FreeBSDs Vorgabe.  Gesetzt direkt nach dem Anhaengen, vor der ersten
 *  Geraeteeinrichtung (die laeuft im Erkundungsfaden, nach den
 *  Anschluss-Resets).
 */

#include <sys/stdint.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/controller/xhci.h>
#include <ober/ntkbdmou.h>

ULONG ntu_xhci_schritt = 0xFFFFFFFFul;      /* aus Parameters\XhciSchritt */

void
ntu_xhci_sonder (device_t d, unsigned vid, unsigned did)
{
    struct xhci_softc *sc = (struct xhci_softc *)device_get_softc(d);

    if (sc == NULL) {
        return;
    }

    ntu_printf("usb: xHCI %04x:%04x  Intel-Sonderbehandlung: Schrittmodus %s, "
               "Anschlussumschaltung %s\n", vid, did,
               sc->sc_ctlstep ? "an" : "aus",
               sc->sc_port_route != NULL ? "an" : "aus");

    if (ntu_xhci_schritt != 0xFFFFFFFFul &&
        (int)(ntu_xhci_schritt != 0) != (int)(sc->sc_ctlstep != 0)) {
        sc->sc_ctlstep = (ntu_xhci_schritt != 0) ? 1 : 0;
        ntu_printf("usb: xHCI Schrittmodus jetzt %s (Parameters\\XhciSchritt)\n",
                   sc->sc_ctlstep ? "an" : "aus");
    }
}
