#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------
 * 内存操作
 * ------------------------------------------------------------ */

void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;

    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* 和 memcpy 的区别是允许区间重叠：从后往前拷 */
void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;

    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;

    while (n--) {
        if (*p == (uint8_t)c) {
            return (void*)p;
        }
        p++;
    }
    return NULL;
}

/* ------------------------------------------------------------
 * 字符串
 * ------------------------------------------------------------ */

size_t strlen(const char* s) {
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;

    while ((*d++ = *src++)) {
        /* 空体 */
    }
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;

    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    /* 标准要求剩余部分补 0 */
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char* strcat(char* dst, const char* src) {
    char* d = dst + strlen(dst);

    while ((*d++ = *src++)) {
        /* 空体 */
    }
    return dst;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a - 'A' + 'a' : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b - 'A' + 'a' : *b;

        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

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

char* strstr(const char* hay, const char* needle) {
    size_t n = strlen(needle);

    if (n == 0) {
        return (char*)hay;
    }
    for (; *hay; hay++) {
        if (*hay == *needle && strncmp(hay, needle, n) == 0) {
            return (char*)hay;
        }
    }
    return NULL;
}

char* strdup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);

    if (!p) {
        return NULL;
    }
    memcpy(p, s, n + 1);
    return p;
}

/* 就地切分。s 为 NULL 时接着上一次的位置继续 */
char* strtok(char* s, const char* delims) {
    static char* next = NULL;
    char* start;

    if (s) {
        next = s;
    }
    if (!next) {
        return NULL;
    }

    /* 跳过开头的一串分隔符 */
    while (*next && strchr(delims, *next)) {
        next++;
    }
    if (*next == '\0') {
        next = NULL;
        return NULL;
    }

    start = next;

    /* 找到这个词的结尾 */
    while (*next && !strchr(delims, *next)) {
        next++;
    }
    if (*next) {
        *next++ = '\0';
    } else {
        next = NULL;
    }
    return start;
}
