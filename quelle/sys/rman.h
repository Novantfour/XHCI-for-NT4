#ifndef _SYS_RMAN_H_
#define _SYS_RMAN_H_
#include <sys/types.h>
#include <machine/bus.h>
#include <machine/resource.h>

struct resource {
    int                 r_type;
    int                 r_rid;
    bus_space_tag_t     r_tag;
    bus_space_handle_t  r_handle;
    ULONG               r_start;
    ULONG               r_len;
    ULONG               r_irq;
};

#define RF_ACTIVE     0x0002
#define RF_SHAREABLE  0x0004

#define rman_get_bustag(r)    ((r)->r_tag)
#define rman_get_bushandle(r) ((r)->r_handle)
#define rman_get_start(r)     ((r)->r_start)
#define rman_get_size(r)      ((r)->r_len)
#define rman_get_rid(r)       ((r)->r_rid)
#endif
