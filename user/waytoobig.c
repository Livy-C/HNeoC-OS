/* waytoobig - 比加载器上限还大的程序
 *
 * PROCESS_MAX_LOAD_BYTES 现在是 1MB（结构上的上限是用户区页数，3MB）。
 * 这里用 1MB 的有初值数组把镜像顶到 1MB 以上，正好越过那条线。
 *
 * 期望的输出是"明确拒绝"，而不是黑屏或者三重故障：
 *
 *     cannot run /bin/waytoobig.lxe: program image is too large to load
 *
 * 它和 toobig.c（65KB，应该跑起来）、bigdata.c（526KB，也应该跑起来）
 * 是一组：两个证明上限已经不是当年的 64KB，一个证明越界时系统只是报错。
 */

#include <hneoc.h>

static char buf[1024 * 1024] = "a way oversized program image";

int main(void) {
    puts(buf);
    puts("waytoobig: if you can read this, the load limit grew");
    return 0;
}
