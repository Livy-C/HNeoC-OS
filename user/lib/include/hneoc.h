#ifndef _HNEOC_H
#define _HNEOC_H

/* ============================================================
 * HNeoC OS 用户�?C API
 *
 * 方便头：一次把常用的都拉进来。新代码也可以只包含自己需要的
 * 标准头（<stdio.h> <unistd.h> <term.h> ...）�? *
 * 分三层：
 *   1. 最底下是系统调用号 + __syscallN 原语（本文件末尾�? *   2. 中间是标准命名的库：stdio / string / stdlib / ctype / unistd / term
 *   3. 最上面是一小撮"简�?，早期程序用的，现在仍然保留
 *
 * 写程序时优先用第 2 层；只有标准接口没覆盖到的能力才直接用第 1 层�? * ============================================================ */

#include "stdint.h"
#include "stddef.h"
#include "stdbool.h"
#include "errno.h"
#include "fcntl.h"
#include "string.h"
#include "ctype.h"
#include "stdlib.h"
#include "stdio.h"
#include "unistd.h"
#include "term.h"

/* ============================================================
 * 系统调用�? *
 * 必须和内�?include/syscall.h 一一对应
 * ============================================================ */
#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_GETCHAR   2
#define SYS_GETPID    3
#define SYS_YIELD     4
#define SYS_SLEEP     5
#define SYS_UPTIME    6
#define SYS_TICKS     7
#define SYS_COLOR     8
#define SYS_CLEAR     9
#define SYS_READFILE  10
#define SYS_LISTDIR   11
#define SYS_OPEN      12
#define SYS_CLOSE     13
#define SYS_READ      14
#define SYS_LSEEK     15
#define SYS_UNLINK    16
#define SYS_FSTAT     17
#define SYS_FSYNC     18
#define SYS_GETKEY    19
#define SYS_WINSIZE   20
#define SYS_SBRK      21
#define SYS_MKDIR     22
#define SYS_GETARGS   23

/* ============================================================
 * 系统调用原语
 *
 * 约定：EAX = 调用号，EBX/ECX/EDX = 参数，返回值在 EAX�? * �?-fno-pic 编译，所�?EBX 是普通寄存器，可以直接用 "b" 约束�? * ============================================================ */
static inline int32_t __syscall0(int32_t n) {
    int32_t r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}

static inline int32_t __syscall1(int32_t n, int32_t a) {
    int32_t r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}

static inline int32_t __syscall2(int32_t n, int32_t a, int32_t b) {
    int32_t r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r) : "a"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static inline int32_t __syscall3(int32_t n, int32_t a, int32_t b, int32_t c) {
    int32_t r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/* 四个参数：第四个走 ESI（内核的 registers_t 里有这个字段） */
static inline int32_t __syscall4(int32_t n, int32_t a, int32_t b,
                                 int32_t c, int32_t d) {
    int32_t r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
                         : "memory");
    return r;
}

/* ============================================================
 * 兼容简�? *
 * 早期程序里到处是 put_int / str_len / sleep_ms 这些名字�? * 现在它们只是标准接口的马甲，新代码请直接�?printf / strlen�? * ============================================================ */

#define str_len(s)  strlen(s)
#define str_eq(a, b) (strcmp((a), (b)) == 0)

/* 颜色旧名：现在统一�?TERM_*，这里保留短名字 */
#define C_BLACK         TERM_BLACK
#define C_BLUE          TERM_BLUE
#define C_GREEN         TERM_GREEN
#define C_CYAN          TERM_CYAN
#define C_RED           TERM_RED
#define C_MAGENTA       TERM_MAGENTA
#define C_BROWN         TERM_BROWN
#define C_LGREY         TERM_LIGHT_GREY
#define C_DGREY         TERM_DARK_GREY
#define C_LBLUE         TERM_LIGHT_BLUE
#define C_LGREEN        TERM_LIGHT_GREEN
#define C_LCYAN         TERM_LIGHT_CYAN
#define C_LRED          TERM_LIGHT_RED
#define C_LMAGENTA      TERM_LIGHT_MAGENTA
#define C_YELLOW        TERM_YELLOW
#define C_WHITE         TERM_WHITE

/* 这几个简写故意不�?printf�? * 走了的话，任何只想打一个数字的小程序都会把整个格式化器链进来，
 * 体积从几百字节涨到好�?KB�?-gc-sections 也救不了 —�? * 一旦引用了 printf 就算"用到�?�? */
static inline void put_uint(uint32_t v) {
    char tmp[11];
    int n = 0;

    if (v == 0) {
        putchar('0');
        return;
    }
    while (v > 0 && n < 10) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        putchar(tmp[--n]);
    }
}

static inline void put_int(int32_t v) {
    if (v < 0) {
        putchar('-');
        put_uint((uint32_t)(-v));
    } else {
        put_uint((uint32_t)v);
    }
}

static inline void put_hex(uint32_t v) {
    const char* d = "0123456789ABCDEF";

    fputs("0x", STDOUT_FILENO);
    for (int i = 7; i >= 0; i--) {
        putchar(d[(v >> (i * 4)) & 0xF]);
    }
}

static inline void putln(const char* s) { puts(s); }

/* 屏幕尺寸打包 / 拆包（早期接口，新代码用 term_size�?*/
static inline int getwinsize(void) {
    return (int)__syscall0(SYS_WINSIZE);
}
static inline int winsize_rows(int packed) { return (packed >> 8) & 0xFF; }
static inline int winsize_cols(int packed) { return packed & 0xFF; }

static inline void set_color(int fg, int bg) { term_set_color(fg, bg); }
static inline void clear_screen(void)        { term_clear(); }

#endif /* _HNEOC_H */
