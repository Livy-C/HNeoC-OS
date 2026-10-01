/* t5.c - enum 与 switch 的诊断程序
 *
 * 期望输出：
 *     1 0 1 2 3        enum 的隐式取值，以及多声明符 `int a = 1, b = 2;`
 *     2 0 7 -3         enum 的显式取值（含负数）
 *     3 100 200 300 999  switch/case/default，含 "case 2: case 3:" 连着写
 *     4 14             switch 在循环里：continue 要跳循环，break 只跳 switch
 *     5 110            fall-through（故意不写 break）
 *     6 27             enum 名字当常量用在表达式里
 *     7 10000          switch 里的 continue：两万次迭代不能漏栈
 *     8 42             switch 体里的局部变量要有栈槽
 *     9 -3             case 的值可以是 enum 常量（含负数）
 *    10 2              case 的值可以是常量表达式（WARN - 5）
 */

enum color { RED, GREEN, BLUE };
enum code { OK = 0, WARN = 7, FAIL = -3 };

int describe(int c) {
    int r;

    switch (c) {
        case 0:
            r = 100;
            break;
        case 1:
            r = 200;
            break;
        case 2:
        case 3:
            r = 300;
            break;
        default:
            r = 999;
            break;
    }
    return r;
}

int main(void) {
    int i;
    int total = 0;
    int first = 1, second = 2;

    printf("1 %d %d %d %d\n", RED, GREEN, BLUE, first + second);
    printf("2 %d %d %d\n", OK, WARN, FAIL);
    printf("3 %d %d %d %d\n", describe(0), describe(1), describe(3), describe(9));

    for (i = 0; i < 6; i = i + 1) {
        switch (i) {
            case 1:
                continue;          /* 跳到循环的步进，不算这一次 */
            case 4:
                break;             /* 只跳出 switch，后面照常累加 */
            default:
                break;
        }
        total = total + i;
    }
    printf("4 %d\n", total);

    {
        int f = 0;

        switch (2) {
            case 1: f = f + 1;
            case 2: f = f + 10;
            case 3: f = f + 100;
        }
        printf("5 %d\n", f);
    }

    printf("6 %d\n", BLUE * 10 + WARN);

    /* continue 从 switch 里跳出去的时候，栈顶那个待比较的临时值必须弹掉。
     * 这里两万次迭代：每次漏 4 字节就是 80KB，用户栈只有 32KB —— 漏了
     * 必崩（或者更糟：把栈写花），不漏就稳稳打出 10000。 */
    {
        int k;
        int hits = 0;

        for (k = 0; k < 20000; k = k + 1) {
            switch (k & 1) {
                case 0:
                    continue;
                default:
                    hits = hits + 1;
                    break;
            }
        }
        printf("7 %d\n", hits);
    }

    /* switch 体里的局部变量：漏掉这一支的话它没有栈槽（偏移 0），
     * 赋值会写到保存的 ebp 上。 */
    {
        switch (3) {
            case 3: {
                int local = 41;

                printf("8 %d\n", local + 1);
                break;
            }
            default:
                break;
        }
    }

    {
        int v = -3;

        switch (v) {
            case FAIL:
                printf("9 %d\n", FAIL);
                break;
            default:
                printf("9 default\n");
                break;
        }
    }

    {
        switch (2) {
            case WARN - 5:
                printf("10 %d\n", 2);
                break;
            default:
                printf("10 default\n");
                break;
        }
    }

    return 0;
}
