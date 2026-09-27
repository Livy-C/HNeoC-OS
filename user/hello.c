/* hello - 最小的用户态程序
 *
 * 它是磁盘上的一个独立文件（hello.lxe），由内核从 HNeoFS 读出来，
 * 装进自己的地址空间，然后 iret 到 ring 3 执行。
 * 屏幕上看到的每一行都是通过 int 0x80 请内核代劳打印的。
 */

#include "hneoc.h"

/* 放在 .bss 里的全局变量，用来验证加载器确实把 bss 清零了 */
static int bss_probe[8];

int main(void) {
    puts("Hello from ring 3!");
    puts("------------------");

    fputs("pid        : ", STDOUT_FILENO);
    put_int(getpid());
    putchar('\n');

    fputs("uptime     : ", STDOUT_FILENO);
    put_uint(uptime());
    puts(" s");

    fputs("timer ticks: ", STDOUT_FILENO);
    put_uint(ticks());
    putchar('\n');

    /* 确认 .bss 被清零了 —— 全是 0 才说明加载器工作正常 */
    fputs("bss probe  : ", STDOUT_FILENO);
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += bss_probe[i];
    }
    puts(sum == 0 ? "zeroed by the loader" : "NOT ZEROED - loader bug!");

    /* 验证系统调用返回值确实回到了用户态 */
    int pid = getpid();
    fputs("syscall ret: ", STDOUT_FILENO);
    put_int(pid);
    puts(pid > 0 ? "  (looks valid)" : "  (suspicious)");

    puts("\nThis program is a separate executable file on disk.");

    return 0;
}
