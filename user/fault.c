/* fault - 故意触发一次用户态 CPU 异常
 *
 * 用来验证"用户程序的错不该让整机陪葬"：
 *   修复前 —— 任何 ring 3 异常都会走到 idt.c 的异常分支，打一屏红字后
 *             System halted，机器彻底停住；
 *   修复后 —— 内核认出异常发生在 ring 3，只把出错的进程杀掉，
 *             Shell 照常拿回提示符。
 *
 * 所以：如果这个程序跑完之后 Shell 还在，说明守卫还在；
 * 如果整机停住、屏幕变成红底 "*** CPU EXCEPTION ***"，说明守卫没了。
 */

#include <hneoc.h>

int main(void) {
    volatile int* p = (volatile int*)0x40200000;   /* 用户区里没有映射的地址 */

    puts("fault: about to write to an unmapped address...");
    puts("       (the kernel should kill this process, not the system)");

    *p = 1;                                        /* 这里会缺页 */

    puts("fault: NOT REACHED - the guard is missing");
    return 1;
}
