/* bigbss - 大块静态数组的回归测试
 *
 * 这是 KNOWN-ISSUES.md 第 1 节那个"大内存三重故障"的复现程序：一个
 * 512KB 的静态数组。
 *
 * 在 -fdata-sections 那个坑修好之前，这个数组会被实体化进 .data，
 * .lxe 变成 527KB，一启动就把虚拟机打成 Guru Meditation。
 * 修好之后它落在真正的 .bss 里（镜像只有几 KB，bss_size 是 512KB），
 * 加载器按 bss_size 清零，程序应该正常跑完。
 *
 * 所以这个程序的用途是：每次改完内存相关的代码就跑一下，确认大块
 * bss 还是好的。
 */

#include <hneoc.h>

static char buf[512 * 1024];

int main(void) {
    unsigned int i;
    int sum = 0;

    puts("bigbss: a 512KB static array");
    puts("------------------------------");

    /* 先确认这些页真的可用、而且初始是 0 */
    if (buf[0] != 0 || buf[sizeof(buf) - 1] != 0) {
        puts("FAIL: the array was not zero filled");
        return 1;
    }

    /* 每 1KB 写一个值，再抽查求和 */
    for (i = 0; i < sizeof(buf); i += 1024) {
        buf[i] = (char)(i & 0x7F);
    }
    for (i = 0; i < sizeof(buf); i += 1024) {
        sum = sum + buf[i];
    }

    puts("zero filled at start : yes");
    printf("touched              : %u bytes, 1KB steps\n",
           (unsigned int)sizeof(buf));
    printf("checksum             : %d\n", sum);
    puts("the 512KB array works");
    return 0;
}
