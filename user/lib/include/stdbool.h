#ifndef _STDBOOL_H
#define _STDBOOL_H

/* 用 _Bool（C99 内建），不要自己 typedef：
 * GCC 从 C23 起把 bool/true/false 当关键字，重新定义会编译报错。 */
#define bool  _Bool
#define true  1
#define false 0
#define __bool_true_false_are_defined 1

#endif /* _STDBOOL_H */
