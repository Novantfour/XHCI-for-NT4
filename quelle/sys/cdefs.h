#ifndef _SYS_CDEFS_H_
#define _SYS_CDEFS_H_
#define __FBSDID(s)
#define __BEGIN_DECLS
#define __END_DECLS
#define __DECONST(t,v)  ((t)(unsigned long)(const void *)(v))
#define __containerof(p,t,f) ((t *)((char *)(p) - __offsetof(t,f)))
/*  Der Name muss je Zeile verschieden sein, sonst kollidieren zwei
 *  Zusicherungen in derselben Kopfdatei.  Zwei Ebenen Makro, weil
 *  __LINE__ sonst als Text statt als Zahl eingesetzt wird.  */
#define __CT2(x, y)  __ct_ ## x ## _ ## y
#define __CT1(x, y)  __CT2(x, y)
#define CTASSERT(e)  typedef char __CT1(__ct, __LINE__)[(e) ? 1 : -1]
#define _Static_assert(e,m)
#define __read_mostly
#define __exclusive_cache_line
#define __weak_symbol
#define __result_use_check
#define __nonnull(x)
#define __alloc_size(x)
#define __malloc_like
#endif
