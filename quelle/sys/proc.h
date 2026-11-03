/*  sys/proc.h -- kthread auf PsCreateSystemThread.
 *  usb_process legt je Bus zwei Faden an (Steuerung und Explore); genau
 *  die tragen unter NT auch die Unterbrechungsarbeit.  */
#ifndef _SYS_PROC_H_
#define _SYS_PROC_H_
#include <sys/types.h>
#include <nt/ntunterlage.h>
struct thread;
struct proc;
int  kproc_kthread_add(void (*fn)(void *), void *arg, struct proc **p,
                       struct thread **td, int flags, int pages,
                       const char *procname, const char *fmt, ...);
void kthread_exit(void);
int  kthread_suspend_check(void);
int  kthread_suspend(struct thread *, int timo);
/*  thread_lock schuetzt in FreeBSD die Planerfelder eines Fadens.  Unter
 *  NT fasst der Stapel keine an -- leerer Rumpf, ausdruecklich.  */
#define thread_lock(td)    ((void)0)
#define thread_unlock(td)  ((void)0)
#define PRIBIO 0
#define RFHIGHPID 0
#endif
