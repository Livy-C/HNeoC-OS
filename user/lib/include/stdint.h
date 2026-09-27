#ifndef _STDINT_H
#define _STDINT_H

/* HNeoC OS 用户态固定宽度整数类型。
 * 目标平台是 32 位 x86，int 和 long 都是 4 字节。 */

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

typedef int32_t  intptr_t;
typedef uint32_t uintptr_t;

#define INT32_MAX  2147483647
#define INT32_MIN  (-2147483647 - 1)
#define UINT32_MAX 4294967295u

#endif /* _STDINT_H */
