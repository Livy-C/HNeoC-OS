/* wtest - 验证可写的 HNeoFS
 *
 * 走一遍完整的文件生命周期：创建、写入、追加、定位读、截断、删除。
 * 每一步都打印返回值，方便从串口日志里核对。
 */

#include "hneoc.h"

#define TESTFILE "wtest.txt"

static void show(const char* tag, int value) {
    fputs("  ", STDOUT_FILENO);
    fputs(tag, STDOUT_FILENO);
    fputs(" = ", STDOUT_FILENO);
    put_int(value);
    putchar('\n');
}

int main(void) {
    char buf[256];
    int fd;
    int n;

    puts("HNeoFS write test");
    puts("=====================================");

    /* 先清掉上次留下的文件，保证从干净状态开始 */
    unlink(TESTFILE);

    /* --- 1. 创建并写入 --- */
    puts("\n[1] create + write");
    fd = open(TESTFILE, O_CREAT | O_WRONLY | O_TRUNC);
    show("open", fd);
    if (fd < 0) {
        puts("cannot create the file");
        return 1;
    }

    {
        const char* line1 = "first line written by a ring 3 program\n";
        const char* line2 = "second line, also from user mode\n";
        int w1 = write(fd, line1, str_len(line1));
        int w2 = write(fd, line2, str_len(line2));

        show("write #1", w1);
        show("write #2", w2);
        show("fstat", fstat(fd));
    }
    show("close", close(fd));

    /* --- 2. 读回来 --- */
    puts("\n[2] read it back");
    fd = open(TESTFILE, O_RDONLY);
    show("open", fd);

    n = read(fd, buf, sizeof(buf) - 1);
    show("read", n);
    if (n > 0) {
        buf[n] = '\0';
        puts("---- file contents ----");
        fputs(buf, STDOUT_FILENO);
        puts("-----------------------");
    }

    /* --- 3. 定位读：直接跳到第 7 个字节 --- */
    puts("\n[3] lseek + partial read");
    show("lseek(7)", lseek(fd, 7, SEEK_SET));

    n = read(fd, buf, 10);
    show("read 10 bytes", n);
    if (n > 0) {
        buf[n] = '\0';
        puts("  got: \"");
        fputs(buf, STDOUT_FILENO);
        puts("\"\n");
    }
    close(fd);

    /* --- 4. 追加 --- */
    puts("\n[4] append");
    fd = open(TESTFILE, O_WRONLY | O_APPEND);
    show("open append", fd);
    show("lseek pos", lseek(fd, 0, SEEK_CUR));

    {
        const char* extra = "third line, appended at the end\n";
        show("write", write(fd, extra, str_len(extra)));
    }
    show("fstat", fstat(fd));
    close(fd);

    /* --- 5. 删除 --- */
    puts("\n[5] unlink");
    show("unlink", unlink(TESTFILE));
    show("open again (should fail)", open(TESTFILE, O_RDONLY));

    puts("\n=====================================");
    puts("done. run 'ls' to check the volume.");

    return 0;
}
