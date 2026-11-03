/*  sys/callout.h -- callout auf KTIMER plus KDPC.
 *
 *  Der DPC laeuft auf DISPATCH_LEVEL, der Rueckruf des USB-Stapels
 *  erwartet aber Prozesskontext (er nimmt mtx).  Der DPC weckt deshalb
 *  denselben Faden, der auch die Unterbrechung abarbeitet -- damit gibt
 *  es genau einen Serialisierungspunkt je Bus und nicht zwei.  */
#ifndef _SYS_CALLOUT_H_
#define _SYS_CALLOUT_H_
#include <sys/types.h>
#include <sys/mutex.h>

struct callout {
    KTIMER   Timer;
    KDPC     Dpc;
    void   (*Fn)(void *);
    void    *Arg;
    struct mtx *Mtx;
    int      Pending;
    void   (*c_func)(void *);   /* FreeBSD prueft das Feld direkt */
};

void callout_init_mtx(struct callout *, struct mtx *, int flags);
void callout_init(struct callout *, int mpsafe);
int  callout_reset(struct callout *, int ticks, void (*)(void *), void *);
int  callout_stop(struct callout *);
int  callout_drain(struct callout *);
int  callout_pending(struct callout *);
#define callout_deactivate(c)  ((c)->Pending = 0)
#define callout_active(c)      ((c)->Pending)
#endif
