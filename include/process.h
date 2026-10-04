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
/* 一个进程最多占多少页（映像 + bss + 堆，用户栈另算）。
 *
 * 用户区一共 4MB = 1024 页，其中 8 页固定给用户栈，所以真实的硬上限是
 * 1016 页。这里原来写 768（3MB）是随手定的，自举的时候撞上了：
 * hncc 自己的映像 + bss 是 223 页，剩下的页不够它 malloc 出
 * "源码缓冲 + 代码缓冲 + 数据缓冲 + 2MB 语法树"（约 2.44MB = 610 页），
 * 于是报 "out of memory"。现在放开到 1000 页，只留 16 页余量。
 * 代价是 process_t 里的 image_frames 数组变大（16 个进程 × 4KB = 64KB 内核 BSS）。 */
#define PROCESS_MAX_IMAGE_PAGES    1000
#define PROCESS_STACK_PAGES        8            /* 32KB 用户栈 */
#define PROCESS_NAME_FIELD         16
#define PROCESS_MAX_FDS            16
#define PROCESS_ARGS_MAX           128
#define PROCESS_CWD_MAX            128   /* 工作目录路径长度上限 */

/* 能加载的 .lxe 最大字节数。
 *
 * 结构上的上限是用户区页数：PROCESS_MAX_IMAGE_PAGES * 4KB ≈ 3.9MB。
 * 这里再压一道 1MB 的策略上限，理由有两条：
 *   - 镜像越大，用户按下回车之后等的时间越长（读 1MB 大约半秒）；
 *   - 有这道上限，"太大了"这条错误路径才有程序能测到（user/waytoobig.c）。
 *
 * 历史：这里曾经是 60KB 的护栏 —— 加载器当年用 kmalloc 出来的大块堆内存
 * 当读盘目的地，镜像一超过 64KB 整机就三重故障。现在加载器不用暂存区了
 * （先读 40 字节头、按头里的尺寸建好页框、正文分块直接读进页框），
 * 那条路已经不存在，上限只受页框和上面这两条约束。
 */
#define PROCESS_MAX_LOAD_BYTES     (1024 * 1024)

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

    /* --- 身份与工作目录 ---
     * uid 来自登录（0 = root），文件权限检查用它。
     * cwd 是当前工作目录，相对路径以它为基准。两者都由子进程从创建者继承。
     */
    uint32_t  uid;
    bool      admin;             /* 管理员：文件权限检查里按"属主"看待 */
    char      cwd[PROCESS_CWD_MAX];

    /* --- 父子关系 ---
     * parent_pid 是 fork 时记下的，只有它会用 wait 去收尸。
     *
     * no_waiter 表示"这个进程没有等待者"：它由内核 Shell 的 run / spawn
     * 直接创建，退出之后没人会调 wait，所以槽位随时可以被 alloc_slot 抢走。
     * fork 出来的孩子正好相反 —— 父母可能还没来得及 wait，槽位就必须留着，
     * 否则 alloc_slot 会在父母取走 exit_code 之前把槽位抹掉，wait 就永远
     * 找不到这个 pid 了。
     *
     * 父进程先退出时会把孩子过继给 Shell（parent_pid = 0）并置上 no_waiter，
     * 这样没人管的孩子不会把进程表占死。
     */
    uint32_t  parent_pid;
    bool      no_waiter;

    int32_t   exit_code;
    uint32_t  image_size;
    uint32_t  memory_used;       /* 映像 + 用户栈，退出后仍保留供 taskmgr 显示 */
    uint32_t  start_tick;
    uint32_t  end_tick;
    uint32_t  cpu_ticks;         /* 被调度到的时钟滴答数 */
    uint32_t  syscall_count;
} process_t;

/* 当前进程的 uid（0 = root）与工作目录。
 * 没有当前进程时分别返回 0 和 "/"，这样文件系统那边可以无脑调用。 */
uint32_t    process_uid(void);
const char* process_cwd(void);

/* 当前进程是不是管理员（uid 0 也算） */
bool        process_is_admin(void);

/* 直接改写当前进程的工作目录。
 * 路径的合法性由调用者负责（Shell 的 cd 会先解析成目录再调它）。 */
void process_set_cwd(const char* path);

/* 初始化进程表，把当前上下文登记成进程 0（Shell） */
void process_init(void);

/* 由 kernel/idt.c 的 isr_handler 调用。
 * 返回下一个任务的内核栈指针；0 表示继续跑当前任务。
 */
uint32_t sched_tick(registers_t* regs);

/* 主动让出 CPU（下一次时钟中断时切换） */
void sched_yield(void);

/* 调度器临界区：sched_lock 之后 sched_tick 不再切换任务，但仍然让中断进来，
 * 所以键盘和时钟保持工作。给"必须开着中断做长时间磁盘 I/O"的代码用。
 * 必须成对调用，允许嵌套。 */
void sched_lock(void);
void sched_unlock(void);

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

/* ------------------------------------------------------------
 * fork / exec / wait
 *
 * 这三个都只在系统调用里用，所以直接收 registers_t*：exec 要改写
 * 保存的现场，不能只靠返回值。
 * ------------------------------------------------------------ */

/* 复制当前进程。成功时子进程拿到 pid、父进程拿到子进程的 pid；
 * 只有内核态任务（进程 0，也就是内核 Shell）不许 fork，返回 -1。
 * 子进程的内核栈上摆的是父进程现场的一份拷贝，只把 eax 改成 0。
 */
int process_fork(registers_t* regs);

/* 用 filename 这个 LXE 覆盖当前进程的映像，保留 pid / fd 表 / 身份 / 工作目录。
 *
 * 成功时**不返回** —— 它把 regs 里的 eip / useresp 改成新程序的入口和栈顶，
 * 于是 isr_common_stub 的 iret 一执行就落在新程序开头，旧程序再也回不来。
 * 失败返回负的错误码，当前进程原样继续跑（地址空间已经还原）。
 */
int process_exec(registers_t* regs, const char* filename, const char* args);

/* 等一个子进程退出并回收它的槽位，退出码写进 *code_out（可为 NULL）。
 * pid > 0 等指定孩子，pid <= 0 等任意一个孩子。
 * 返回被回收的子进程 pid；没有这样的孩子返回 -1。
 *
 * nohang = true 时完全不阻塞：手头没有已经退出的孩子就直接返回 0。
 * 用户态 Shell 用它顺手收掉后台任务留下的僵尸。
 *
 * 等待期间会把被等的进程设成"前台"，Ctrl+C 就是杀它 —— 见下面对
 * fg_pid 的说明。process_run 也是靠这个函数实现的。
 */
int process_wait_child(int32_t pid, bool nohang, int32_t* code_out);

/* 把 pid 设成前台进程。0 表示"没有前台进程"。
 *
 * Ctrl+C 落在谁头上就靠它：内核里只有一个键盘，没有进程组也没有终端
 * 的前台概念，所以用"最内层的那个 wait"来定义前台 ——
 * 内核 Shell 等用户态 Shell 时先把前台设成 Shell，Shell 再等某条命令时
 * 又把它改成那条命令，Shell 的 wait 结束时恢复成 Shell。
 * 于是"谁在前台"永远等于"嵌套最深的那个等待的目标"，这正是终端的语义。
 */
void process_set_foreground(uint32_t pid);

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
