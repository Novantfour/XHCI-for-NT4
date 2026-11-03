/*
 *  ober/ntxhcipcd.c -- Anschlussaenderungen am xHCI nachholen.
 *
 *  FreeBSDs xHCI-Treiber stoesst die Aufzaehlung ausschliesslich ueber
 *  USBSTS.PCD an: xhci_interrupt ruft xhci_root_intr nur, wenn dieses Bit
 *  steht.  Das Ereignis PORT_STATUS_CHANGE im Ereignisring wird in
 *  xhci_interrupt_poll abgeholt, aber nicht ausgewertet.
 *
 *  Nach der xHCI-Norm (1.2, 5.4.2) ist das in Ordnung: der Baustein setzt
 *  PCD, sobald an irgendeinem Anschluss ein Aenderungsbit von 0 auf 1
 *  springt.  QEMU 8.2 tut das aber nie -- es legt nur das Ereignis ab.
 *  Unter QEMU bemerkte der Treiber deshalb nach der ersten Aufzaehlung
 *  keine einzige Anschlussaenderung mehr: abgezogene Geraete blieben
 *  stehen, neu angesteckte wurden nie adressiert.  Nachgewiesen im
 *  QEMU-Ereignisprotokoll: port_notify, queue_event PORT_STATUS_CHANGE,
 *  FreeBSD quittiert USBSTS mit 8 (nur EINT) -- und dann nichts mehr.
 *
 *  Echte Bausteine, die NEC-Karte eingeschlossen, setzen PCD.  Aber ein
 *  nicht bemerktes Abziehen ist fuer einen Benutzer der schlimmste Fehler,
 *  den ein USB-Treiber haben kann, und Linux verlaesst sich aus gutem Grund
 *  nicht auf das Bit.  Also holt die Unterlage es nach: nach jedem
 *  Handlerlauf die Aenderungsbits aller Anschluesse lesen, und steht eines,
 *  die Aufzaehlung anstossen -- mit derselben oeffentlichen Funktion, die
 *  xhci_root_intr am Ende ruft.
 *
 *  Auf einem normgerechten Baustein ist das harmlos: usb_proc_msignal
 *  fasst doppelte Anstoesse zusammen, und der Aufzaehlungsfaden findet
 *  beim zweiten Mal nichts mehr zu tun.  Auch die kurzlebigen PRC-Bits
 *  waehrend eines Anschlussreset stoessen nur einen weiteren, leeren
 *  Durchlauf an.
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
#include <machine/bus.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/controller/xhci.h>

/*  Die Aenderungsbits im PORTSC: CSC PEC WRC OCC PRC PLC CEC,
 *  Bit 17 bis 23 -- dieselben, die xhci_root_intr abfragt.  */

#define NTU_PORTSC_AENDERUNG  0x00FE0000ul

ULONG ntu_pcd_nachgeholt;

VOID
ntu_xhci_pcd_nachholen (void *softc, ntu_space_tag tag,
                        ntu_space_handle handle, ULONG opbase)
{
    struct xhci_softc *sc = (struct xhci_softc *)softc;
    ULONG              n, k;

    if (sc == 0) {
        return;
    }

    /*  HCSPARAMS1 Bit 24..31: Zahl der Anschluesse.  */

    n = (ntu_r4(tag, handle, 0x04) >> 24) & 0xFF;

    for (k = 0; k < n; k += 1) {

        ULONG p = ntu_r4(tag, handle, opbase + 0x400 + k * 0x10);

        if (p != 0xFFFFFFFFul && (p & NTU_PORTSC_AENDERUNG) != 0) {
            ntu_pcd_nachgeholt += 1;
            usb_needs_explore(&sc->sc_bus, 0);
            return;
        }
    }
}
