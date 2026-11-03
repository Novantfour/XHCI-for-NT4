/*  sys/sx.h -- Leser/Schreiber-Sperre.
 *
 *  Der USB-Stapel benutzt sx nur zur Serialisierung der Aufzaehlung
 *  (usbd_enum_lock).  Echte Nebenlaeufigkeit unter Lesern bringt dort
 *  nichts, also ist ein KMUTEX ausreichend und deutlich weniger Code,
 *  der falsch sein kann.  ERESOURCE waere die genauere, aber unnoetige
 *  Abbildung.  */
#ifndef _SYS_SX_H_
#define _SYS_SX_H_
#include <sys/mutex.h>

struct sx { ntu_mtx u; };

#define SX_DUPOK 0
#define SX_NOWITNESS 0
#define sx_init(s,d)         ntu_mtx_init(&(s)->u, (d), "sx", 0)
#define sx_init_flags(s,d,f) ntu_mtx_init(&(s)->u, (d), "sx", 0)
#define sx_destroy(s)        ntu_mtx_destroy(&(s)->u)
#define sx_xlock(s)          ntu_mtx_lock(&(s)->u)
#define sx_xlock_sig(s)      (ntu_mtx_lock(&(s)->u), 0)
#define sx_xunlock(s)        ntu_mtx_unlock(&(s)->u)
#define sx_slock(s)          ntu_mtx_lock(&(s)->u)
#define sx_sunlock(s)        ntu_mtx_unlock(&(s)->u)
#define sx_unlock(s)         ntu_mtx_unlock(&(s)->u)
#define sx_xlocked(s)        ntu_mtx_owned(&(s)->u)
#define sx_assert(s,w)       ((void)0)
#endif
