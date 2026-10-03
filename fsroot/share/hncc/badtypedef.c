/* badtypedef.c - 故意写错的源码：把内建类型改成别的类型
 *
 * uint32_t 是编译器内建的（TYPE_SOURCE）。再 typedef 成**同一个**类型
 * 不算错（现实代码里到处都是），但 definition 成别的类型就是在打架了 ——
 * 静默覆盖会让同一份源码在不同的地方对同一个名字有两种理解，
 * 所以这种情况必须报错。tools/regress.ps1 里有一条检查盯着这句错误。
 */

typedef char uint32_t;

int main(void) {
    uint32_t x = 1;

    return x;
}
