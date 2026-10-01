/* t7.c - typedef 的写法、声明符列表里的指针、负数的十六进制打印
 *
 * 期望输出：
 *     1 11 22 8   匿名结构体的 typedef：typedef struct { ... } Pair;
 *     2 33 44     带名字的结构体 typedef：typedef struct node {...} Node;
 *     3 7 6       声明符列表：`int a = 1, *b;` 和 `int* c, d;`
 *     4 9         typedef int A, *B;（B 才是指针）
 *     5 8         typedef int *C2, D2;（D2 只是 int，不是指针）
 *     6 6         函数体里面的 typedef
 *     7 ffffffff  printf("%x", -1)：负数按 32 位补码打满 8 位
 */

typedef struct {
    int x;
    int y;
} Pair;

typedef struct node {
    int v;
    struct node* next;
} Node;

typedef int A, *B;
typedef int *C2, D2;

int main(void) {
    Pair p;
    Node n1;
    Node n2;
    int a = 1, *b;
    int* c, d;

    p.x = 11;
    p.y = 22;
    printf("1 %d %d %d\n", p.x, p.y, sizeof(Pair));

    n1.v = 33;
    n2.v = 44;
    n1.next = &n2;
    printf("2 %d %d\n", n1.v, n1.next->v);

    b = &a;
    *b = 7;                       /* 通过 b 改 a */
    d = 5;
    c = &d;
    *c = 6;                       /* 通过 c 改 d */
    printf("3 %d %d\n", a, d);

    {
        A e = 8;
        B pe = &e;

        *pe = 9;
        printf("4 %d\n", e);
    }

    {
        C2 pc;
        D2 dd = 5;

        pc = &dd;
        *pc = 8;
        printf("5 %d\n", dd);
    }

    {
        typedef int Small;
        Small s = 6;

        printf("6 %d\n", s);
    }

    printf("7 %x\n", -1);

    return 0;
}
