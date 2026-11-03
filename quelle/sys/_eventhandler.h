/*  Leerer Rumpf -- siehe sys/eventhandler.h.  */
#ifndef _SYS__EVENTHANDLER_H_
#define _SYS__EVENTHANDLER_H_
#define EVENTHANDLER_DECLARE(n, t)
#define EVENTHANDLER_DEFINE(n, f, a, p)
#define EVENTHANDLER_INVOKE(n, ...)        ((void)0)
#define EVENTHANDLER_REGISTER(n, f, a, p)  ((void *)0)
#define EVENTHANDLER_DEREGISTER(n, t)      ((void)0)
#define EVENTHANDLER_LIST_DECLARE(n)
#define EVENTHANDLER_LIST_DEFINE(n)
#define EVENTHANDLER_DIRECT_INVOKE(n, ...) ((void)0)
#endif
