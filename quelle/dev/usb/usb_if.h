/*
 *  dev/usb/usb_if.h -- die beiden kobj-Schnittstellen, die der USB-
 *  Stapel wirklich ruft.  Erzeugt FreeBSD aus usb_if.m; hier von Hand,
 *  weil zwei Methoden keine Werkzeugkette rechtfertigen.
 *
 *      take_controller   gibt den Baustein an das BIOS zurueck, bevor
 *                        der Rechner heruntergefahren wird
 *      handle_request    Geraetebetrieb (USB-Ziel statt USB-Wirt);
 *                        wird hier nie gerufen, muss aber uebersetzen
 *
 *  Kein kobj darunter: ein Nachschlagen in der Methodentabelle des
 *  Treibers genuegt, weil sonst nichts dispatcht wird.
 */
#ifndef _DEV_USB_USB_IF_H_
#define _DEV_USB_USB_IF_H_
#include <sys/bus.h>

typedef int usb_take_controller_t(device_t);
/*  Genau die Lage aus usb_if.m -- abgeschrieben, nicht geraten:
 *
 *      METHOD int handle_request {
 *          device_t dev;
 *          const void *req;
 *          void **pptr;
 *          uint16_t *plen;
 *          uint16_t offset;      <- zwei Byte, nicht eines
 *          uint8_t *pstate;
 *      };
 *
 *  Der Versatz ist ein Wort.  Das faellt nicht auf, solange man ihn
 *  nicht benutzt -- und dann ist der halbe Aufrufrahmen verschoben.  */
typedef int usb_handle_request_t(device_t dev, const void *req,
                                 void **pptr, unsigned short *plen,
                                 unsigned short offset,
                                 unsigned char *pstate);

int ntu_usb_take_controller(device_t);
int ntu_usb_handle_request(device_t dev, const void *req, void **pptr,
                           unsigned short *plen, unsigned short offset,
                           unsigned char *pstate);

#define USB_TAKE_CONTROLLER(d)  ntu_usb_take_controller(d)
#define USB_HANDLE_REQUEST(d, req, pptr, plen, off, state) \
        ntu_usb_handle_request((d), (req), (pptr), (plen), (off), (state))
#endif
