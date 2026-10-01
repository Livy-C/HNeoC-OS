/* t4.c - 变参函数与 printf
 *
 * printf 不是编译器内建的，而是运行时里用 hncc 自己编译的一段 C 代码，
 * 靠"从第一个参数在栈上的位置往后数"取可变参数（和 user/lib 里同一招）。
 *
 * 期望输出：
 *     1 42                %d
 *     2 hncc has 4        %s 和 %d 混用
 *     3 xyz               %c
 *     4 ff -5 12          %x %d %u
 *     5 1 2 3 4 5         一次给五个可变参数
 *     6 n=7%              %s%d%% 连着写
 *     7 107               普通语句仍然正常
 */

int main(void) {
    int n = 7;
    char* name = "hncc";

    printf("1 %d\n", 42);
    printf("2 %s has %d\n", name, 4);
    printf("3 %c%c%c\n", 'x', 'y', 'z');
    printf("4 %x %d %u\n", 255, -5, 12);
    printf("5 %d %d %d %d %d\n", 1, 2, 3, 4, 5);
    printf("6 %s%d%%\n", "n=", n);

    print_int(7);
    putchar(32);
    print_int(100 + n);
    putchar(10);

    return 0;
}
