/* bigdata - 526KB 的大镜像（当年的极端复现程序，现在常驻镜像里当检查）
 *
 * KNOWN-ISSUES 第 1 节最早的形状：一个 512KB 的**有初值**静态数组，
 * 按 C 的语义必须进 .data，于是 .lxe 有 500 多 KB —— 加载它会稳定地把
 * 整机打成三重故障。
 *
 * 配套的对照是 user/bigbss.c：同样的 512KB，但没有初值，落进 .bss，
 * 镜像只有 2.7KB。以前"bigbss 正常、bigdata 崩"这组对照把范围缩到了
 * "大镜像"这条路上；现在两个都正常，tools/regress.ps1 里各有一条检查。
 *
 * 加载器改成"把正文分块直接读进页框"之后（kernel/process.c 的
 * fill_image_pages），546KB 的映像一次读完，这个程序能跑到底。
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
