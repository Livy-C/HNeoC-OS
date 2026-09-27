/* fib.c - 递归、数组、指针、while 都过一遍
 *
 *     hncc /share/hncc/fib.c
 *     ./fib.lxe
 */

int fib(int n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int sum(int* p, int n) {
    int total = 0;
    int i = 0;

    while (i < n) {
        total = total + p[i];
        i = i + 1;
    }
    return total;
}

int main(void) {
    int table[12];
    int i;
    char* msg = "fib:";

    for (i = 0; i < 12; i = i + 1) {
        table[i] = fib(i);
    }

    puts(msg);
    for (i = 0; i < 12; i = i + 1) {
        putchar(' ');
        print_int(table[i]);
    }
    putchar(10);

    puts("sum of the table:");
    print_int(sum(table, 12));
    putchar(10);

    return 0;
}
