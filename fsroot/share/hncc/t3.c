/* t3.c - 数组初始化列表与常见修饰词
 *
 * 期望输出（每行"编号 结果"）：
 *     1 1234   全局 int 数组的初始化列表
 *     2 hncc   全局 char 数组逐字符初始化
 *     3 12     int a[] = {...} 长度自动推断（3 * 4 字节）
 *     4 30     推断出来的数组内容对
 *     5 ac     char w[] = "abc" 推断出 4 字节（含结尾 0）
 *     6 5008   局部 int 数组的初始化列表
 *     7 xyz    局部 char 数组用字符串初始化
 *     8 109    const / long / unsigned 这些修饰词能编译，且值正确（99 + 7 + 3）
 */

static int table[4] = {1, 2, 3, 4};
char msg[5] = {'h', 'n', 'c', 'c', 0};
int inferred[] = {10, 20, 30};
char word[] = "abc";
const int limit = 99;
long counter = 7;

int main(void) {
    int local[4] = {5, 6, 7, 8};
    char lbuf[] = "xyz";
    unsigned hits = 3;

    print_int(1);
    putchar(32);
    print_int(table[0] * 1000 + table[1] * 100 + table[2] * 10 + table[3]);
    putchar(10);

    print_int(2);
    putchar(32);
    puts(msg);

    print_int(3);
    putchar(32);
    print_int(sizeof(inferred));
    putchar(10);

    print_int(4);
    putchar(32);
    print_int(inferred[2]);
    putchar(10);

    print_int(5);
    putchar(32);
    putchar(word[0]);
    putchar(word[2]);
    putchar(10);

    print_int(6);
    putchar(32);
    print_int(local[0] * 1000 + local[3]);
    putchar(10);

    print_int(7);
    putchar(32);
    puts(lbuf);

    print_int(8);
    putchar(32);
    print_int(limit + counter + hits);
    putchar(10);

    return 0;
}
