/* hello.c - hncc 的入门例子
 *
 * 在 HNeoC 里这样用：
 *     hncc /share/hncc/hello.c
 *     ./hello.lxe              （或者直接 hello）
 *
 * 注意：不需要 #include，也不需要链接任何库 —— putchar / puts /
 * print_int / malloc 这些都是编译器内建的（见 hncc 的说明）。
 */

int score = 7;                  /* 有初值的全局变量 -> 数据区 */
int counter;                    /* 没初值的全局变量 -> bss 区 */

int add(int a, int b) {
    return a + b;
}

void say_hello(void) {
    puts("hello from a program that hncc compiled on HNeoC");
}

int main(void) {
    char name[6];
    int i;

    name[0] = 'h';
    name[1] = 'n';
    name[2] = 'c';
    name[3] = 'c';
    name[4] = 0;
    name[5] = 0;

    say_hello();
    puts(name);

    print_int(score);
    putchar(10);

    for (i = 0; i < 5; i = i + 1) {
        counter = counter + add(i, score);
    }
    puts("counter after the loop:");
    print_int(counter);
    putchar(10);

    return 0;
}
