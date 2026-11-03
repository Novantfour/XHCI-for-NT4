#ifndef _SYS_ERRNO_H_
#define _SYS_ERRNO_H_
#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define EBADF 9
#define EAGAIN 35
#define EWOULDBLOCK EAGAIN
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define EINVAL 22
#define EINPROGRESS 36
#define ERESTART (-1)
#define ENOIOCTL (-6)
#define ENOTTY 25
#define EWOULDBLOCK_DUP EAGAIN
#define ENOSPC 28
#define EPIPE 32
#define ENOTSUP 45
#define ETIMEDOUT 60
#define ECANCELED 85
#define EOPNOTSUPP ENOTSUP
#endif
