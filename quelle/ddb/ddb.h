/*  Kein DDB unter NT -- der Kerneldebugger ist i386kd.  */
#ifndef _DDB_DDB_H_
#define _DDB_DDB_H_
#define DB_SHOW_COMMAND(n,f)  static void f(void)
#define DB_SHOW_ALL_COMMAND(n,f) static void f(void)
#define db_printf ntu_printf
#endif
