#ifndef _TERM_H
#define _TERM_H

#include "stddef.h"

/* ============================================================
 * 终端 / 全屏应用 API
 *
 * 这一层包住 ANSI 转义序列，写全屏程序不用再拼 "\x1b[..."。
 * 内核的控制台会解释这些序列（见 README 的转义序列表）。
 *
 * 典型用法：
 *
 *     int rows, cols;
 *     term_init_fullscreen(&rows, &cols);   // 进备用屏幕
 *
 *     for (;;) {
 *         term_begin_frame();               // 隐藏光标
 *         term_gotoxy(0, 0);
 *         term_printf("hello");
 *         term_end_frame();                 // flush + 显示光标
 *
 *         int k = getkey();
 *         if (k == 'q') break;
 *     }
 *
 *     term_leave_fullscreen();              // 回到原来的画面
 * ============================================================ */

/* 颜色编号，对应内核 vga_color */
enum {
    TERM_BLACK = 0,
    TERM_BLUE,
    TERM_GREEN,
    TERM_CYAN,
    TERM_RED,
    TERM_MAGENTA,
    TERM_BROWN,
    TERM_LIGHT_GREY,
    TERM_DARK_GREY,
    TERM_LIGHT_BLUE,
    TERM_LIGHT_GREEN,
    TERM_LIGHT_CYAN,
    TERM_LIGHT_RED,
    TERM_LIGHT_MAGENTA,
    TERM_YELLOW,
    TERM_WHITE
};

/* ---- 初始化与屏幕尺寸 ---- */

/* 取屏幕行数和列数，任意一个指针可以为 NULL */
void term_size(int* rows, int* cols);

/* 清屏并把光标移到左上角 */
void term_clear(void);

/* 进入全屏模式：切到备用屏幕缓冲并清屏。
 * 这时候内核不再截走 PageUp / Home 之类的按键，全部交给程序；
 * 退出时原来的画面会原样恢复。
 * rows / cols 可以为 NULL
 */
void term_init_fullscreen(int* rows, int* cols);

/* 退出全屏模式，恢复进入之前的画面 */
void term_leave_fullscreen(void);

/* ---- 光标 ---- */

/* 绝对定位，行列都从 0 开始（对外用 0 基更符合直觉，
 * 0 基转 1 基的活由这一层干） */
void term_gotoxy(int row, int col);
void term_get_cursor(int* row, int* col);

void term_move_up(int n);
void term_move_down(int n);
void term_move_left(int n);
void term_move_right(int n);

/* 显示 / 隐藏光标。整屏重绘时先藏起来能明显减少闪烁 */
void term_show_cursor(int visible);

/* ---- 颜色与属性 ---- */

void term_set_color(int fg, int bg);
void term_set_fg(int fg);
void term_set_bg(int bg);
void term_reset(void);          /* 属性全部恢复默认 */

void term_set_bold(int on);
void term_set_reverse(int on);

/* ---- 擦除 ---- */

void term_clear_line(void);     /* 整行 */
void term_clear_eol(void);      /* 光标到行尾 */
void term_clear_bol(void);      /* 行首到光标 */
void term_clear_eos(void);      /* 光标到屏幕末尾 */

/* ---- 滚动区域 ---- */

/* 只让 [top, bottom] 这几行参与滚动，编辑器靠它固定状态行。
 * 行列从 0 开始，设置后光标会回到区域左上角
 */
void term_set_scroll_region(int top, int bottom);
void term_reset_scroll_region(void);
void term_scroll_up(int lines);
void term_scroll_down(int lines);

/* ---- 行与字符的插入 / 删除 ---- */

void term_insert_lines(int n);
void term_delete_lines(int n);
void term_insert_chars(int n);
void term_delete_chars(int n);
void term_erase_chars(int n);

/* ---- 输出 ----
 *
 * 输出先进内部缓冲区，攒够或者调用 term_flush 才真正写出去。
 * 一帧画很多东西的时候，这样能少发几十次系统调用。
 */

void term_write(const char* s, size_t n);
void term_puts(const char* s);
void term_putc(char c);
void term_printf(const char* fmt, ...);
void term_flush(void);

/* 一帧的开始 / 结束：开始时藏光标，结束时刷新并显示光标 */
void term_begin_frame(void);
void term_end_frame(void);

#endif /* _TERM_H */
