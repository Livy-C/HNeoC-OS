/* t1.c - hncc 诊断程序
 *
 * 输出全部是数字（print_int 已经验证可靠），方便一眼对上：
 * 每项打印"编号 结果"，期望值写在注释里。
 */

int gsum(int* p, int n) {
    int total = 0;
    int i = 0;
    while (i < n) {
        total = total + p[i];
        i = i + 1;
    }
    return total;
}

int main(void) {
    char* p;
    char* g;
    int table[4];
    int v;
    int* q;
    int x;

    p = "ABCD";
    g = "WXYZ";

    /* 1: 局部 char* 的下标取值 -> 65 66 */
    print_int(1);
    putchar(32);
    print_int(p[0]);
    putchar(32);
    print_int(p[1]);
    putchar(10);

    /* 2: 直接给 puts 传字面量 -> GLOBAL-OK */
    print_int(2);
    putchar(32);
    puts("LITERAL-OK");

    /* 3: 数组传参 + 函数里用 int* 下标 -> 10 */
    table[0] = 1;
    table[1] = 2;
    table[2] = 3;
    table[3] = 4;
    print_int(3);
    putchar(32);
    print_int(gsum(table, 4));
    putchar(10);

    /* 4: 取地址 + 解引用 -> 7 */
    v = 7;
    q = &v;
    print_int(4);
    putchar(32);
    print_int(*q);
    putchar(10);

    /* 5: 指针算术 p+2 -> 67 ('C') */
    print_int(5);
    putchar(32);
    print_int(*(p + 2));
    putchar(10);

    /* 6: 全局 char* 下标 -> 88 90 */
    print_int(6);
    putchar(32);
    print_int(g[0]);
    putchar(32);
    print_int(g[3]);
    putchar(10);

    /* 7: 复合赋值 x=5 -> +=3 -> 8 -> *=2 -> 16 */
    x = 5;
    x += 3;
    x *= 2;
    print_int(7);
    putchar(32);
    print_int(x);
    putchar(10);

    /* 8: 三目 -> 1 */
    print_int(8);
    putchar(32);
    print_int(x > 10 ? 1 : 0);
    putchar(10);

    return 0;
}
