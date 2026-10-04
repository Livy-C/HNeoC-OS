/* forktest.c - fork / exec / wait 的自检程序
 *
 * 用法：
 *   forktest          跑全套
 *   forktest exec     由 exec 测试重新拉起来的那个实例（内部用，不是给人调的）
 *
 * 这个程序为什么存在：fork/exec/wait 是"把 Shell 从内核搬到 ring 3"的前置条件，
 * 而它们又是整个系统里最容易出错的一块 —— 地址空间复制、僵尸回收、父子的
 * 返回值分岔，任何一处错了都表现为"莫名其妙崩机"或者"偶尔卡死"。
 * 所以这里把它们一条条钉死成可复现的检查。
 */
#include <hneoc.h>

/* 用来验证"父子各有一份内存"的探针 */
static int marker = 100;

int main(void) {
    char args[64];
    int  pid;
    int  status;
    int  rc;

    getargs(args, sizeof(args));

    /* ============================================================
     * exec 测试会把本程序重新拉起来一次，参数是 "exec"。
     * 那个实例报个到就退出 —— 不这么分岔的话，它会从头再跑一遍全套，
     * 于是无限地自我复制下去。
     * ============================================================ */
    if (strcmp(args, "exec") == 0) {
        printf("[exec] fresh image of forktest, pid=%d, args=\"%s\"\n",
               getpid(), args);
        return 0;
    }

    /* ============================================================
     * 压测模式：forktest stress [轮数]
     *
     * 反复 fork / exit / wait，专门盯槽位和内核栈有没有漏。
     * 进程表一共 16 个槽，每轮 fork 都要一个内核栈（8KB）加几页用户内存，
     * 任何一处漏了，十来轮之内就会 fork 失败或者直接崩机 ——
     * 这个模式存在的意义就是让那种漏比"跑一次看看"早暴露得多。
     * ============================================================ */
    if (strncmp(args, "stress", 6) == 0) {
        int rounds = atoi(args + 6);

        if (rounds <= 0) {
            rounds = 40;
        }
        printf("== fork/wait leak stress: %d rounds ==\n", rounds);

        for (int i = 0; i < rounds; i++) {
            int want = i & 0x7F;
            char* p;

            /* 每轮都顺手 malloc 一点、让堆页也走一遍复制路径 */
            p = (char*)malloc(512);
            if (!p) {
                printf("FAIL: malloc failed at round %d\n", i);
                return 1;
            }
            p[0] = (char)want;

            pid = fork();
            if (pid < 0) {
                printf("FAIL: fork() failed at round %d (returned %d)\n",
                       i, pid);
                return 1;
            }
            if (pid == 0) {
                exit(p[0] == (char)want ? want : 1);
            }

            status = -1;
            rc = waitpid(pid, &status);
            if (rc != pid || status != want) {
                printf("FAIL: round %d: reaped %d exit %d (wanted %d / %d)\n",
                       i, rc, status, pid, want);
                return 1;
            }
            free(p);
        }
        printf("ok: %d fork/wait rounds, nothing leaked\n", rounds);
        return 0;
    }

    printf("== fork / exec / wait selftest ==\n");
    printf("pid=%d, marker=%d\n", getpid(), marker);

    /* ============================================================
     * 1. fork 的基本语义
     *    子进程里返回 0，父进程里返回子进程的 pid。
     * ============================================================ */
    marker = 100;
    pid = fork();
    if (pid < 0) {
        printf("FAIL: fork() returned %d\n", pid);
        return 1;
    }

    if (pid == 0) {
        /* --- 子进程 ---
         * 先改掉自己的 marker，再睡一会儿。父进程会在它退出之前看自己
         * 那份 marker：要是打印出 999，说明两份地址空间根本没分开。 */
        marker = 999;
        printf("[child ] pid=%d, marker=%d (mine, changed)\n",
               getpid(), marker);
        sleep_ms(300);
        exit(7);
    }

    /* --- 父进程 --- */
    printf("[parent] fork() -> %d, my pid=%d\n", pid, getpid());
    printf("[parent] marker=%d (must still be 100)\n", marker);

    if (marker != 100) {
        printf("FAIL: the child's write leaked into the parent\n");
        return 1;
    }

    /* ============================================================
     * 2. waitpid：等指定的孩子，取它的退出码
     * ============================================================ */
    status = -1;
    rc = waitpid(pid, &status);
    if (rc != pid) {
        printf("FAIL: waitpid returned %d, expected %d\n", rc, pid);
        return 1;
    }
    if (status != 7) {
        printf("FAIL: exit code is %d, expected 7\n", status);
        return 1;
    }
    printf("[parent] reaped pid=%d, exit=%d\n", rc, status);
    printf("ok: fork + waitpid\n");

    /* ============================================================
     * 3. 一次开三个孩子，用 wait(-1) 随便收
     *
     * 这一段顺带压了"僵尸槽位不许被抢走"那条规则：三个孩子同时挂着，
     * 父进程一个一个收。中间只要有一次槽位被 alloc_slot 抹掉，
     * 就会有一个孩子永远收不回来（wait 报 -1）。
     * ============================================================ */
    {
        int got   = 0;
        int codes[3] = { 0, 0, 0 };

        for (int i = 0; i < 3; i++) {
            pid = fork();
            if (pid < 0) {
                printf("FAIL: fork #%d returned %d\n", i, pid);
                return 1;
            }
            if (pid == 0) {
                /* 三个人错开时间退出，好让 wait(-1) 结结实实地
                 * "收第一个变成僵尸的" */
                sleep_ms(50 + i * 50);
                exit(20 + i);
            }
        }

        while (got < 3) {
            status = 0;
            rc = wait(&status);
            if (rc < 0) {
                printf("FAIL: wait(&status) returned %d after %d child(ren)\n",
                       rc, got);
                return 1;
            }
            if (status < 20 || status > 22) {
                printf("FAIL: unexpected exit code %d from pid %d\n",
                       status, rc);
                return 1;
            }
            codes[status - 20] = 1;
            got++;
        }

        if (!codes[0] || !codes[1] || !codes[2]) {
            printf("FAIL: a child went missing (seen %d %d %d)\n",
                   codes[0], codes[1], codes[2]);
            return 1;
        }
        printf("ok: 3 children, 3 distinct exit codes collected\n");
    }

    /* ============================================================
     * 4. 堆也整份复制
     *
     * 前面几节碰的都是映像页。堆页在 image_frames[] 里紧跟在映像后面，
     * 是另一条分配路径（process_sbrk 长出来的），copy 的时候最容易漏。
     * 这里 malloc 12KB（跨 3 页）写满花样再 fork：孩子必须看到一模一样
     * 的内容，之后两边各改各的。
     * ============================================================ */
    {
        const int heap_len = 4096 * 3;
        char* buf = (char*)malloc((size_t)heap_len);

        if (!buf) {
            printf("FAIL: malloc(%d) returned NULL\n", heap_len);
            return 1;
        }
        for (int i = 0; i < heap_len; i++) {
            buf[i] = (char)(i & 0x3F);
        }

        pid = fork();
        if (pid < 0) {
            printf("FAIL: fork() with a live heap returned %d\n", pid);
            return 1;
        }
        if (pid == 0) {
            int bad = 0;

            for (int i = 0; i < heap_len; i++) {
                if (buf[i] != (char)(i & 0x3F)) {
                    bad++;
                }
            }
            printf("[child ] heap copy: %d/%d bytes wrong\n", bad, heap_len);
            buf[0] = 'X';           /* 只改自己那份 */
            exit(bad == 0 ? 0 : 1);
        }

        status = -1;
        rc = waitpid(pid, &status);
        if (rc != pid || status != 0) {
            printf("FAIL: the child did not get a faithful heap copy "
                   "(exit %d)\n", status);
            return 1;
        }
        if (buf[0] != 0) {
            printf("FAIL: the child's heap write reached the parent\n");
            return 1;
        }
        printf("ok: heap copy (12KB over 3 pages), then private\n");
        free(buf);
    }

    /* ============================================================
     * 5. exec：把孩子整个换成另一个程序，pid 不变
     *
     * 这里 exec 的是本程序自己，所以能顺带验证两件事：
     *   - 参数确实传给了新映像（新实例打印 args="exec"）
     *   - pid 没变（下面 rc == pid；新实例的 printf 里也能看到同一个 pid）
     * ============================================================ */
    pid = fork();
    if (pid < 0) {
        printf("FAIL: fork() before exec returned %d\n", pid);
        return 1;
    }
    if (pid == 0) {
        printf("[child ] about to exec /bin/forktest.lxe with args \"exec\"\n");
        exec("/bin/forktest.lxe", "exec");
        /* 只有 exec 失败才会走到这儿 */
        printf("FAIL: exec returned (it must never return on success)\n");
        exit(99);
    }

    status = -1;
    rc = waitpid(pid, &status);
    if (rc != pid || status != 0) {
        printf("FAIL: exec'd child: reaped %d, exit %d (expected %d and 0)\n",
               rc, status, pid);
        return 1;
    }
    printf("ok: exec (the child kept pid %d across the image swap)\n", pid);

    printf("== all fork/exec/wait tests passed ==\n");
    return 0;
}
