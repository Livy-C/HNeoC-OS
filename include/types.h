#ifndef TYPES_H
#define TYPES_H

// 基础类型定义
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint32_t size_t;
typedef int32_t  ssize_t;

// 布尔类型 - 使用C99/C11标准定义
#define bool _Bool
#define false 0
#define true 1
#define __bool_true_false_are_defined 1

// NULL定义
#ifndef NULL
#define NULL ((void*)0)
#endif

#endif // TYPES_H
