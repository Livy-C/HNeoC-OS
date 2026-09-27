#ifndef _STDLIB_H
#define _STDLIB_H

#include "stddef.h"

/* 内存分配：建在 sbrk 之上 */
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
void  free(void* ptr);

/* 数字转换 */
int    atoi(const char* s);
long   atol(const char* s);

/* 进程 */
void exit(int code) __attribute__((noreturn));
int  abs(int v);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif /* _STDLIB_H */
