/*  sys/mutex.h -- mtx auf KMUTEX.
 *
 *  Warum nicht KSPIN_LOCK, obwohl der Skill spin_lock_irqsave und
 *  KeAcquireSpinLock "dasselbe Bauteil" nennt: weil der USB-Stapel unter
 *  gehaltenem mtx auf Bedingungsvariablen wartet (usbd_transfer_drain,
 *  usb_proc_mwait).  Unter einem Spinlock waere das ein Bugcheck.
 *  Deshalb der Fadenumbau in nt/ntunterlage.h.  */
#ifndef _SYS_MUTEX_H_
#define _SYS_MUTEX_H_
#include <sys/lock.h>
#include <nt/ntunterlage.h>

struct mtx { ntu_mtx u; };

#define MTX_DEF      0x0000
#define MTX_SPIN     0x0001
#define MTX_RECURSE  0x0004
#define MTX_NEW      0x0000
#define MTX_QUIET    0x0000

#define mtx_init(m,n,t,o)   ntu_mtx_init(&(m)->u, (n), (t), (o))
#define mtx_destroy(m)      ntu_mtx_destroy(&(m)->u)
#define mtx_lock(m)         ntu_mtx_lock(&(m)->u)
#define mtx_unlock(m)       ntu_mtx_unlock(&(m)->u)
#define mtx_lock_spin(m)    ntu_mtx_lock(&(m)->u)
#define mtx_unlock_spin(m)  ntu_mtx_unlock(&(m)->u)
#define mtx_owned(m)        ntu_mtx_owned(&(m)->u)
#define mtx_assert(m,w)     KASSERT(1, ("mtx_assert"))
#define mtx_initialized(m)  ((m)->u.Name != 0)
#define mtx_sleep(i,m,p,w,t) ntu_pause_ticks(t)

/*  Giant gibt es nicht -- unter NT haelt niemand eine Weltsperre.  Die
 *  Stellen, die darauf prueft, laufen damit durch, und das ist richtig:
 *  sie fragen "darf ich hier schlafen", und die Antwort lautet ja, weil
 *  der Fadenumbau alles auf PASSIVE_LEVEL bringt.  */
extern struct mtx Giant;
#define WARN_GIANTOK  0
#define WARN_SLEEPOK  0
#define WITNESS_WARN(f,l,m)  ((void)0)
#endif
