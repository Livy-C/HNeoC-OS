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

/* 找一个空槽。没有空槽就回收最老的僵尸。 */
static process_t* alloc_slot(void) {
    for (uint32_t i = 1; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROC_UNUSED) {
            return &processes[i];
        }
    }

    /* 全都占着，挑一个僵尸回收掉 */
    for (uint32_t i = 1; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROC_ZOMBIE && processes[i].kernel_stack == NULL) {
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

/* ------------------------------------------------------------
 * 从一个 LXE 映像创建进程
 *
 * 注意：这个函数会在任务还没构造完时就把它标成 READY，而此刻
 * esp / 页目录 / 页表可能还是空的。如果这段时间来了时钟中断，
 * 调度器就会切进一个半成品任务（esp = 0）直接崩掉。
 * 所以真正的构造过程全部在关中断状态下进行，外面套一层包装。
 * 构造期间没有磁盘 I/O，关闭时间很短。
 * ------------------------------------------------------------ */
static process_t* create_from_image_locked(const char* filename, const uint8_t* image,
                                           uint32_t size) {
    const lxe_header_t* hdr;
    process_t* p;
    uint32_t need_pages;
    uint32_t vaddr;

    if (size < LXE_HEADER_SIZE) {
        return NULL;
    }

    hdr = (const lxe_header_t*)image;
    if (hdr->magic != LXE_MAGIC || hdr->version != LXE_VERSION) {
        return NULL;
    }
    if (hdr->size == 0 || LXE_HEADER_SIZE + hdr->size > size) {
        return NULL;
    }
    if (hdr->entry >= hdr->size) {
        return NULL;
    }

    p = alloc_slot();
    if (!p) {
        return NULL;
    }

    memset(p, 0, sizeof(process_t));
    p->index = (uint32_t)(p - processes);
    p->pid   = next_pid++;
    p->state = PROC_READY;
    p->is_user = true;
    p->start_tick = timer_get_ticks();
    total_created++;

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
        return NULL;
    }
    memset(p->kernel_stack, 0, PROCESS_KERNEL_STACK_SIZE);
    p->kernel_stack_size = PROCESS_KERNEL_STACK_SIZE;
    p->kernel_stack_top  = (uint32_t)p->kernel_stack + PROCESS_KERNEL_STACK_SIZE;

    /* --- 地址空间 --- */
    p->page_directory = (uint32_t)paging_create_directory();
    p->user_table     = paging_create_user_table();
    if (!p->page_directory || !p->user_table) {
        free_user_resources(p);
        kfree(p->kernel_stack);
        p->state = PROC_UNUSED;
        return NULL;
    }
    paging_attach_user_table((uint32_t*)p->page_directory, p->user_table);

    /* --- 映像 --- */
    need_pages = (hdr->size + hdr->bss_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (need_pages == 0 || need_pages > PROCESS_MAX_IMAGE_PAGES ||
        need_pages + PROCESS_STACK_PAGES > USER_MAX_PAGES) {
        free_user_resources(p);
        kfree(p->kernel_stack);
        p->state = PROC_UNUSED;
        return NULL;
    }

    for (uint32_t i = 0; i < need_pages; i++) {
        void* frame = pmm_alloc_page();

        if (!frame) {
            free_user_resources(p);
            kfree(p->kernel_stack);
            p->state = PROC_UNUSED;
            return NULL;
        }
        p->image_frames[i] = (uint32_t)frame;
        p->image_pages++;

        /* 整页清零：.bss 天然为 0，映像尾部多余的部分也是 0 */
        memset(frame, 0, PAGE_SIZE);

        vaddr = USER_BASE + i * PAGE_SIZE;
        if (!paging_map_user_page(p->user_table, vaddr, (uint32_t)frame, true)) {
            free_user_resources(p);
            kfree(p->kernel_stack);
            p->state = PROC_UNUSED;
            return NULL;
        }
    }

    /* 把代码和数据逐页拷进页框。页框之间不一定连续，必须按页算偏移。 */
    {
        uint32_t remaining = hdr->size;
        uint32_t offset    = 0;

        while (remaining > 0) {
            uint32_t page_index = offset / PAGE_SIZE;
            uint32_t in_page    = offset % PAGE_SIZE;
            uint32_t chunk      = PAGE_SIZE - in_page;

            if (chunk > remaining) {
                chunk = remaining;
            }
            memcpy((uint8_t*)p->image_frames[page_index] + in_page,
                   image + LXE_HEADER_SIZE + offset, chunk);

            offset    += chunk;
            remaining -= chunk;
        }
    }

    p->image_size = hdr->size + hdr->bss_size;
    p->memory_used = p->image_size + PROCESS_STACK_PAGES * PAGE_SIZE;
    p->entry      = USER_BASE + hdr->entry;

    /* --- 用户栈：铺在 4MB 用户区的顶端 --- */
    for (uint32_t i = 0; i < PROCESS_STACK_PAGES; i++) {
        void* frame = pmm_alloc_page();

        if (!frame) {
            free_user_resources(p);
            kfree(p->kernel_stack);
            p->state = PROC_UNUSED;
            return NULL;
        }
        p->stack_frames[i] = (uint32_t)frame;
        p->stack_pages++;

        memset(frame, 0, PAGE_SIZE);

        vaddr = USER_BASE + USER_REGION_SIZE - (PROCESS_STACK_PAGES - i) * PAGE_SIZE;
        if (!paging_map_user_page(p->user_table, vaddr, (uint32_t)frame, true)) {
            free_user_resources(p);
            kfree(p->kernel_stack);
            p->state = PROC_UNUSED;
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

    return p;
}

/* 从文件系统读出一个 LXE 并创建进程 */
static process_t* load_program(const char* filename, const char* args, int* err) {
    const hneofs_file_t* f;
    uint8_t* image;
    process_t* p;
    int32_t got;

    if (!hneofs_mounted()) {
        *err = -2;
        return NULL;
    }

    f = hneofs_lookup(filename);
    if (!f || f->type != HNEOFS_TYPE_FILE) {
        *err = -1;
        return NULL;
    }
    if (f->size < LXE_HEADER_SIZE || f->size > 1024 * 1024) {
        *err = -1;
        return NULL;
    }

    image = (uint8_t*)kmalloc(f->size);
    if (!image) {
        *err = -3;
        return NULL;
    }

    got = hneofs_read_file(f, image, f->size);
    if (got < 0) {
        kfree(image);
        *err = -4;
        return NULL;
    }

    /* 从构造到"参数拷好、映像缓冲区释放掉"之间必须一直关着中断。
     *
     * create_from_image_locked 返回时新任务已经是 PROC_READY 了，
     * 只要 irq_restore 一执行，时钟中断就能立刻把它切上去跑；而此刻它的
     * 命令行参数还没拷（getargs 会读到半成品），内核还在动那块映像缓冲区。
     * 这个窗口是真实存在的：串口插桩时抓到过切换就发生在参数拷贝之前
     * （[LP2] 和 [SW] 挤在同一行输出里）。
     *
     * 所以把参数拷贝和 kfree 一起放进关中断区间，等一切就绪再放它跑。
     * 这段没有磁盘 I/O，关中断的时间很短。
     */
    {
        uint32_t flags = irq_save();

        p = create_from_image_locked(filename, image, (uint32_t)got);

        /* 命令行参数交给进程自己保管，程序通过 getargs 系统调用取 */
        if (p && args) {
            strncpy(p->args, args, PROCESS_ARGS_MAX - 1);
            p->args[PROCESS_ARGS_MAX - 1] = '\0';
        }

        kfree(image);

        irq_restore(flags);
    }

    if (!p) {
        *err = -5;
        return NULL;
    }

    *err = 0;
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
    int32_t code;

    if (!p) {
        return err;
    }

    /* 丢掉可能在提示符处按下的 Ctrl+C，免得刚启动的程序立刻被杀 */
    (void)keyboard_take_ctrl_c();

    /* 等它跑完。cpu_halt 会让出 CPU，时钟中断随时可以切过去执行它。 */
    while (p->state != PROC_ZOMBIE) {
        cpu_halt();
        stack_guard("process_run");

        /* Ctrl+C 打断前台程序。
         * 这是把跑飞的程序拉回来的唯一办法：Shell 此刻正阻塞在这个循环里，
         * 没有别的地方能收命令。杀掉之后循环条件不成立，正常往下走。
         */
        if (keyboard_take_ctrl_c()) {
            process_kill(p->pid);
        }
    }

    code = p->exit_code;

    /* 回收槽位。
     *
     * 这里不能指望 reap_zombies 已经回收了内核栈：它只在时钟中断里回收
     * "当前不在跑"的僵尸，而 Shell 早在孩子退出那一刻就被切回来了，
     * 我们下面马上就把槽位标成 UNUSED，reap_zombies 再也不会看它一眼 ——
     * 那 8KB 内核栈就永远漏掉了（紧接着 alloc_slot 会把槽位 memset 掉，
     * 连指针一起丢）。所以在这里自己释放，和 reap_zombies 做的一样。
     *
     * 这一条不是洁癖：实测把这段去掉（故意留泄漏）之后，重复运行前台程序
     * 到第 8~9 次就会让虚拟机直接 Guru Meditation 崩掉，堆里的内核栈
     * 地址一路从 0x101F40 涨到 0x10FFE8。释放掉之后，同一个负载跑 25 次
     * 都没事（栈块被复用，堆不再单调增长）。
     *
     * 关中断是为了别让时钟中断插在"释放栈"和"标记 UNUSED"之间：
     * 否则 reap_zombies 可能对同一个指针再释放一次。
     */
    {
        uint32_t flags = irq_save();

        if (p->kernel_stack) {
            kfree(p->kernel_stack);
            p->kernel_stack      = NULL;
            p->kernel_stack_top  = 0;
            p->kernel_stack_size = 0;
        }
        p->state = PROC_UNUSED;

        irq_restore(flags);
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
        free_user_resources(p);

        irq_restore(flags);
    }

    return 0;
}
