/*  Leerer Rumpf.  Gehoert zum Zeichengeraet /dev/ugen, das ueber
 *  USB_HAVE_UGEN = 0 abgeschaltet ist -- siehe dev/usb/usb_freebsd.h.  */
#ifndef _SYS_STUB_selinfo_
#define _SYS_STUB_selinfo_
struct selinfo { int si_unused; };
#define selrecord(td, si)   ((void)0)
#define selwakeup(si)       ((void)0)
#define knlist_init_mtx(k,m) ((void)0)
#define knlist_destroy(k)    ((void)0)
#endif
