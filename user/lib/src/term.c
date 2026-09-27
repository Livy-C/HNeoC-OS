#include <term.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <hneoc.h>

/* ============================================================
 * 终端 / 全屏应用 API
 *
 * 把 ANSI 转义序列包起来，写全屏程序不用再自己拼字符串。
 * 序列本身由内核的控制台解释（drivers/vga.c 里的状态机）。
 *
 * 输出先攒在内部缓冲区里，term_flush 的时候一次性写出去：
 * 一帧要画 25 行的话，这样能从上百次系统调用降到个位数。
 * ============================================================ */

#define OBUF_SIZE 4096

static char obuf[OBUF_SIZE];
static int  olen = 0;

static void oflush(void) {
    if (olen > 0) {
        write(STDOUT_FILENO, obuf, (size_t)olen);
        olen = 0;
    }
}

static void oputc(char c) {
    if (olen >= OBUF_SIZE - 2) {
        oflush();
    }
    obuf[olen++] = c;
}

static void oputs(const char* s) {
    while (*s) {
        oputc(*s++);
    }
}

static void oputuint(int v) {
    char tmp[12];
    int n = 0;

    if (v <= 0) {
        oputc('0');
        return;
    }
    while (v > 0 && n < 11) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        oputc(tmp[--n]);
    }
}

/* 输出一个 CSI 参数序列，比如 "\x1b[" + "12;34" + "H" */
static void csi_start(void) {
    oputc(0x1B);
    oputc('[');
}

static void csi_end(char final) {
    oputc(final);
}

/* 输出 "n" 或者 ";n"（n 为 0 时按默认值处理） */
static void csi_num(int n) {
    oputuint(n);
}

/* ============================================================
 * 初始化与屏幕尺寸
 * ============================================================ */

void term_size(int* rows, int* cols) {
    int packed = (int)__syscall0(SYS_WINSIZE);

    if (rows) { *rows = (packed >> 8) & 0xFF; }
    if (cols) { *cols = packed & 0xFF; }
}

void term_clear(void) {
    oputs("\x1b[2J\x1b[H");
    oflush();
}

void term_init_fullscreen(int* rows, int* cols) {
    /* 切到备用屏幕缓冲：退出时原来的画面会自动恢复。
     * 同时也告诉内核别再截走 PageUp / Home 这些键。
     */
    oputs("\x1b[?1049h\x1b[2J\x1b[H");
    oflush();

    term_size(rows, cols);
}

void term_leave_fullscreen(void) {
    oputs("\x1b[0m\x1b[?25h\x1b[?1049l");
    oflush();
}

/* ============================================================
 * 光标
 * ============================================================ */

void term_gotoxy(int row, int col) {
    if (row < 0) { row = 0; }
    if (col < 0) { col = 0; }

    csi_start();
    csi_num(row + 1);      /* ANSI 的行列从 1 开始 */
    oputc(';');
    csi_num(col + 1);
    csi_end('H');
}

void term_get_cursor(int* row, int* col) {
    /* 内核没有实现 DSR（设备状态报告），拿不到就返回 0 并说明 */
    if (row) { *row = 0; }
    if (col) { *col = 0; }
}

void term_move_up(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('A');
}

void term_move_down(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('B');
}

void term_move_right(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('C');
}

void term_move_left(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('D');
}

void term_show_cursor(int visible) {
    oputs(visible ? "\x1b[?25h" : "\x1b[?25l");
}

/* ============================================================
 * 颜色与属性
 * ============================================================ */

void term_set_color(int fg, int bg) {
    term_set_fg(fg);
    term_set_bg(bg);
}

void term_set_fg(int fg) {
    /* VGA 的 0-7 是普通色，8-15 是亮色，映射到 ANSI 的 30-37 / 90-97 */
    csi_start();
    if (fg & 8) {
        csi_num(90 + (fg & 7));
    } else {
        csi_num(30 + (fg & 7));
    }
    csi_end('m');
}

void term_set_bg(int bg) {
    csi_start();
    if (bg & 8) {
        csi_num(100 + (bg & 7));
    } else {
        csi_num(40 + (bg & 7));
    }
    csi_end('m');
}

void term_reset(void) {
    oputs("\x1b[0m");
}

void term_set_bold(int on) {
    oputs(on ? "\x1b[1m" : "\x1b[22m");
}

void term_set_reverse(int on) {
    oputs(on ? "\x1b[7m" : "\x1b[27m");
}

/* ============================================================
 * 擦除
 * ============================================================ */

void term_clear_line(void) {
    oputs("\x1b[2K");
}

void term_clear_eol(void) {
    oputs("\x1b[0K");
}

void term_clear_bol(void) {
    oputs("\x1b[1K");
}

void term_clear_eos(void) {
    oputs("\x1b[0J");
}

/* ============================================================
 * 滚动区域
 * ============================================================ */

void term_set_scroll_region(int top, int bottom) {
    csi_start();
    csi_num(top + 1);
    oputc(';');
    csi_num(bottom + 1);
    csi_end('r');
}

void term_reset_scroll_region(void) {
    oputs("\x1b[r");
}

void term_scroll_up(int lines) {
    if (lines <= 0) { return; }
    csi_start(); csi_num(lines); csi_end('S');
}

void term_scroll_down(int lines) {
    if (lines <= 0) { return; }
    csi_start(); csi_num(lines); csi_end('T');
}

/* ============================================================
 * 行与字符
 * ============================================================ */

void term_insert_lines(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('L');
}

void term_delete_lines(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('M');
}

void term_insert_chars(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('@');
}

void term_delete_chars(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('P');
}

void term_erase_chars(int n) {
    if (n <= 0) { return; }
    csi_start(); csi_num(n); csi_end('X');
}

/* ============================================================
 * 输出
 * ============================================================ */

void term_write(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        oputc(s[i]);
    }
}

void term_puts(const char* s) {
    oputs(s);
}

void term_putc(char c) {
    oputc(c);
}

void term_printf(const char* fmt, ...) {
    uint32_t* args = (uint32_t*)(&fmt) + 1;
    char tmp[256];

    vsnprintf(tmp, sizeof(tmp), fmt, args);
    oputs(tmp);
}

void term_flush(void) {
    oflush();
}

void term_begin_frame(void) {
    oputs("\x1b[?25l");   /* 重绘前先藏光标，能明显减少闪烁 */
}

void term_end_frame(void) {
    oputs("\x1b[?25h");
    oflush();
}
