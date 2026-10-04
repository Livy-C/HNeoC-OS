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
#include "../include/ata.h"
#include "../include/heap.h"
#include "../include/pmm.h"
#include "../include/user.h"
#include "../include/sysinfo.h"

/* kernel/arch.asm 里的 int 0x80 存根 */
extern void isr128(void);

static uint32_t total_syscalls = 0;

/* 定义在后面，sys_read 里要用（fd 0 是标准输入） */
static int32_t sys_getchar(void);

/* 进程控制（实现在后面，要用 copy_user_string） */
static int32_t sys_exec(uint32_t path_ptr, uint32_t args_ptr, registers_t* regs);
static int32_t sys_wait(int32_t pid, uint32_t status_ptr, uint32_t options);
static int32_t sys_login(uint32_t name_ptr, uint32_t pass_ptr, uint32_t info_ptr);
static int32_t sys_gettask(uint32_t index, uint32_t buf_ptr);
static int32_t sys_sysinfo(uint32_t buf_ptr);

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

/* ------------------------------------------------------------
 * fork / exec / wait
 * ------------------------------------------------------------ */

/* exec：路径和参数都先拷进内核缓冲区再动手。
 *
 * 这一步绝不能省。exec 一旦成功，当前进程的地址空间就换成新的了，
 * 用户空间里那个字符串指针指向的页可能已经还给 PMM 了 —— 内核要是
 * 还拿着它去开文件，读到的是别人的内容（或者直接踩进没映射的地址）。
 * 拷成内核缓冲区之后，process_exec 拿着 p 的地址空间翻来覆去都不影响它。
 */
static int32_t sys_exec(uint32_t path_ptr, uint32_t args_ptr, registers_t* regs) {
    char path[128];
    char args[PROCESS_ARGS_MAX];

    if (!copy_user_string(path_ptr, path, sizeof(path))) {
        return -1;
    }

    args[0] = '\0';
    if (args_ptr != 0 && !copy_user_string(args_ptr, args, sizeof(args))) {
        return -1;
    }

    /* 成功的话这里不会返回：process_exec 已经把中断帧改成新程序的入口了 */
    return (int32_t)process_exec(regs, path, args);
}

/* wait：等一个子进程退出并回收它。status 写成子进程的退出码。
 *
 * options 的 WNOHANG 位表示"不阻塞"：如果一个已经退出的孩子都没有，
 * 立刻返回 0。用户态 Shell 靠它顺手收掉后台任务留下的僵尸。
 */
static int32_t sys_wait(int32_t pid, uint32_t status_ptr, uint32_t options) {
    int32_t code = 0;
    int rc;

    if (status_ptr != 0 && !user_range_ok(status_ptr, sizeof(int32_t))) {
        return -1;
    }

    /* 系统调用门是 interrupt gate，进来时 IF=0；而 process_wait_child
     * 靠 cpu_halt() 让出 CPU 等孩子退出 —— 关着中断执行 hlt 就是永久停机，
     * 时钟中断永远进不来，调度器也不会切到那个孩子。
     * 所以和 sys_getchar / sys_sleep 一样，阻塞等待必须临时打开中断。
     * （WNOHANG 这条路不会停，但也一并开着，省得两套写法。） */
    enable_interrupts();
    rc = process_wait_child(pid, (options & WNOHANG) ? true : false, &code);
    disable_interrupts();

    if (rc < 0) {
        return -1;
    }

    if (status_ptr != 0) {
        *(int32_t*)status_ptr = code;
    }
    return rc;
}

/* ------------------------------------------------------------
 * Shell 从内核搬到 ring 3 之后要用到的几个
 * ------------------------------------------------------------ */

/* login：校验用户名和口令，成功就把当前进程的身份和工作目录改掉。
 *
 * 校验一定在内核里做：口令表（djb2 哈希）不能交给用户程序去比对，
 * 否则任何程序都能自己宣布"我是 root"。用户程序只负责收集这两行输入和
 * 显示结果，它拿到的是一个已经决定好的 uid。
 */
static int32_t sys_login(uint32_t name_ptr, uint32_t pass_ptr, uint32_t info_ptr) {
    char name[LOGIN_NAME_MAX];
    char pass[64];
    logininfo_t info;
    uint32_t uid = 0;
    bool admin = false;

    if (!user_range_ok(info_ptr, sizeof(logininfo_t))) {
        return -1;
    }
    if (!copy_user_string(name_ptr, name, sizeof(name))) {
        return -1;
    }
    if (!copy_user_string(pass_ptr, pass, sizeof(pass))) {
        return -1;
    }

    memset(&info, 0, sizeof(info));
    if (!user_authenticate(name, pass, &uid, &admin,
                           info.home, LOGIN_HOME_MAX)) {
        return -2;   /* 用户名或口令不对 */
    }

    info.uid   = uid;
    info.admin = admin ? 1 : 0;
    strncpy(info.name, name, LOGIN_NAME_MAX - 1);

    {
        process_t* p = process_current();

        if (p) {
            p->uid   = uid;
            p->admin = admin;
        }
        process_set_cwd((info.home[0] != '\0') ? info.home : "/");
    }
    user_set_current_name(name);

    /* 结构体整块拷回用户空间。
     * 这里不用 memcpy 到用户指针上直接写：copy_to_user 那套在别处也没做，
     * 一律走"内核缓冲 + 逐字节搬"的老路子，和 sys_getcwd 保持一致。 */
    {
        const uint8_t* src = (const uint8_t*)&info;
        uint8_t* dst = (uint8_t*)info_ptr;

        for (uint32_t i = 0; i < sizeof(logininfo_t); i++) {
            dst[i] = src[i];
        }
    }
    return 0;
}

/* gettask：把进程表第 index 项填进用户的 taskinfo_t。
 *
 * 和 listdir 一个套路：调用方从 0 开始一个一个拿，返回 -1 表示没有了。
 * 中间的空槽位直接跳过（不然 Shell 得自己知道过滤规则）。
 */
static int32_t sys_gettask(uint32_t index, uint32_t buf_ptr) {
    taskinfo_t info;
    process_t* p = process_get(index);

    if (!p || !user_range_ok(buf_ptr, sizeof(taskinfo_t))) {
        return -1;
    }
    if (p->state == PROC_UNUSED) {
        return -1;   /* 拿不到有效项：告诉调用方到此为止 */
    }
    /* process_get 在越界时返回 NULL，所以"表尾"和"空槽"都归到 -1；
     * 空槽后面还可能有人，但进程表是从 0 紧凑使用的，不需要担心。 */

    memset(&info, 0, sizeof(info));
    info.pid        = p->pid;
    info.ppid       = p->parent_pid;
    info.state      = (uint32_t)p->state;
    info.cpu_ticks  = p->cpu_ticks;
    info.syscalls   = p->syscall_count;
    info.memory     = p->memory_used + PROCESS_KERNEL_STACK_SIZE;
    info.start_tick = p->start_tick;
    info.is_user    = p->is_user ? 1 : 0;
    strncpy(info.name, p->name, SYSINFO_NAME_MAX - 1);

    {
        uint8_t* dst = (uint8_t*)buf_ptr;
        uint8_t* src = (uint8_t*)&info;

        for (uint32_t i = 0; i < sizeof(taskinfo_t); i++) {
            dst[i] = src[i];
        }
    }
    return 0;
}

/* sysinfo：一整张系统状态快照，mem / heap / fsstat / diskinfo 四个
 * 内置命令需要的东西一次给全。 */
static int32_t sys_sysinfo(uint32_t buf_ptr) {
    sysinfo_t s;
    const hneofs_super_t* super = hneofs_super();
    uint32_t total = pmm_total_frames();
    uint32_t used  = pmm_used_frames();

    if (!user_range_ok(buf_ptr, sizeof(sysinfo_t))) {
        return -1;
    }

    memset(&s, 0, sizeof(s));

    s.mem_total_kb = total * (PAGE_SIZE / 1024);
    s.mem_used_kb  = used  * (PAGE_SIZE / 1024);
    s.mem_free_kb  = (total - used) * (PAGE_SIZE / 1024);

    heap_stats(&s.heap_total, &s.heap_used, &s.heap_free, &s.heap_blocks);
    s.heap_start = heap_region_start();

    s.uptime_sec = timer_get_uptime_seconds();
    s.ticks      = timer_get_ticks();
    s.tick_hz    = 100;
    s.syscalls   = syscall_total();

    s.procs_live  = process_live_count();
    s.procs_total = process_total_created();

    s.disk_sectors = 0;
    {
        const ata_device_t* dev = ata_get_device(0);

        if (dev) {
            s.disk_sectors = dev->sectors;
        }
    }
    s.disk_reads   = ata_sectors_read();
    s.disk_writes  = ata_sectors_written();

    if (super) {
        /* 目录项总数和空洞无关：数一遍真正有效的项 */
        uint32_t live = 0;

        for (uint32_t i = 0; i < super->file_count; i++) {
            const hneofs_file_t* f = hneofs_file(i);

            if (f && f->name[0] != '\0') {
                live++;
            }
        }
        s.fs_entries    = super->file_count;
        s.fs_live       = live;
        s.fs_data_lba   = super->data_lba;
        s.fs_free_lba   = super->free_lba;
        s.fs_free_bytes = hneofs_free_bytes();
    }

    s.screen_rows = VGA_HEIGHT;
    s.screen_cols = VGA_WIDTH;

    {
        uint8_t* dst = (uint8_t*)buf_ptr;
        uint8_t* src = (uint8_t*)&s;

        for (uint32_t i = 0; i < sizeof(sysinfo_t); i++) {
            dst[i] = src[i];
        }
    }
    return 0;
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

        case SYS_FORK:
            ret = process_fork(regs);
            break;

        case SYS_EXEC:
            /* 注意：成功时它不返回，regs 已经被改成新程序的入口了 */
            ret = sys_exec(regs->ebx, regs->ecx, regs);
            break;

        case SYS_WAIT:
            ret = sys_wait((int32_t)regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_LOGIN:
            /* 名字、口令在 sys_login 里才从用户空间拷进来 —— 这里只是转发 */
            ret = sys_login(regs->ebx, regs->ecx, regs->edx);
            break;

        case SYS_CHDIR: {
            char path[128];

            if (!copy_user_string(regs->ebx, path, sizeof(path))) {
                ret = -1;
                break;
            }
            {
                int32_t idx = hneofs_resolve(path);

                if (idx < 0) {
                    ret = -2;   /* 没有这个路径 */
                } else if (hneofs_file((uint32_t)idx)->type != HNEOFS_TYPE_DIR) {
                    ret = -3;   /* 不是目录 */
                } else {
                    process_set_cwd(path);
                    ret = 0;
                }
            }
            break;
        }

        case SYS_KILL:
            ret = process_kill(regs->ebx);
            break;

        case SYS_GETTASK:
            ret = sys_gettask(regs->ebx, regs->ecx);
            break;

        case SYS_SYSINFO:
            ret = sys_sysinfo(regs->ebx);
            break;

        case SYS_USERADD: {
            char name[LOGIN_NAME_MAX];
            char pass[64];

            if (!process_is_admin()) {
                ret = -4;   /* 不是管理员 */
                break;
            }
            if (!copy_user_string(regs->ebx, name, sizeof(name)) ||
                !copy_user_string(regs->ecx, pass, sizeof(pass))) {
                ret = -1;
                break;
            }
            ret = user_add(name, pass, regs->edx != 0);
            break;
        }

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
