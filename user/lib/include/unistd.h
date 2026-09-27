#ifndef _UNISTD_H
#define _UNISTD_H

#include "stddef.h"

/* ============================================================
 * 文件描述符 I/O
 *
 * fd 0/1/2 固定映射到控制台，其余由 open() 分配。
 * 出错时返回负数。
 * ============================================================ */

int  open(const char* name, int flags);
int  close(int fd);
int  read(int fd, void* buf, size_t len);
int  write(int fd, const void* buf, size_t len);
int  lseek(int fd, int offset, int whence);
int  unlink(const char* name);
int  fstat(int fd);          /* 返回文件大小 */
int  fsync(int fd);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* 一次性读整个文件（方便，但会一次性占用整个文件大小的内存） */
int  readfile(const char* name, char* buf, size_t max);

/* 遍历目录。path 可以是 "/" 或 "/bin" 之类；index 从 0 开始。
 * 文件名写进 name 缓冲区。
 * 返回值：文件大小；如果是子目录，最高位（DIR_MARK）为 1。
 * 已经没有更多条目时返回 -1。
 */
int  listdir(const char* path, int index, char* name, size_t max);

/* bit30：不能用最高位。置上 bit31 返回值就成了负数，会和 -1（没有更多条目）撞车，
 * 目录遍历会在遇到第一个子目录时静默结束。必须与内核的 SYS_DIR_MARK 保持一致。
 */
#define DIR_MARK 0x40000000

/* 判断 listdir 的返回值是不是目录 */
#define IS_DIR(result) (((result) >= 0) && (((unsigned)(result)) & DIR_MARK))
/* 去掉目录标记，取出大小 */
#define DIR_SIZE(result) ((int)(((unsigned)(result)) & ~DIR_MARK))

/* 创建目录。成功返回 0，失败返回负的错误码 */
int  mkdir(const char* path);

/* 取命令行参数（命令名之后的那段文本）。返回长度 */
int  getargs(char* buf, size_t max);

/* 取当前工作目录，写进 buf（含结尾的 '\0'），成功返回 buf，失败返回 NULL。
 * Shell 的提示符是内核拼的，所以程序想知道自己在哪个目录，只能问这个。
 */
char* getcwd(char* buf, size_t max);

/* ============================================================
 * 时间与进程
 * ============================================================ */
unsigned int uptime(void);    /* 开机后经过的秒数 */
unsigned int ticks(void);     /* 定时器滴答数（100Hz） */
void sleep_ms(unsigned int ms);
int  getpid(void);
void yield(void);

/* ============================================================
 * 终端输入
 * ============================================================ */

/* 读一个按键，含功能键（返回下面的 KEY_* 常量） */
int getkey(void);

/* 只读普通字符，功能键会被跳过 */
int getchar(void);

#define KEY_UP        0x101
#define KEY_DOWN      0x102
#define KEY_LEFT      0x103
#define KEY_RIGHT     0x104
#define KEY_PAGEUP    0x105
#define KEY_PAGEDOWN  0x106
#define KEY_HOME      0x107
#define KEY_END       0x108
#define KEY_DELETE    0x109

#define KEY_ESC       27
#define KEY_ENTER     13
#define KEY_TAB       9
#define KEY_BACKSPACE 8

/* ============================================================
 * 内存
 * ============================================================ */

/* 把堆顶往上推 increment 字节，返回原来的堆顶；失败返回 (void*)0。
 * malloc 就建在它上面，一般不用直接调。
 */
void* sbrk(int increment);

#endif /* _UNISTD_H */
