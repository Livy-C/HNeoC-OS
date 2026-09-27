/* ls - 列出 HNeoFS 里的目录
 *
 * 这个程序不是内核里的内置命令，而是磁盘上 /bin/ls.lxe 里的一个
 * ring 3 程序。目录名从命令行参数拿（内核通过 getargs 交给它）。
 *
 *   ls            列根目录
 *   ls /bin       列 /bin
 */

#include <hneoc.h>

int main(void) {
    char args[128];
    char path[128];
    char name[48];
    int index;
    int count = 0;

    /* --- 解析参数：第一个词就是目录 --- */
    getargs(args, sizeof(args));

    {
        int i = 0;
        int n = 0;

        while (args[i] == ' ' || args[i] == '\t') { i++; }
        while (args[i] && args[i] != ' ' && args[i] != '\t' &&
               n < (int)sizeof(path) - 1) {
            path[n++] = args[i++];
        }
        path[n] = '\0';

        if (n == 0) {
            path[0] = '/';
            path[1] = '\0';
        }
    }

    printf("HNeoFS  %s\n", path);
    printf("--------------------------------------------------\n");
    printf("  name                 size      kind\n");

    for (index = 0; ; index++) {
        int r = listdir(path, index, name, sizeof(name));

        if (r < 0) {
            break;   /* 没有更多条目了 */
        }

        if (IS_DIR(r)) {
            term_set_fg(TERM_LIGHT_CYAN);
            printf("  %-19s %8s  dir\n", name, "-");
            term_reset();
        } else {
            int is_exec = (strstr(name, ".lxe") != NULL);

            if (is_exec) {
                term_set_fg(TERM_LIGHT_GREEN);
            } else {
                term_reset();
            }
            printf("  %-19s %8d  %s\n", name, DIR_SIZE(r),
                   is_exec ? "exec" : "data");
            term_reset();
        }
        count++;
    }

    printf("--------------------------------------------------\n");
    printf("  %d entries\n", count);

    return 0;
}
