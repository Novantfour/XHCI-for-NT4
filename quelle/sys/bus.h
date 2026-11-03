/*  sys/bus.h -- newbus, nachgebaut statt abgebildet.
 *
 *  Der USB-Stapel benutzt den Geraetebaum wirklich: jeder Hub haengt
 *  seine Anschluesse als Kinder ein, jedes Kind traegt sein softc.  Ein
 *  NT-DEVICE_OBJECT je USB-Geraet waere teuer und brächte nichts, denn
 *  NT 4.0 kennt keinen USB-Bus, an dem es haengen koennte.  Nur der
 *  Wurzelknoten haelt ein echtes DEVICE_OBJECT.
 *
 *  Das ist der einzige groessere Nachbau der ganzen Unterlage -- und mit
 *  neunzehn Namen immer noch ein Fuenftel dessen, was das Linux-
 *  Geraetemodell verlangt haette.  */
#ifndef _SYS_BUS_H_
#define _SYS_BUS_H_
#include <sys/types.h>
#include <sys/rman.h>
#include <nt/ntunterlage.h>

/*  driver_t und device_method_t muessen die FreeBSD-Lagen haben, weil
 *  die Treiber sie mit benannten Feldern fuellen (.name, .methods,
 *  .size).  Was fehlt, ist kobj: die Treiber rufen keine DEVICE_*- oder
 *  BUS_*-Makros, also reicht ein Nachschlagen im methods-Feld.  Genau
 *  eine Ausnahme gibt es -- USB_TAKE_CONTROLLER -- und die steht in
 *  dev/usb/usb_if.h.  */

/*  Der Zeiger ist ein Funktionszeiger, kein void *.  Ein Funktions-
 *  zeiger in void * ist in C kein konstanter Ausdruck, und die
 *  Methodentabellen sind statische Anfangswerte -- der Uebersetzer hat
 *  genau das beanstandet.  */
typedef void (*ntu_methodfn)(void);

typedef struct device_method {
    const char  *name;
    ntu_methodfn func;
} device_method_t, kobj_method_t;

typedef struct driver {
    const char       *name;
    device_method_t  *methods;
    size_t            size;
    void             *baseclasses;
    unsigned int      refs;
    void             *ops;
} driver_t, kobj_class_t;

typedef struct devclass *devclass_t;
struct sbuf;

#define DEVICE_UNIT_ANY  (-1)

/*  Die Treiber schreiben ihre Rumpfe als
 *      static device_probe_t  ohci_pci_probe;
 *  also braucht es die Typnamen, nicht nur die Zeiger.  */
typedef int  device_probe_t   (device_t);
typedef int  device_attach_t  (device_t);
typedef int  device_detach_t  (device_t);
typedef int  device_shutdown_t(device_t);
typedef int  device_suspend_t (device_t);
typedef int  device_resume_t  (device_t);
typedef int  bus_child_location_t(device_t, device_t, struct sbuf *);
typedef int  bus_child_pnpinfo_t (device_t, device_t, struct sbuf *);
typedef void bus_driver_added_t  (device_t, driver_t *);
typedef int  bus_get_device_path_t(device_t, device_t, const char *,
                                   struct sbuf *);
typedef void driver_intr_t(void *);
typedef int  driver_filter_t(void *);

#define BUS_PROBE_GENERIC   (-20)
#define BUS_PROBE_DEFAULT   (-20)
#define BUS_PROBE_SPECIFIC  0
#define BUS_PROBE_VENDOR    (-10)
#define BUS_PROBE_LOW_PRIORITY (-40)
#define BUS_PROBE_HOOVER    (-1000000)   /* nimmt, was sonst keiner will */

/*  kobj-Klassenankuendigung.  Ein Treiber, der DECLARE_CLASS benutzt,
 *  sagt damit nur "diesen driver_t gibt es woanders".  */
#define DECLARE_CLASS(n)  extern driver_t n

void       *device_get_softc(device_t);
void        device_set_softc(device_t, void *);
void       *device_get_ivars(device_t);
void        device_set_ivars(device_t, void *);
device_t    device_get_parent(device_t);
const char *device_get_nameunit(device_t);
const char *device_get_desc(device_t);
void        device_set_desc(device_t, const char *);
void        device_set_desc_copy(device_t, const char *);
int         device_get_unit(device_t);
int         device_is_attached(device_t);
int         device_is_alive(device_t);
void        device_quiet(device_t);
device_t    device_add_child(device_t, const char *, int);
int         device_delete_child(device_t, device_t);
int         device_probe_and_attach(device_t);
void        device_printf(device_t, const char *, ...);

int         bus_generic_detach(device_t);
int         ntu_dev_method_call(device_t, const char *);

/*  Die beiden einzigen DEVICE_*-Makros, die der Stapel ruft.  */
#define DEVICE_SUSPEND(d)  ntu_dev_method_call((d), "device_suspend")
#define DEVICE_RESUME(d)   ntu_dev_method_call((d), "device_resume")
int         bus_generic_suspend(device_t);
int         bus_generic_resume(device_t);
int         bus_generic_shutdown(device_t);
int         bus_detach_children(device_t);
int         bus_generic_get_device_path(device_t, device_t, const char *,
                                        struct sbuf *);
/*  devclass: FreeBSD fuehrt je Treibername eine Liste seiner Geraete.
 *  Der USB-Stapel benutzt davon nur das Aufzaehlen aller "usbus" beim
 *  Herunterfahren.  Nachgebaut als Gang durch die Kinder der Wurzel --
 *  bei hoechstens einer Handvoll Bussen ist das billiger als eine
 *  zweite Liste, die konsistent gehalten werden muesste.  */
devclass_t  devclass_find(const char *);
int         devclass_get_maxunit(devclass_t);
device_t    devclass_get_device(devclass_t, int unit);
void       *devclass_get_softc(devclass_t, int unit);

bus_dma_tag_t bus_get_dma_tag(device_t);

void        bus_topo_lock(void);
void        bus_topo_unlock(void);

struct resource *bus_alloc_resource_any(device_t, int type, int *rid,
                                        unsigned int flags);
int   bus_release_resource(device_t, int type, int rid, struct resource *);
int   bus_setup_intr(device_t, struct resource *, int flags,
                     int (*filter)(void *), void (*handler)(void *),
                     void *arg, void **cookiep);
int   bus_teardown_intr(device_t, struct resource *, void *cookie);

#define INTR_TYPE_BIO   0x0002
#define INTR_TYPE_MISC  0x0080
#define INTR_MPSAFE     0x0200
#define BUS_LOCATOR_ACPI  "ACPI"
#define BUS_LOCATOR_UEFI  "UEFI"
#define BUS_LOCATOR_FREEBSD "FreeBSD"

/*  sysctl-Baum: der USB-Stapel legt Zaehler an, die niemand liest.
 *  Leerer Rumpf, ausdruecklich.  */
struct sysctl_ctx_list;
struct sysctl_oid;
#define device_get_sysctl_ctx(d)   ((struct sysctl_ctx_list *)0)
#define device_get_sysctl_tree(d)  ((struct sysctl_oid *)0)
#endif
