#ifndef _STRING_H
#define _STRING_H

#include "stddef.h"

/* 内存操作 */
void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);

/* 字符串操作 */
size_t strlen(const char* s);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strcat(char* dst, const char* src);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr(const char* hay, const char* needle);
char*  strdup(const char* s);
char*  strtok(char* s, const char* delims);

/* 反向版本：src 在前，方便写 strcmp("literal", var) 这种比较 */
int    strcasecmp(const char* a, const char* b);

#endif /* _STRING_H */
