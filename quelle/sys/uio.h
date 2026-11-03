#ifndef _SYS_UIO_H_
#define _SYS_UIO_H_
#include <sys/types.h>
struct uio { int uio_resid; int uio_rw; };
enum uio_rw { UIO_READ, UIO_WRITE };
#endif
