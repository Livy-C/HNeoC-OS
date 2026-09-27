#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

/* 特殊按键的编码。
 *
 * 必须用 int 传递，不能塞进 char：char 只有 8 位，0x101 会被截断成 0x01，
 * 那样 KEY_PAGEUP 就变成了 0x05，和 switch 里的常量永远匹配不上；
 * 更糟的是 KEY_END(0x108) 会截断成 0x08，和退格('\b') 撞在一起。
 */
#define KEY_UP        0x101
#define KEY_DOWN      0x102
#define KEY_LEFT      0x103
#define KEY_RIGHT     0x104
#define KEY_PAGEUP    0x105
#define KEY_PAGEDOWN  0x106
#define KEY_HOME      0x107
#define KEY_END       0x108
#define KEY_DELETE    0x109

/* 初始化 PS/2 键盘（注册 IRQ1 处理函数） */
void keyboard_init(void);

/* 键盘缓冲区中是否有字符可取 */
bool keyboard_has_char(void);

/* 取一个按键。
 * 普通字符返回 ASCII 值（0-255），功能键返回上面的 KEY_* 常量。
 * 无键可读时阻塞等待。
 * 翻页键（PageUp / PageDown / Home / End）在这里就地处理掉，不返回。
 */
int keyboard_getchar(void);

/* 当前是否按住了 Shift */
bool keyboard_shift_pressed(void);

#endif /* KEYBOARD_H */
