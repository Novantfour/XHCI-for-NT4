#ifndef _SYS_SYSTM_H_
#define _SYS_SYSTM_H_
#include <sys/param.h>
#include <sys/libkern.h>
#include <sys/errno.h>

#define printf        ntu_printf
#define snprintf      _snprintf
#define panic         ntu_panic

#define KASSERT(e, m) do { if (!(e)) { ntu_printf m; \
                                       ntu_panic("KASSERT %s:%d\n", \
                                                 __FILE__, __LINE__); } } while (0)
#define MPASS(e)      KASSERT((e), ("MPASS"))

#define DELAY(us)     ntu_delay_us(us)
#define pause(w,t)    ntu_pause_ticks(t)

#define cold          0
extern volatile int ticks;

int copyin (const void *, void *, size_t);
int copyout(const void *, void *, size_t);
struct uio;
int uiomove(void *, int, struct uio *);

#define bootverbose   0
extern int rebooting;   /* bleibt 0: NT meldet uns kein Herunterfahren */
#define curthread     ((void *)KeGetCurrentThread())
#endif
