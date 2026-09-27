/* ver - 打印版本信息
 *
 * 同样是磁盘上的可执行文件，验证"每条命令都是一个程序"这件事：
 * Shell 里敲 ver 和敲 exec ver 效果一样。
 */

#include "hneoc.h"

int main(void) {
    set_color(C_LGREEN, C_BLACK);
    puts("HNeoC OS");
    set_color(C_WHITE, C_BLACK);

    puts("  kernel        : 0.3.0");
    puts("  architecture  : x86 (i386, 32-bit protected mode)");
    fputs("  this program  : ring 3 user mode, pid ", STDOUT_FILENO);
    put_int(getpid());
    putchar('\n');

    puts("  loaded from   : HNeoFS, over ATA PIO");
    fputs("  uptime        : ", STDOUT_FILENO);
    put_uint(uptime());
    puts(" s");

    return 0;
}
