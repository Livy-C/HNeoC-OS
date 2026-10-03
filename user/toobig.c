/* toobig - 镜像刚好跨过 64KB 的程序
 *
 * 这个程序是给 KNOWN-ISSUES 第 1 节那个缺陷做对照的：它的镜像是 64KB 的
 * 有初值数组，算下来约 65KB —— 正好落在那条"镜像一超过 64KB 加载器就三重
 * 故障"的线上。
 *
 * 现在的期望输出是**它真的跑起来并打印下面两行**：
 *
 *     an oversized program image
 *     toobig: if you can read this, the size guard is gone
 *
 * 加载器改用静态暂存区之后（见 kernel/process.c 的 load_staging），
 * 65KB 的镜像能正常读完、进程正常创建、程序正常跑到退出；
 * tools/regress.ps1 里有一条检查盯着这两行输出。
 * 比暂存区（PROCESS_MAX_LOAD_BYTES）还大的程序由 waytoobig.c 负责测。
 *
 * 注意 main 里必须**真的用到** buf：第一版只声明不用，GCC 直接把整个
 * 数组优化掉了，.lxe 只有 324 字节，什么都没测到 —— 是 regress 里那条
 * 检查把它抓出来的。
 */

#include <hneoc.h>

static char buf[64 * 1024] = "an oversized program image";

int main(void) {
    puts(buf);
    puts("toobig: if you can read this, the size guard is gone");
    return 0;
}
