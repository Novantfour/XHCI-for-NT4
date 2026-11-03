/*  Leerer Rumpf.  Absichtlich leer und nicht weggelassen: die Zaehler
 *  sind Diagnose, und wenn sie einmal gebraucht werden, ist hier der
 *  Ort, an dem sie auf den p08-Bericht umgelenkt werden.  */
#ifndef _SYS_SYSCTL_H_
#define _SYS_SYSCTL_H_
#define SYSCTL_DECL(n)
#define SYSCTL_NODE(...)
#define SYSCTL_ROOT_NODE(a,b,c,d,e)
/*  SYSCTL_INT legt "Name -> Adresse" in eine eigene Binder-Sektion, wie
 *  DRIVER_MODULE (sys/module.h).  Beim Start setzt nt/ntsysctl.c daraus
 *  die Werte aus Services\<dienst>\Parameters\Sysctl, z. B.
 *      hw.usb.xhci.ctlstep=1;hw.usb.xhci.polling=1
 *  FreeBSDs Quelltext bleibt unveraendert.  */

typedef struct ntu_sysctlreg {
    const char *eltern;         /* "_hw_usb_xhci" */
    const char *name;           /* "ctlstep" */
    int        *wert;
} ntu_sysctlreg;

#pragma section("NTUSYS$a", read)
#pragma section("NTUSYS$m", read)
#pragma section("NTUSYS$z", read)

#define SYSCTL_INT(a,b,c,d,e,f,g)                                         \
    static const ntu_sysctlreg __ntu_sc_##a##_##c = { #a, #c, (int *)(e) }; \
    __declspec(allocate("NTUSYS$m")) __attribute__((used))                \
    static const ntu_sysctlreg * const __ntu_scp_##a##_##c = &__ntu_sc_##a##_##c
#define SYSCTL_UINT(...)
#define SYSCTL_ULONG(...)
#define SYSCTL_STRING(...)
struct sysctl_req { void *oldptr; void *newptr; unsigned int newlen; };
struct sysctl_oid;
int sysctl_handle_int(struct sysctl_oid *, void *, int, struct sysctl_req *);
#define SYSCTL_HANDLER_ARGS \
        struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req
#define SYSCTL_IN(r, p, l)   (0)
#define SYSCTL_OUT(r, p, l)  (0)
#define SYSCTL_ADD_INT(c,p,n,nm,f,pt,v,d)  ((struct sysctl_oid *)0)
#define SYSCTL_ADD_NODE(c,p,n,nm,f,h,d)    ((struct sysctl_oid *)0)
#define SYSCTL_ADD_PROC(c,p,n,nm,f,a1,a2,h,fmt,d) ((struct sysctl_oid *)0)
#define SYSCTL_PROC(...)
#define SYSCTL_CHILDREN(o)                 ((void *)0)
#define SYSCTL_STATIC_CHILDREN(o)          ((void *)0)
#define CTLFLAG_RD 0
#define CTLFLAG_RW 0
#define CTLFLAG_RWTUN 0
#define CTLFLAG_MPSAFE 0
#define CTLTYPE_INT 0
#define OID_AUTO 0
#endif
