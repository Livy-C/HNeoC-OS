#include "../include/vga.h"
#include "../include/string.h"
#include "../include/ports.h"
#include "../include/serial.h"

/* VGA 文本模式缓冲区指针（物理地址 0xB8000） */
static uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;

/* 当前光标位置 */
static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;

/* 当前颜色属性 */
static uint8_t current_color = 0;

/* ------------------------------------------------------------
 * 实时画面在内存里的副本
 *
 * 为什么需要它：往回翻历史的时候屏幕显示的不再是实时画面，
 * 但 VGA 显存不能当唯一数据源 —— 否则一翻页就把实时内容覆盖了。
 * 所以实时画面始终维护在 screen[][] 里，显存只是一个投影。
 * ------------------------------------------------------------ */
static uint16_t screen[VGA_HEIGHT][VGA_WIDTH];

/* 光标是否可见（ANSI 的 ESC[?25h / l） */
static bool cursor_visible = true;

/* 滚动区域（ANSI 的 ESC[t;br）。默认整屏 */
static uint8_t scroll_top    = 0;
static uint8_t scroll_bottom = VGA_HEIGHT - 1;

/* 备用屏幕缓冲（ANSI 的 ESC[?1049h / l）。
 * 全屏程序进来时切过去、退出时切回来，原来的画面原样恢复。 */
static uint16_t alt_screen[VGA_HEIGHT][VGA_WIDTH];
static uint8_t  alt_cursor_x = 0;
static uint8_t  alt_cursor_y = 0;
static bool     alt_active   = false;

/* 保存 / 恢复光标位置（ANSI 的 ESC[s / ESC[u） */
static uint8_t saved_x = 0;
static uint8_t saved_y = 0;

/* ------------------------------------------------------------
 * 回滚缓冲（环形）
 * ------------------------------------------------------------ */
#define SCROLLBACK_MAX 300

static uint16_t scrollback[SCROLLBACK_MAX][VGA_WIDTH];
static uint32_t sb_count       = 0;
static uint32_t sb_next        = 0;
static uint32_t total_scrolled = 0;
static uint32_t view_offset    = 0;

/* CRT 控制器端口，用于控制硬件光标 */
#define VGA_CTRL_REGISTER 0x3D4
#define VGA_DATA_REGISTER 0x3D5
#define VGA_CURSOR_HIGH   0x0E
#define VGA_CURSOR_LOW    0x0F

static inline uint8_t vga_entry_color(vga_color fg, vga_color bg) {
    return (uint8_t)(fg | (bg << 4));
}

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

static inline uint16_t blank_cell(void) {
    return vga_entry(' ', current_color);
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* ------------------------------------------------------------
 * 写一个格子的统一入口：内存副本和显存一起更新
 * ------------------------------------------------------------ */
static inline void set_cell(int y, int x, uint16_t cell) {
    if (y < 0 || y >= VGA_HEIGHT || x < 0 || x >= VGA_WIDTH) {
        return;
    }
    screen[y][x] = cell;
    if (view_offset == 0) {
        vga_buffer[y * VGA_WIDTH + x] = cell;
    }
}

/* ------------------------------------------------------------
 * 光标
 * ------------------------------------------------------------ */
void vga_update_cursor(void) {
    uint16_t pos;

    if (!cursor_visible || view_offset != 0) {
        /* 藏起来：写到 25 行之外，CRT 控制器就不会画光标 */
        pos = (uint16_t)(VGA_WIDTH * VGA_HEIGHT);
    } else {
        pos = (uint16_t)(cursor_y * VGA_WIDTH + cursor_x);
    }

    outb(VGA_CTRL_REGISTER, VGA_CURSOR_HIGH);
    outb(VGA_DATA_REGISTER, (uint8_t)((pos >> 8) & 0xFF));
    outb(VGA_CTRL_REGISTER, VGA_CURSOR_LOW);
    outb(VGA_DATA_REGISTER, (uint8_t)(pos & 0xFF));
}

void vga_get_cursor(int* row, int* col) {
    if (row) *row = cursor_y;
    if (col) *col = cursor_x;
}

/* 取实时画面某个格子的内容。
 * 串口日志是线性的字符流，看不出光标定位和擦除的效果；
 * 这个接口让 screendump 之类的调试命令能直接读到渲染结果。
 */
uint16_t vga_get_cell(int row, int col) {
    if (row < 0 || row >= VGA_HEIGHT || col < 0 || col >= VGA_WIDTH) {
        return blank_cell();
    }
    return screen[row][col];
}

void vga_cursor_to(int row, int col) {
    cursor_y = (uint8_t)imin(imax(row, 0), VGA_HEIGHT - 1);
    cursor_x = (uint8_t)imin(imax(col, 0), VGA_WIDTH - 1);
    vga_update_cursor();
}

void vga_move_cursor(int drow, int dcol) {
    vga_cursor_to(cursor_y + drow, cursor_x + dcol);
}

void vga_show_cursor(bool visible) {
    cursor_visible = visible;
    vga_update_cursor();
}

/* ------------------------------------------------------------
 * 回滚缓冲
 * ------------------------------------------------------------ */
static void scrollback_push(const uint16_t* line) {
    for (int x = 0; x < VGA_WIDTH; x++) {
        scrollback[sb_next][x] = line[x];
    }
    sb_next = (sb_next + 1) % SCROLLBACK_MAX;
    if (sb_count < SCROLLBACK_MAX) {
        sb_count++;
    }
}

static const uint16_t* scrollback_line(uint32_t index) {
    uint32_t oldest = (sb_next + SCROLLBACK_MAX - sb_count) % SCROLLBACK_MAX;
    return scrollback[(oldest + index) % SCROLLBACK_MAX];
}

/* ------------------------------------------------------------
 * 视角重绘
 * ------------------------------------------------------------ */

/* 视角变化时往串口写一行追踪，便于没有图形界面时自动化验证 */
static void scroll_trace(void) {
    char line[65];
    int i = 0;
    uint32_t v;

    if (view_offset == 0) {
        serial_write("[scroll] back to live view\n");
        return;
    }

    serial_write("[scroll] offset=");
    {
        char tmp[11];
        int n = 0;

        v = view_offset;
        while (v > 0 && n < 10) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
        while (n > 0) { serial_putchar(tmp[--n]); }
    }
    serial_write("/");
    {
        char tmp[11];
        int n = 0;

        v = sb_count;
        while (v > 0 && n < 10) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
        while (n > 0) { serial_putchar(tmp[--n]); }
    }

    serial_write("  top: ");
    for (int x = 0; x < 60; x++) {
        char c = (char)(vga_buffer[x] & 0xFF);
        if (c < 32 || c > 126) { c = ' '; }
        line[i++] = c;
    }
    line[i] = '\0';
    serial_write(line);
    serial_write("\n");
}

static void draw_scroll_indicator(void) {
    char text[24];
    int p = 0;
    uint32_t v = view_offset;
    char tmp[11];
    int n = 0;
    uint8_t color = vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    int start;

    text[p++] = ' ';
    text[p++] = '-';
    text[p++] = '-';
    text[p++] = ' ';

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < 10) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    }
    text[p++] = ' ';
    while (n > 0) { text[p++] = tmp[--n]; }
    text[p++] = ' ';
    text[p++] = ' ';
    text[p++] = '-';
    text[p++] = '-';
    text[p++] = ' ';
    text[p]   = '\0';

    start = VGA_WIDTH - p;
    if (start < 0) { start = 0; }

    for (int i = 0; text[i] && start + i < VGA_WIDTH; i++) {
        vga_buffer[start + i] = vga_entry(text[i], color);
    }
}

static void render_view(void) {
    uint16_t blank = blank_cell();

    if (view_offset == 0) {
        for (int y = 0; y < VGA_HEIGHT; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                vga_buffer[y * VGA_WIDTH + x] = screen[y][x];
            }
        }
        scroll_trace();
        vga_update_cursor();
        return;
    }

    {
        uint32_t top = total_scrolled - view_offset;

        for (int r = 0; r < VGA_HEIGHT; r++) {
            uint32_t line = top + (uint32_t)r;

            for (int x = 0; x < VGA_WIDTH; x++) {
                uint16_t cell;

                if (line < total_scrolled) {
                    uint32_t hist_base = total_scrolled - sb_count;
                    cell = (line >= hist_base) ? scrollback_line(line - hist_base)[x]
                                               : blank;
                } else {
                    uint32_t live = line - total_scrolled;
                    cell = (live < VGA_HEIGHT) ? screen[live][x] : blank;
                }
                vga_buffer[r * VGA_WIDTH + x] = cell;
            }
        }
    }

    draw_scroll_indicator();
    scroll_trace();
    vga_update_cursor();
}

void vga_scrollback(int lines) {
    int32_t off = (int32_t)view_offset + lines;

    if (off < 0) { off = 0; }
    if (off > (int32_t)sb_count) { off = (int32_t)sb_count; }
    if ((uint32_t)off == view_offset) { return; }

    view_offset = (uint32_t)off;
    render_view();
}

void vga_scrollback_top(void) {
    if (sb_count == 0) { return; }
    view_offset = sb_count;
    render_view();
}

void vga_scrollback_reset(void) {
    if (view_offset == 0) { return; }
    view_offset = 0;
    render_view();
}

bool vga_in_scrollback(void)     { return view_offset != 0; }
uint32_t vga_scrollback_depth(void) { return view_offset; }
uint32_t vga_scrollback_lines(void) { return sb_count; }

/* ------------------------------------------------------------
 * 屏幕操作原语（ANSI 解释器用）
 * ------------------------------------------------------------ */

/* 上滚一行。scrolling 只在滚动区域里进行 */
static void scroll_up_region(void) {
    int top    = scroll_top;
    int bottom = scroll_bottom;

    /* 只有滚动区域从第 0 行开始时，滚出去的内容才进历史缓冲 */
    if (top == 0) {
        scrollback_push(screen[0]);
        total_scrolled++;
        if (view_offset > 0 && view_offset < sb_count) {
            view_offset++;
        }
    }

    for (int y = top; y < bottom; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            screen[y][x] = screen[y + 1][x];
        }
    }
    for (int x = 0; x < VGA_WIDTH; x++) {
        screen[bottom][x] = blank_cell();
    }

    if (view_offset == 0) {
        for (int y = top; y <= bottom; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                vga_buffer[y * VGA_WIDTH + x] = screen[y][x];
            }
        }
    }
}

/* 在滚动区域里下滚 n 行 */
void vga_scroll_down_region(int lines) {
    int top    = scroll_top;
    int bottom = scroll_bottom;

    if (lines <= 0) { return; }
    if (lines > bottom - top + 1) { lines = bottom - top + 1; }

    for (int y = bottom; y >= top + lines; y--) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            set_cell(y, x, screen[y - lines][x]);
        }
    }
    for (int y = top; y < top + lines; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            set_cell(y, x, blank_cell());
        }
    }
}

/* 在滚动区域里上滚 n 行（ANSI 的 ESC[nS） */
void vga_scroll_up_region(int lines) {
    if (lines <= 0) { return; }
    while (lines-- > 0) {
        scroll_up_region();
    }
}

void vga_set_scroll_region(int top, int bottom) {
    if (top < 0) { top = 0; }
    if (bottom >= VGA_HEIGHT) { bottom = VGA_HEIGHT - 1; }
    if (top >= bottom) { return; }   /* 非法区域直接忽略 */

    scroll_top    = (uint8_t)top;
    scroll_bottom = (uint8_t)bottom;

    /* 规范要求设置滚动区域后光标回到左上角（区域顶部） */
    vga_cursor_to(top, 0);
}

void vga_reset_scroll_region(void) {
    scroll_top    = 0;
    scroll_bottom = VGA_HEIGHT - 1;
}

/* 在光标处插入 n 行，区域内的行往下挤，溢出区域的行被丢弃 */
void vga_insert_lines(int lines) {
    int top    = cursor_y;
    int bottom = scroll_bottom;

    if (lines <= 0) { return; }
    if (cursor_y < scroll_top || cursor_y > scroll_bottom) { return; }
    if (lines > bottom - top + 1) { lines = bottom - top + 1; }

    for (int y = bottom; y >= top + lines; y--) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            set_cell(y, x, screen[y - lines][x]);
        }
    }
    for (int y = top; y < top + lines; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            set_cell(y, x, blank_cell());
        }
    }
    vga_cursor_to(top, 0);
}

/* 删除光标处的 n 行，下面的行往上提 */
void vga_delete_lines(int lines) {
    int top    = cursor_y;
    int bottom = scroll_bottom;

    if (lines <= 0) { return; }
    if (cursor_y < scroll_top || cursor_y > scroll_bottom) { return; }
    if (lines > bottom - top + 1) { lines = bottom - top + 1; }

    for (int y = top; y <= bottom - lines; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            set_cell(y, x, screen[y + lines][x]);
        }
    }
    for (int y = bottom - lines + 1; y <= bottom; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            set_cell(y, x, blank_cell());
        }
    }
    vga_cursor_to(top, 0);
}

/* 在光标处插入 n 个空格，右侧字符右移 */
void vga_insert_chars(int count) {
    if (count <= 0) { return; }

    for (int x = VGA_WIDTH - 1; x >= cursor_x + count; x--) {
        set_cell(cursor_y, x, screen[cursor_y][x - count]);
    }
    for (int x = cursor_x; x < cursor_x + count && x < VGA_WIDTH; x++) {
        set_cell(cursor_y, x, blank_cell());
    }
}

/* 删除光标处的 n 个字符，右侧字符左移 */
void vga_delete_chars(int count) {
    if (count <= 0) { return; }

    for (int x = cursor_x; x < VGA_WIDTH; x++) {
        int src = x + count;
        set_cell(cursor_y, x, src < VGA_WIDTH ? screen[cursor_y][src]
                                              : blank_cell());
    }
}

/* 擦掉光标处的 n 个字符（用空格覆盖） */
void vga_erase_chars(int count) {
    for (int i = 0; i < count && cursor_x + i < VGA_WIDTH; i++) {
        set_cell(cursor_y, cursor_x + i, blank_cell());
    }
}

/* ANSI 的 ED：0=光标到屏幕末尾 1=屏幕开头到光标 2=整屏 3=整屏并清历史 */
void vga_erase_in_display(int mode) {
    if (mode == 2 || mode == 3) {
        for (int y = 0; y < VGA_HEIGHT; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                set_cell(y, x, blank_cell());
            }
        }
        if (mode == 3) {
            sb_count = 0;
            sb_next  = 0;
        }
        return;
    }

    if (mode == 0) {
        for (int x = cursor_x; x < VGA_WIDTH; x++) {
            set_cell(cursor_y, x, blank_cell());
        }
        for (int y = cursor_y + 1; y < VGA_HEIGHT; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                set_cell(y, x, blank_cell());
            }
        }
    } else if (mode == 1) {
        for (int y = 0; y < cursor_y; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                set_cell(y, x, blank_cell());
            }
        }
        for (int x = 0; x <= cursor_x; x++) {
            set_cell(cursor_y, x, blank_cell());
        }
    }
}

/* ANSI 的 EL：0=光标到行尾 1=行首到光标 2=整行 */
void vga_erase_in_line(int mode) {
    if (mode == 2) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            set_cell(cursor_y, x, blank_cell());
        }
    } else if (mode == 1) {
        for (int x = 0; x <= cursor_x; x++) {
            set_cell(cursor_y, x, blank_cell());
        }
    } else {
        for (int x = cursor_x; x < VGA_WIDTH; x++) {
            set_cell(cursor_y, x, blank_cell());
        }
    }
}

/* ------------------------------------------------------------
 * 备用屏幕缓冲
 * ------------------------------------------------------------ */
void vga_use_alternate_screen(bool on) {
    if (on == alt_active) {
        return;
    }

    if (on) {
        /* 存下当前画面和光标，然后清屏给全屏程序用 */
        alt_cursor_x = cursor_x;
        alt_cursor_y = cursor_y;
        for (int y = 0; y < VGA_HEIGHT; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                alt_screen[y][x] = screen[y][x];
            }
        }

        alt_active = true;
        vga_reset_scroll_region();

        for (int y = 0; y < VGA_HEIGHT; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                set_cell(y, x, blank_cell());
            }
        }
        vga_cursor_to(0, 0);
    } else {
        for (int y = 0; y < VGA_HEIGHT; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                set_cell(y, x, alt_screen[y][x]);
            }
        }
        alt_active = false;
        vga_reset_scroll_region();
        vga_cursor_to(alt_cursor_y, alt_cursor_x);
    }
}

bool vga_alternate_active(void) {
    return alt_active;
}

/* ------------------------------------------------------------
 * ANSI / VT100 转义序列解释
 * ------------------------------------------------------------ */

#define ANSI_MAX_PARAMS 8

typedef enum {
    ANS_NORMAL,
    ANS_ESC,      /* 收到 ESC */
    ANS_CSI,      /* 收到 ESC [ */
    ANS_OSC       /* 收到 ESC ] ，忽略到 BEL 或 ESC \ */
} ansi_state_t;

static ansi_state_t ans_state       = ANS_NORMAL;
static int          ans_param[ANSI_MAX_PARAMS];
static int          ans_count       = 0;
static int          ans_value       = 0;
static bool         ans_has_value   = false;
static bool         ans_private     = false;

/* 当前的 SGR 状态，用 ANSI 的色号保存，实际颜色在 apply 时算 */
static uint8_t ans_fg_index  = 7;      /* 0-7 */
static uint8_t ans_bg_index  = 0;
static bool    ans_fg_bright = false;  /* 来自 90-97 */
static bool    ans_bg_bright = false;
static bool    ans_bold      = false;
static bool    ans_reverse   = false;

/* ANSI 色号 -> VGA 颜色。两边的顺序不一样，必须查表 */
static const uint8_t ansi_to_vga[8] = {
    VGA_COLOR_BLACK, VGA_COLOR_RED, VGA_COLOR_GREEN, VGA_COLOR_BROWN,
    VGA_COLOR_BLUE, VGA_COLOR_MAGENTA, VGA_COLOR_CYAN, VGA_COLOR_LIGHT_GREY
};
static const uint8_t ansi_to_vga_bright[8] = {
    VGA_COLOR_DARK_GREY, VGA_COLOR_LIGHT_RED, VGA_COLOR_LIGHT_GREEN,
    VGA_COLOR_YELLOW, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_LIGHT_MAGENTA,
    VGA_COLOR_LIGHT_CYAN, VGA_COLOR_WHITE
};

static void ansi_apply_color(void) {
    bool fg_bright = ans_fg_bright || ans_bold;
    uint8_t fg = fg_bright ? ansi_to_vga_bright[ans_fg_index]
                           : ansi_to_vga[ans_fg_index];
    uint8_t bg = ans_bg_bright ? ansi_to_vga_bright[ans_bg_index]
                               : ansi_to_vga[ans_bg_index];

    if (ans_reverse) {
        uint8_t t = fg;
        fg = bg;
        bg = t;
    }
    vga_set_color((vga_color)fg, (vga_color)bg);
}

static void ansi_sgr(int code) {
    if (code == 0) {
        ans_fg_index = 7;  ans_fg_bright = false;
        ans_bg_index = 0;  ans_bg_bright = false;
        ans_bold = false;  ans_reverse = false;
    } else if (code == 1) {
        ans_bold = true;
    } else if (code == 22) {
        ans_bold = false;
    } else if (code == 7) {
        ans_reverse = true;
    } else if (code == 27) {
        ans_reverse = false;
    } else if (code >= 30 && code <= 37) {
        ans_fg_index = (uint8_t)(code - 30);
        ans_fg_bright = false;
    } else if (code >= 90 && code <= 97) {
        ans_fg_index = (uint8_t)(code - 90);
        ans_fg_bright = true;
    } else if (code >= 40 && code <= 47) {
        ans_bg_index = (uint8_t)(code - 40);
        ans_bg_bright = false;
    } else if (code >= 100 && code <= 107) {
        ans_bg_index = (uint8_t)(code - 100);
        ans_bg_bright = true;
    } else if (code == 39) {
        ans_fg_index = 7;
        ans_fg_bright = false;
    } else if (code == 49) {
        ans_bg_index = 0;
        ans_bg_bright = false;
    }
    /* 其余（下划线、闪烁等）VGA 文本模式表达不了，忽略 */

    ansi_apply_color();
}

/* 参数缺省时取 1（光标移动类序列的默认值） */
static int ansi_p(int index, int fallback) {
    if (index >= ans_count) { return fallback; }
    return ans_param[index];
}

static void ansi_execute_csi(char final) {
    int p0 = ansi_p(0, 0);
    int p1 = ansi_p(1, 0);

    /* 带 '?' 前缀的是私有序列 */
    if (ans_private) {
        if (final == 'h' || final == 'l') {
            bool on = (final == 'h');
            for (int i = 0; i < ans_count; i++) {
                if (ans_param[i] == 25) {
                    vga_show_cursor(on);            /* 光标显示 / 隐藏 */
                } else if (ans_param[i] == 1049 || ans_param[i] == 47) {
                    vga_use_alternate_screen(on);   /* 备用屏幕缓冲 */
                }
            }
        }
        return;
    }

    switch (final) {
        case 'A': vga_move_cursor(-imax(p0, 1), 0); break;   /* 上 */
        case 'B': vga_move_cursor(imax(p0, 1), 0);  break;   /* 下 */
        case 'C': vga_move_cursor(0, imax(p0, 1));  break;   /* 右 */
        case 'D': vga_move_cursor(0, -imax(p0, 1)); break;   /* 左 */
        case 'E': vga_cursor_to(cursor_y + imax(p0, 1), 0); break;  /* 下一行行首 */
        case 'F': vga_cursor_to(cursor_y - imax(p0, 1), 0); break;  /* 上一行行首 */

        case 'G': vga_cursor_to(cursor_y, imax(p0, 1) - 1); break;   /* 指定列 */
        case 'd': vga_cursor_to(imax(p0, 1) - 1, cursor_x); break;   /* 指定行 */

        case 'H':   /* CUP：行列都从 1 开始 */
        case 'f':
            vga_cursor_to(imax(p0, 1) - 1, imax(p1, 1) - 1);
            break;

        case 'J': vga_erase_in_display(p0); break;
        case 'K': vga_erase_in_line(p0);    break;

        case 'm':
            if (ans_count == 0) {
                ansi_sgr(0);
            } else {
                for (int i = 0; i < ans_count; i++) {
                    ansi_sgr(ans_param[i]);
                }
            }
            break;

        case 'r':   /* DECSTBM：设置滚动区域 */
            if (ans_count == 0) {
                vga_reset_scroll_region();
                vga_cursor_to(0, 0);
            } else {
                vga_set_scroll_region(imax(p0, 1) - 1,
                                      p1 > 0 ? p1 - 1 : VGA_HEIGHT - 1);
            }
            break;

        case 'S': vga_scroll_up_region(imax(p0, 1));   break;   /* 上滚 */
        case 'T': vga_scroll_down_region(imax(p0, 1)); break;   /* 下滚 */
        case 'L': vga_insert_lines(imax(p0, 1));       break;   /* 插入行 */
        case 'M': vga_delete_lines(imax(p0, 1));       break;   /* 删除行 */
        case 'P': vga_delete_chars(imax(p0, 1));       break;   /* 删除字符 */
        case '@': vga_insert_chars(imax(p0, 1));       break;   /* 插入字符 */
        case 'X': vga_erase_chars(imax(p0, 1));        break;   /* 擦除字符 */

        case 's': saved_x = cursor_x; saved_y = cursor_y; break; /* 保存光标 */
        case 'u': vga_cursor_to(saved_y, saved_x);        break; /* 恢复光标 */

        /* DSR（设备状态报告）需要往输入流回写，暂时不做；
         * 各种模式设置（h/l）对 VGA 文本模式也没有意义，一并忽略。
         */
        default: break;
    }
}

static void ansi_reset_params(void) {
    ans_count     = 0;
    ans_value     = 0;
    ans_has_value = false;
    ans_private   = false;
    for (int i = 0; i < ANSI_MAX_PARAMS; i++) {
        ans_param[i] = 0;
    }
}

static void ansi_push_param(void) {
    if (ans_count < ANSI_MAX_PARAMS) {
        ans_param[ans_count++] = ans_has_value ? ans_value : 0;
    }
    ans_value     = 0;
    ans_has_value = false;
}

/* 处理一个普通可见字符（含控制字符） */
static void vga_putchar_raw(char c);

/* 喂一个字节给状态机 */
static void ansi_feed(char c) {
    switch (ans_state) {
        case ANS_NORMAL:
            if (c == 0x1B) {
                ansi_reset_params();
                ans_state = ANS_ESC;
            } else {
                vga_putchar_raw(c);
            }
            return;

        case ANS_ESC:
            if (c == '[') {
                ans_state = ANS_CSI;
            } else if (c == ']') {
                ans_state = ANS_OSC;         /* 窗口标题之类的，整段丢掉 */
            } else {
                /* 单字符转义序列：ESC c = 完全复位，其余忽略 */
                if (c == 'c') {
                    vga_reset_scroll_region();
                    vga_erase_in_display(2);
                    vga_cursor_to(0, 0);
                    ansi_sgr(0);
                    vga_show_cursor(true);
                }
                ans_state = ANS_NORMAL;
            }
            return;

        case ANS_CSI:
            if (c >= '0' && c <= '9') {
                ans_value = ans_value * 10 + (c - '0');
                if (ans_value > 9999) { ans_value = 9999; }
                ans_has_value = true;
                return;
            }
            if (c == ';') {
                ansi_push_param();
                return;
            }
            if (c == '?' && ans_count == 0 && !ans_has_value) {
                ans_private = true;
                return;
            }
            if (c == '>' || c == '!' || c == '=') {
                ans_private = true;   /* 别的私有前缀，同样只做忽略处理 */
                return;
            }
            if (c >= 0x20 && c <= 0x2F) {
                return;               /* 中间字节，忽略 */
            }
            if (c >= 0x40 && c <= 0x7E) {
                ansi_push_param();    /* 终结字节：先把当前参数收尾 */
                ansi_execute_csi(c);
                ans_state = ANS_NORMAL;
                return;
            }
            ans_state = ANS_NORMAL;   /* 非法字节，放弃这个序列 */
            return;

        case ANS_OSC:
            /* 一直吃到 BEL 或 ESC \ 为止 */
            if (c == 0x07) {
                ans_state = ANS_NORMAL;
            } else if (c == 0x1B) {
                ans_state = ANS_ESC;  /* 交给 ESC 分支处理剩下的 '\' */
            }
            return;
    }
}

/* ------------------------------------------------------------
 * 原始字符输出（不含转义解释）
 * ------------------------------------------------------------ */
static void vga_putchar_raw(char c) {
    /* 串口镜像放在这一层：转义序列本身不写进日志，免得刷屏 */
    if (c == '\n' || (c >= 32 && c < 127)) {
        serial_putchar(c);
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (uint8_t)((cursor_x + 8) & ~(8 - 1));
    } else if (c == '\b') {
        vga_backspace();
        return;
    } else {
        set_cell(cursor_y, cursor_x, vga_entry(c, current_color));
        cursor_x++;
    }

    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }

    /* 越过滚动区域下沿就上滚 */
    if (cursor_y > scroll_bottom) {
        scroll_up_region();
        cursor_y = scroll_bottom;
    }

    vga_update_cursor();
}

/* ------------------------------------------------------------
 * 对外输出接口
 * ------------------------------------------------------------ */

/* 带回滚状态的字符输出：内核自己的输出也走这里，
 * 只是内核字符串里没有 ESC，解释器原样透传。
 */
void vga_putchar(char c) {
    ansi_feed(c);
}

void vga_write(const char* str) {
    if (!str) return;
    while (*str) {
        ansi_feed(*str++);
    }
}

void vga_writeln(const char* str) {
    vga_write(str);
    vga_putchar('\n');
}

/* 按长度写出（用户程序用，字符串里可能含 0 字节） */
void vga_write_n(const char* str, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        ansi_feed(str[i]);
    }
}

void vga_write_uint(uint32_t value) {
    char buf[11];
    int i = 0;

    if (value == 0) {
        vga_putchar('0');
        return;
    }
    while (value > 0 && i < 10) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        vga_putchar(buf[--i]);
    }
}

void vga_write_hex(uint32_t value) {
    const char* digits = "0123456789ABCDEF";
    char buf[9];

    for (int i = 0; i < 8; i++) {
        buf[i] = digits[(value >> ((7 - i) * 4)) & 0xF];
    }
    buf[8] = '\0';

    vga_write("0x");
    vga_write(buf);
}

/* ------------------------------------------------------------
 * 光标回退与行内擦除
 * ------------------------------------------------------------ */
void vga_backspace(void) {
    if (cursor_x > 0) {
        cursor_x--;
    } else if (cursor_y > scroll_top) {
        cursor_y--;
        cursor_x = VGA_WIDTH - 1;
    } else {
        return;
    }

    set_cell(cursor_y, cursor_x, blank_cell());
    vga_update_cursor();
}

void vga_clear_to_end_of_line(void) {
    for (int x = cursor_x; x < VGA_WIDTH; x++) {
        set_cell(cursor_y, x, blank_cell());
    }
    vga_update_cursor();
}

/* ------------------------------------------------------------
 * 初始化与清屏
 * ------------------------------------------------------------ */
void vga_set_color(vga_color fg, vga_color bg) {
    current_color = vga_entry_color(fg, bg);
}

void vga_init(void) {
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    cursor_x = 0;
    cursor_y = 0;

    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            screen[y][x]     = blank_cell();
            vga_buffer[y * VGA_WIDTH + x] = blank_cell();
        }
    }

    vga_reset_scroll_region();
    ansi_reset_params();
    ans_state = ANS_NORMAL;
    vga_update_cursor();
}

void vga_clear(void) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            screen[y][x] = blank_cell();
            vga_buffer[y * VGA_WIDTH + x] = blank_cell();
        }
    }

    cursor_x = 0;
    cursor_y = 0;
    view_offset = 0;

    /* 历史缓冲不清空：和终端一样，清屏后还能往回翻 */
    vga_update_cursor();
}
