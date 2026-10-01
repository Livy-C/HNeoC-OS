/* t6.c - typedef、结构体前向声明、运行时字符串函数
 *
 * 期望输出：
 *     1 42       互相引用的两个结构体（b 先用前向声明开个头）
 *     2 7        顺着指针绕回来：first.other->other->value
 *     3 42 8     typedef 出来的类型 + sizeof(typedef 名)
 *     4 5        参数类型也用 typedef
 *     5 abcdef 6   strcpy + strcat + strlen
 *     6 1 0      strchr 找到 / 找不到
 *     7 cdef 0   strstr 返回的是指针（找到就指向那一处），找不到是 0
 *     8 0        memcmp：前两个字节相同
 *     9 1        NULL 是内建常量
 */

struct b;                    /* 前向声明：让 a 和 b 互相引用 */

struct a {
    int value;
    struct b* other;
};

struct b {
    int value;
    struct a* other;
};

typedef struct a A;          /* typedef 结构体 */
typedef int Number;          /* typedef 基本类型 */
typedef char* String;        /* typedef 指针 */

int slen(String s) {
    return strlen(s);
}

int main(void) {
    A first;
    struct b second;
    Number n = 42;
    String msg = "hello";
    char buf[32];
    A* p;

    first.value = 7;
    second.value = 35;
    first.other = &second;         /* a -> b */
    second.other = &first;         /* b -> a，绕回来 */
    p = &first;

    printf("1 %d\n", p->value + p->other->value);
    printf("2 %d\n", first.other->other->value);
    printf("3 %d %d\n", n, sizeof(A));
    printf("4 %d\n", slen(msg));

    strcpy(buf, "abc");
    strcat(buf, "def");
    printf("5 %s %d\n", buf, strlen(buf));

    printf("6 %d %d\n", strchr(buf, 'd') != 0, strchr(buf, 'z') != 0);
    printf("7 %s %d\n", strstr(buf, "cde"), strstr(buf, "xyz") != 0);
    printf("8 %d\n", memcmp("abc", "abd", 2));

    if (strchr(buf, 'z') == NULL) {
        printf("9 %d\n", 1);
    } else {
        printf("9 %d\n", 0);
    }

    return 0;
}
