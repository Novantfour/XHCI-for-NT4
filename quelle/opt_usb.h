/*  opt_usb.h -- FreeBSDs Baukasten legt diese Datei an; hier steht sie
 *  fest.  USB_DEBUG bleibt an: die Ausgabe geht ueber ntu_printf auf
 *  COM1, und bei einem Stapel, der noch nie auf dieser Maschine lief,
 *  ist das der Unterschied zwischen einer Stunde und einem Tag.
 *  Ausschalten, sobald er laeuft -- die DPRINTF-Rufe sind 617 Stueck.  */
#ifndef _OPT_USB_H_
#define _OPT_USB_H_
#define USB_DEBUG 1
#endif
