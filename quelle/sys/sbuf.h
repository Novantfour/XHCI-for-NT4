/*  Leerer Rumpf.  sbuf wird nur fuer Geraetepfade gebraucht, und die
 *  sind Beschriftung -- siehe bus_generic_get_device_path.  */
#ifndef _SYS_SBUF_H_
#define _SYS_SBUF_H_
#include <sys/types.h>
struct sbuf;
#define sbuf_printf(s, ...)  (0)
#define sbuf_cat(s, t)       (0)
#endif
