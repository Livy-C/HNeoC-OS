#include <unistd.h>
#include <hneoc.h>

/* ============================================================
 * 系统调用的薄封装
 *
 * 这一层不做任何缓冲和加工，只是把参数摆到寄存器里调 int 0x80。
 * 缓冲在 stdio / term 那两层做。
 *
 * 约定：内核出错时返回负值，这里原样透传，不做 errno 转换。
 * ============================================================ */

/* ---- 文件 ---- */

int open(const char* name, int flags) {
    return (int)__syscall2(SYS_OPEN, (int32_t)name, flags);
}

int close(int fd) {
    return (int)__syscall1(SYS_CLOSE, fd);
}

int read(int fd, void* buf, size_t len) {
    return (int)__syscall3(SYS_READ, fd, (int32_t)buf, (int32_t)len);
}

int write(int fd, const void* buf, size_t len) {
    return (int)__syscall3(SYS_WRITE, fd, (int32_t)buf, (int32_t)len);
}

int lseek(int fd, int offset, int whence) {
    return (int)__syscall3(SYS_LSEEK, fd, offset, whence);
}

int unlink(const char* name) {
    return (int)__syscall1(SYS_UNLINK, (int32_t)name);
}

int fstat(int fd) {
    return (int)__syscall1(SYS_FSTAT, fd);
}

int fsync(int fd) {
    return (int)__syscall1(SYS_FSYNC, fd);
}

int readfile(const char* name, char* buf, size_t max) {
    return (int)__syscall3(SYS_READFILE, (int32_t)name, (int32_t)buf,
                           (int32_t)max);
}

int listdir(const char* path, int index, char* name, size_t max) {
    return (int)__syscall4(SYS_LISTDIR, (int32_t)path, index,
                           (int32_t)name, (int32_t)max);
}

int mkdir(const char* path) {
    return (int)__syscall1(SYS_MKDIR, (int32_t)path);
}

int getargs(char* buf, size_t max) {
    return (int)__syscall2(SYS_GETARGS, (int32_t)buf, (int32_t)max);
}

char* getcwd(char* buf, size_t max) {
    int r;

    if (!buf || max == 0) {
        return NULL;
    }
    r = (int)__syscall2(SYS_GETCWD, (int32_t)buf, (int32_t)max);
    if (r < 0) {
        buf[0] = '\0';
        return NULL;
    }
    return buf;
}

/* ---- 时间与进程 ---- */

unsigned int uptime(void) {
    return (unsigned int)__syscall0(SYS_UPTIME);
}

unsigned int ticks(void) {
    return (unsigned int)__syscall0(SYS_TICKS);
}

void sleep_ms(unsigned int ms) {
    (void)__syscall1(SYS_SLEEP, (int32_t)ms);
}

int getpid(void) {
    return (int)__syscall0(SYS_GETPID);
}

void yield(void) {
    (void)__syscall0(SYS_YIELD);
}

/* ---- 终端输入 ---- */

int getkey(void) {
    return (int)__syscall0(SYS_GETKEY);
}

int getchar(void) {
    return (int)__syscall0(SYS_GETCHAR);
}

/* ---- 内存 ---- */

void* sbrk(int increment) {
    int32_t r = __syscall1(SYS_SBRK, increment);

    return (r < 0) ? NULL : (void*)r;
}
