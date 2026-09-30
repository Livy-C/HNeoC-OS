/* t2.c - hncc 的结构体诊断程序
 *
 * 输出全是"编号 结果"，直接和注释里的期望值对照：
 *     1 34    局部结构体字段
 *     2 34    通过指针访问（->）
 *     3 42    链表：a -> b，a.next->value
 *     4 8     sizeof(struct point)
 *     5 8     sizeof(struct node)
 *     6 40    通过指针改字段，改的就是原变量
 *     7 56    结构体数组：pts[1].x/.y
 *     8 24    sizeof(pts) = 8 * 3
 *     9 5     让函数通过指针读取结构体
 */

struct point {
    int x;
    int y;
};

struct node {
    int value;
    struct node* next;
};

/* 结构体只能通过指针传参，这个函数演示标准用法 */
int sum_point(struct point* p) {
    return p->x + p->y;
}

int main(void) {
    struct point p;
    struct point* q;
    struct point pts[3];
    struct node a;
    struct node b;
    int total;

    p.x = 3;
    p.y = 4;
    q = &p;

    print_int(1);
    putchar(32);
    print_int(p.x * 10 + p.y);
    putchar(10);

    print_int(2);
    putchar(32);
    print_int(q->x * 10 + q->y);
    putchar(10);

    /* 链表：a 指向 b */
    a.value = 7;
    a.next = &b;
    b.value = 35;
    b.next = 0;

    total = a.value + a.next->value;
    print_int(3);
    putchar(32);
    print_int(total);
    putchar(10);

    print_int(4);
    putchar(32);
    print_int(sizeof(struct point));
    putchar(10);

    print_int(5);
    putchar(32);
    print_int(sizeof(struct node));
    putchar(10);

    /* 通过指针写字段 */
    q->y = 40;
    print_int(6);
    putchar(32);
    print_int(p.y);
    putchar(10);

    /* 结构体数组 */
    pts[1].x = 5;
    pts[1].y = 6;
    print_int(7);
    putchar(32);
    print_int(pts[1].x * 10 + pts[1].y);
    putchar(10);

    print_int(8);
    putchar(32);
    print_int(sizeof(pts));
    putchar(10);

    print_int(9);
    putchar(32);
    print_int(sum_point(&p));
    putchar(10);

    return 0;
}
