/* bigio - 验证长时间磁盘 I/O 期间中断不会被关死
 *
 * 系统调用门是 interrupt gate，进来时 IF=0。修复前 sys_write 全程关着中断
 * 做 PIO 传输：请求越大，键盘和 100Hz 时钟被冻住的时间越长（一次可以请求
 * 到 4MB）。
 *
 * 缓冲区用 malloc 从用户堆拿，不放在 .bss 里：MinGW 链接出来的 PE 在大
 * 数组时会把 .bss 实体化进二进制（512KB 的 .bss 会让 .lxe 变成 527KB），
 * 那样测的就不是 I/O 而是加载了。
 *
 * 判据：
 *   - 一次写完 256KB 之后文件大小必须正好是 256KB（不能悄悄截断）；
 *   - 传输期间的 ticks() 必须往前走 —— 那是时钟中断真的进来了的证据。
 *     修复前这段差值基本是 0，因为整段传输都在关中断。
 */

#include <hneoc.h>
#include <stdlib.h>

#define BIG_SIZE (256 * 1024)

int main(void) {
    const char* name = "bigio.bin";
    unsigned int t0, t1;
    char* buf;
    int fd, w, sz, i;

    puts("bigio: one large write");
    puts("------------------------------");

    buf = (char*)malloc(BIG_SIZE);
    if (!buf) {
        puts("  malloc failed");
        return 1;
    }
    for (i = 0; i < BIG_SIZE; i++) {
        buf[i] = (char)('a' + (i % 26));
    }

    unlink(name);
    fd = open(name, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) {
        printf("  open failed = %d\n", fd);
        free(buf);
        return 1;
    }

    t0 = ticks();
    w  = write(fd, buf, BIG_SIZE);
    t1 = ticks();
    sz = fstat(fd);

    printf("  requested        = %d\n", BIG_SIZE);
    printf("  write returned   = %d\n", w);
    printf("  file size        = %d\n", sz);
    printf("  ticks during I/O = %d\n", (int)(t1 - t0));

    close(fd);
    unlink(name);
    free(buf);

    puts("------------------------------");
    if (w != BIG_SIZE || sz != BIG_SIZE) {
        puts("FAIL: the write was truncated");
        return 1;
    }
    puts("size intact");
    return 0;
}
