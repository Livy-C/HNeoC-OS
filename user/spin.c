/* spin - 故意跑飞的前台程序，用来验证 Ctrl+C 能把控制权收回来
 *
 * 它会一直循环，每两秒打一个点证明自己还活着。
 *
 * 修复前：Shell 阻塞在 process_run 的等待循环里，没有任何办法收回控制权，
 *         只能重启虚拟机。
 * 修复后：按 Ctrl+C（左 Ctrl + C），keyboard_take_ctrl_c() 被 Shell 的
 *         等待循环取到，它调用 process_kill 杀掉本进程，提示符回来。
 *         注意这个字符不会被塞进键盘缓冲区，所以命令行里不会多出一个 'c'。
 */

#include <hneoc.h>

int main(void) {
    unsigned int last = uptime();

    puts("spin: looping forever - press Ctrl+C to interrupt me");

    for (;;) {
        unsigned int t = uptime();

        if (t != last) {
            last = t;
            if ((t % 2) == 0) {
                putchar('.');
            }
        }
        /* SYS_YIELD 目前是空实现（见 syscall.c），这里只是表个态：
         * 真正的抢占由时钟中断负责。 */
        yield();
    }

    return 0;
}
