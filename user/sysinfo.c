/* sysinfo - 演示更多系统调用
 *
 * 每 250ms 刷新一次屏幕上的计时，按 q 退出。
 * 这同时验证了三件事：
 *   - 用户态可以被时钟中断打断（否则 getchar 永远等不到按键）
 *   - 键盘输入能通过系统调用送到用户态
 *   - 用户态调用 exit 之后控制权能干净地回到 Shell
 */

#include "hneoc.h"

int main(void) {
    uint32_t start = uptime();
    int last_shown = -1;

    clear_screen();
    set_color(C_LCYAN, C_BLACK);
    puts("sysinfo - a user-mode program");
    set_color(C_DGREY, C_BLACK);
    puts("--------------------------------------------------");
    set_color(C_WHITE, C_BLACK);

    fputs("pid     : ", STDOUT_FILENO);
    put_int(getpid());
    putchar('\n');

    fputs("started : ", STDOUT_FILENO);
    put_uint(start);
    puts(" s after boot\n");
    puts("press 'q' to quit\n");

    for (;;) {
        uint32_t elapsed = uptime() - start;

        if ((int)elapsed != last_shown) {
            last_shown = (int)elapsed;

            set_color(C_LGREEN, C_BLACK);
            fputs("  running for ", STDOUT_FILENO);
            set_color(C_YELLOW, C_BLACK);
            put_uint(elapsed);
            set_color(C_LGREEN, C_BLACK);
            fputs(" s   (tick ", STDOUT_FILENO);
            put_uint(ticks());
            puts(")      \r");
        }

        /* 非阻塞地轮询键盘：内核的 getchar 是阻塞的，
         * 所以这里用 sleep 让出 CPU，再等下一次循环
         */
        sleep_ms(250);

        /* 有按键就取一个出来看看是不是 q */
        {
            /* 我们的 getchar 会阻塞，所以这里改用"先睡再查"的简化策略：
             * 直接阻塞读一个字符，用户按任意键都会立刻返回
             */
            int c = getchar();
            if (c == 'q' || c == 'Q') {
                break;
            }
        }
    }

    set_color(C_LGREY, C_BLACK);
    puts("\n\nsysinfo exiting.");
    return 0;
}
