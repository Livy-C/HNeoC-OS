#include "../include/string.h"

/* 内存设置 */
void* memset(void* dest, int val, size_t count) {
    unsigned char* d = (unsigned char*)dest;
    while (count--) {
        *d++ = (unsigned char)val;
    }
    return dest;
}

/* 内存复制 */
void* memcpy(void* dest, const void* src, size_t count) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (count--) {
        *d++ = *s++;
    }
    return dest;
}

/* 内存比较 */
int memcmp(const void* s1, const void* s2, size_t count) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;

    while (count--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

/* 字符串长度 */
size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

/* 字符串复制 */
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

/* 限定长度的字符串复制，并保证以 '\0' 结尾 */
char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;

    for (; i + 1 < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    if (n > 0) {
        dest[i] = '\0';
    }
    return dest;
}

/* 字符串比较 */
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/* 限定长度的字符串比较 */
int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/* 判断是否为空白字符 */
bool isspace(char c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

/* 判断是否为数字 */
bool isdigit(char c) {
    return (c >= '0' && c <= '9');
}

/* 追加字符串 */
char* strcat(char* dest, const char* src) {
    char* d = dest + strlen(dest);

    while ((*d++ = *src++)) {
        /* 空体 */
    }
    return dest;
}

/* 最多追加 n 个字符，并保证以 0 结尾 */
char* strncat(char* dest, const char* src, size_t n) {
    char* d = dest + strlen(dest);

    while (n-- && *src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

/* 找第一个出现的字符，找不到返回 NULL */
char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char*)s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}

/* 找最后一个出现的字符 */
char* strrchr(const char* s, int c) {
    const char* last = NULL;

    for (;; s++) {
        if (*s == (char)c) {
            last = s;
        }
        if (*s == '\0') {
            break;
        }
    }
    return (char*)last;
}