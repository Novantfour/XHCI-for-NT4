/*  dev/pci/pcivar.h -- PCI-Konfigurationsraum.
 *
 *  Alles geht ueber HalGetBusDataByOffset / HalSetBusDataByOffset.  Die
 *  Bus- und Slotnummer steht in den ivars des Wurzelknotens; sie kommt
 *  aus demselben Suchlauf, der den Bericht fuellt (IopScanPciBus).
 *
 *  pci_alloc_msi/msix melden ENXIO.  NT 4.0 kennt kein MSI, und das ist
 *  keine Luecke, sondern das Zielsystem: xHCI faellt in FreeBSD sauber
 *  auf die Leitungsunterbrechung zurueck, wenn MSI fehlschlaegt.  */
#ifndef _DEV_PCI_PCIVAR_H_
#define _DEV_PCI_PCIVAR_H_
#include <sys/types.h>
#include <sys/bus.h>

typedef struct ntu_pci_ivars {
    uint8_t  Bus;
    uint8_t  Slot;
    uint8_t  Func;
    uint16_t Vendor;
    uint16_t Device;
    uint16_t SubVendor;
    uint16_t SubDevice;
    uint8_t  Class;
    uint8_t  SubClass;
    uint8_t  ProgIf;
    uint8_t  Revision;
    uint8_t  Irq;
} ntu_pci_ivars;

uint32_t pci_read_config (device_t, int reg, int width);
void     pci_write_config(device_t, int reg, uint32_t val, int width);

uint16_t pci_get_vendor  (device_t);
uint16_t pci_get_device  (device_t);
uint32_t pci_get_devid   (device_t);
uint8_t  pci_get_class   (device_t);
uint8_t  pci_get_subclass(device_t);
uint8_t  pci_get_progif  (device_t);
uint8_t  pci_get_revid   (device_t);

void     pci_enable_busmaster (device_t);
void     pci_disable_busmaster(device_t);
int      pci_get_powerstate(device_t);
int      pci_set_powerstate(device_t, int state);
device_t pci_find_device(uint16_t vendor, uint16_t device);

#define PCI_POWERSTATE_D0  0
#define PCI_POWERSTATE_D3  3

/*  Kein MSI unter NT 4.0.  Ehrlicher Fehler statt halber Umsetzung.  */
#define pci_alloc_msi(d, c)      (ENXIO)
#define pci_alloc_msix(d, c)     (ENXIO)
#define pci_release_msi(d)       (0)
#define pci_msix_table_bar(d)    (-1)
#endif
