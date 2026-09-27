/* bigdata - 对照实验：大镜像到底会不会崩
 *
 * BIGBSS（user/bigbss.c）验证的是"大块 .bss"：数组没有初值，落进 bss，
 * .lxe 只有 2.7KB，跑得好好的。
 *
 * 这个程序是它的对照组：同样的 512KB，但是**有初值**。按 C 的语义它必须
 * 进 .data（初始化数据要写进文件），于是 .lxe 又会变成 500 多 KB ——
 * 也就是当年触发三重故障的那个形状。
 *
 * 两个程序一起看，就能判断 KNOWN-ISSUES 第 1 节的根因是不是"大镜像"：
 *   bigbss  正常、bigdata 崩  -> 根因在大镜像那条路上（加载器/内核堆）
 *   两个都正常                -> 当年的触发条件已经彻底消失了
 */

#include <hneoc.h>

static char buf[512 * 1024] = "the first few bytes are initialised";

int main(void) {
    unsigned int i;
    int sum = 0;

    puts("bigdata: a 512KB *initialised* array");
    puts("-----------------------------------");
    puts(buf);

    for (i = 0; i < sizeof(buf); i += 1024) {
        sum = sum + buf[i];
    }
    printf("checksum : %d\n", sum);
    puts("the big .data image works");
    return 0;
}
