#ifndef _STDIO_H
#define _STDIO_H

#include "stddef.h"

/* 没有 FILE 结构体：fd 0/1/2 就是标准流，其余都是裸 fd。
 * printf 直接写 fd 1。
 */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define EOF (-1)

/* 字符输出 */
int putchar(int c);
int puts(const char* s);
int fputc(int c, int fd);
int fputs(const char* s, int fd);

/* 字符输入 */
int getchar(void);

/* 格式化：printf 走 stdout，snprintf 写进缓冲区 */
int printf(const char* fmt, ...);
int fprintf(int fd, const char* fmt, ...);
int snprintf(char* buf, size_t max, const char* fmt, ...);
int vsnprintf(char* buf, size_t max, const char* fmt, void* args);

/* 反显 / 颜色这些也常被当"打印"用，见 term.h */

#endif /* _STDIO_H */
