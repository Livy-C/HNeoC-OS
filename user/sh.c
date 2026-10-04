/* sh.c - HNeoC 的用户态 Shell（ring 3）
 *
 * 这个文件就是"把 Shell 从内核搬出来"的产物。原来提示符、行编辑、命令表、
 * PATH 查找全在 kernel/shell.c 里，跑在 ring 0；现在它们是一个普通用户程序，
 * 磁盘上的命令靠 fork + exec + wait 跑起来 —— 和真实 Unix 一样。
 *
 * 内核那边只剩两件事：
 *   - 开机把 /bin/sh.lxe 拉起来（kernel/shell.c 的 shell_run 就是那个
 *     respawn 循环，相当于 getty：sh 一退出就再拉一个）；
 *   - 拉起失败时退回内置 Shell。也就是说 kernel/shell.c 现在是**备用**，
 *     正常路径上根本不会跑到。
 *
 * 分工的边界很清楚：凡是"用户自己能做"的（提示符、行编辑、内建命令、
 * 找文件、fork/exec/wait、收僵尸）都在这里；凡是"必须知道内核内部状态"
 * 或者"必须由内核决定权限"的（口令校验、改工作目录、杀进程、进程表、
 * 内存/磁盘统计）走系统调用 —— 见 include/sysinfo.h。
 *
 * 关于输出的一个小陷阱：term.h 那一层的输出（term_set_fg / term_clear_eol
 * / term_move_left ...）是**先进内部缓冲**的，而 fputs / putchar 是直接写
 * 系统调用的。两者混用就会乱序 —— 颜色转义会跑到文字后面去。
 * 所以这个文件不用 term_set_fg，自己在 OUTFG 里吐 ANSI 序列；
 * 一次只调一个的 term_size / term_clear 才留着用。
 */
#include <hneoc.h>
#include <sysinfo.h>

#define LINE_MAX   128
#define HIST_MAX   20
#define JOBS_MAX   8

/* 颜色：核 VGA 的 0-7 是普通色、8-15 是亮色，控制台把它们映射成
 * ANSI 的 30-37 / 90-97（和 user/lib/src/term.c 用的是同一套）。 */
#define OUTFG "\033[37m"    /* 默认：浅灰，和内核 Shell 一个观感 */
#define OFCYN "\033[96m"
#define OFGRN "\033[92m"
#define OFRED "\033[91m"
#define OFBLU "\033[94m"

/* ============================================================
 * 小工具
 * ============================================================ */

static void out(const char* s) {
    fputs(s, STDOUT_FILENO);
}

static int is_space(int c) {
    return c == ' ' || c == '\t';
}

static char* skip_spaces(char* s) {
    while (*s && is_space(*s)) {
        s++;
    }
    return s;
}

/* 光标左移 n 格（n 是变量，所以得拼一下序列） */
static void move_left(int n) {
    char b[16];

    if (n <= 0) {
        return;
    }
    snprintf(b, sizeof(b), "\033[%dD", n);
    out(b);
}

static void clear_eol(void) {
    out("\033[0K");
}

/* 把 s 里第一个空白处切开：命令名留在 s 里，返回值指向参数部分。
 * 就地写 '\0'，因为调用方给的是一份可写的行缓冲。 */
static char* split_word(char* s) {
    while (*s && !is_space(*s)) {
        s++;
    }
    if (*s) {
        *s = '\0';
        s++;
    }
    return skip_spaces(s);
}

/* ============================================================
 * 行编辑
 *
 * 只在最后一行上操作，所以不需要处理折行：可输入的长度被压到
 * "一屏宽 - 提示符宽度 - 1"，永远不会绕到下一行去。
 * 这样重画用 '\r' 回到行首就是正确的。
 *
 * 提示符刻意传两样东西：可见文本（可能是空串）和它的**可见宽度**。
 * 主提示符是带颜色的，颜色由调用方在进来之前设好 —— readline 自己
 * 不改颜色属性，所以重画出来的提示符仍然是那个颜色。
 * ============================================================ */

static char hist[HIST_MAX][LINE_MAX];
static int  hist_count = 0;      /* 已经存了多少条 */
static int  hist_head  = 0;      /* 下一条写到哪个槽（环形） */

static void history_add(const char* line) {
    if (!line[0]) {
        return;
    }
    if (hist_count > 0) {
        /* 和上一条一样就不重复记 —— 连按回车不该把历史刷满 */
        int last = (hist_head + HIST_MAX - 1) % HIST_MAX;

        if (strcmp(hist[last], line) == 0) {
            return;
        }
    }
    strncpy(hist[hist_head], line, LINE_MAX - 1);
    hist[hist_head][LINE_MAX - 1] = '\0';
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) {
        hist_count++;
    }
}

/* n = 0 是最新的一条 */
static const char* history_get(int n) {
    if (n < 0 || n >= hist_count) {
        return NULL;
    }
    return hist[(hist_head + HIST_MAX - 1 - n) % HIST_MAX];
}

/* 整行重画：回到行首、重画提示符和内容、擦掉尾巴、把光标挪回去 */
static void redraw(const char* prompt, const char* buf, int len, int pos) {
    putchar('\r');
    out(prompt);
    write(STDOUT_FILENO, buf, (size_t)len);
    clear_eol();
    move_left(len - pos);
}

/* 读一行，返回字符数。prompt 必须是纯文本，width 是它的可见宽度。 */
static int readline(char* buf, int max, const char* prompt, int width) {
    int len = 0;
    int pos = 0;
    int hcur = -1;             /* -1 = 正在编辑新行，>=0 表示在看历史第 n 条 */
    int rows, cols, limit;

    term_size(&rows, &cols);
    (void)rows;

    /* 提示符占掉的位置要留出来，否则行尾折下去 '\r' 就回不到行首了 */
    limit = cols - width - 1;
    if (limit > max - 1) {
        limit = max - 1;
    }
    if (limit < 8) {
        limit = 8;             /* 屏幕再窄也得能敲几个字 */
    }

    buf[0] = '\0';
    out(prompt);

    for (;;) {
        int k = getkey();

        /* 回车在键盘驱动里可能落成 CR(13) 或者 LF(10)，两个都认
         * （user/vi.c 也是这么判的）。 */
        if (k == 13 || k == 10 || k == KEY_ENTER) {
            buf[len] = '\0';
            putchar('\n');
            return len;
        }

        if (k == KEY_LEFT)  { if (pos > 0)   { pos--; move_left(1); } continue; }
        if (k == KEY_RIGHT) { if (pos < len) { pos++; out("\033[1C"); } continue; }

        if (k == KEY_HOME)  { pos = 0;   redraw(prompt, buf, len, pos); continue; }
        if (k == KEY_END)   { pos = len; redraw(prompt, buf, len, pos); continue; }

        if (k == KEY_UP) {
            const char* h = history_get(hcur + 1);

            if (h) {
                hcur++;
                strncpy(buf, h, (size_t)max - 1);
                buf[max - 1] = '\0';
                len = (int)strlen(buf);
                pos = len;
                redraw(prompt, buf, len, pos);
            }
            continue;
        }
        if (k == KEY_DOWN) {
            if (hcur > 0) {
                const char* h = history_get(hcur - 1);

                hcur--;
                if (h) {
                    strncpy(buf, h, (size_t)max - 1);
                    buf[max - 1] = '\0';
                }
                len = (int)strlen(buf);
                pos = len;
                redraw(prompt, buf, len, pos);
            } else if (hcur == 0) {
                hcur = -1;
                buf[0] = '\0';
                len = 0;
                pos = 0;
                redraw(prompt, buf, len, pos);
            }
            continue;
        }

        if (k == 12) {                       /* Ctrl+L：清屏重画 */
            term_clear();
            redraw(prompt, buf, len, pos);
            continue;
        }
        if (k == 21) {                       /* Ctrl+U：清掉整行 */
            len = 0;
            pos = 0;
            hcur = -1;
            buf[0] = '\0';
            redraw(prompt, buf, len, pos);
            continue;
        }

        if (k == KEY_BACKSPACE || k == 127) {
            if (pos > 0) {
                if (pos == len) {
                    /* 在行尾删：退一格擦掉就行，不必整行重画 */
                    len--;
                    pos--;
                    buf[len] = '\0';
                    out("\b \b");
                } else {
                    for (int i = pos - 1; i < len - 1; i++) {
                        buf[i] = buf[i + 1];
                    }
                    len--;
                    pos--;
                    buf[len] = '\0';
                    redraw(prompt, buf, len, pos);
                }
            }
            continue;
        }

        if (k == KEY_DELETE) {
            if (pos < len) {
                for (int i = pos; i < len - 1; i++) {
                    buf[i] = buf[i + 1];
                }
                len--;
                buf[len] = '\0';
                redraw(prompt, buf, len, pos);
            }
            continue;
        }

        if (k == KEY_TAB) {
            k = ' ';                         /* 没做补全，当空格 */
        }

        if (k >= 32 && k < 127) {
            if (len >= limit) {
                continue;                    /* 满了：不折行，直接不理 */
            }
            if (pos == len) {
                /* 最常见的一条路：在行尾敲字符。直接回显一个就够了，
                 * 不必整行重画 —— 整行重画在串口日志里会堆成一坨，
                 * 排错时几乎没法看。 */
                buf[len] = (char)k;
                len++;
                pos++;
                buf[len] = '\0';
                putchar((char)k);
                hcur = -1;
                continue;
            }
            for (int i = len; i > pos; i--) {
                buf[i] = buf[i - 1];
            }
            buf[pos] = (char)k;
            len++;
            pos++;
            buf[len] = '\0';
            hcur = -1;
            redraw(prompt, buf, len, pos);
            continue;
        }
        /* 其它按键直接忽略。
         * Ctrl+C 不走这条路：它是内核里的一个标志位，由"前台进程"的
         * 等待循环收走（见 include/process.h 里 fg_pid 的说明）。 */
    }
}

/* ============================================================
 * 全局状态
 * ============================================================ */

static char cur_user[LOGIN_NAME_MAX] = "?";
static char cur_home[LOGIN_HOME_MAX] = "/";

static char prompt_plain[160];
static int  prompt_width = 0;

static void build_prompt(void) {
    char cwd[128];
    char show[128];
    const char* p;

    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, "/");
    }
    p = cwd;

    /* HOME 目录里面的东西显示成 ~，这是提示符里唯一一处"美化" */
    if (cur_home[0] == '/' && strcmp(cur_home, "/") != 0) {
        int hl = (int)strlen(cur_home);

        if (strncmp(cwd, cur_home, (size_t)hl) == 0 &&
            (cwd[hl] == '\0' || cwd[hl] == '/')) {
            show[0] = '~';
            strncpy(show + 1, cwd + hl, sizeof(show) - 2);
            show[sizeof(show) - 1] = '\0';
            p = show;
        }
    }

    snprintf(prompt_plain, sizeof(prompt_plain), "hneoc$%s~%s ",
             cur_user, p);
    prompt_width = (int)strlen(prompt_plain);
}

/* ============================================================
 * 登录
 * ============================================================ */

/* 读一行不回显（口令用），自己打 '*' */
static void read_secret(char* buf, int max) {
    int len = 0;

    buf[0] = '\0';
    for (;;) {
        int k = getkey();

        /* 回车：键盘驱动可能给 CR(13) 也可能给 LF(10)，两个都认
         * （user/vi.c 里也是这么判的）。 */
        if (k == 13 || k == 10 || k == KEY_ENTER) {
            putchar('\n');
            buf[len] = '\0';
            return;
        }
        if ((k == KEY_BACKSPACE || k == 127) && len > 0) {
            len--;
            out("\b \b");
            continue;
        }
        if (k >= 32 && k < 127 && len < max - 1) {
            buf[len++] = (char)k;
            putchar('*');
        }
    }
}

static int do_login(void) {
    char name[LOGIN_NAME_MAX];
    char pass[64];
    logininfo_t info;
    int tries;

    for (tries = 0; tries < 3; tries++) {
        out(OFCYN "HNeoC OS login\n" OUTFG);

        readline(name, sizeof(name), "login: ", 7);

        out("Password: ");
        read_secret(pass, sizeof(pass));

        if (login_user(name, pass, &info) == 0) {
            strncpy(cur_user, info.name, LOGIN_NAME_MAX - 1);
            cur_user[LOGIN_NAME_MAX - 1] = '\0';
            strncpy(cur_home, info.home, LOGIN_HOME_MAX - 1);
            cur_home[LOGIN_HOME_MAX - 1] = '\0';

            out(OFGRN);
            printf("welcome, %s%s\n", cur_user, info.admin ? " (admin)" : "");
            out(OUTFG);
            return 0;
        }

        out(OFRED "login incorrect\n" OUTFG);
    }

    out("too many attempts\n");
    return -1;
}

/* ============================================================
 * 后台任务
 * ============================================================ */

static struct {
    int  pid;
    char name[24];
} jobs[JOBS_MAX];
static int job_count = 0;

static void jobs_add(int pid, const char* name) {
    if (job_count >= JOBS_MAX) {
        /* 满了就丢掉最早那条记录 —— 反正 wait(-1) 迟早会收掉它 */
        for (int i = 1; i < JOBS_MAX; i++) {
            jobs[i - 1] = jobs[i];
        }
        job_count = JOBS_MAX - 1;
    }
    jobs[job_count].pid = pid;
    strncpy(jobs[job_count].name, name, sizeof(jobs[0].name) - 1);
    jobs[job_count].name[sizeof(jobs[0].name) - 1] = '\0';
    job_count++;
}

static void jobs_forget(int pid) {
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].pid == pid) {
            for (int j = i + 1; j < job_count; j++) {
                jobs[j - 1] = jobs[j];
            }
            job_count--;
            return;
        }
    }
}

/* 顺手收掉后台任务留下的僵尸。WNOHANG：一个都没退出就立刻回来。
 * 每次走到提示符之前都调一次，后台任务就不会把进程表的槽位占死。 */
static void reap_jobs(void) {
    for (;;) {
        int status = 0;
        int pid = waitpid(-1, &status, WNOHANG);

        if (pid <= 0) {
            return;
        }
        jobs_forget(pid);
    }
}

/* ============================================================
 * 内建命令
 * ============================================================ */

static int builtin_jobs(void) {
    if (job_count == 0) {
        out("no background jobs\n");
        return 0;
    }
    for (int i = 0; i < job_count; i++) {
        printf("  [%d] pid %d  %s\n", i + 1, jobs[i].pid, jobs[i].name);
    }
    return 0;
}

static int builtin_kill(const char* args) {
    int pid = atoi(skip_spaces((char*)args));
    int rc;

    if (pid <= 0) {
        out("usage: kill <pid>\n");
        return 1;
    }
    rc = kill(pid);
    if (rc != 0) {
        out(OFRED);
        printf("kill: cannot kill %d (%d)\n", pid, rc);
        out(OUTFG);
        return 1;
    }
    jobs_forget(pid);
    return 0;
}

static int builtin_history(void) {
    for (int i = hist_count - 1; i >= 0; i--) {
        const char* h = history_get(i);

        if (h) {
            printf("%4d  %s\n", hist_count - 1 - i, h);
        }
    }
    return 0;
}

static int builtin_help(void) {
    out(OFCYN "HNeoC OS shell (ring 3)\n" OUTFG);
    out("  built-ins:\n");
    out("    cd [dir]        change directory    pwd        print it\n");
    out("    login           log in again        exit       quit (the\n");
    out("                                                   kernel restarts\n");
    out("                                                   a fresh shell)\n");
    out("    whoami          current account     users      account table\n");
    out("    useradd NAME    add an account (admin only)\n");
    out("    echo TEXT       print TEXT          history    command history\n");
    out("    cat FILE        print a file        mkdir DIR  create a directory\n");
    out("    rm FILE         delete a file       chmod M P  change permissions\n");
    out("    mem [what]      memory / heap / disk / HNeoFS\n");
    out("                    what = mem | heap | disk | fs\n");
    out("    taskmgr         list processes      kill PID   end one\n");
    out("    jobs            background jobs     clear      clear screen\n");
    out("    help            this page\n");
    out("  anything else is looked up on disk: /bin/<name>.lxe, then ./<name>.lxe\n");
    out("  a trailing & runs it in the background; Ctrl+C kills the foreground one\n");
    return 0;
}

static int builtin_cd(const char* args) {
    const char* path = skip_spaces((char*)args);

    if (!path[0]) {
        path = cur_home[0] ? cur_home : "/";
    }
    if (chdir(path) != 0) {
        out(OFRED);
        printf("cd: no such directory: %s\n", path);
        out(OUTFG);
        return 1;
    }
    return 0;
}

static int builtin_pwd(void) {
    char buf[128];

    if (getcwd(buf, sizeof(buf))) {
        printf("%s\n", buf);
    }
    return 0;
}

static int builtin_echo(const char* args) {
    printf("%s\n", skip_spaces((char*)args));
    return 0;
}

static int builtin_whoami(void) {
    printf("%s   uid %d%s\n", cur_user, getuid(),
           is_admin() ? "  (admin)" : "");
    return 0;
}

/* 账户表就是磁盘上一个文本文件，直接读出来打 */
static int builtin_users(void) {
    static char buf[2048];
    int n = readfile("/etc/passwd", buf, sizeof(buf) - 1);

    if (n < 0) {
        out("users: cannot read /etc/passwd\n");
        return 1;
    }
    buf[n] = '\0';

    out(OFCYN "name:password_hash:uid:home:admin\n" OUTFG);
    out(buf);
    return 0;
}

static int builtin_clear(void) {
    term_clear();
    return 0;
}

/* 把系统调用返回的负错误码翻译成一句人话。
 * 这几个错误码必须和内核 include/hneofs.h 里的 HNEOFS_ERR_* 对上。 */
static void fs_error(const char* cmd, const char* path, int rc) {
    out(OFRED);
    switch (rc) {
    case -1:  printf("%s: no such file or directory: %s\n", cmd, path); break;
    case -2:  printf("%s: already exists: %s\n", cmd, path); break;
    case -3:  printf("%s: no space left on the disk\n", cmd); break;
    case -5:  printf("%s: bad name: %s\n", cmd, path); break;
    case -8:  printf("%s: not a directory in: %s\n", cmd, path); break;
    case -9:  printf("%s: is a directory: %s\n", cmd, path); break;
    case -10: printf("%s: directory not empty: %s\n", cmd, path); break;
    case -11: printf("%s: permission denied\n", cmd); break;
    default:  printf("%s: failed (%d) on %s\n", cmd, rc, path); break;
    }
    out(OUTFG);
}

static int builtin_mkdir(const char* args) {
    const char* path = skip_spaces((char*)args);
    int rc;

    if (!path[0]) {
        out("usage: mkdir <directory>\n");
        return 1;
    }
    rc = mkdir(path);
    if (rc != 0) {
        fs_error("mkdir", path, rc);
        return 1;
    }
    return 0;
}

static int builtin_rm(const char* args) {
    const char* path = skip_spaces((char*)args);
    int rc;

    if (!path[0]) {
        out("usage: rm <file>\n");
        return 1;
    }
    rc = unlink(path);
    if (rc != 0) {
        fs_error("rm", path, rc);
        return 1;
    }
    return 0;
}

static int builtin_chmod(const char* args) {
    char path[128];
    const char* p = skip_spaces((char*)args);
    int mode = 0;
    int i = 0;
    int rc;

    /* 用法是 chmod <八进制> <路径>，和内核那版一样 */
    if (!*p) {
        out("usage: chmod <mode> <path>\n");
        return 1;
    }
    while (*p >= '0' && *p <= '7') {
        mode = mode * 8 + (*p - '0');
        p++;
    }
    p = skip_spaces((char*)p);
    while (p[i] && !is_space(p[i]) && i < (int)sizeof(path) - 1) {
        path[i] = p[i];
        i++;
    }
    path[i] = '\0';

    if (!path[0]) {
        out("usage: chmod <mode> <path>\n");
        return 1;
    }
    rc = chmod(path, mode);
    if (rc != 0) {
        fs_error("chmod", path, rc);
        return 1;
    }
    return 0;
}

/* cat 是内建而不是磁盘程序，纯粹因为"读一个文件打出来"用 readfile
 * 一行就够了，为它编一个 .lxe 不划算。 */
static int builtin_cat(const char* args) {
    static char buf[4096];
    const char* path = skip_spaces((char*)args);
    int n;

    if (!path[0]) {
        out("usage: cat <file>\n");
        return 1;
    }
    n = readfile(path, buf, sizeof(buf) - 1);
    if (n < 0) {
        fs_error("cat", path, -1);
        return 1;
    }
    buf[n] = '\0';
    out(buf);
    if (n > 0 && buf[n - 1] != '\n') {
        putchar('\n');
    }
    return 0;
}

static int builtin_login(void) {
    term_clear();
    do_login();
    return 0;
}

static int builtin_useradd(const char* args) {
    char name[LOGIN_NAME_MAX];
    char pass[64];
    char again[64];
    const char* p = skip_spaces((char*)args);
    int rc;

    if (!is_admin()) {
        out(OFRED "useradd: only an admin can add accounts\n" OUTFG);
        return 1;
    }
    if (!p[0]) {
        out("usage: useradd <name>\n");
        return 1;
    }

    {
        int i = 0;

        while (p[i] && !is_space(p[i]) && i < LOGIN_NAME_MAX - 1) {
            name[i] = p[i];
            i++;
        }
        name[i] = '\0';
    }

    out("password: ");
    read_secret(pass, sizeof(pass));
    out("again   : ");
    read_secret(again, sizeof(again));

    if (strcmp(pass, again) != 0) {
        out(OFRED "useradd: the two passwords differ\n" OUTFG);
        return 1;
    }

    rc = useradd(name, pass, 0);
    if (rc == 0) {
        out(OFGRN);
        printf("added account %s\n", name);
        out(OUTFG);
        return 0;
    }
    out(OFRED);
    printf("useradd: failed (%d)\n", rc);
    out(OUTFG);
    return 1;
}

/* ============================================================
 * 系统状态：内存 / 堆 / 磁盘 / 文件系统
 *
 * 数据全来自一个 sysinfo 系统调用。原来这四个命令是直接读内核变量
 * 格式化的，搬到 ring 3 之后只能由内核把快照递出来。
 * ============================================================ */

static void fmt_kb(uint32_t kb, char* b, int max) {
    if (kb >= 1024) {
        snprintf(b, (size_t)max, "%u.%02u MB", kb / 1024,
                 (kb % 1024) * 100 / 1024);
    } else {
        snprintf(b, (size_t)max, "%u KB", kb);
    }
}

static int builtin_mem(const char* args) {
    sysinfo_t s;
    const char* what = skip_spaces((char*)args);
    int all = !what[0];
    char a[32], b[32];

    if (get_sysinfo(&s) != 0) {
        out("mem: sysinfo failed\n");
        return 1;
    }

    if (all || strcmp(what, "mem") == 0) {
        out(OFCYN "physical memory\n" OUTFG);
        out("  frame size : 4 KB\n");
        fmt_kb(s.mem_total_kb, a, sizeof(a));
        fmt_kb(s.mem_used_kb, b, sizeof(b));
        printf("  total      : %s\n", a);
        printf("  used       : %s\n", b);
        fmt_kb(s.mem_free_kb, a, sizeof(a));
        printf("  free       : %s\n", a);
    }

    if (all || strcmp(what, "heap") == 0) {
        out(OFCYN "kernel heap\n" OUTFG);
        printf("  range   : 0x%08X - 0x%08X\n", s.heap_start,
               s.heap_start + s.heap_total);
        fmt_kb(s.heap_total / 1024, a, sizeof(a));
        fmt_kb(s.heap_used / 1024, b, sizeof(b));
        printf("  total   : %s\n", a);
        printf("  used    : %s\n", b);
        printf("  blocks  : %u\n", s.heap_blocks);
    }

    if (all || strcmp(what, "disk") == 0) {
        out(OFCYN "ATA disk (primary master)\n" OUTFG);
        printf("  sectors    : %u (%u KB)\n", s.disk_sectors,
               s.disk_sectors / 2);
        printf("  reads      : %u sectors\n", s.disk_reads);
        printf("  writes     : %u sectors\n", s.disk_writes);
    }

    if (all || strcmp(what, "fs") == 0) {
        out(OFCYN "HNeoFS\n" OUTFG);
        printf("  entries    : %u (%u live)\n", s.fs_entries, s.fs_live);
        printf("  data LBA   : %u\n", s.fs_data_lba);
        printf("  free LBA   : %u\n", s.fs_free_lba);
        fmt_kb(s.fs_free_bytes / 1024, a, sizeof(a));
        printf("  free       : %s\n", a);
    }
    return 0;
}

static const char* task_state_name(uint32_t st) {
    switch (st) {
    case TASK_STATE_READY:   return "ready";
    case TASK_STATE_RUNNING: return "running";
    case TASK_STATE_ZOMBIE:  return "zombie";
    default:                 return "unused";
    }
}

static int builtin_taskmgr(void) {
    sysinfo_t s;

    if (get_sysinfo(&s) != 0) {
        out("taskmgr: sysinfo failed\n");
        return 1;
    }

    out(OFCYN);
    printf("HNeoC task manager   uptime %us   ticks %u   syscalls %u\n",
           s.uptime_sec, s.ticks, s.syscalls);
    out(OUTFG);
    printf("processes: %u live, %u created since boot\n\n",
           s.procs_live, s.procs_total);

    printf("  pid  ppid  %-10s %-8s %7s %8s %9s  %s\n",
           "name", "state", "cpu", "syscalls", "memory", "ring");
    out("  ------------------------------------------------------------"
        "--------\n");

    for (int i = 0; ; i++) {
        taskinfo_t t;
        uint32_t pct;

        if (gettask(i, &t) != 0) {
            break;
        }
        /* cpu_ticks 是"被调度到的时钟滴答数"，几个人加起来约等于总滴答数，
         * 所以除以总滴答就是占用率 */
        pct = (s.ticks > 0) ? (t.cpu_ticks * 100u / s.ticks) : 0;

        printf("  %3u  %4u  %-10s %-8s %5u%% %8u %8uK  %s\n",
               t.pid, t.ppid, t.name, task_state_name(t.state),
               pct, t.syscalls, t.memory / 1024,
               t.is_user ? "ring 3" : "ring 0");
    }
    return 0;
}

/* ============================================================
 * 执行命令
 * ============================================================ */

/* 内建命令表。返回 -1 表示"不是内建命令"。 */
static int run_builtin(char* name, char* args) {
    if (strcmp(name, "help") == 0)    { return builtin_help(); }
    if (strcmp(name, "cd") == 0)      { return builtin_cd(args); }
    if (strcmp(name, "pwd") == 0)     { return builtin_pwd(); }
    if (strcmp(name, "echo") == 0)    { return builtin_echo(args); }
    if (strcmp(name, "whoami") == 0)  { return builtin_whoami(); }
    if (strcmp(name, "users") == 0)   { return builtin_users(); }
    if (strcmp(name, "history") == 0) { return builtin_history(); }
    if (strcmp(name, "clear") == 0)   { return builtin_clear(); }
    if (strcmp(name, "mkdir") == 0)   { return builtin_mkdir(args); }
    if (strcmp(name, "rm") == 0)      { return builtin_rm(args); }
    if (strcmp(name, "chmod") == 0)   { return builtin_chmod(args); }
    if (strcmp(name, "cat") == 0)     { return builtin_cat(args); }
    if (strcmp(name, "jobs") == 0)    { return builtin_jobs(); }
    if (strcmp(name, "kill") == 0)    { return builtin_kill(args); }
    if (strcmp(name, "mem") == 0)     { return builtin_mem(args); }
    if (strcmp(name, "login") == 0)   { return builtin_login(); }
    if (strcmp(name, "useradd") == 0) { return builtin_useradd(args); }
    if (strcmp(name, "taskmgr") == 0 || strcmp(name, "tasks") == 0) {
        return builtin_taskmgr();
    }

    /* 这三个原来也是内核内置命令，现在都走 mem 的分节输出 */
    if (strcmp(name, "heap") == 0)     { return builtin_mem("heap"); }
    if (strcmp(name, "fsstat") == 0)   { return builtin_mem("fs"); }
    if (strcmp(name, "diskinfo") == 0) { return builtin_mem("disk"); }

    return -1;
}

static int file_exists(const char* path) {
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

/* 把命令名变成磁盘路径：带 '/' 就当路径用，否则依次试 /bin/<name>.lxe
 * 和 <cwd>/<name>.lxe。没写后缀就自动补 .lxe。 */
static int resolve_command(const char* name, char* out, int max) {
    char cwd[128];
    char base[64];
    int bl;

    if (strchr(name, '/')) {
        strncpy(base, name, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        bl = (int)strlen(base);
        if (bl < 4 || strcmp(base + bl - 4, ".lxe") != 0) {
            if (bl + 4 < (int)sizeof(base)) {
                strcpy(base + bl, ".lxe");
            }
        }
        if (!file_exists(base)) {
            return -1;
        }
        strncpy(out, base, (size_t)max - 1);
        out[max - 1] = '\0';
        return 0;
    }

    snprintf(base, sizeof(base), "%s.lxe", name);

    snprintf(out, (size_t)max, "/bin/%s", base);
    if (file_exists(out)) {
        return 0;
    }
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(out, (size_t)max, "%s/%s", cwd, base);
        if (file_exists(out)) {
            return 0;
        }
    }
    return -1;
}

/* 跑一条磁盘命令。background 为真时不等待。
 * 返回退出码；找不到命令返回 -1（并且把话说明白）。 */
static int run_disk(const char* cmd, const char* args, int background) {
    char path[160];
    int pid;
    int status = 0;

    if (resolve_command(cmd, path, sizeof(path)) != 0) {
        out(OFRED);
        printf("%s: command not found\n", cmd);
        out(OUTFG);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        out(OFRED "cannot fork\n" OUTFG);
        return -1;
    }

    if (pid == 0) {
        /* --- 子进程 ---
         * exec 成功的话这一行之后就没有代码了：内核把返回的现场改成了
         * 新程序的入口，我们这段代码再也不会执行。
         * 走得到这里就说明 exec 失败了，返回值是负的错误码。 */
        int rc = exec(path, args);

        out(OFRED);
        switch (rc) {
        case -6:
            /* 内核的 PROCESS_MAX_LOAD_BYTES 上限。原来这句是内核 Shell
             * 说的，Shell 搬出来之后得由我们替它说。 */
            out("program image is too large to load\n");
            break;
        case -1:
            printf("cannot run %s: not a program\n", path);
            break;
        case -5:
            out("cannot fork: the process table is full\n");
            break;
        default:
            printf("cannot run %s (%d)\n", path, rc);
            break;
        }
        out(OUTFG);
        exit(127);
    }

    if (background) {
        jobs_add(pid, cmd);
        printf("[%d] %d\n", job_count, pid);
        return 0;
    }

    /* 前台：等它退出，退出码就是它的返回值。
     * 内核在这个等待里把前台设成了这个孩子，所以 Ctrl+C 杀的是它 ——
     * 不是我们这个 Shell。 */
    if (waitpid(pid, &status, 0) < 0) {
        out("wait: the child disappeared\n");
        return -1;
    }
    return status;
}

static void execute(char* line) {
    char* p = skip_spaces(line);
    char* args;
    char* cmd;
    int background = 0;
    int len;
    int rc;

    if (!p[0]) {
        return;
    }

    /* 行尾的 & 表示后台。放在切词之前处理，这样 "bg &" 的参数不会被
     * 当成 "&"。 */
    len = (int)strlen(p);
    while (len > 0 && is_space(p[len - 1])) {
        p[--len] = '\0';
    }
    if (len > 0 && p[len - 1] == '&') {
        background = 1;
        p[--len] = '\0';
        while (len > 0 && is_space(p[len - 1])) {
            p[--len] = '\0';
        }
    }
    if (!p[0]) {
        return;
    }

    cmd  = p;
    args = split_word(p);

    rc = run_builtin(cmd, args);
    if (rc >= 0) {
        return;
    }
    (void)run_disk(cmd, args, background);
}

/* ============================================================
 * main
 * ============================================================ */

static void banner(void) {
    /* 开机时内核已经打过那块 ASCII 艺术字了，这里只补一行"我是谁"——
     * 顺带把 pid 打出来，一眼就能看出它是个独立的用户态进程，
     * 而不是内核里那个备用 Shell。 */
    out(OFCYN);
    printf("HNeoC OS user-mode shell   pid %d   ring 3\n", getpid());
    out(OUTFG);
    out("Type 'help' for the command list.\n");
    out("Up/Down arrows browse the command history.\n\n");
}

int main(void) {
    char line[LINE_MAX];

    if (do_login() != 0) {
        return 1;
    }

    banner();

    for (;;) {
        /* 后台任务退出之后就收掉，免得僵尸占着进程表的槽位 */
        reap_jobs();

        build_prompt();
        out(OFGRN);
        /* 提示符由 readline 自己打（重画时需要再打一遍），
         * 所以颜色在这里设好、在外面恢复。 */
        readline(line, sizeof(line), prompt_plain, prompt_width);
        out(OUTFG);

        if (!line[0]) {
            continue;
        }

        if (strcmp(line, "exit") == 0 || strcmp(line, "logout") == 0) {
            out("bye\n");
            /* 退出之后内核的 respawn 循环会再拉起一个 Shell，
             * 效果就是回到登录界面 —— 和 getty 一模一样。 */
            return 0;
        }

        history_add(line);
        execute(line);
    }
}
