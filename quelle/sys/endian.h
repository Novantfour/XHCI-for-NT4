/*  sys/endian.h -- x86 ist little endian, also sind die le*-Formen
 *  Nullkosten und die be*-Formen ein Byteschub.  Kein Aufruf, keine
 *  64-Bit-Arithmetik: nur Schieben und Odern.  */
#ifndef _SYS_ENDIAN_H_
#define _SYS_ENDIAN_H_
#include <sys/types.h>
#define _BYTE_ORDER     1234
#define _LITTLE_ENDIAN  1234
#define _BIG_ENDIAN     4321
#define BYTE_ORDER      _BYTE_ORDER

#define htole16(x)  ((uint16_t)(x))
#define htole32(x)  ((uint32_t)(x))
#define htole64(x)  ((uint64_t)(x))
#define le16toh(x)  ((uint16_t)(x))
#define le32toh(x)  ((uint32_t)(x))
#define le64toh(x)  ((uint64_t)(x))

#define bswap16(x)  ((uint16_t)((((x) & 0xffu) << 8) | (((x) >> 8) & 0xffu)))
#define bswap32(x)  ((uint32_t)((((x) & 0xffu) << 24) | (((x) & 0xff00u) << 8) | \
                                (((x) >> 8) & 0xff00u) | (((x) >> 24) & 0xffu)))
#define htobe16(x)  bswap16(x)
#define htobe32(x)  bswap32(x)
#define be16toh(x)  bswap16(x)
#define be32toh(x)  bswap32(x)
#endif
