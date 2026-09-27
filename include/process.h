#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "idt.h"

/* ============================================================
 * 进程与调度器
 *
 * 每个进程拥有：
 *   - 一张自己的页目录（内核映射从主目录复制，用户区挂自己的页表）
 *   - 一段用户地址空间：映像从 USER_BASE 往上，栈从 4MB 边界往下
 *   - 一个独立的内核栈。用户态陷入内核（中断或系统调用）时，
 *     CPU 会按 TSS.esp0 切到这个栈上，不会踩坏别人的栈。
 *
 * 调度是时钟中断驱动的抢占式轮转：isr_handler 末尾调用 sched_tick，
 * 它返回下一个任务的内核栈指针，汇编存根把 esp 一换，
 * 后面的 popa / iret 弹出的就是那个任务的现场。
 *
 * 进程 0 是 Shell 自己，跑在内核态，充当空闲任务。
 * ============================================================ */

#define PROCESS_NAME_MAX   16
#define PROCESS_MAX        16
#define PROCESS_KERNEL_STACK_SIZE  (8 * 1024)
#define PROCESS_MAX_IMAGE_PAGES    768          /* 3MB 上限 */
#define PROCESS_STACK_PAGES        4            /* 16KB 用户栈 */
#define PROCESS_NAME_FIELD         16
#define PROCESS_MAX_FDS            16
#define PROCESS_ARGS_MAX           128

/* 文件描述符 0/1/2 固定映射到控制台，file_index 用这个哨兵值表示 */
#define FD_CONSOLE 0xFFFFFFFFu

/* 一个打开的文件 */
typedef struct {
    bool     used;
    uint32_t file_index;   /* HNeoFS 目录项下标；FD_CONSOLE 表示控制台 */
    uint32_t offset;       /* 当前读写位置 */
    uint32_t flags;        /* HNEOFS_O_* */
} fd_entry_t;

typedef enum {
    PROC_UNUSED = 0,   /* 槽位空闲 */
    PROC_READY,        /* 可运行，等待被调度 */
    PROC_RUNNING,      /* 正在 CPU 上 */
    PROC_ZOMBIE        /* 已退出，等待回收 */
} proc_state_t;

typedef struct {
    uint32_t pid;
    char     name[PROCESS_NAME_MAX];
    proc_state_t state;
    uint32_t index;              /* 在进程表里的下标 */

    /* --- 调度现场 --- */
    uint32_t  esp;               /* 保存的内核栈指针（指向中断帧里的 ds 槽） */
    uint8_t*  kernel_stack;      /* 内核栈基址；Shell 用的是引导栈，为 NULL */
    uint32_t  kernel_stack_top;
    uint32_t  kernel_stack_size;
    bool      is_user;           /* true = ring 3 任务 */

    /* --- 地址空间 --- */
    uint32_t  page_directory;    /* 页目录物理地址 */
    uint32_t* user_table;        /* 用户区页表 */
    uint32_t  image_frames[PROCESS_MAX_IMAGE_PAGES];  /* 映像页 + 堆页共用一张表 */
    uint32_t  image_pages;
    uint32_t  heap_pages;        /* 堆占的页数，紧跟在映像后面 */
    uint32_t  stack_frames[PROCESS_STACK_PAGES];
    uint32_t  stack_pages;

    uint32_t  heap_start;        /* 用户堆起点（映像末尾，页对齐） */
    uint32_t  heap_end;          /* 当前堆顶，sbrk 往上推 */
    uint32_t  user_stack_base;   /* 用户栈最低地址，堆不能越过它 */

    uint32_t  entry;             /* 用户态入口虚拟地址 */
    uint32_t  user_stack_top;    /* 用户栈顶虚拟地址 */

    /* --- 打开的文件 --- */
    fd_entry_t fds[PROCESS_MAX_FDS];

    /* --- 命令行参数 ---
     * 加载程序的人（内核 Shell，将来是用户态 Shell）把命令名之后的
     * 那段文本存这儿，程序自己通过 getargs 系统调用取。
     */
    char args[PROCESS_ARGS_MAX];

    /* --- 统计，taskmgr 会展示 --- */
    int32_t   exit_code;
    uint32_t  image_size;
    uint32_t  memory_used;       /* 映像 + 用户栈，退出后仍保留供 taskmgr 显示 */
    uint32_t  start_tick;
    uint32_t  end_tick;
    uint32_t  cpu_ticks;         /* 被调度到的时钟滴答数 */
    uint32_t  syscall_count;
} process_t;

/* 初始化进程表，把当前上下文登记成进程 0（Shell） */
void process_init(void);

/* 由 kernel/idt.c 的 isr_handler 调用。
 * 返回下一个任务的内核栈指针；0 表示继续跑当前任务。
 */
uint32_t sched_tick(registers_t* regs);

/* 主动让出 CPU（下一次时钟中断时切换） */
void sched_yield(void);

/* 当前正在运行的进程 */
process_t* process_current(void);

/* 遍历进程表；返回 NULL 表示后面没有有效项了 */
process_t* process_get(uint32_t index);

/* 统计 */
uint32_t process_live_count(void);
uint32_t process_total_created(void);

/* 加载并前台运行一个 LXE 程序，阻塞到它退出，返回退出码；失败返回负数 */
int process_run(const char* filename, const char* args);

/* 后台启动一个 LXE 程序，立刻返回新进程的 pid；失败返回负数 */
int process_spawn(const char* filename, const char* args);

/* 强行结束某个进程。返回 0 成功，负数是各种拒绝原因：
 * -1 没有这个 pid，-2 不能杀 Shell，-3 不能杀自己，-4 已经退出
 */
int process_kill(uint32_t pid);

/* 结束当前进程。它只负责把任务标成僵尸并释放资源，
 * 真正的切换由随后 isr_handler 里的 sched_tick 完成。
 */
void process_exit_current(int32_t code);

/* ------------------------------------------------------------
 * 文件描述符表（系统调用用）
 * ------------------------------------------------------------ */

/* 分配一个新的 fd（从 3 开始）。返回 fd，失败返回 -1 */
int process_fd_alloc(uint32_t file_index, uint32_t flags, uint32_t offset);

/* 取 fd 对应的表项；无效 fd 返回 NULL */
fd_entry_t* process_fd_get(int fd);

/* 关闭 fd。fd 0/1/2 是控制台，不允许关闭 */
bool process_fd_close(int fd);

/* 用户堆：把当前进程的堆顶往上推 increment 字节，返回原来的堆顶。
 * increment 为 0 时只查询。失败返回 -1
 */
int32_t process_sbrk(int32_t increment);

#endif /* PROCESS_H */
