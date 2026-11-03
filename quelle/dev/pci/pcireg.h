#ifndef _DEV_PCI_PCIREG_H_
#define _DEV_PCI_PCIREG_H_
#define PCIR_COMMAND     0x04
#define PCIM_CMD_PORTEN  0x0001
#define PCIM_CMD_MEMEN   0x0002
#define PCIM_CMD_BUSMASTEREN 0x0004
#define PCIR_BAR(x)      (0x10 + (x) * 4)
#define PCIR_INTLINE     0x3C
#define PCIS_SERIALBUS_USB 0x03
#define PCIC_SERIALBUS   0x0C
#define PCIP_SERIALBUS_USB_UHCI 0x00
#define PCIP_SERIALBUS_USB_OHCI 0x10
#define PCIP_SERIALBUS_USB_EHCI 0x20
#define PCIP_SERIALBUS_USB_XHCI 0x30
/*  Serial Bus Release Number -- bei allen vier Bauarten an derselben
 *  Stelle.  Ein EHCI ohne 0x20 hier ist ein Brettfehler.  */
#define PCI_USB_SBRN     0x60
/*  UHCI: BIOS-Uebergabe.  Bit 0x8000 an heisst, das BIOS emuliert noch.  */
#define PCI_UHCI_LEGSUP  0xC0
#define PCI_LEGSUP_USBPIRQDEN 0x2000
/*  EHCI: die Uebergabe steht nicht hier, sondern hinter dem EECP-Zeiger
 *  in HCCPARAMS -- also im Speicherraum, nicht im Konfigraum.  */
#endif
