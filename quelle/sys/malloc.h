/*  sys/malloc.h
 *
 *  M_WAITOK heisst in FreeBSD "darf schlafen".  NT kennt das nicht:
 *  ExAllocatePoolWithTag schlaeft nie, es liefert NULL.  Der Aufrufer
 *  im USB-Stapel prueft ohnehin auf NULL -- das ist geprueft, nicht
 *  angenommen (usb_alloc_device, usbd_transfer_setup).
 *
 *  Alles kommt aus NonPagedPool.  PagedPool waere hier ein Bugcheck mit
 *  Ansage: die Puffer werden auf DISPATCH_LEVEL angefasst.  */
#ifndef _SYS_MALLOC_H_
#define _SYS_MALLOC_H_
#include <sys/param.h>

#define M_WAITOK   0x0000
#define M_NOWAIT   0x0001
#define M_ZERO     0x0100
#define M_USB      0
#define M_USBDEV   0
#define M_DEVBUF   0
/*  Wie im echten FreeBSD bekannt machen.  Ohne diese Zeile gilt
 *  "struct malloc_type" in einer Parameterliste nur fuer diese eine Liste
 *  -- Deklaration und Definition von usbd_req_get_hid_desc (usb_hid.c)
 *  bekaemen zwei verschiedene Typen.  */
struct malloc_type;

#define M_TEMP     0

#define MALLOC_DECLARE(t)
#define MALLOC_DEFINE(t, s, l)

void *ntu_malloc(size_t, int flags);
void  ntu_free  (void *);

#define malloc(s, t, f)  ntu_malloc((s), (f))
#define free(p, t)       ntu_free(p)

char *ntu_strdup(const char *, int);
#define strdup(s, t)     ntu_strdup((s), 0)
#endif
