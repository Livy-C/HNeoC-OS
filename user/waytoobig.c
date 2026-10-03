/* waytoobig - 比加载器暂存区还大的程序
 *
 * 加载器的暂存区是 PROCESS_MAX_LOAD_BYTES（现在 80KB），这里用 96KB 的
 * 有初值数组把镜像顶到 96KB 以上，正好越过那条线。
 *
 * 期望的输出是"明确拒绝"，而不是黑屏或者三重故障：
 *
 *     cannot run /bin/waytoobig.lxe: program image is too large to load
 *
 * 这一条和 toobig.c 是一对：toobig 证明 64KB 那道坎已经过去了，
 * waytoobig 证明越过暂存区上限时系统仍然只是报错、不会崩。
 */

#include <hneoc.h>

static char buf[96 * 1024] = "a way oversized program image";

int main(void) {
    puts(buf);
    puts("waytoobig: if you can read this, the staging buffer grew");
    return 0;
}
