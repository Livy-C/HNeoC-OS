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

/* 权限检查实现在文件系统那一节（因为 sys_write/sys_read 更靠前） */
static bool access_ok(const hneofs_file_t* f, uint32_t want);
static bool parent_access_ok(const char* path, uint32_t want);

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
     *
     * 系统调用门是 interrupt gate，进来时 IF=0，原来这里全程关着中断，
     * 注释说"PIO 写盘只需要几毫秒" —— 那是按写几百字节算的。用户一次可以
     * 请求到 4MB（user_range_ok 的上限），PIO 搬这么多要几十毫秒到几秒，
     * 那段时间键盘收不到键、100Hz 时钟也不走，uptime 直接偏掉。
     *
     * 现在改成"开着中断但不换任务"：中断照常进来收键、数滴答，sched_lock
     * 保证调度器不切走 —— 文件系统的全局状态还在我们手里，切走就可能
     * 被另一个任务改坏。
     */
    if (!e) {
        return -1;
    }

    /* 权限：要写就得是属主（或 other 写位），root 例外 */
    {
        const hneofs_file_t* wf = hneofs_file(e->file_index);

        if (!wf || !access_ok(wf, HNEOFS_ACCESS_WRITE)) {
            return HNEOFS_ERR_PERM;
        }
    }

    sched_lock();
    enable_interrupts();

    rc = hneofs_write_at(e->file_index, e->offset, (const void*)buf, len);

    disable_interrupts();
    sched_unlock();

    if (rc < 0) {
        return rc;
    }
    e->offset += (uint32_t)rc;
    return rc;
}

/* ------------------------------------------------------------
 * 权限
 *
 * 所有碰文件系统的系统调用都要过这一关。进程的 uid 由登录写进
 * process_t，子进程继承，所以"谁在跑这个程序"是明确的。
 * root（uid 0）一律放行。
 * ------------------------------------------------------------ */

static bool access_ok(const hneofs_file_t* f, uint32_t want) {
    return hneofs_access(f, process_uid(), want);
}

/* 在 path 的父目录上做权限检查 —— 创建/删除/建目录都要先过这里。
 *
 * 根目录没有自己的目录项，所以"往根里写"一律要求 root：这和 Unix 里
 * / 属于 root 是一个意思。
 */
static bool parent_access_ok(const char* path, uint32_t want) {
    char dir[HNEOFS_PATH_MAX];
    int  n = 0;
    int  cut = -1;
    int32_t idx;

    if (!path) {
        return false;
    }

    for (int i = 0; path[i] && i < (int)sizeof(dir) - 1; i++) {
        dir[n++] = path[i];
        if (path[i] == '/') {
            cut = n;              /* 记住最后一个 '/' 之后的位置 */
        }
    }
    dir[n] = '\0';

    if (cut < 0) {
        idx = hneofs_resolve(".");            /* 父目录就是工作目录 */
    } else {
        dir[cut] = '\0';                      /* 砍掉最后一段 */
        if (dir[0] == '\0') {
            return process_uid() == 0;        /* 形如 "/x"：父目录是根 */
        }
        idx = hneofs_resolve(dir);
    }

    if (idx < 0) {
        return process_uid() == 0;            /* 解析不出来就按根处理 */
    }
    return access_ok(hneofs_file((uint32_t)idx), want);
}

/* 每个用户对自己 home 下的东西有完全权限；这也是 mkdir 之类能用的前提 */

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
        /* 新建文件：父目录必须可写 */
        if (!parent_access_ok(path, HNEOFS_ACCESS_WRITE)) {
            return HNEOFS_ERR_PERM;
        }
        index = hneofs_create(path);      /* 父目录必须已经存在 */
        if (index < 0) {
            return index;
        }
    } else {
        if (hneofs_file((uint32_t)index)->type == HNEOFS_TYPE_DIR) {
            return HNEOFS_ERR_ISDIR;      /* 目录不能当文件打开 */
        }

        /* 打开已有文件：按要做的事检查权限。
         * 注意这里用的是 HNEOFS_ACCESS_*（要读/写/执行），不是 HNEOFS_O_*（打开标志），
         * 也不是 HNEOFS_MODE_*（权限掩码） */
        {
            uint32_t want = 0;

            if (flags & (HNEOFS_O_WRONLY | HNEOFS_O_RDWR)) {
                want |= HNEOFS_ACCESS_WRITE;
            }
            if ((flags & HNEOFS_O_WRONLY) == 0) {
                want |= HNEOFS_ACCESS_READ;   /* 只读和读写都要读权限 */
            }
            if (want != 0 && !access_ok(hneofs_file((uint32_t)index), want)) {
                return HNEOFS_ERR_PERM;
            }
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
    if (!access_ok(f, HNEOFS_ACCESS_READ)) {
        return HNEOFS_ERR_PERM;
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
    /* 删除要能写父目录：写不了 /etc 就不能删 /etc/passwd */
    if (!parent_access_ok(path, HNEOFS_ACCESS_WRITE)) {
        return HNEOFS_ERR_PERM;
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

/* whoami：把 uid 和管理员标志一起带回去。
 *
 * 低位 16 位是 uid，bit16 是"是不是管理员"。本来想拆成两个系统调用，
 * 但"我是谁"永远是一起问的，一个调用省一次陷入。
 * 用户态用 user/lib 的 getuid() / is_admin() 拆开，别自己抠位。
 */
static int32_t sys_whoami(void) {
    return (int32_t)((process_uid() & 0xFFFFu) |
                     (process_is_admin() ? 0x10000u : 0u));
}

/* chmod：改权限位。
 *
 * 只有 root / 管理员 / 文件属主能改。mode 只取低 9 位（属主三位 + 其他人
 * 三位，中间那组位留着不用）。
 *
 * 装包必须要它：hpm 把 .lxe 写进 /bin 时，新建的文件一律是默认的 0644，
 * 没有执行位的话非 root 用户根本跑不起来。
 * 顺手把执行位同步到 HNEOFS_FLAG_EXEC —— mkfs 打包时也是这么做的，
 * 不然 ls 里显示的 kind 会和权限位对不上。
 */
static int32_t sys_chmod(uint32_t path_ptr, uint32_t mode) {
    char path[HNEOFS_PATH_MAX];
    hneofs_file_t* f;
    int32_t idx;

    if (!hneofs_mounted()) {
        return HNEOFS_ERR_MOUNT;
    }
    if (!copy_user_string(path_ptr, path, sizeof(path))) {
        return HNEOFS_ERR_NAME;
    }

    idx = hneofs_resolve(path);
    if (idx < 0) {
        return HNEOFS_ERR_NOENT;
    }

    f = hneofs_file_mut((uint32_t)idx);
    if (!f) {
        return HNEOFS_ERR_NOENT;
    }

    if (!process_is_admin() && f->uid != process_uid()) {
        return HNEOFS_ERR_PERM;
    }

    mode &= 0x1FFu;
    f->mode = mode;
    if (mode & (HNEOFS_MODE_OWNER_EXEC | HNEOFS_MODE_OTHER_EXEC)) {
        f->flags |= HNEOFS_FLAG_EXEC;
    } else {
        f->flags &= ~HNEOFS_FLAG_EXEC;
    }

    return hneofs_sync() ? 0 : HNEOFS_ERR_IO;
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

    /* 建目录要求父目录可写 */
    if (!parent_access_ok(path, HNEOFS_ACCESS_WRITE)) {
        return HNEOFS_ERR_PERM;
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

/* 把当前进程的工作目录拷给用户程序。
 *
 * 有了它，用户态的 ls / vi 才能知道自己"站在哪儿"：
 * 内核 Shell 的提示符是内核拼的，ring 3 的程序只能靠这个系统调用问。
 */
static int32_t sys_getcwd(uint32_t buf, uint32_t max) {
    const char* cwd = process_cwd();
    uint32_t n = 0;

    if (!buf || max == 0) {
        return -1;
    }
    if (!user_range_ok(buf, max)) {
        return -1;
    }
    if (!cwd || cwd[0] == '\0') {
        cwd = "/";
    }

    while (n + 1 < max && cwd[n]) {
        ((char*)buf)[n] = cwd[n];
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
    if (!access_ok(f, HNEOFS_ACCESS_READ)) {
        return HNEOFS_ERR_PERM;
    }

    want = f->size;
    if (want > max - 1) {
        want = max - 1;   /* 留一个字节给结尾的 0 */
    }

    int32_t got;

    /* 同 sys_write：读一份大文件也可能要几十毫秒，别把中断关死 */
    sched_lock();
    enable_interrupts();

    got = hneofs_read_at(f, 0, (void*)buf, want);

    disable_interrupts();
    sched_unlock();

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

        case SYS_GETCWD:
            ret = sys_getcwd(regs->ebx, regs->ecx);
            break;

        case SYS_CHMOD:
            ret = sys_chmod(regs->ebx, regs->ecx);
            break;

        case SYS_WHOAMI:
            ret = sys_whoami();
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
