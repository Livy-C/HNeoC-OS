#ifndef SYSINFO_H
#define SYSINFO_H

#include "types.h"

/* ============================================================
 * 内核状态快照
 *
 * Shell 从内核搬到 ring 3 之后，原来那些"直接读内核变量"的内置命令
 * （mem / heap / fsstat / diskinfo / taskmgr）就没有这条路了，得由内核
 * 把数据递出来。这里就是递出来的那几种结构。
 *
 * **user/lib/include/sysinfo.h 是本文件的一份副本，必须逐字节一致** ——
 * SYS_SYSINFO / SYS_GETTASK 是直接把整块结构 memcpy 进用户缓冲区的，
 * 两边字段对不上就会读到垃圾。改了这里就得改那边。
 *
 * 只用 uint32_t 是有意的：两边一个是 types.h、一个是 stdint.h，
 * 固定宽度类型是两边唯一都保证的定义。
 * ============================================================ */

#define SYSINFO_NAME_MAX  16

/* 任务状态。数值必须和内核的 proc_state_t 一一对应 */
#define TASK_STATE_UNUSED   0
#define TASK_STATE_READY    1
#define TASK_STATE_RUNNING  2
#define TASK_STATE_ZOMBIE   3

typedef struct {
    uint32_t pid;
    uint32_t ppid;           /* 父进程 pid；0 表示内核 Shell */
    uint32_t state;          /* TASK_STATE_* */
    uint32_t cpu_ticks;      /* 被调度到的时钟滴答数 */
    uint32_t syscalls;       /* 这个进程发起过的系统调用次数 */
    uint32_t memory;         /* 映像 + bss + 用户栈的字节数 */
    uint32_t start_tick;
    uint32_t is_user;        /* 1 = ring 3 任务，0 = 内核态 */
    char     name[SYSINFO_NAME_MAX];
} taskinfo_t;

typedef struct {
    /* 物理内存（KB） */
    uint32_t mem_total_kb;
    uint32_t mem_used_kb;
    uint32_t mem_free_kb;

    /* 内核堆（字节） */
    uint32_t heap_start;
    uint32_t heap_total;
    uint32_t heap_used;
    uint32_t heap_free;
    uint32_t heap_blocks;

    /* 时间与系统调用 */
    uint32_t uptime_sec;
    uint32_t ticks;
    uint32_t tick_hz;
    uint32_t syscalls;

    /* 进程 */
    uint32_t procs_live;
    uint32_t procs_total;

    /* 磁盘与文件系统 */
    uint32_t disk_sectors;
    uint32_t disk_reads;
    uint32_t disk_writes;
    uint32_t fs_entries;     /* 文件表高水位（已用到的最大下标 + 1） */
    uint32_t fs_live;        /* 真正有效的目录项个数 */
    uint32_t fs_data_lba;
    uint32_t fs_free_lba;
    uint32_t fs_free_bytes;

    /* 屏幕 */
    uint32_t screen_rows;
    uint32_t screen_cols;
} sysinfo_t;

/* SYS_WAIT 的选项 */
#define WNOHANG  1   /* 没有已退出的孩子就立刻返回 0，不阻塞 */

/* SYS_LOGIN 回填的信息 */
typedef struct {
    uint32_t uid;
    uint32_t admin;
    char     name[SYSINFO_NAME_MAX];
    char     home[64];
} logininfo_t;

#define LOGIN_NAME_MAX  16
#define LOGIN_HOME_MAX  64

#endif /* SYSINFO_H */
