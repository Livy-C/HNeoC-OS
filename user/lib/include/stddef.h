#ifndef _STDDEF_H
#define _STDDEF_H

#include "stdint.h"

typedef uint32_t size_t;
typedef int32_t  ssize_t;
typedef int32_t  ptrdiff_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define offsetof(type, member) ((size_t)&(((type*)0)->member))

#endif /* _STDDEF_H */
