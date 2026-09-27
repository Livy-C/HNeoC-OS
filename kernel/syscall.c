#include "../include/syscall.h"
#include "../include/idt.h"
#include "../include/paging.h"
#include "../include/process.h"
#include "../include/keyboard.h"
#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/hneofs.h"
#include "../include/string.h"
#include "../include/ports.h"
#include "../include/kernel.h"
#include "../include/gdt.h"

/* kernel/arch.asm 里的 int 0x80 存根 */
extern void isr128(void);

static uint32_t total_syscalls = 0;

/* 定义在后面，sys_read 里要用（fd 0 是标准输入） */
static int32_t sys_getchar(void);

uint32_t syscall_total(void) {
    return total_syscalls;
}

/* ------------------------------------------------------------
 * 用户指针校验
 *
 * 用户传进来的地址必须先确认落在自己的地址空间里、而且对应的页
 * 确实映射过。否则内核会因为一个野指针直接缺页崩掉。
 * ------------------------------------------------------------ */
static bool user_range_ok(uint32_t addr, uint32_t len) {
    process_t* p = process_current();
    uint32_t first, last;

    if (!p || !p->user_table) {
        return false;
    }
    if (addr < USER_BASE) {
        return false;
    }
    if (addr + len < addr) {
        return false;   /* 加法溢出 */
    }
    if (addr + len > USER_BASE + USER_REGION_SIZE) {
        return false;
    }
    if (len == 0) {
        return true;
    }

    /* 逐页确认页表项是"存在"的 */
    first = (addr - USER_BASE) / PAGE_4KB_SIZE;
    last  = (addr + len - 1 - USER_BASE) / PAGE_4KB_SIZE;

    for (uint32_t i = first; i <= last && i < 1024; i++) {
        if ((p->user_table[i] & PAGE_PRESENT) == 0) {
            return false;
        }
    }
    return true;
}

/* 把用户空间的字符串拷进内核缓冲区，最多 max-1 个字符 */
static bool copy_user_string(uint32_t uaddr, char* out, uint32_t max) {
    if (max == 0) {
        return false;
    }

    for (uint32_t i = 0; i < max - 1; i++) {
        /* 每读一个字节都确认它所属的页是映射过的 */
        if (!user_range_ok(uaddr + i, 1)) {
            out[0] = '\0';
            return false;
        }
        char c = *(const char*)(uaddr + i);
        out[i] = c;
        if (c == '\0') {
            return true;
        }
    }
    out[max - 1] = '\0';
    return true;
}

/* ------------------------------------------------------------
 * 各个系统调用的实现
 * ------------------------------------------------------------ */

static int32_t sys_write(int32_t fd, uint32_t buf, uint32_t len) {
    fd_entry_t* e;
    int32_t rc;

    if (!user_range_ok(buf, len)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    /* fd 0/1/2 是控制台 */
    e = process_fd_get((int)fd);
    if (e && e->file_index == FD_CONSOLE) {
        if (len > 4096) {
            len = 4096;   /* 一次别写太多，防止刷爆屏幕 */
        }
        /* 用带长度的版本：缓冲区里可能有 0 字节，不能按 C 字符串处理。
         * ANSI 转义序列由 vga 那层解释，所以用户程序可以直接画界面。
         */
        vga_write_n((const char*)buf, len);
        return (int32_t)len;
    }

    /* 普通文件：写进文件系统。
     * 注意这里全程关着中断（系统调用门是 interrupt gate），
     * 而 PIO 写盘只需要几毫秒，不会把时钟拖垮。
     */
    if (!e) {
        return -1;
    }

    rc = hneofs_write_at(e->file_index, e->offset, (const void*)buf, len);
    if (rc < 0) {
        return rc;
    }
    e->offset += (uint32_t)rc;
    return rc;
}

/* ------------------------------------------------------------
 * 文件描述符相关的系统调用
 * ------------------------------------------------------------ */

/* 打开或创建一个文件。name 是路径，可以是 "/bin/ls" 也可以是 "readme.txt"
 * （没有工作目录的概念，相对路径一律从根目录开始算）
 */
static int32_t sys_open(uint32_t name_ptr, uint32_t flags) {
    char path[HNEOFS_PATH_MAX];
    int32_t index;
    const hneofs_file_t* f;
    uint32_t offset = 0;

    if (!hneofs_mounted()) {
        return HNEOFS_ERR_MOUNT;
    }
    if (!copy_user_string(name_ptr, path, sizeof(path))) {
        return HNEOFS_ERR_NAME;
    }

    index = hneofs_resolve(path);

    if (index < 0) {
        if ((flags & HNEOFS_O_CREAT) == 0) {
            return HNEOFS_ERR_NOENT;
        }
        index = hneofs_create(path);      /* 父目录必须已经存在 */
        if (index < 0) {
            return index;
        }
    } else {
        if (hneofs_file((uint32_t)index)->type == HNEOFS_TYPE_DIR) {
            return HNEOFS_ERR_ISDIR;      /* 目录不能当文件打开 */
        }
        if (flags & HNEOFS_O_TRUNC) {
            hneofs_truncate((uint32_t)index, 0);
        }
    }

    f = hneofs_file((uint32_t)index);
    if (!f) {
        return HNEOFS_ERR_NOENT;
    }

    if (flags & HNEOFS_O_APPEND) {
        offset = f->size;
    }

    return process_fd_alloc((uint32_t)index, flags, offset);
}

static int32_t sys_read(int32_t fd, uint32_t buf, uint32_t len) {
    fd_entry_t* e = process_fd_get((int)fd);
    const hneofs_file_t* f;
    int32_t got;

    if (!e) {
        return -1;
    }

    /* fd 0 是标准输入：从键盘读一个字符 */
    if (e->file_index == FD_CONSOLE) {
        return sys_getchar();
    }
    if (!user_range_ok(buf, len)) {
        return -1;
    }

    f = hneofs_file(e->file_index);
    if (!f) {
        return -1;
    }
    if (e->offset >= f->size) {
        return 0;   /* 到文件末尾了 */
    }

    got = hneofs_read_at(f, e->offset, (void*)buf, len);
    if (got < 0) {
        return -1;
    }
    e->offset += (uint32_t)got;
    return got;
}

static int32_t sys_close(int32_t fd) {
    return process_fd_close((int)fd) ? 0 : -1;
}

static int32_t sys_lseek(int32_t fd, int32_t offset, int32_t whence) {
    fd_entry_t* e = process_fd_get((int)fd);
    const hneofs_file_t* f;
    int32_t base;
    int32_t target;

    if (!e || e->file_index == FD_CONSOLE) {
        return -1;
    }

    f = hneofs_file(e->file_index);
    if (!f) {
        return -1;
    }

    switch (whence) {
        case 0:  base = 0;                  break;   /* SEEK_SET */
        case 1:  base = (int32_t)e->offset; break;   /* SEEK_CUR */
        case 2:  base = (int32_t)f->size;   break;   /* SEEK_END */
        default: return -1;
    }

    target = base + offset;

    /* 上界必须卡住，不能只挡住负数。
     *
     * 偏移量会原样存进 fd，之后的 write 直接把 start_lba + offset/512 当作 LBA
     * 来用：一个越界的偏移量就等于"往任意扇区写"的原语，能写到超级块、文件表
     * 或者别的文件的数据上，而且只更新本次文件自己的 size，事后完全看不出来。
     * 读也一样，能读到磁盘任意位置。这个文件系统没有稀疏文件的概念，
     * "越过文件末尾"没有任何合法用途，直接拒绝。
     */
    if (target < 0 || target > (int32_t)f->size) {
        return -1;
    }

    e->offset = (uint32_t)target;
    return target;
}

static int32_t sys_unlink(uint32_t name_ptr) {
    char path[HNEOFS_PATH_MAX];

    if (!hneofs_mounted()) {
        return HNEOFS_ERR_MOUNT;
    }
    if (!copy_user_string(name_ptr, path, sizeof(path))) {
        return HNEOFS_ERR_NAME;
    }
    return hneofs_unlink(path) ? 0 : HNEOFS_ERR_NOENT;
}

/* 目前只回一个文件大小，够用户程序判断读到哪里为止了 */
static int32_t sys_fstat(int32_t fd) {
    fd_entry_t* e = process_fd_get((int)fd);
    const hneofs_file_t* f;

    if (!e || e->file_index == FD_CONSOLE) {
        return -1;
    }
    f = hneofs_file(e->file_index);
    return f ? (int32_t)f->size : -1;
}

static int32_t sys_fsync(int32_t fd) {
    (void)fd;
    return hneofs_sync() ? 0 : -1;
}

/* ------------------------------------------------------------
 * 全屏程序用的三个
 * ------------------------------------------------------------ */

/* 原始按键：和 getchar 的区别是不会把方向键、翻页键过滤掉，
 * 全屏编辑器要靠这些键做光标移动
 */
static int32_t sys_getkey(void) {
    int32_t c;

    enable_interrupts();
    c = keyboard_getchar();
    disable_interrupts();

    return c;
}

/* 屏幕尺寸打包成 (行 << 8) | 列 */
static int32_t sys_winsize(void) {
    return (int32_t)((VGA_HEIGHT << 8) | VGA_WIDTH);
}

static int32_t sys_sbrk(int32_t increment) {
    return process_sbrk(increment);
}

/* 创建一个目录 */
static int32_t sys_mkdir(uint32_t path_ptr) {
    char path[HNEOFS_PATH_MAX];

    if (!hneofs_mounted()) {
        return HNEOFS_ERR_MOUNT;
    }
    if (!copy_user_string(path_ptr, path, sizeof(path))) {
        return HNEOFS_ERR_NAME;
    }

    return hneofs_mkdir(path);
}

/* 把内核替程序保存的命令行参数（命令名之后的部分）拷给用户 */
static int32_t sys_getargs(uint32_t buf, uint32_t max) {
    process_t* p = process_current();
    uint32_t n = 0;

    if (!p || !buf || max == 0) {
        return 0;
    }
    if (!user_range_ok(buf, max)) {
        return -1;
    }

    while (n + 1 < max && p->args[n]) {
        ((char*)buf)[n] = p->args[n];
        n++;
    }
    ((char*)buf)[n] = '\0';
    return (int32_t)n;
}

static int32_t sys_getchar(void) {
    int32_t c;

    /* 系统调用门是 interrupt gate，进来时 IF=0。
     * 阻塞等待必须临时打开中断，否则键盘中断永远进不来，直接死锁。
     * 等待结束后立刻关回去，保证系统调用返回路径上的 sched_tick
     * 不会被时钟中断嵌套重入。
     */
    enable_interrupts();

    /* 功能键（方向键、翻页键等）编码大于 255，用户程序现在只处理
     * 普通字符，所以直接跳过继续等。
     */
    do {
        c = keyboard_getchar();
    } while (c > 255);

    disable_interrupts();

    return c;
}

static void sys_sleep(uint32_t ms) {
    /* 同理：等滴答的时候要开着中断 */
    uint32_t target = timer_get_ticks() + (ms / 10);

    enable_interrupts();
    while (timer_get_ticks() < target) {
        cpu_halt();
    }
    disable_interrupts();
}

static int32_t sys_readfile(uint32_t name_ptr, uint32_t buf, uint32_t max) {
    char name[HNEOFS_NAME_MAX];
    const hneofs_file_t* f;
    uint32_t want;

    if (!copy_user_string(name_ptr, name, sizeof(name))) {
        return -1;
    }

    /* max 至少要有 2：下面要留一个字节写结尾的 '\0'。
     *
     * 不能只靠 user_range_ok 把关 —— 它在 len == 0 时会直接返回 true，
     * 而 max == 0 时 want 会被算成 (max - 1) == 0xFFFFFFFF，那个钳位形同虚设，
     * 内核就会把整份文件写进一个完全没校验过的用户地址（未映射页 → 内核态
     * 缺页 → 整机 System halted）。
     */
    if (max < 2) {
        return -1;
    }
    if (!user_range_ok(buf, max)) {
        return -1;
    }
    if (!hneofs_mounted()) {
        return -1;
    }

    f = hneofs_lookup(name);
    if (!f || f->type != HNEOFS_TYPE_FILE) {
        return -1;
    }

    want = f->size;
    if (want > max - 1) {
        want = max - 1;   /* 留一个字节给结尾的 0 */
    }

    int32_t got = hneofs_read_at(f, 0, (void*)buf, want);
    if (got < 0) {
        return -1;
    }
    ((char*)buf)[got] = '\0';
    return got;
}

/* 列出目录 path 里的第 index 个条目，把名字拷进用户缓冲区。
 * 返回值：文件大小；如果是个目录，最高位置 1（SYS_DIR_MARK）。
 * index 越界返回 -1。
 */
static int32_t sys_listdir(uint32_t path_ptr, uint32_t index,
                           uint32_t name_buf, uint32_t name_max) {
    char path[HNEOFS_PATH_MAX];
    uint32_t dir = HNEOFS_ROOT;
    int32_t idx;
    const hneofs_file_t* f;
    uint32_t i = 0;

    if (!hneofs_mounted() || name_max == 0) {
        return -1;
    }
    if (!copy_user_string(path_ptr, path, sizeof(path))) {
        return -1;
    }
    if (!user_range_ok(name_buf, name_max)) {
        return -1;
    }

    /* "/" 或者空串表示根目录；根目录没有自己的目录项 */
    {
        const char* p = path;
        bool only_slashes = true;

        while (*p) {
            if (*p != '/') {
                only_slashes = false;
                break;
            }
            p++;
        }
        if (!only_slashes) {
            int32_t r = hneofs_resolve(path);

            if (r < 0) {
                return -1;
            }
            if (hneofs_file((uint32_t)r)->type != HNEOFS_TYPE_DIR) {
                return -1;   /* 不是目录 */
            }
            dir = (uint32_t)r;
        }
    }

    idx = hneofs_child_at(dir, index);
    if (idx < 0) {
        return -1;   /* 没有更多条目了 */
    }

    f = hneofs_file((uint32_t)idx);
    if (!f) {
        return -1;
    }

    for (; i + 1 < name_max && f->name[i]; i++) {
        ((char*)name_buf)[i] = f->name[i];
    }
    ((char*)name_buf)[i] = '\0';

    if (f->type == HNEOFS_TYPE_DIR) {
        return (int32_t)(f->size | SYS_DIR_MARK);
    }
    return (int32_t)f->size;
}

/* ------------------------------------------------------------
 * 总入口
 * ------------------------------------------------------------ */
void syscall_handler(registers_t* regs) {
    uint32_t num = regs->eax;
    int32_t  ret = 0;

    total_syscalls++;

    {
        process_t* p = process_current();
        if (p) {
            p->syscall_count++;
        }
    }

    switch (num) {
        case SYS_EXIT:
            /* process_exit_current 不会返回，它直接跳回 Shell */
            process_exit_current((int32_t)regs->ebx);
            break;

        case SYS_WRITE:
            ret = sys_write((int32_t)regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_GETCHAR:
            ret = sys_getchar();
            break;

        case SYS_GETPID: {
            process_t* p = process_current();
            ret = p ? (int32_t)p->pid : 0;
            break;
        }

        case SYS_YIELD:
            /* 调度器还没做，先什么都不做 */
            ret = 0;
            break;

        case SYS_SLEEP:
            sys_sleep(regs->ebx);
            ret = 0;
            break;

        case SYS_UPTIME:
            ret = (int32_t)timer_get_uptime_seconds();
            break;

        case SYS_TICKS:
            ret = (int32_t)timer_get_ticks();
            break;

        case SYS_COLOR:
            /* 只允许前 16 种颜色，避免用户传进越界值 */
            vga_set_color((vga_color)(regs->ebx & 0x0F),
                          (vga_color)(regs->ecx & 0x0F));
            ret = 0;
            break;

        case SYS_CLEAR:
            vga_clear();
            ret = 0;
            break;

        case SYS_READFILE:
            ret = sys_readfile(regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_LISTDIR:
            ret = sys_listdir(regs->ebx, regs->ecx, regs->edx, regs->esi);
            break;

        case SYS_MKDIR:
            ret = sys_mkdir(regs->ebx);
            break;

        case SYS_GETARGS:
            ret = sys_getargs(regs->ebx, regs->ecx);
            break;

        case SYS_OPEN:
            ret = sys_open(regs->ebx, regs->ecx);
            break;

        case SYS_CLOSE:
            ret = sys_close((int32_t)regs->ebx);
            break;

        case SYS_READ:
            ret = sys_read((int32_t)regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_LSEEK:
            ret = sys_lseek((int32_t)regs->ebx, (int32_t)regs->ecx,
                            (int32_t)regs->edx);
            break;

        case SYS_UNLINK:
            ret = sys_unlink(regs->ebx);
            break;

        case SYS_FSTAT:
            ret = sys_fstat((int32_t)regs->ebx);
            break;

        case SYS_FSYNC:
            ret = sys_fsync((int32_t)regs->ebx);
            break;

        case SYS_GETKEY:
            ret = sys_getkey();
            break;

        case SYS_WINSIZE:
            ret = sys_winsize();
            break;

        case SYS_SBRK:
            ret = sys_sbrk((int32_t)regs->ebx);
            break;

        default:
            ret = -1;   /* 未知系统调用 */
            break;
    }

    /* 返回值写回保存的 EAX；isr_common_stub 里的 popa 会把它恢复给用户程序 */
    regs->eax = (uint32_t)ret;
}

void syscall_init(void) {
    /* 0xEE = 存在 | DPL=3 | 32 位 interrupt gate
     *
     * DPL=3 是必须的，否则 ring 3 执行 int 0x80 会触发保护性错误。
     *
     * 用 interrupt gate 而不是 trap gate：进门时 IF 会被硬件清零，
     * 这样系统调用返回路径上的 sched_tick 不会被时钟中断嵌套重入。
     * 需要阻塞等待的系统调用（getchar / sleep）会自己临时开中断。
     */
    idt_set_gate(0x80, (uint32_t)isr128, GDT_KERNEL_CODE, 0xEE);
    register_interrupt_handler(0x80, syscall_handler);
    total_syscalls = 0;
}
