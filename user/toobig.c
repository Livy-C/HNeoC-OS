/* toobig - 镜像刚好超过加载器上限的程序
 *
 * 这个程序存在的唯一目的是**验证护栏**：它的镜像是 64KB 的有初值数组，
 * 算下来约 65KB，超过 include/process.h 里的 PROCESS_MAX_LOAD_BYTES
 * （60KB）。加载器应该明确拒绝它，而不是把整机打成三重故障
 * —— 后者是 KNOWN-ISSUES 第 1 节那个还没查清的缺陷的行为。
 *
 * 所以期望的输出是启动失败 + "program image is too large to load"，
 * 而不是这个程序真的跑起来打印东西。
 *
 * 注意 main 里必须**真的用到** buf：第一版只声明不用，GCC 直接把整个
 * 数组优化掉了，.lxe 只有 324 字节，护栏根本没被测到 —— 是
 * tools/regress.ps1 里那条检查把它抓出来的。
 */

#include <hneoc.h>

static char buf[64 * 1024] = "an oversized program image";

int main(void) {
    puts(buf);
    puts("toobig: if you can read this, the size guard is gone");
    return 0;
}
