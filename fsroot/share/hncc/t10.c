/* t10.c - fprintf / vsnprintf / fputs / fputc / 终端颜色
 *
 * 这一层是给自举试验补的：hncc 自己的源码要用 fprintf 报错、用 vsnprintf
 * 拼消息、用 term_set_fg/term_reset 上色，而编译器又忽略 #include，
 * 所以这些必须内建在运行时里。
 *
 * 期望输出：
 *     1 42 7ff    fprintf 送到 fd 2
 *     2 5 abcde   vsnprintf 拼进缓冲区（字符串里 10 个字符）
 *     2 ret 10    返回值 = 写进去的长度
 *     01234       截断：max=6 只装得下 5 个字符
 *     3 ret 10    返回值是"本来想写多少"
 *     4 hi!       fputs 整串 + fputc 单个字符
 *     5 OK        带颜色输出（串口镜像会跳过转义序列，所以日志里只看得到 OK）
 *     6 0 1 2     STDIN_FILENO / STDOUT_FILENO / STDERR_FILENO 是内建常量
 */

#include <hneoc.h>

/* 运行时的 vsnprintf 收的是"指向第一个可变参数的指针"（见 hncc.c 里 fatal()
 * 的用法），所以这里包一层变参函数来转发 —— 顺便也测了"自己的变参函数"。 */
int my_snprintf(char* out, int max, char* fmt, ...) {
    int* args = &fmt;

    args = args + 1;
    return vsnprintf(out, max, fmt, args);
}

int main(void) {
    char buf[32];
    char small[6];
    int  n;

    fprintf(2, "1 %d %x\n", 42, 2047);

    n = my_snprintf(buf, sizeof(buf), "2 %d %s\n", 5, "abcde");
    putstr(buf);
    printf("2 ret %d\n", n);

    n = my_snprintf(small, sizeof(small), "0123456789", 0);
    fputs(small, STDOUT_FILENO);
    putchar('\n');
    printf("3 ret %d\n", n);

    fputs("4 hi", STDOUT_FILENO);
    fputc('!', STDOUT_FILENO);
    putchar('\n');

    fputs("5 ", STDOUT_FILENO);
    term_set_fg(TERM_LIGHT_RED);
    fputs("OK", STDOUT_FILENO);
    term_reset();
    putchar('\n');

    printf("6 %d %d %d\n", STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);

    return 0;
}
