#include "../include/process.h"
#include "../include/lxe.h"
#include "../include/hneofs.h"
#include "../include/paging.h"
#include "../include/heap.h"
#include "../include/pmm.h"
#include "../include/gdt.h"
#include "../include/timer.h"
#include "../include/string.h"
#include "../include/ports.h"
#include "../include/kernel.h"
#include "../include/keyboard.h"

static process_t  processes[PROCESS_MAX];
static process_t* current       = NULL;
static uint32_t   next_pid      = 1;
static uint32_t   total_created = 0;

/* 时钟中断之间的轮转时间片（滴答数，100Hz 下 5 = 50ms） */
#define SCHED_TIME_SLICE 5

static uint32_t slice_left = SCHED_TIME_SLICE;
static bool     force_switch = false;   /* 主动让出或被退出时置位 */

/* >0 表示有临界区正开着中断做长时间 I/O：中断照常进来（键照收、滴答照数），
 * 但调度器不许换任务 —— 临界区手里的文件系统全局状态还没交接完。
 * 关中断也能挡住切换，代价是把键盘和时钟一起冻住；这个计数器专门用来避免那个代价。
 */
static volatile uint32_t sched_locked = 0;

/* 前台进程：Ctrl+C 杀的就是它。用"保存 + 恢复"的栈式纪律管着，
 * 所以嵌套的等待会自动让最内层的那个当前台（见 process_set_foreground）。 */
static uint32_t fg_pid = 0;

void process_set_foreground(uint32_t pid) {
    fg_pid = pid;
}

/* 定义在后面，process_init 里要用 */
static void init_fds(process_t* p);

/* ------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------ */
void process_init(void) {
    for (uint32_t i = 0; i < PROCESS_MAX; i++) {
        memset(&processes[i], 0, sizeof(process_t));
        processes[i].state = PROC_UNUSED;
        processes[i].index = i;
    }

    /* 进程 0 就是 Shell 自己。
     * 它此刻正跑在引导栈上，内核态，所以我们不分配任何东西，
     * 只登记一下。它的 esp 要等到第一次被时钟中断打断时才会被保存，
     * 在那之前调度器不会选它（它不是 READY）。
     */
    process_t* shell = &processes[0];
    shell->pid              = 0;
    shell->state            = PROC_RUNNING;
    shell->index            = 0;
    shell->is_user          = false;
    shell->kernel_stack     = NULL;      /* 用的是引导栈 */
    shell->kernel_stack_top = 0x90000;
    shell->kernel_stack_size = 0;
    shell->page_directory   = 0;         /* 用内核主目录 */
    strncpy(shell->name, "shell", PROCESS_NAME_MAX - 1);
    shell->start_tick       = 0;

    /* 登录之前先当 root、待在根目录；shell_run 会做真正的登录并改写这两个。 */
    shell->uid = 0;
    strncpy(shell->cwd, "/", PROCESS_CWD_MAX - 1);

    current       = shell;
    next_pid      = 1;
    total_created = 1;
    slice_left    = SCHED_TIME_SLICE;
    force_switch  = false;

    init_fds(shell);
}

process_t* process_current(void) {
    return current;
}

/* 当前进程的 uid。没有当前进程（比如挂载文件系统时）就当 root。 */
uint32_t process_uid(void) {
    return current ? current->uid : 0;
}

/* 当前进程的工作目录。没有当前进程时给 "/"，让调用方不用判空。 */
const char* process_cwd(void) {
    if (current && current->cwd[0] != '\0') {
        return current->cwd;
    }
    return "/";
}

bool process_is_admin(void) {
    if (!current) {
        return true;      /* 还没有进程上下文（启动早期）就当 root */
    }
    return current->uid == 0 || current->admin;
}

void process_set_cwd(const char* path) {
    if (!current || !path || path[0] == '\0') {
        return;
    }
    strncpy(current->cwd, path, PROCESS_CWD_MAX - 1);
    current->cwd[PROCESS_CWD_MAX - 1] = '\0';
}

process_t* process_get(uint32_t index) {
    if (index >= PROCESS_MAX || processes[index].state == PROC_UNUSED) {
        return NULL;
    }
    return &processes[index];
}

uint32_t process_live_count(void) {
    uint32_t n = 0;

    for (uint32_t i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state != PROC_UNUSED) {
            n++;
        }
    }
    return n;
}

uint32_t process_total_created(void) {
    return total_created;
}

/* ------------------------------------------------------------
 * 回收
 * ------------------------------------------------------------ */

/* 僵尸任务的内核栈不能在自己身上释放，所以留到下一次调度、
 * 已经在别的栈上运行时再回收。
 */
static void reap_zombies(void) {
    for (uint32_t i = 0; i < PROCESS_MAX; i++) {
        process_t* p = &processes[i];

        if (p->state != PROC_ZOMBIE) {
            continue;
        }
        if (p == current) {
            continue;   /* 还没切走，不能动 */
        }
        if (p->kernel_stack) {
            kfree(p->kernel_stack);
            p->kernel_stack         = NULL;
            p->kernel_stack_top     = 0;
            p->kernel_stack_size    = 0;
        }
    }
}

/* 释放一个进程的用户态资源（页框、页表、页目录）。
 * 内核栈不在这里处理。
 */
static void free_user_resources(process_t* p) {
    uint32_t total_image = p->image_pages + p->heap_pages;

    for (uint32_t i = 0; i < total_image; i++) {
        pmm_free_page((void*)p->image_frames[i]);
    }
    for (uint32_t i = 0; i < p->stack_pages; i++) {
        pmm_free_page((void*)p->stack_frames[i]);
    }
    if (p->user_table) {
        pmm_free_page(p->user_table);
        p->user_table = NULL;
    }
    if (p->page_directory) {
        pmm_free_page((void*)p->page_directory);
        p->page_directory = 0;
    }

    p->image_pages = 0;
    p->heap_pages  = 0;
    p->stack_pages = 0;
}

/* 父进程没了：把孩子过继给内核 Shell（pid 0），并允许 alloc_slot 回收它们。
 *
 * 不过继的话，fork 出来的孩子在父母死后就成了没人 wait 的僵尸 ——
 * no_waiter 还是 false，alloc_slot 不敢动它，那 4KB 的 process_t 槽位
 * 就永远占着了。过继 + 置 no_waiter 之后，孤儿退出时能被正常回收，
 * 相当于 Unix 把孤儿挂到 init 底下。
 */
static void reparent_children(uint32_t dying_pid) {
    for (uint32_t i = 1; i < PROCESS_MAX; i++) {
        process_t* p = &processes[i];

        if (p->state == PROC_UNUSED) {
            continue;
        }
        if (p->parent_pid == dying_pid) {
            p->parent_pid = 0;
            p->no_waiter  = true;
        }
    }
}

/* 找一个空槽。没有空槽就回收最老的僵尸。 */
static process_t* alloc_slot(void) {
    for (uint32_t i = 1; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROC_UNUSED) {
            return &processes[i];
        }
    }

    /* 全都占着，挑一个僵尸回收掉。
     *
     * 只动 no_waiter 的僵尸：fork 出来的孩子即使已经退出，它的父进程也可能
     * 还没调 wait。槽位在这儿被 memset 掉的话，wait 就永远找不到那个 pid 了
     * —— 父母会一直等一个再也看不见的孩子。
     */
    for (uint32_t i = 1; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROC_ZOMBIE &&
            processes[i].kernel_stack == NULL &&
            processes[i].no_waiter) {
            memset(&processes[i], 0, sizeof(process_t));
            processes[i].index = i;
            processes[i].state = PROC_UNUSED;
            return &processes[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------
 * 构造新任务的初始内核栈
 *
 * isr_common_stub 的收尾代码是这样用的：
 *     pop eax / mov ds,ax ... popa / add esp,8 / iret
 * 所以栈上必须按 registers_t 的布局摆好一份"假现场"，
 * iret 之后 CPU 就会带着我们指定的 eip / cs / eflags / esp / ss
 * 跳进 ring 3。
 * ------------------------------------------------------------ */
static uint32_t craft_initial_stack(process_t* p, uint32_t entry, uint32_t user_stack_top) {
    uint32_t* sp = (uint32_t*)p->kernel_stack_top;

    *(--sp) = GDT_USER_DATA;      /* ss      */
    *(--sp) = user_stack_top;     /* useresp */
    *(--sp) = 0x00000202;         /* eflags：IF=1，保留位必须为 1 */
    *(--sp) = GDT_USER_CODE;      /* cs      */
    *(--sp) = entry;              /* eip     */
    *(--sp) = 0;                  /* err_code */
    *(--sp) = 0;                  /* int_no   */
    *(--sp) = 0;                  /* eax */
    *(--sp) = 0;                  /* ecx */
    *(--sp) = 0;                  /* edx */
    *(--sp) = 0;                  /* ebx */
    *(--sp) = 0;                  /* esp（pusha 的槽位，会被忽略） */
    *(--sp) = 0;                  /* ebp */
    *(--sp) = 0;                  /* esi */
    *(--sp) = 0;                  /* edi */
    *(--sp) = GDT_USER_DATA;      /* ds  <- 保存的 esp 指向这里 */

    return (uint32_t)sp;
}

/* 建进程的过程中出错：把已经拿到的资源还回去，槽位标回空闲。
 * 以前这段清理在 create_from_image_locked 里重复了六遍，加一个"读正文
 * 失败"的出口之后就更多了，所以收成一个函数。 */
static void fail_create(process_t* p) {
    free_user_resources(p);
    if (p->kernel_stack) {
        kfree(p->kernel_stack);
        p->kernel_stack = NULL;
    }
    p->kernel_stack_top  = 0;
    p->kernel_stack_size = 0;
    p->state = PROC_UNUSED;
}

/* 按 LXE 头建进程骨架：页框、页表、内核栈、初始现场。
 *
 * **这里不再拷贝映像正文** —— 正文由 fill_image_pages 直接读进页框。
 * 这一条是照着 KNOWN-ISSUES 第 1 节改的：那个缺陷出在"拿 kmalloc 的一大块
 * 内存当读盘目的地"上，所以加载路径上现在**不存在**任何"整份镜像"的缓冲区。
 *
 * 返回时任务的 state 已经是 PROC_READY（槽位不会再被别人抢走），但正文还
 * 没读进来，所以调用方必须用 sched_lock 挡住任务切换，见 load_program。
 */
static process_t* create_from_header_locked(const char* filename,
                                            const lxe_header_t* hdr,
                                            int* err) {
    process_t* p;
    uint32_t need_pages;
    uint32_t vaddr;

    p = alloc_slot();
    if (!p) {
        *err = -5;                     /* 进程表满了 */
        return NULL;
    }

    memset(p, 0, sizeof(process_t));
    p->index = (uint32_t)(p - processes);
    p->pid   = next_pid++;
    p->state = PROC_READY;
    p->is_user = true;
    p->start_tick = timer_get_ticks();
    total_created++;

    /* run / spawn 直接建出来的进程没有等待者：退出之后槽位随时可以回收。
     * fork 出来的孩子走的不是这条路（见 process_fork），它们要留给父母 wait。 */
    p->parent_pid = current ? current->pid : 0;
    p->no_waiter  = true;

    /* 身份和工作目录从创建者那里继承：Shell 是 root 时子程序也是 root，
     * 普通用户跑的程序同理 —— 系统调用里的权限检查就靠这个 uid。 */
    p->uid = current ? current->uid : 0;
    p->admin = current ? current->admin : false;
    if (current && current->cwd[0] != '\0') {
        strncpy(p->cwd, current->cwd, PROCESS_CWD_MAX - 1);
    } else {
        strncpy(p->cwd, "/", PROCESS_CWD_MAX - 1);
    }

    /* 程序名优先用 LXE 头部里的，没有就退回文件名 */
    {
        int i = 0;

        while (i < PROCESS_NAME_FIELD && hdr->name[i] &&
               i < PROCESS_NAME_MAX - 1) {
            p->name[i] = hdr->name[i];
            i++;
        }
        if (i == 0 && filename) {
            for (i = 0; i < PROCESS_NAME_MAX - 1 && filename[i]; i++) {
                p->name[i] = filename[i];
            }
        }
        p->name[i] = '\0';
    }

    /* --- 内核栈 --- */
    p->kernel_stack = (uint8_t*)kmalloc(PROCESS_KERNEL_STACK_SIZE);
    if (!p->kernel_stack) {
        p->state = PROC_UNUSED;
        *err = -3;
        return NULL;
    }
    memset(p->kernel_stack, 0, PROCESS_KERNEL_STACK_SIZE);
    p->kernel_stack_size = PROCESS_KERNEL_STACK_SIZE;
    p->kernel_stack_top  = (uint32_t)p->kernel_stack + PROCESS_KERNEL_STACK_SIZE;

    /* --- 地址空间 --- */
    p->page_directory = (uint32_t)paging_create_directory();
    p->user_table     = paging_create_user_table();
    if (!p->page_directory || !p->user_table) {
        fail_create(p);
        *err = -3;
        return NULL;
    }
    paging_attach_user_table((uint32_t*)p->page_directory, p->user_table);

    /* --- 映像页框：内容等正文读进来 --- */
    need_pages = (hdr->size + hdr->bss_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (need_pages == 0 || need_pages > PROCESS_MAX_IMAGE_PAGES ||
        need_pages + PROCESS_STACK_PAGES > USER_MAX_PAGES) {
        fail_create(p);
        *err = -6;                     /* 用户区装不下 */
        return NULL;
    }

    for (uint32_t i = 0; i < need_pages; i++) {
        void* frame = pmm_alloc_page();

        if (!frame) {
            fail_create(p);
            *err = -3;
            return NULL;
        }
        p->image_frames[i] = (uint32_t)frame;
        p->image_pages++;

        /* 整页清零：.bss 天然为 0，映像尾部多余的部分也是 0。
         * 正文随后按页盖在清零后的页框上。 */
        memset(frame, 0, PAGE_SIZE);

        vaddr = USER_BASE + i * PAGE_SIZE;
        if (!paging_map_user_page(p->user_table, vaddr, (uint32_t)frame, true)) {
            fail_create(p);
            *err = -3;
            return NULL;
        }
    }

    p->image_size = hdr->size + hdr->bss_size;
    p->memory_used = p->image_size + PROCESS_STACK_PAGES * PAGE_SIZE;
    p->entry      = USER_BASE + hdr->entry;

    /* --- 用户栈：铺在 4MB 用户区的顶端 --- */
    for (uint32_t i = 0; i < PROCESS_STACK_PAGES; i++) {
        void* frame = pmm_alloc_page();

        if (!frame) {
            fail_create(p);
            *err = -3;
            return NULL;
        }
        p->stack_frames[i] = (uint32_t)frame;
        p->stack_pages++;

        memset(frame, 0, PAGE_SIZE);

        vaddr = USER_BASE + USER_REGION_SIZE - (PROCESS_STACK_PAGES - i) * PAGE_SIZE;
        if (!paging_map_user_page(p->user_table, vaddr, (uint32_t)frame, true)) {
            fail_create(p);
            *err = -3;
            return NULL;
        }
    }
    p->user_stack_top = USER_BASE + USER_REGION_SIZE;

    /* 用户堆从映像末尾开始往上长，最多长到用户栈底 */
    p->heap_start      = USER_BASE + need_pages * PAGE_SIZE;
    p->heap_end        = p->heap_start;
    p->heap_pages      = 0;
    p->user_stack_base = USER_BASE + USER_REGION_SIZE
                       - PROCESS_STACK_PAGES * PAGE_SIZE;

    /* --- 初始现场 --- */
    p->esp = craft_initial_stack(p, p->entry, p->user_stack_top);

    init_fds(p);

    *err = 0;
    return p;
}

/* 把映像正文分块读进已经建好的页框。
 *
 * 一次最多读一页，读完直接落在目标页框里 —— 内核里**没有**"整份镜像"
 * 的缓冲区，这也是这次改造的重点（KNOWN-ISSUES 第 1 节）。
 * 页框是恒等映射的物理地址，所以可以直接当读盘目的地用。
 */
static bool fill_image_pages(process_t* p, const hneofs_file_t* f,
                             uint32_t image_size) {
    uint32_t offset = 0;

    while (offset < image_size) {
        uint32_t page_index = offset / PAGE_SIZE;
        uint32_t in_page    = offset % PAGE_SIZE;
        uint32_t chunk      = PAGE_SIZE - in_page;

        if (chunk > image_size - offset) {
            chunk = image_size - offset;
        }
        if (page_index >= p->image_pages) {
            return false;              /* 页数对不上，防御性检查 */
        }
        if (hneofs_read_at(f, LXE_HEADER_SIZE + offset,
                           (uint8_t*)p->image_frames[page_index] + in_page,
                           chunk) != (int32_t)chunk) {
            return false;              /* 读到一半失败：调用方会把进程拆掉 */
        }
        offset += chunk;
    }
    return true;
}

/* 从文件系统读出一个 LXE 并创建进程。
 *
 * **调用者必须已经拿着 sched_lock**（理由见 include/process.h 里 sched_lock
 * 那一段）：骨架一建好，新任务就是 PROC_READY 了，正文却还没读进来 ——
 * 这时候被切上去跑，跑的就是一个读到一半的程序。
 * fork 和 exec 都得在这把锁底下建任务，所以这里拆成"不锁"的核心，
 * 外面由 load_program 包一层锁。
 */
static process_t* load_program_locked(const char* filename, const char* args, int* err) {
    const hneofs_file_t* f;
    lxe_header_t hdr;
    process_t* p;

    if (!hneofs_mounted()) {
        *err = -2;
        return NULL;
    }

    f = hneofs_lookup(filename);
    if (!f || f->type != HNEOFS_TYPE_FILE) {
        *err = -1;
        return NULL;
    }
    if (f->size < LXE_HEADER_SIZE) {
        *err = -1;
        return NULL;
    }
    if (f->size > PROCESS_MAX_LOAD_BYTES) {
        *err = -6;                     /* 太大了：Shell 会打印一句人话 */
        return NULL;
    }

    /* 先只把 40 字节的头读到栈上：建多少页框、要读多少正文全看它。
     * 头没读全、或者魔数/长度不对，就直接拒绝，一个页框都不用分配。 */
    if (hneofs_read_at(f, 0, &hdr, LXE_HEADER_SIZE) != (int32_t)LXE_HEADER_SIZE) {
        *err = -4;
        return NULL;
    }
    if (hdr.magic != LXE_MAGIC || hdr.version != LXE_VERSION) {
        *err = -1;
        return NULL;
    }
    if (hdr.size == 0 || LXE_HEADER_SIZE + hdr.size > f->size) {
        *err = -1;
        return NULL;
    }
    if (hdr.entry >= hdr.size) {
        *err = -1;
        return NULL;
    }

    p = create_from_header_locked(filename, &hdr, err);
    if (p) {
        if (fill_image_pages(p, f, hdr.size)) {
            /* 命令行参数交给进程自己保管，程序通过 getargs 系统调用取 */
            if (args) {
                strncpy(p->args, args, PROCESS_ARGS_MAX - 1);
                p->args[PROCESS_ARGS_MAX - 1] = '\0';
            }
        } else {
            fail_create(p);
            p = NULL;
            *err = -4;
        }
    }

    if (!p) {
        return NULL;
    }

    *err = 0;
    return p;
}

/* load_program_locked 的加锁外壳。
 *
 * 为什么不是关中断：读正文是几十毫秒的磁盘 I/O，关中断会把键盘和时钟
 * 一起冻住。sched_lock 是"中断照进、但不换任务"，
 * 和 sys_readfile 里那段用的是同一个办法。
 */
static process_t* load_program(const char* filename, const char* args, int* err) {
    process_t* p;

    sched_lock();
    p = load_program_locked(filename, args, err);
    sched_unlock();

    return p;
}

/* ------------------------------------------------------------
 * 文件描述符表
 * ------------------------------------------------------------ */

/* 初始化 fd 表：0/1/2 固定是控制台，其余全部空闲 */
static void init_fds(process_t* p) {
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        p->fds[i].used       = false;
        p->fds[i].file_index = 0;
        p->fds[i].offset     = 0;
        p->fds[i].flags      = 0;
    }
    for (int i = 0; i < 3; i++) {
        p->fds[i].used       = true;
        p->fds[i].file_index = FD_CONSOLE;
    }
}

int process_fd_alloc(uint32_t file_index, uint32_t flags, uint32_t offset) {
    process_t* p = current;

    if (!p) {
        return -1;
    }
    /* 从 3 开始：0/1/2 留给标准输入输出 */
    for (int i = 3; i < PROCESS_MAX_FDS; i++) {
        if (!p->fds[i].used) {
            p->fds[i].used       = true;
            p->fds[i].file_index = file_index;
            p->fds[i].offset     = offset;
            p->fds[i].flags      = flags;
            return i;
        }
    }
    return -1;   /* fd 用完了 */
}

fd_entry_t* process_fd_get(int fd) {
    process_t* p = current;

    if (!p || fd < 0 || fd >= PROCESS_MAX_FDS) {
        return NULL;
    }
    if (!p->fds[fd].used) {
        return NULL;
    }
    return &p->fds[fd];
}

bool process_fd_close(int fd) {
    process_t* p = current;

    if (!p || fd < 3 || fd >= PROCESS_MAX_FDS) {
        return false;   /* 控制台 fd 不允许关闭 */
    }
    if (!p->fds[fd].used) {
        return false;
    }

    p->fds[fd].used       = false;
    p->fds[fd].file_index = 0;
    p->fds[fd].offset     = 0;
    p->fds[fd].flags      = 0;
    return true;
}

/* ------------------------------------------------------------
 * 用户堆
 *
 * 用户程序没有 brk/mmap，想要动态内存只能靠这个：把堆顶往上推。
 * 堆紧跟在映像后面往上涨，用户栈固定在 4MB 用户区的顶端往下，
 * 中间的空隙就是可用的堆空间。
 * ------------------------------------------------------------ */
int32_t process_sbrk(int32_t increment) {
    process_t* p = current;
    uint32_t old_end;
    uint32_t new_end;

    if (!p || !p->is_user) {
        return -1;   /* 内核任务没有用户堆 */
    }

    old_end = p->heap_end;

    if (increment == 0) {
        return (int32_t)old_end;
    }

    if (increment < 0) {
        /* 目前不支持收缩：堆页一旦映射就留着，实现简单也不会踩到别人 */
        return -1;
    }

    if (old_end + (uint32_t)increment < old_end) {
        return -1;   /* 溢出 */
    }
    new_end = old_end + (uint32_t)increment;

    if (new_end > p->user_stack_base) {
        return -1;   /* 要撞上用户栈了 */
    }

    /* 把新跨越的整页补齐 */
    {
        uint32_t first = (old_end + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
        uint32_t last  = (new_end + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
        uint32_t vaddr;

        for (vaddr = first; vaddr < last; vaddr += PAGE_SIZE) {
            void* frame;
            uint32_t slot;

            if (p->image_pages + p->heap_pages >= PROCESS_MAX_IMAGE_PAGES) {
                return -1;   /* 页表数组满了 */
            }

            frame = pmm_alloc_page();
            if (!frame) {
                return -1;   /* 物理内存不够 */
            }
            memset(frame, 0, PAGE_SIZE);

            slot = p->image_pages + p->heap_pages;
            p->image_frames[slot] = (uint32_t)frame;
            p->heap_pages++;

            if (!paging_map_user_page(p->user_table, vaddr, (uint32_t)frame, true)) {
                return -1;
            }
        }
    }

    p->heap_end = new_end;
    return (int32_t)old_end;
}

/* ------------------------------------------------------------
 * 调度器
 * ------------------------------------------------------------ */

/* 见 include/process.h：禁止切换任务，但允许中断进来 */
void sched_lock(void)   { sched_locked++; }
void sched_unlock(void) { if (sched_locked > 0) { sched_locked--; } }

/* 选下一个可运行的任务：从当前下标往后找第一个 READY 的。
 * 一定跳过 current —— 否则会"切换到自己"，白白多做一次 CR3 写入。
 */
static process_t* pick_next(void) {
    uint32_t start = current ? current->index : 0;

    for (uint32_t n = 1; n < PROCESS_MAX; n++) {
        uint32_t idx = (start + n) % PROCESS_MAX;
        process_t* p = &processes[idx];

        if (p == current) {
            continue;
        }
        if (p->state == PROC_READY) {
            return p;
        }
    }
    return NULL;
}

uint32_t sched_tick(registers_t* regs) {
    process_t* next;

    reap_zombies();

    /* 当前任务被时钟打断了，先把它挂起 */
    if (current) {
        current->esp = (uint32_t)regs;
        if (current->state == PROC_RUNNING) {
            current->state = PROC_READY;
        }
        current->cpu_ticks++;
    }

    /* 有临界区开着中断时只数滴答、不换任务 */
    if (sched_locked > 0) {
        if (current) {
            current->state = PROC_RUNNING;
        }
        return 0;
    }

    /* 只有时钟中断才做时间片轮转；主动让出或任务退出时强制切换 */
    if (regs->int_no == IRQ0) {
        if (slice_left > 0) {
            slice_left--;
        }
        if (slice_left > 0 && !force_switch) {
            /* 时间片还没用完，继续跑当前任务 */
            if (current) {
                current->state = PROC_RUNNING;
            }
            return 0;
        }
    } else if (!force_switch) {
        return 0;
    }

    force_switch = false;
    slice_left   = SCHED_TIME_SLICE;

    /* 当前任务已经退出的话必须换人 */
    if (current && current->state == PROC_ZOMBIE) {
        next = pick_next();
        if (next) {
            goto do_switch;
        }
        /* 没有别的任务可跑：Shell 是空闲任务，但它也可能正因为
         * 等子进程而空转，这里没有更好的选择，只能回到它。
         */
        if (current != &processes[0]) {
            next = &processes[0];
            goto do_switch;
        }
        return 0;
    }

    next = pick_next();
    if (!next) {
        /* 没有别的任务，继续跑当前任务 */
        if (current) {
            current->state = PROC_RUNNING;
        }
        return 0;
    }

do_switch:
    /* 切地址空间：每个用户任务有自己的页目录，
     * 里面同样包含完整的内核恒等映射，所以内核代码在切换过程中始终可执行
     */
    if (next->page_directory) {
        paging_switch_directory(next->page_directory);
    } else {
        paging_switch_directory(paging_kernel_directory());
    }

    /* ring 3 陷入内核时用的栈。内核任务用不到，但设一下也无妨 */
    tss_set_kernel_stack(next->kernel_stack_top);

    current = next;
    next->state = PROC_RUNNING;


    return next->esp;
}

void sched_yield(void) {
    force_switch = true;
    slice_left   = 0;
}

void process_exit_current(int32_t code) {
    process_t* p = current;

    if (!p) {
        kernel_panic("process_exit called with no current process");
    }
    if (p->pid == 0) {
        /* Shell 不应该退出 */
        kernel_panic("the shell process tried to exit");
    }

    p->exit_code = code;
    p->end_tick  = timer_get_ticks();

    /* 孩子过继给 Shell —— 我们马上就不在了，没人再会 wait 它们 */
    reparent_children(p->pid);

    /* 用户资源可以立刻释放：反正不会再回到那个地址空间了。
     * 内核栈不行 —— 我们此刻正踩在它上面，交给 reap_zombies 稍后回收。
     */
    free_user_resources(p);

    p->state     = PROC_ZOMBIE;
    force_switch = true;
    /* 注意：这里不设置 current = NULL。
     * 接下来的 sched_tick 靠 current->state == PROC_ZOMBIE 判断必须换人。
     */
}

/* ------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------ */
int process_run(const char* filename, const char* args) {
    int err = 0;
    process_t* p = load_program(filename, args, &err);
    int32_t code = 0;
    int rc;

    if (!p) {
        return err;
    }

    /* 丢掉可能在提示符处按下的 Ctrl+C，免得刚启动的程序立刻被杀 */
    (void)keyboard_take_ctrl_c();

    /* 等它跑完。
     *
     * 这一段原来是自带一份"轮询 + 回收 + Ctrl+C"的循环，和 process_wait_child
     * 是重复的 —— 两处都要处理"释放内核栈和标记 UNUSED 之间不能进时钟中断"、
     * 都要处理 Ctrl+C 杀前台。合并成一次调用之后只剩一个地方要维护。
     *
     * load_program 建的进程 parent_pid 就是当前进程（内核 Shell，pid 0），
     * 所以 process_wait_child 按父进程找孩子的规则天然适用。
     */
    rc = process_wait_child((int32_t)p->pid, false, &code);
    if (rc < 0) {
        return err;
    }

    return (int)code;
}

int process_spawn(const char* filename, const char* args) {
    int err = 0;
    process_t* p = load_program(filename, args, &err);

    if (!p) {
        return err;
    }
    return (int)p->pid;
}

/* ------------------------------------------------------------
 * fork / exec / wait
 * ------------------------------------------------------------ */

/* exec 交换地址空间时用的中转缓冲。
 *
 * 放 BSS 而不是栈上：image_frames 有 4KB，而用户进程的内核栈一共才 8KB，
 * 在栈上摆一个 4KB 的临时数组再叠上调用链很容易砸穿栈底
 * （当年 hneofs_sync 就是这么撞出 __chkstk_ms 的）。
 * 同一时刻只可能有一个进程在 exec 里，所以静态变量是安全的。
 */
static uint32_t swap_frames[PROCESS_MAX_IMAGE_PAGES];
static uint32_t swap_stack[PROCESS_STACK_PAGES];

/* 把 a 和 b 的用户地址空间整个对调。
 * 交换之后 a 拿的是 b 原来那套映像，反之亦然。CR3 由调用者自己刷 ——
 * 这个函数只动 process_t 里的字段。
 */
static void swap_address_space(process_t* a, process_t* b) {
    uint32_t a_pages = a->image_pages + a->heap_pages;
    uint32_t b_pages = b->image_pages + b->heap_pages;
    uint32_t a_stack = a->stack_pages;
    uint32_t b_stack = b->stack_pages;
    uint32_t  t;
    uint32_t* tp;

    t = a->page_directory;  a->page_directory  = b->page_directory;  b->page_directory  = t;
    t = a->image_size;      a->image_size      = b->image_size;      b->image_size      = t;
    t = a->memory_used;     a->memory_used     = b->memory_used;     b->memory_used     = t;
    t = a->heap_start;      a->heap_start      = b->heap_start;      b->heap_start      = t;
    t = a->heap_end;        a->heap_end        = b->heap_end;        b->heap_end        = t;
    t = a->user_stack_base; a->user_stack_base = b->user_stack_base; b->user_stack_base = t;
    t = a->entry;           a->entry           = b->entry;           b->entry           = t;
    t = a->user_stack_top;  a->user_stack_top  = b->user_stack_top;  b->user_stack_top  = t;
    t = a->image_pages;     a->image_pages     = b->image_pages;     b->image_pages     = t;
    t = a->heap_pages;      a->heap_pages      = b->heap_pages;      b->heap_pages      = t;
    t = a->stack_pages;     a->stack_pages     = b->stack_pages;     b->stack_pages     = t;

    tp = a->user_table;     a->user_table      = b->user_table;      b->user_table      = tp;

    /* 页框数组。两边的条数通常不一样（新映像一般和旧的大小不同），
     * 所以各自按交换前的条数搬：先把 a 的挪进中转区，再把 b 的搬给 a，
     * 最后把中转区里的搬给 b。 */
    memcpy(swap_frames, a->image_frames, a_pages * sizeof(uint32_t));
    memcpy(a->image_frames, b->image_frames, b_pages * sizeof(uint32_t));
    memcpy(b->image_frames, swap_frames, a_pages * sizeof(uint32_t));

    memcpy(swap_stack, a->stack_frames, a_stack * sizeof(uint32_t));
    memcpy(a->stack_frames, b->stack_frames, b_stack * sizeof(uint32_t));
    memcpy(b->stack_frames, swap_stack, a_stack * sizeof(uint32_t));
}

int process_fork(registers_t* regs) {
    process_t* parent = current;
    process_t* child;
    uint32_t total_image;

    if (!parent || !parent->is_user || !parent->page_directory) {
        return -1;   /* 内核态任务（进程 0）没有用户地址空间可复制 */
    }

    /* 复制一整份地址空间，期间不能让别的任务插进来碰 PMM 和内核堆。
     * sched_lock 只是"不换任务"，中断照进，键盘和时钟都不受影响。 */
    sched_lock();

    child = alloc_slot();
    if (!child) {
        sched_unlock();
        return -5;   /* 进程表满了 */
    }

    memset(child, 0, sizeof(process_t));
    child->index      = (uint32_t)(child - processes);
    child->pid        = next_pid++;
    child->state      = PROC_UNUSED;   /* 全部建完才置 READY */
    child->is_user    = true;
    child->start_tick = timer_get_ticks();
    total_created++;

    strncpy(child->name, parent->name, PROCESS_NAME_MAX - 1);
    child->uid        = parent->uid;
    child->admin      = parent->admin;
    child->parent_pid = parent->pid;
    child->no_waiter  = false;   /* 父母会 wait，槽位得留到那时候 */
    strncpy(child->cwd, parent->cwd, PROCESS_CWD_MAX - 1);
    memcpy(child->args, parent->args, PROCESS_ARGS_MAX);

    /* fd 表整份复制。
     *
     * 真实 Unix 里 fork 之后父子的文件偏移是共享的（同一个 file 结构）；
     * 这里每个 fd 一份独立的 offset，父子各读各的。因为内核里没有
     * "打开文件表"那一层，要共享就得引入引用计数 —— 对 Shell 这种
     * "fork 完马上 exec"的用法没有区别，先按简单的来。
     */
    memcpy(child->fds, parent->fds, sizeof(parent->fds));

    /* --- 内核栈 --- */
    child->kernel_stack = (uint8_t*)kmalloc(PROCESS_KERNEL_STACK_SIZE);
    if (!child->kernel_stack) {
        fail_create(child);
        sched_unlock();
        return -3;
    }
    memset(child->kernel_stack, 0, PROCESS_KERNEL_STACK_SIZE);
    child->kernel_stack_size = PROCESS_KERNEL_STACK_SIZE;
    child->kernel_stack_top  = (uint32_t)child->kernel_stack + PROCESS_KERNEL_STACK_SIZE;

    /* --- 页目录与页表 --- */
    child->page_directory = (uint32_t)paging_create_directory();
    child->user_table     = paging_create_user_table();
    if (!child->page_directory || !child->user_table) {
        fail_create(child);
        sched_unlock();
        return -3;
    }
    paging_attach_user_table((uint32_t*)child->page_directory, child->user_table);

    /* --- 映像页 + 堆页：整份复制 ---
     * 两类页在 image_frames[] 里是连着的，所以一个循环走完。 */
    total_image = parent->image_pages + parent->heap_pages;
    for (uint32_t i = 0; i < total_image; i++) {
        void* frame = pmm_alloc_page();

        if (!frame) {
            fail_create(child);
            sched_unlock();
            return -3;
        }

        child->image_frames[i] = (uint32_t)frame;
        /* 这一步兼作"已经复制了几页"的记账：中途失败时 fail_create 靠
         * image_pages 决定释放多少页框，记少了就漏、记多了就误释放别人的页。 */
        child->image_pages = i + 1;

        memcpy(frame, (const void*)parent->image_frames[i], PAGE_SIZE);

        if (!paging_map_user_page(child->user_table, USER_BASE + i * PAGE_SIZE,
                                  (uint32_t)frame, true)) {
            fail_create(child);
            sched_unlock();
            return -3;
        }
    }
    /* 复制完才把两个计数还原成各自的真值：页框连着放，记账是分开的 */
    child->image_pages = parent->image_pages;
    child->heap_pages  = parent->heap_pages;

    /* --- 用户栈：同样整份复制 --- */
    for (uint32_t i = 0; i < parent->stack_pages; i++) {
        void* frame = pmm_alloc_page();

        if (!frame) {
            fail_create(child);
            sched_unlock();
            return -3;
        }

        child->stack_frames[i] = (uint32_t)frame;
        child->stack_pages = i + 1;
        memcpy(frame, (const void*)parent->stack_frames[i], PAGE_SIZE);

        if (!paging_map_user_page(child->user_table,
                                  USER_BASE + USER_REGION_SIZE
                                            - (PROCESS_STACK_PAGES - i) * PAGE_SIZE,
                                  (uint32_t)frame, true)) {
            fail_create(child);
            sched_unlock();
            return -3;
        }
    }

    /* 堆和栈的边界、入口地址直接继承 —— 地址空间是逐字节复制出来的，
     * 虚地址布局当然一模一样，连 sbrk 记到哪儿了都一致。 */
    child->heap_start      = parent->heap_start;
    child->heap_end        = parent->heap_end;
    child->user_stack_base = parent->user_stack_base;
    child->entry           = parent->entry;
    child->user_stack_top  = parent->user_stack_top;
    child->image_size      = parent->image_size;
    child->memory_used     = parent->memory_used;

    /* --- 子进程的初始现场 ---
     *
     * 就是把父进程此刻这份 int 0x80 的中断帧照抄一份，只把 eax 改成 0。
     * 子进程从系统调用返回之后看到的和父进程完全一样：同一条指令、
     * 同一个用户栈、同一批寄存器 —— 唯独 fork() 的返回值是 0，
     * 而父进程那边拿到的是下面返回的这个 pid。这就是 fork 的全部奥义。
     *
     * 栈顶那 64 字节正好放得下一整帧（registers_t 就是 16 个 dword），
     * isr_common_stub 的 popa / iret 会把它弹干净，esp 刚好落回栈顶。
     */
    {
        registers_t* frame = (registers_t*)(child->kernel_stack_top - sizeof(registers_t));

        memcpy(frame, regs, sizeof(registers_t));
        frame->eax = 0;
        child->esp = (uint32_t)frame;
    }

    child->state = PROC_READY;
    sched_unlock();

    return (int)child->pid;
}

int process_exec(registers_t* regs, const char* filename, const char* args) {
    process_t* p = current;
    process_t* tmp;
    char name[PROCESS_NAME_MAX];
    char newargs[PROCESS_ARGS_MAX];
    int err = 0;

    if (!p || !p->is_user) {
        return -1;   /* 内核态任务没有用户映像可以替换 */
    }

    /* 先按"新建一个进程"的路子把新映像装在临时槽里。
     *
     * 这样查找文件、校验头、分配页框、分块读正文全都复用 load_program，
     * 一行都不用重写；代价是要有一个空槽，进程表满了 exec 就失败。
     * load_program_locked 而不是 load_program：锁必须从这儿一直拿到底，
     * 中间松手的话调度器可能切到临时槽里那个还等着被换掉的进程。
     */
    sched_lock();

    tmp = load_program_locked(filename, args, &err);
    if (!tmp) {
        sched_unlock();
        return err;
    }

    /* 新程序的程序名和命令行参数先抄出来：下面一交换，tmp 手里就是旧映像了，
     * 而这两个字段我们要写到 p 身上（p 才是活下来的那个进程）。 */
    memcpy(name, tmp->name, PROCESS_NAME_MAX);
    memcpy(newargs, tmp->args, PROCESS_ARGS_MAX);

    swap_address_space(p, tmp);

    memcpy(p->name, name, PROCESS_NAME_MAX);
    memcpy(p->args, newargs, PROCESS_ARGS_MAX);

    /* CR3 必须紧跟着换：p 的页目录指针变了，但 CPU 里那个还是旧的 ——
     * 也就是刚归到临时槽名下、马上就要被拆掉的那一张。
     * 顺序不能颠倒，先刷 CR3 再释放旧页目录，否则 PMM 可能把那几页
     * 重新分配出去，而 CR3 还指着它们。 */
    paging_switch_directory(p->page_directory);

    /* 旧映像连同它的页框、页表、页目录一起还给 PMM。
     * 此刻它们挂在临时槽名下，所以直接拆临时槽就行。 */
    free_user_resources(tmp);

    /* 临时槽的内核栈是 load_program_locked 刚分配的，我们没跑在上面，
     * 可以直接释放，然后把槽位还回去。 */
    if (tmp->kernel_stack) {
        kfree(tmp->kernel_stack);
        tmp->kernel_stack = NULL;
    }
    memset(tmp, 0, sizeof(process_t));
    tmp->index = (uint32_t)(tmp - processes);
    tmp->state = PROC_UNUSED;

    sched_unlock();

    /* 改写返回到用户态的现场。
     *
     * 这里是 exec 和别的系统调用最不一样的地方：它不是"返回一个值"，
     * 而是把整份中断帧改成"一个全新的进程正要从入口点开始跑"的样子。
     * isr_handler 返回之后，isr_common_stub 的 popa / iret 就把控制权
     * 交给新程序了 —— 旧程序再也回不来，这正是 exec 的语义。
     *
     * eflags / cs / ss 保持原样：它们本来就是这个进程的用户态选择子和
     * 开着中断的标志位，而新老程序跑的是同一个 ring 3。
     */
    regs->edi = 0;
    regs->esi = 0;
    regs->ebp = 0;
    regs->esp = 0;          /* pusha 的槽位，popa 时被忽略 */
    regs->ebx = 0;
    regs->edx = 0;
    regs->ecx = 0;
    regs->eax = 0;
    regs->eip     = p->entry;
    regs->useresp = p->user_stack_top;

    return 0;
}

int process_wait_child(int32_t pid, bool nohang, int32_t* code_out) {
    process_t* me = current;
    process_t* child;
    uint32_t saved_fg;
    uint32_t child_pid;
    int32_t  code;

    if (!me) {
        return -1;
    }

    saved_fg = fg_pid;

    for (;;) {
        child = NULL;

        /* 找一个符合条件的孩子：pid > 0 是指定等谁，pid <= 0 是等任意一个。 */
        for (uint32_t i = 1; i < PROCESS_MAX; i++) {
            process_t* p = &processes[i];

            if (p->state == PROC_UNUSED) {
                continue;
            }
            if (p->parent_pid != me->pid) {
                continue;
            }
            if (pid > 0 && (int32_t)p->pid != pid) {
                continue;
            }
            child = p;
            break;
        }

        if (!child) {
            fg_pid = saved_fg;
            return -1;   /* 没有这个孩子（或者一个孩子都没有） */
        }
        if (child->state == PROC_ZOMBIE) {
            break;
        }
        if (nohang) {
            fg_pid = saved_fg;
            return 0;    /* 还都在跑，调用方不想等 */
        }

        /* 前台就是它：下面这段时间里按的 Ctrl+C 都算在它头上。
         * 嵌套的时候内层的 wait 会把它覆盖掉，于是"谁在前台"自动等于
         * "嵌套最深的那个等待的目标"（见 process_set_foreground）。 */
        fg_pid = child->pid;

        /* 孩子还在跑。cpu_halt 让出 CPU，下一次时钟中断就会切过去执行它。 */
        cpu_halt();
        stack_guard("process_wait");

        /* Ctrl+C 打断前台程序。
         *
         * 这是把跑飞的程序拉回来的唯一办法：等待的人此刻正阻塞在这个循环里，
         * 没有别的地方能收命令。杀掉之后循环条件不成立，正常往下走。
         *
         * 为什么要先比一下 fg_pid：嵌套等待时外层的 wait 也在转这个循环，
         * 而 Ctrl+C 只有一个。比一下就能保证只有最内层（真正的前台）收走它，
         * 否则外层会抢先把用户态 Shell 自己杀掉，而用户想杀的是它跑的那条命令。
         */
        if (fg_pid == child->pid && keyboard_take_ctrl_c()) {
            process_kill(child->pid);
        }
    }

    fg_pid = saved_fg;

    child_pid = child->pid;
    code      = child->exit_code;

    /* 回收槽位。和 process_run 结尾那段一样，两件事都得做对：
     *
     *   - 必须是我们自己的栈上：孩子已经不在 CPU 上了，但它那份内核栈
     *     此刻没人踩，kfree 是安全的；
     *   - 必须关中断：否则时钟中断可以插在"释放内核栈"和"标记 UNUSED"
     *     中间，reap_zombies 会对同一个指针再释放一次（双重释放）。
     */
    {
        uint32_t flags = irq_save();

        if (child->kernel_stack) {
            kfree(child->kernel_stack);
            child->kernel_stack      = NULL;
            child->kernel_stack_top  = 0;
            child->kernel_stack_size = 0;
        }
        child->state = PROC_UNUSED;

        irq_restore(flags);
    }

    if (code_out) {
        *code_out = code;
    }
    return (int)child_pid;
}

int process_kill(uint32_t pid) {
    process_t* p = NULL;

    for (uint32_t i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state != PROC_UNUSED && processes[i].pid == pid) {
            p = &processes[i];
            break;
        }
    }

    if (!p) {
        return -1;   /* 没有这个 pid */
    }
    if (p->pid == 0) {
        return -2;   /* Shell 不能被杀 */
    }
    if (p == current) {
        return -3;   /* 自己杀自己会让回收逻辑变复杂，先不支持 */
    }
    if (p->state == PROC_ZOMBIE) {
        return -4;   /* 已经退出了 */
    }

    /* 关中断，并且先标僵尸再释放资源。
     *
     * 原来的顺序（先 free_user_resources、后标 ZOMBIE）有个真实的竞态：
     * 这个函数是 Shell 调的，中断开着。时钟中断只要插在 free_user_resources
     * 中间，pick_next 就可能挑中这个还是 READY、但页目录已经被回收的任务 ——
     * 轻则切进没有用户页的地址空间缺页崩机，重则 CR3 指向已经交还给 pmm、
     * 可能被重新分配出去的页框。
     * 先标 ZOMBIE，任务立刻就不可能被调度到；内核栈仍然留着，由
     * reap_zombies 在下一次调度时回收。
     */
    {
        uint32_t flags = irq_save();

        p->exit_code = -1;
        p->end_tick  = timer_get_ticks();
        p->state     = PROC_ZOMBIE;
        reparent_children(p->pid);
        free_user_resources(p);

        irq_restore(flags);
    }

    return 0;
}
