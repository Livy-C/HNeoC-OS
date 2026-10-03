/* t9.c - #define 的常量表达式
 *
 * 以前 #define 只认"名字 十进制数"这一种写法，别的一律**静默忽略**：
 * `(96 * 1024)`、`0x1F`、`(1 << 3)`、引用另一个常量……全都不算数，
 * 然后在用到的地方报一个毫不相干的 "undeclared variable"。
 * 自举试验第一轮卡在 hncc.c 第 96 行的 CODE_CAP 就是这个原因。
 *
 * 期望输出：
 *     1 98304     #define CODE_CAP (96 * 1024)
 *     2 31        0x1F 十六进制
 *     3 8         1 << 3
 *     4 7         引用另一个常量再运算（A + B * 2）
 *     5 12        括号决定优先级，(2 + 4) * 2
 *     6 3         % 取余
 *     7 4         0x10 & 0x0F | 0x04、以及 ~ 和 ! 这些位运算
 *     8 65 10     'A' 和 '\n' 这样的字符常量
 *     9 1024      自己引用自己之前定义的值（CHAIN 用 BASE）
 *    10 9 abcdefgh 数组长度也可以是常量表达式（不只是数字字面量）
 */

#define CODE_CAP (96 * 1024)
#define MASK     0x1F
#define SHIFTED  (1 << 3)
#define A        3
#define B        2
#define CHAIN    (A + B * 2)
#define PARENS   ((2 + 4) * 2)
#define MOD      (10 % 7)
#define BITS     ((0x10 & 0x0F) | 0x04)
#define CHAR_A   'A'
#define NEWLINE  '\n'
#define BASE     1024
#define DERIVED  (BASE * 1)
#define ARR_LEN  (4 * 2 + 1)

/* 数组长度用常量表达式 */
static char arr[ARR_LEN] = "abcdefgh";

/* 看不懂的宏仍然要被静默忽略（函数式宏、字符串宏），不能报错 */
#define SYM(x)   x
#define GREETING "hello"

int main(void) {
    printf("1 %d\n", CODE_CAP);
    printf("2 %d\n", MASK);
    printf("3 %d\n", SHIFTED);
    printf("4 %d\n", CHAIN);
    printf("5 %d\n", PARENS);
    printf("6 %d\n", MOD);
    printf("7 %d\n", BITS);
    printf("8 %d %d\n", CHAR_A, NEWLINE);
    printf("9 %d\n", DERIVED);
    printf("10 %d %s\n", sizeof(arr), arr);

    return 0;
}
