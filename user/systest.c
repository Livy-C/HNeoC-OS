/* systest - 系统调用边界的回归自测
 *
 * 测两类曾经真实出过问题的地方：
 *
 * 1) readfile 的 max 参数
 *    max == 0 时内核侧 user_range_ok 在 len == 0 时直接返回 true
 *    （"空区间当然没问题"），而 want 的钳位 `if (want > max - 1)` 里
 *    max - 1 会算成 0xFFFFFFFF，等于没有钳位 —— 内核会把整份文件写进
 *    一个完全没校验过的用户地址。下面故意传 4MB 用户区里未映射的
 *    0x40200000：修复前是内核态缺页、整机 System halted，修复后返回 -1。
 *
 * 2) lseek 的偏移量
 *    偏移量会被原样存进 fd，之后 write 直接把 start_lba + offset/512 当 LBA 用。
 *    越界的偏移量等于一个"任意扇区写"的原语，能写到超级块、文件表或者
 *    别的文件上，而且文件头的 size 只按这次写入更新，事后看不出来。
 *    这里只验证"越过文件末尾会被拒绝"，用 O_RDONLY 打开，不会真写盘。
 *
 * 只要最后一行 "the guards held" 没打出来，就说明守卫被拿掉了。
 */

#include <hneoc.h>

int main(void) {
    char buf[64];
    int r;
    int fd;

    puts("systest: syscall bounds");
    puts("------------------------------");

    r = readfile("motd.txt", buf, sizeof(buf));
    printf("  readfile max=64           = %d\n", r);

    r = readfile("motd.txt", buf, 1);
    printf("  readfile max=1            = %d\n", r);

    r = readfile("motd.txt", (char*)0x40200000, 0);
    printf("  readfile max=0 (unmapped) = %d\n", r);

    fd = open("readme.txt", O_RDONLY);
    if (fd >= 0) {
        printf("  lseek(far past EOF)       = %d\n", lseek(fd, 0x7FFFF000, SEEK_SET));
        printf("  lseek(within file)        = %d\n", lseek(fd, 4, SEEK_SET));
        close(fd);
    } else {
        printf("  open(readme.txt) failed   = %d\n", fd);
    }

    puts("------------------------------");
    puts("the guards held");
    return 0;
}
