/* readbig - 对照实验：读一个大文件（不是加载大程序）
 *
 * bigdata（512KB 有初值的数组，镜像 526KB）一加载就把虚拟机打成三重故障。
 * 但那个崩溃发生在"加载器把整个 .lxe 读进内核缓冲区"这一步。
 *
 * 这个程序问的是另一个问题：**任何一次大读取都会崩吗？**
 * 它用 readfile 系统调用把同一个 526KB 的文件读进用户态缓冲区 ——
 * 走的是完全不同的路径（内核逐扇区拷到用户页，不经过 kmalloc 的大映像缓冲区）。
 *
 *   readbig 正常、bigdata 崩  -> 毛病在"加载器/大内核缓冲区"这一条路上
 *   两个都崩                  -> 内核里凡是"一次搬运几十 KB"都会崩，
 *                               那 cat 大文件、hpm 装大包都会中招，影响面完全不同
 */

#include <hneoc.h>

#define CHUNK (600 * 1024)

int main(void) {
    char* buf = (char*)malloc(CHUNK);
    int n;

    if (!buf) {
        puts("readbig: malloc failed");
        return 1;
    }

    puts("readbig: readfile /bin/bigdata.lxe (526KB) into a malloc buffer");
    n = readfile("/bin/bigdata.lxe", buf, CHUNK);

    printf("readbig: readfile returned %d\n", n);
    if (n > 0) {
        printf("readbig: first=%d last=%d\n", buf[0], buf[n - 1]);
    }
    puts("readbig: survived the big read");
    return 0;
}
