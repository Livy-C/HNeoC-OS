#ifndef STRING_H
#define STRING_H

#include "types.h"

/* 内存操作 */
void* memset(void* dest, int val, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
int memcmp(const void* s1, const void* s2, size_t count);

/* 字符串操作 */
size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
int strncmp(const char* s1, const char* s2, size_t n);

/* 判断 c 是否为空白字符 */
bool isspace(char c);
/* 判断 c 是否为十进制数字 */
bool isdigit(char c);

#endif /* STRING_H */
