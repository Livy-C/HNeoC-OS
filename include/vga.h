#ifndef VGA_H
#define VGA_H

#include "types.h"

/* VGA 文本模式常量 */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

/* VGA 颜色定义 */
typedef enum {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_YELLOW = 14,
    VGA_COLOR_WHITE = 15,
} vga_color;

/* 初始化与清屏 */
void vga_init(void);
void vga_clear(void);

/* 字符与字符串输出。
 *
 * vga_putchar / vga_write 会解释 ANSI 转义序列，所以全屏程序
 * （光标定位、擦除、颜色、滚动区域、备用屏幕）直接写转义序列就行。
 * 内核自己的字符串里没有 ESC，会原样透传。
 */
void vga_putchar(char c);
void vga_write(const char* str);
void vga_writeln(const char* str);
void vga_write_n(const char* str, uint32_t len);

/* 数字输出 */
void vga_write_uint(uint32_t value);
void vga_write_hex(uint32_t value);

/* 颜色控制 */
void vga_set_color(vga_color fg, vga_color bg);

/* 光标控制 */
void vga_update_cursor(void);
void vga_backspace(void);
void vga_clear_to_end_of_line(void);

/* ------------------------------------------------------------
 * 屏幕操作原语（ANSI 转义序列解释器内部使用，
 * 用户程序也可以直接通过转义序列间接触发）
 * ------------------------------------------------------------ */
void vga_cursor_to(int row, int col);
void vga_move_cursor(int drow, int dcol);
void vga_get_cursor(int* row, int* col);
void vga_show_cursor(bool visible);

/* 读实时画面某个格子的内容（低字节字符，高字节属性）。
 * 串口日志是线性的字符流，看不出光标定位和擦除的效果，
 * screendump 之类的调试命令用它直接读渲染结果。
 */
uint16_t vga_get_cell(int row, int col);

/* 滚动区域：只在 [top, bottom] 之间滚动，编辑器靠它固定状态行 */
void vga_set_scroll_region(int top, int bottom);
void vga_reset_scroll_region(void);
void vga_scroll_up_region(int lines);
void vga_scroll_down_region(int lines);

/* 行与字符的插入 / 删除 / 擦除 */
void vga_insert_lines(int lines);
void vga_delete_lines(int lines);
void vga_insert_chars(int count);
void vga_delete_chars(int count);
void vga_erase_chars(int count);
void vga_erase_in_display(int mode);
void vga_erase_in_line(int mode);

/* 备用屏幕缓冲：全屏程序进来切过去，退出切回来 */
void vga_use_alternate_screen(bool on);
bool vga_alternate_active(void);

/* ------------------------------------------------------------
 * 回滚缓冲
 * ------------------------------------------------------------ */

/* lines > 0 往上看更旧的内容，lines < 0 往下回到实时画面 */
void vga_scrollback(int lines);

/* 直接跳到缓冲里最旧的一行 */
void vga_scrollback_top(void);

/* 回到实时画面 */
void vga_scrollback_reset(void);

/* 当前是否正在看历史 */
bool vga_in_scrollback(void);

/* 当前往回翻了多少行 */
uint32_t vga_scrollback_depth(void);

/* 历史缓冲里现存多少行 */
uint32_t vga_scrollback_lines(void);

#endif /* VGA_H */
