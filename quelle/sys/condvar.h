/*  sys/condvar.h -- cv auf KEVENT (SynchronizationEvent).
 *
 *  cv_wait gibt den Mutex ab, wartet, nimmt ihn wieder.  KeWaitForSingle-
 *  Object kann das nicht in einem Zug; deshalb steht die Reihenfolge in
 *  ntunterlage.c und nicht hier.  Das ist die eine Stelle, an der ein
 *  Fenster entsteht -- sie ist dort beschrieben.  */
#ifndef _SYS_CONDVAR_H_
#define _SYS_CONDVAR_H_
#include <sys/mutex.h>

struct cv { ntu_cv u; };

#define cv_init(c,d)          ntu_cv_init(&(c)->u, (d))
#define cv_destroy(c)         ntu_cv_destroy(&(c)->u)
#define cv_wait(c,m)          ntu_cv_wait(&(c)->u, &(m)->u)
#define cv_wait_sig(c,m)      (ntu_cv_wait(&(c)->u, &(m)->u), 0)
#define cv_timedwait(c,m,t)   ntu_cv_timedwait(&(c)->u, &(m)->u, (t))
#define cv_timedwait_sig(c,m,t) ntu_cv_timedwait(&(c)->u, &(m)->u, (t))
#define cv_signal(c)          ntu_cv_signal(&(c)->u)
#define cv_broadcast(c)       ntu_cv_broadcast(&(c)->u)
#define cv_broadcastpri(c,p)  ntu_cv_broadcast(&(c)->u)
#endif
