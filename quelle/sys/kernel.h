#ifndef _SYS_KERNEL_H_
#define _SYS_KERNEL_H_
#include <sys/param.h>
/*  Unter FreeBSD kommt systm.h (pause, printf, ...) ueber andere Koepfe
 *  mit; dev/hid/hid.c verlaesst sich darauf.  */
#include <sys/systm.h>
#define SYSINIT(a,b,c,d,e)
#define SYSUNINIT(a,b,c,d,e)
#define TUNABLE_INT(a,b)
#define SI_SUB_KLD 0
#define SI_ORDER_ANY 0
#endif
