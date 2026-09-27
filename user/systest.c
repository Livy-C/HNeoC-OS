/* systest - 系统调用边界的回归自测
 *
 * 目前只测一件事：readfile 的 max 参数校验。
 *
 * max == 0 时的漏洞：内核侧 user_range_ok 在 len == 0 时会直接返回 true
 * （"空区间当然没问题"），而 want 的钳位 `if (want > max - 1)` 里
 * max - 1 会算成 0xFFFFFFFF，等于没有钳位。结果内核会把整份文件写进一个
 * 完全没校验过的用户地址。
 *
 * 所以这里故意传一个 4MB 用户区里"没有映射"的地址 0x40200000：
 *   修复前 —— 内核往未映射页写 → 内核态缺页 → 整机 System halted；
 *   修复后 —— 直接返回 -1，程序继续跑，最后打印 still alive。
 * 换句话说，只要这个程序没打完最后一行，就说明守卫被拿掉了。
 */

#include <hneoc.h>

int main(void) {
    char buf[64];
    int r;

    puts("systest: readfile bounds");
    puts("------------------------------");

    r = readfile("motd.txt", buf, sizeof(buf));
    printf("  max=64 (normal)            = %d\n", r);

    r = readfile("motd.txt", buf, 1);
    printf("  max=1  (no room for NUL)   = %d\n", r);

    r = readfile("motd.txt", (char*)0x40200000, 0);
    printf("  max=0  (unmapped address)  = %d\n", r);

    puts("------------------------------");
    puts("still alive: the guard held");
    return 0;
}
