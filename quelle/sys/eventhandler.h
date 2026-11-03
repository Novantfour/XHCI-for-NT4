/*  Leerer Rumpf.  eventhandler haengt am FreeBSD-Bootablauf; unter NT
 *  gibt es die Ereignisse nicht, an die man sich haengen koennte.  */
#ifndef _SYS_EVENTHANDLER_H_
#define _SYS_EVENTHANDLER_H_
#define EVENTHANDLER_DECLARE(n,t)
#define EVENTHANDLER_DEFINE(n,f,a,p)
#define EVENTHANDLER_INVOKE(n, ...)  ((void)0)
#define EVENTHANDLER_REGISTER(n,f,a,p)  ((void *)0)
#define EVENTHANDLER_DEREGISTER(n,t)    ((void)0)
#endif
