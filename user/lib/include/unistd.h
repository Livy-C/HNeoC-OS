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

/* 改权限位（只取低 9 位：属主三位 + 其他人三位）。
 * 只有 root / 管理员 / 文件属主能改，其它情况返回负的错误码。
 * 装包时靠它给 .lxe 补执行位。
 */
int  chmod(const char* path, int mode);

/* 我是谁：getuid() 是当前进程的 uid（0 是 root），
 * is_admin() 表示这个账户能不能碰别人的东西（uid 0 或 /etc/passwd 里 admin=1）。
 * 内核用一个 whoami 调用把两个信息一起带回来。
 */
int  getuid(void);
int  is_admin(void);

/* ============================================================
 * 时间与进程
 * ============================================================ */
unsigned int uptime(void);    /* 开机后经过的秒数 */
unsigned int ticks(void);     /* 定时器滴答数（100Hz） */
void sleep_ms(unsigned int ms);
int  getpid(void);
void yield(void);

/* ============================================================
 * fork / exec / wait
 * ============================================================ */

/* 复制当前进程。
 *
 * 子进程里返回 0，父进程里返回子进程的 pid，失败返回 -1。
 * 两边的内存是各一份拷贝：父子各改各的变量，互不影响。
 *
 * 只有用户态程序能 fork —— 内核态任务没有用户地址空间可复制。
 */
int fork(void);

/* 用 path 这个可执行文件覆盖当前进程，不新建进程。
 *
 * 成功时**不返回** —— 调用它的那段代码再也不会执行了，控制权直接
 * 落到新程序的入口。所以"exec 失败了"是唯一能回来的情况，此时返回
 * 负的错误码，当前进程原样继续跑。
 *
 * args 是命令行参数（命令名之后的那段文本，和 getargs 拿到的是同一种），
 * 传 NULL 表示没有参数。
 */
int exec(const char* path, const char* args);

/* 等一个子进程退出并回收它，退出码写进 *status（可以为 NULL）。
 *
 * pid > 0 等指定那个孩子；pid <= 0 等任意一个先退出的孩子。
 * options 里带 WNOHANG（见 sysinfo.h）时不阻塞：手头没有已经退出的
 * 孩子就立刻返回 0 —— 用户态 Shell 靠它顺手收掉后台任务的僵尸。
 * 返回被回收的子进程 pid；没有这样的孩子返回 -1。
 *
 * 必须是这个进程自己 fork 出来的孩子才能等 —— 别人家的孩子等不到。
 */
int waitpid(int pid, int* status, int options);

/* waitpid(-1, status, 0) 的简写 */
int wait(int* status);

/* 换当前工作目录。成功返回 0，失败返回负的错误码。
 * 和内核 Shell 的 cd 走的是同一条路径解析。 */
int chdir(const char* path);

/* 结束一个进程。返回 0 成功，负数是各种拒绝原因
 * （-1 没有这个 pid、-2 那是内核 Shell、-3 不能杀自己、-4 已经退出）。
 * 只能杀自己的子进程或者（管理员）别人的进程。 */
int kill(int pid);

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
