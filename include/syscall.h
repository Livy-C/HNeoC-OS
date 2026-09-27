#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "idt.h"

/* ============================================================
 * 系统调用
 *
 * 用户程序用 int 0x80 陷入内核，调用号放在 EAX，参数依次放在
 * EBX、ECX、EDX、ESI、EDI，返回值通过 EAX 带回。
 *
 * 之所以用 trap gate 而不是 interrupt gate：trap gate 不会清 IF，
 * 系统调用执行期间时钟和键盘中断照常到达，getchar 才能阻塞等待。
 * ============================================================ */

#define SYS_EXIT      0    /* exit(code) */
#define SYS_WRITE     1    /* write(fd, buf, len) -> 实际写出的字节数 */
#define SYS_GETCHAR   2    /* getchar() -> 字符，阻塞 */
#define SYS_GETPID    3    /* getpid() -> pid */
#define SYS_YIELD     4    /* yield() */
#define SYS_SLEEP     5    /* sleep(milliseconds) */
#define SYS_UPTIME    6    /* uptime_seconds() */
#define SYS_TICKS     7    /* timer ticks since boot */
#define SYS_COLOR     8    /* set_color(fg, bg) */
#define SYS_CLEAR     9    /* clear_screen() */
#define SYS_READFILE  10   /* readfile(path, buf, max) -> 字节数，-1 表示失败 */
#define SYS_LISTDIR   11   /* listdir(path, index, namebuf, maxname) -> 大小|DIR_MARK */

/* 文件描述符相关。0/1/2 固定是控制台 */
#define SYS_OPEN      12   /* open(name, flags) -> fd */
#define SYS_CLOSE     13   /* close(fd) -> 0 */
#define SYS_READ      14   /* read(fd, buf, len) -> 字节数 */
#define SYS_LSEEK     15   /* lseek(fd, offset, whence) -> 新位置 */
#define SYS_UNLINK    16   /* unlink(name) -> 0 */
#define SYS_FSTAT     17   /* fstat(fd) -> 文件大小 */
#define SYS_FSYNC     18   /* fsync(fd) -> 0，把文件表刷回磁盘 */

/* 全屏程序需要的几个 */
#define SYS_GETKEY    19   /* getkey() -> 原始按键码，含方向键等 0x101+ */
#define SYS_WINSIZE   20   /* winsize() -> (rows << 8) | cols */
#define SYS_SBRK      21   /* sbrk(n) -> 原来的堆顶，失败返回 -1 */

/* 目录与参数 */
#define SYS_MKDIR     22   /* mkdir(path) -> 0，失败返回负的错误码 */
#define SYS_GETARGS   23   /* getargs(buf, max) -> 拷贝出来的字节数 */

#define SYS_COUNT     24

/* 目录遍历的返回值里，用 bit30 表示"这是个目录"。
 *
 * 这里不能用最高位（bit31）：返回值是 int32_t，置上 bit31 就成了负数，
 * 而 -1 已经被约定为"没有更多条目了"，调用方（如 user/ls.c 的
 * `if (r < 0) break;`）会把这个标记当成目录结束，于是一个目录只要第一个
 * 子项是子目录就一条都列不出来 —— 根目录必然如此，因为 mkfs 是按
 * "目录在前"的顺序打包目录项的。
 * 文件大小远小于 0x40000000，所以这个位同样空着不用。
 */
#define SYS_DIR_MARK  0x40000000u

/* 标准输出文件描述符 */
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* 注册 0x80 号中断门（DPL=3）并挂上处理函数 */
void syscall_init(void);

/* 中断总入口转发过来的系统调用处理函数 */
void syscall_handler(registers_t* regs);

/* 累计的系统调用次数，供 taskmgr 展示 */
uint32_t syscall_total(void);

#endif /* SYSCALL_H */
