/* note - 每次运行都往 notes.txt 追加一行
 *
 * 用来验证写操作真的落到了磁盘上：跑几次，然后重启（不重新打包镜像），
 * 再用 cat 读出来，之前写的内容应该还在。
 */

#include "hneoc.h"

#define NOTEFILE "notes.txt"

int main(void) {
    char line[96];
    char buf[512];
    int n = 0;
    int fd;
    int got;

    /* --- 追加一行 --- */
    fd = open(NOTEFILE, O_WRONLY | O_APPEND | O_CREAT);
    if (fd < 0) {
        fputs("cannot open notes.txt, error ", STDOUT_FILENO);
        put_int(fd);
        putchar('\n');
        return 1;
    }

    /* 手工拼一行："note added at N s after boot\n" */
    {
        const char* prefix = "note added at ";
        const char* suffix = " s after boot\n";
        uint32_t t = uptime();
        char num[11];
        int d = 0;

        while (*prefix) { line[n++] = *prefix++; }

        if (t == 0) {
            num[d++] = '0';
        } else {
            while (t > 0 && d < 10) { num[d++] = (char)('0' + (t % 10)); t /= 10; }
        }
        while (d > 0) { line[n++] = num[--d]; }

        while (*suffix) { line[n++] = *suffix++; }
    }

    write(fd, line, (uint32_t)n);
    fsync(fd);

    fputs("appended: ", STDOUT_FILENO);
    write(1, line, (uint32_t)n);

    /* --- 读回整个文件 --- */
    fputs("\n---- ", STDOUT_FILENO);
    fputs(NOTEFILE, STDOUT_FILENO);
    puts(" now contains ----");

    {
        int rfd = open(NOTEFILE, O_RDONLY);

        if (rfd < 0) {
            puts("(cannot reopen)");
            close(fd);
            return 1;
        }

        got = read(rfd, buf, sizeof(buf) - 1);
        if (got > 0) {
            buf[got] = '\0';
            fputs(buf, STDOUT_FILENO);
        } else {
            puts("(empty)");
        }
        puts("-------------------------------");
        fputs("total ", STDOUT_FILENO);
        put_int(got);
        puts(" bytes on disk");
        close(rfd);
    }

    close(fd);
    return 0;
}
