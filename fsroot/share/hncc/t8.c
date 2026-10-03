/* t8.c - 内建类型别名（stdint/stddef 那一套名字）
 *
 * 这个编译器不看 #include，所以 uint32_t / size_t / bool 这些名字是它
 * 内建的（见 hncc.c 的 TYPE_SOURCE）。这组诊断盯着三件事：
 *   1. 这些名字真的能当类型用（声明、参数、返回值、sizeof）；
 *   2. 用户自己再 typedef 一遍同一个类型不算错（现实代码里到处都是）；
 *   3. 定义成**别的**类型仍然要报错（那是真打架，下面第 9 项用不到，
 *      但有回归检查盯着错误信息，见 tools/regress.ps1）。
 *
 * 宽度说明：这个子集里只有 8 位的 char 和 32 位的 int，没有 16 位类型，
 * 所以 uint8_t/int8_t 是真的 1 字节（就是 char），而 uint16_t 只能退化成
 * 4 字节 —— 下面第 5 项的 sizeof 就把这件事打出来。
 *
 * 期望输出：
 *     1 1 4 4      sizeof(uint8_t) sizeof(uint32_t) sizeof(size_t)
 *     2 42 42      各种别名声明的变量
 *     3 7          用别名当参数类型和返回值
 *     4 1 0        bool 类型的真假
 *     5 4 1        sizeof(uint16_t)=4（没有 16 位类型）、sizeof(int8_t)=1
 *     6 12345678   重复 typedef 同一个类型不算错，值照用
 */

typedef unsigned int uint32_t;      /* 编译器内建过一遍，这里再来一遍 */
typedef unsigned int size_t;        /* 同上 */

typedef uint32_t myword;

int add_word(uint32_t a, uint32_t b) {
    return a + b;
}

size_t word_size(void) {
    return sizeof(uint32_t);
}

int main(void) {
    uint8_t  a = 42;
    uint32_t b = 42;
    size_t   n = 4;
    bool     yes = 1;
    bool     no = 0;
    myword   big = 12345678;

    printf("1 %d %d %d\n", sizeof(uint8_t), sizeof(uint32_t), sizeof(size_t));
    printf("2 %d %d\n", a, b);
    printf("3 %d\n", add_word(3, 4));
    printf("4 %d %d\n", yes, no);
    printf("5 %d %d\n", sizeof(uint16_t), sizeof(int8_t));
    printf("6 %d\n", big);

    if (word_size() != n) {
        printf("6 broken\n");
    }
    return 0;
}
