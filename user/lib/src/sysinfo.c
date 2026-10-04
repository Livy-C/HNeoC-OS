#include <sysinfo.h>
#include <hneoc.h>

/* ============================================================
 * 内核状态快照与登录的薄封装
 *
 * 这一层和 unistd.c 一样：只把参数摆到寄存器里调 int 0x80，
 * 出错时原样透传内核的负返回值。真正的加工在 sh.c / taskmgr 那边。
 *
 * 三个"结构体进结构体出"的调用（get_sysinfo / gettask / login_user）
 * 都是内核把一整块内存逐字节搬过来，所以结构体布局必须和内核
 * include/sysinfo.h 完全一致 —— 那个文件顶端有说明。
 * ============================================================ */

int get_sysinfo(sysinfo_t* out) {
    if (!out) {
        return -1;
    }
    return (int)__syscall1(SYS_SYSINFO, (int32_t)out);
}

int gettask(int index, taskinfo_t* out) {
    if (!out || index < 0) {
        return -1;
    }
    return (int)__syscall2(SYS_GETTASK, index, (int32_t)out);
}

int login_user(const char* name, const char* password, logininfo_t* info) {
    return (int)__syscall3(SYS_LOGIN, (int32_t)name, (int32_t)password,
                           (int32_t)info);
}

int useradd(const char* name, const char* password, int admin) {
    return (int)__syscall3(SYS_USERADD, (int32_t)name, (int32_t)password,
                           admin);
}
