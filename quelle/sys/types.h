/*  sys/types.h -- Grundtypen.  NT bringt sie alle mit, nur anders
 *  geschrieben.  Nichts hier ist eine Entscheidung; es ist eine Liste.  */
#ifndef _SYS_TYPES_H_
#define _SYS_TYPES_H_
#include <nt/ntunterlage.h>

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;

/*  Grenzwerte wie in <stdint.h>.  dev/hid/hid.c braucht INT32_MAX.  */
#ifndef INT32_MAX
#define INT8_MIN    (-0x7f - 1)
#define INT8_MAX    0x7f
#define INT16_MIN   (-0x7fff - 1)
#define INT16_MAX   0x7fff
#define INT32_MIN   (-0x7fffffff - 1)
#define INT32_MAX   0x7fffffff
#define UINT8_MAX   0xff
#define UINT16_MAX  0xffff
#define UINT32_MAX  0xffffffffU
#endif
typedef unsigned int       uint32_t;
typedef __int64            int64_t;
typedef unsigned __int64   uint64_t;

typedef uint8_t            u_int8_t;
typedef uint16_t           u_int16_t;
typedef uint32_t           u_int32_t;
typedef uint64_t           u_int64_t;
typedef unsigned char      u_char;
typedef unsigned short     u_short;
typedef unsigned int       u_int;
typedef unsigned long      u_long;

typedef int                ssize_t;
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned int       size_t;
#endif
#ifndef _UINTPTR_T_DEFINED   /* freie DDK-Koepfe (mingw-w64) bringen sie schon mit */
#define _UINTPTR_T_DEFINED
typedef ULONG_PTR          uintptr_t;
#endif
#ifndef _INTPTR_T_DEFINED
#define _INTPTR_T_DEFINED
typedef LONG_PTR           intptr_t;
#endif
typedef uint64_t           off_t;
typedef char              *caddr_t;
typedef unsigned int       uid_t;
typedef unsigned int       gid_t;
typedef unsigned int       pid_t;
typedef unsigned long      u_quad_t;

/*  bool: C89 kennt es nicht, und der NT-4.0-Baum uebersetzt als C89.
 *  int, nicht char -- FreeBSD weist Ausdruecke zu, die mehr als 0/1
 *  liefern koennen.  */
typedef int                bool;
#define true  1
#define false 0

/*  bus_addr_t bleibt 32 Bit.  Das ist keine Sparsamkeit, sondern die
 *  Falle aus dem Skill: 64-Bit-Arithmetik ruft __allmul, und das gibt es
 *  unter /nodefaultlib nicht.  Alle gemeinsamen Puffer liegen unter 4 GB,
 *  bei xHCI wird die obere Haelfte als Null geschrieben.  */
typedef uint32_t           bus_addr_t;
typedef uint32_t           bus_size_t;

typedef ntu_space_tag      bus_space_tag_t;
typedef ntu_space_handle   bus_space_handle_t;
typedef ntu_dma_tag_t      bus_dma_tag_t;
typedef ntu_dma_map_t      bus_dmamap_t;
typedef ntu_device_t       device_t;

#ifndef NULL
#define NULL ((void *)0)
#endif
#endif
