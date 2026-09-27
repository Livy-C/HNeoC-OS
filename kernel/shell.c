/* ============================================================
 * HNeoC OS Shell
 *
 * 界面文字一律使用 ASCII：VGA 文本模式是 CP437 字符集，没有汉字字形。
 * 中文只出现在注释里。
 * ============================================================ */

#include "../include/shell.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/keyboard.h"
#include "../include/timer.h"
#include "../include/ports.h"
#include "../include/kernel.h"
#include "../include/e820.h"
#include "../include/pmm.h"
#include "../include/heap.h"
#include "../include/paging.h"
#include "../include/ata.h"
#include "../include/hneofs.h"
#include "../include/process.h"
#include "../include/syscall.h"
#include "../include/mouse.h"

/* 键盘驱动送来的特殊按键编码在 keyboard.h 里统一定义 */

/* ------------------------------------------------------------
 * 命令行编辑状态
 * ------------------------------------------------------------ */
static char line_buffer[SHELL_MAX_LINE];
static int  line_length = 0;

/* 历史记录环形缓冲 */
static char history[SHELL_HISTORY_LEN][SHELL_MAX_LINE];
static int  history_count = 0;   /* 已记录的命令条数（最多 SHELL_HISTORY_LEN） */
static int  history_index = -1;  /* 浏览历史时的当前位置 */

/* ------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------ */

/* 跳过字符串开头的空格 */
static char* skip_spaces(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* 取出第 index 个参数（0 起算），写入 out */
static void get_arg(const char* s, int index, char* out, int out_size) {
    int current = 0;
    int i = 0;

    while (*s) {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '\0') {
            break;
        }

        if (current == index) {
            while (*s && *s != ' ' && *s != '\t' && i < out_size - 1) {
                out[i++] = *s++;
            }
            break;
        }

        while (*s && *s != ' ' && *s != '\t') {
            s++;
        }
        current++;
    }

    out[i] = '\0';
}

/* 把颜色名转换成 VGA 颜色枚举，未识别返回 -1 */
static int color_from_name(const char* name) {
    static const char* names[] = {
        "black", "blue", "green", "cyan", "red", "magenta", "brown",
        "lightgrey", "darkgrey", "lightblue", "lightgreen", "lightcyan",
        "lightred", "lightmagenta", "yellow", "white"
    };

    for (int i = 0; i < 16; i++) {
        if (strcmp(name, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------
 * 数字与大小格式化
 *
 * 注意：这里只做 2 的幂次方除法，编译器会优化成移位。
 * 一旦出现非常量的 64 位除法，就会需要 libgcc 的 __udivdi3，
 * 而我们是用 -nostdlib 链接的，链接会直接失败。
 * ------------------------------------------------------------ */

static void print_size64(uint64_t bytes) {
    if (bytes >= (1ull << 30)) {
        vga_write_uint((uint32_t)(bytes >> 30));
        vga_write(" GB");
    } else if (bytes >= (1ull << 20)) {
        vga_write_uint((uint32_t)(bytes >> 20));
        vga_write(" MB");
    } else if (bytes >= (1ull << 10)) {
        vga_write_uint((uint32_t)(bytes >> 10));
        vga_write(" KB");
    } else {
        vga_write_uint((uint32_t)bytes);
        vga_write(" B");
    }
}

static void print_size32(uint32_t bytes) {
    print_size64((uint64_t)bytes);
}

/* ------------------------------------------------------------
 * 命令行渲染
 * ------------------------------------------------------------ */

/* 提示符 "hneoc> "，固定 8 个字符宽 */
#define PROMPT_WIDTH 8

static void shell_prompt(void) {
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_write("hneoc");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("> ");
}

/* 把当前 line_buffer 重画到屏幕上（翻历史时用） */
static void redraw_line(void) {
    vga_putchar('\r');                       /* 回到行首 */

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (int i = 0; i < PROMPT_WIDTH; i++) {
        vga_putchar(' ');                    /* 用空格盖掉旧内容 */
    }
    vga_putchar('\r');

    shell_prompt();

    for (int i = 0; i < line_length; i++) {
        vga_putchar(line_buffer[i]);
    }
    vga_clear_to_end_of_line();
}

/* 读取一行输入，返回 line_buffer */
static char* shell_readline(void) {
    line_length = 0;
    line_buffer[0] = '\0';
    history_index = -1;

    for (;;) {
        /* 没有按键时 keyboard_getchar 内部会 hlt，不会空转烧 CPU。
         * 返回值用 int 接：功能键是 0x101 起的常量，char 装不下。
         */
        int c = keyboard_getchar();

        /* 如果正翻在历史里，那么"任意键返回实时画面"。
         * 这个键被吃掉，免得在看不见提示符的地方误输入命令。
         */
        if (vga_in_scrollback()) {
            vga_scrollback_reset();
            redraw_line();
            continue;
        }

        if (c == '\n') {
            vga_putchar('\n');
            line_buffer[line_length] = '\0';
            return line_buffer;
        }

        if (c == '\b') {
            if (line_length > 0) {
                line_length--;
                vga_backspace();
            }
            continue;
        }

        if (c == KEY_UP) {
            if (history_count > 0) {
                if (history_index == -1) {
                    history_index = history_count - 1;
                } else if (history_index > 0) {
                    history_index--;
                }
                strncpy(line_buffer, history[history_index], SHELL_MAX_LINE);
                line_length = (int)strlen(line_buffer);
                redraw_line();
            }
            continue;
        }

        if (c == KEY_DOWN) {
            if (history_index != -1) {
                if (history_index < history_count - 1) {
                    history_index++;
                    strncpy(line_buffer, history[history_index], SHELL_MAX_LINE);
                } else {
                    history_index = -1;
                    line_buffer[0] = '\0';
                }
                line_length = (int)strlen(line_buffer);
                redraw_line();
            }
            continue;
        }

        /* 左右方向键和 Tab 暂不支持，直接忽略 */
        if (c == KEY_LEFT || c == KEY_RIGHT || c == '\t') {
            continue;
        }

        /* 可打印字符 */
        if (c >= 32 && c < 127 && line_length < SHELL_MAX_LINE - 1) {
            line_buffer[line_length++] = (char)c;
            vga_putchar((char)c);
        }
    }
}

/* 把非空命令写入历史记录 */
static void history_add(const char* cmd) {
    if (cmd[0] == '\0') {
        return;
    }

    /* 与上一条相同则不重复记录 */
    if (history_count > 0) {
        int last = (history_count < SHELL_HISTORY_LEN) ? history_count - 1
                                                       : SHELL_HISTORY_LEN - 1;
        if (strcmp(history[last], cmd) == 0) {
            return;
        }
    }

    if (history_count < SHELL_HISTORY_LEN) {
        strncpy(history[history_count], cmd, SHELL_MAX_LINE);
        history_count++;
    } else {
        /* 缓冲满了就整体前移，丢掉最旧的一条 */
        for (int i = 1; i < SHELL_HISTORY_LEN; i++) {
            strncpy(history[i - 1], history[i], SHELL_MAX_LINE);
        }
        strncpy(history[SHELL_HISTORY_LEN - 1], cmd, SHELL_MAX_LINE);
    }
}

/* ------------------------------------------------------------
 * 内置命令
 * ------------------------------------------------------------ */

static void cmd_help(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("HNeoC OS built-in commands");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    vga_writeln("  help                show this help");
    vga_writeln("  clear               clear the screen");
    vga_writeln("  echo <text>         print text back");
    vga_writeln("  about               what HNeoC OS is");
    vga_writeln("  version             kernel version");
    vga_writeln("  uptime              time since boot");
    vga_writeln("  ticks               raw timer ticks");
    vga_writeln("  mem                 memory map, frame allocator, heap");
    vga_writeln("  heap                run the allocator self-test");
    vga_writeln("  diskinfo            ATA devices and a PIO read test");
    vga_writeln("  mouse               PS/2 mouse and scrollback status");
    vga_writeln("  screendump [colors] dump the screen as text (debug aid)");
    vga_writeln("  ls                  list files on the HNeoFS volume");
    vga_writeln("  cat <file>          print a file");
    vga_writeln("  fsstat              filesystem superblock and usage");
    vga_writeln("  exec <program>      load and run a program from disk (ring 3)");
    vga_writeln("  spawn <program>     run it in the background");
    vga_writeln("  taskmgr             task list, CPU and memory usage");
    vga_writeln("  kill <pid>          terminate a process");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("");
    vga_writeln("  Any other name is looked up on disk first, so a file such as");
    vga_writeln("  ls.lxe becomes a command you can just type. The names above are");
    vga_writeln("  only the built-in fallbacks.");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_writeln("  color <fg> <bg>     change the palette");
    vga_writeln("  colors              list colour names");
    vga_writeln("  history             show command history");
    vga_writeln("  banner              reprint the boot banner");
    vga_writeln("  panic <msg>         trigger a kernel panic");
    vga_writeln("  reboot              reboot the machine");
    vga_writeln("  halt                stop the CPU");

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

static void cmd_colors(void) {
    static const char* names[] = {
        "black", "blue", "green", "cyan", "red", "magenta", "brown",
        "lightgrey", "darkgrey", "lightblue", "lightgreen", "lightcyan",
        "lightred", "lightmagenta", "yellow", "white"
    };

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("VGA text-mode colours:");

    for (int i = 0; i < 16; i++) {
        char buf[32];
        int p = 0;

        /* 手工拼出 "  [xx] name"，避免引入 sprintf */
        buf[p++] = ' ';
        buf[p++] = ' ';
        buf[p++] = '[';
        if (i >= 10) {
            buf[p++] = (char)('0' + i / 10);
        }
        buf[p++] = (char)('0' + i % 10);
        buf[p++] = ']';
        buf[p++] = ' ';

        const char* n = names[i];
        while (*n && p < 30) {
            buf[p++] = *n++;
        }
        buf[p] = '\0';

        vga_set_color((vga_color)i, VGA_COLOR_BLACK);
        vga_write(buf);

        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_write((i % 4) == 3 ? "\n" : "   ");
    }

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_putchar('\n');
}

static void cmd_echo(const char* args) {
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_writeln(args);
}

static void cmd_about(void) {
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_writeln("HNeoC OS - a small Unix-like system for learning OS internals");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_writeln("");
    vga_writeln("Not a Linux clone: a minimal kernel written from scratch.");
    vga_writeln("  - custom boot sector, switches to 32-bit protected mode");
    vga_writeln("  - flat-segment kernel running entirely in ring 0");
    vga_writeln("  - 8259A PIC remapping plus a 256-entry IDT");
    vga_writeln("  - PS/2 keyboard and PIT timer drivers");
    vga_writeln("  - this shell is what you are typing into right now");
}

static void cmd_version(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_write("HNeoC OS kernel version ");
    vga_writeln(KERNEL_VERSION);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_writeln("target: x86 (i386, 32-bit protected mode)");
}

static void cmd_uptime(void) {
    uint32_t seconds = timer_get_uptime_seconds();

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("uptime: ");
    vga_write_uint(seconds / 60);
    vga_write(" min ");
    vga_write_uint(seconds % 60);
    vga_write(" sec  (");
    vga_write_uint(seconds);
    vga_writeln(" seconds total)");
}

static void cmd_ticks(void) {
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("timer ticks: ");
    vga_write_uint(timer_get_ticks());
    vga_writeln("");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_write("frequency: ");
    vga_write_uint(TIMER_HZ);
    vga_writeln(" Hz (one IRQ0 every 10 ms)");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * 字符串拼装小工具
 * 没有 sprintf，只能自己拼；这些函数都返回新的写入位置。
 * ------------------------------------------------------------ */

static int append_str(char* buf, int pos, int max, const char* s) {
    while (*s && pos < max - 1) {
        buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}

static int append_uint(char* buf, int pos, int max, uint32_t v) {
    char tmp[11];
    int n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < 10) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (n > 0 && pos < max - 1) {
        buf[pos++] = tmp[--n];
    }
    buf[pos] = '\0';
    return pos;
}

static int append_hex(char* buf, int pos, int max, uint32_t value, int digits) {
    const char* d = "0123456789ABCDEF";

    for (int i = digits - 1; i >= 0; i--) {
        if (pos < max - 1) {
            buf[pos++] = d[(value >> (i * 4)) & 0xF];
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* 格式化成 "0x00000000"；高 32 位非 0 时输出 16 位 */
static void format_addr(uint64_t v, char* out, int out_size) {
    int p = 0;
    uint32_t hi = (uint32_t)(v >> 32);

    p = append_str(out, p, out_size, "0x");
    if (hi != 0) {
        p = append_hex(out, p, out_size, hi, 8);
    }
    (void)append_hex(out, p, out_size, (uint32_t)v, 8);
}

/* 格式化成 "512 MB" 这类形式，右侧补空格便于列对齐。
 * 单位换算做四舍五入，否则 4MB 的堆会因为减掉块头而显示成 3MB。
 */
static void format_size(uint64_t bytes, char* out, int out_size) {
    int p = 0;

    if (bytes >= (1ull << 30)) {
        p = append_uint(out, p, out_size, (uint32_t)((bytes + (1ull << 29)) >> 30));
        p = append_str(out, p, out_size, " GB");
    } else if (bytes >= (1ull << 20)) {
        p = append_uint(out, p, out_size, (uint32_t)((bytes + (1ull << 19)) >> 20));
        p = append_str(out, p, out_size, " MB");
    } else if (bytes >= (1ull << 10)) {
        p = append_uint(out, p, out_size, (uint32_t)((bytes + (1ull << 9)) >> 10));
        p = append_str(out, p, out_size, " KB");
    } else {
        p = append_uint(out, p, out_size, (uint32_t)bytes);
        p = append_str(out, p, out_size, " B");
    }

    while (p < 9 && p < out_size - 1) {
        out[p++] = ' ';
    }
    out[p] = '\0';
}

/* 打印字符串并补空格到指定宽度 */
static void print_padded(const char* s, int width) {
    int n = 0;

    vga_write(s);
    while (s[n]) {
        n++;
    }
    for (; n < width; n++) {
        vga_putchar(' ');
    }
}

/* ------------------------------------------------------------
 * mem：E820 内存表 + 页框分配器 + 内核堆
 * ------------------------------------------------------------ */
static void cmd_mem(void) {
    const e820_map_t* map = e820_get_map();
    char buf[24];
    uint32_t total, used, freef;

    /* --- BIOS 探测到的物理内存布局 --- */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("E820 physical memory map");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");

    if (!map || map->count == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("  BIOS did not report a memory map.");
    } else {
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_writeln("   #  base               size       type");

        for (uint32_t i = 0; i < map->count; i++) {
            const e820_entry_t* e = &map->entries[i];

            vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
            vga_write("  ");
            vga_write_uint(i);
            vga_write("  ");

            format_addr(e->base, buf, sizeof(buf));
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            print_padded(buf, 12);

            format_size(e->length, buf, sizeof(buf));
            print_padded(buf, 10);

            vga_set_color(e->type == E820_TYPE_USABLE ? VGA_COLOR_LIGHT_GREEN
                                                      : VGA_COLOR_DARK_GREY,
                          VGA_COLOR_BLACK);
            vga_writeln(e820_type_name(e->type));
        }
    }

    /* --- 物理页框分配器 --- */
    total = pmm_total_frames();
    used  = pmm_used_frames();
    freef = pmm_free_frames();

    vga_writeln("");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("Physical frame allocator (4KB frames, bitmap)");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    format_size((uint64_t)total * PAGE_SIZE, buf, sizeof(buf));
    vga_write("  managed : ");
    print_padded(buf, 10);
    vga_write("  ");
    vga_write_uint(total);
    vga_writeln(" frames");

    format_size((uint64_t)used * PAGE_SIZE, buf, sizeof(buf));
    vga_write("  used    : ");
    print_padded(buf, 10);
    vga_write("  ");
    vga_write_uint(used);
    vga_writeln(" frames");

    format_size((uint64_t)freef * PAGE_SIZE, buf, sizeof(buf));
    vga_write("  free    : ");
    print_padded(buf, 10);
    vga_write("  ");
    vga_write_uint(freef);
    vga_writeln(" frames");

    /* --- 内核堆 --- */
    uint32_t h_total = 0, h_used = 0, h_free = 0, h_blocks = 0;
    heap_stats(&h_total, &h_used, &h_free, &h_blocks);

    vga_writeln("");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("Kernel heap (kmalloc / kfree)");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    format_size(h_total, buf, sizeof(buf));
    vga_write("  total   : ");
    vga_writeln(buf);

    format_size(h_used, buf, sizeof(buf));
    vga_write("  in use  : ");
    vga_writeln(buf);

    format_size(h_free, buf, sizeof(buf));
    vga_write("  free    : ");
    vga_writeln(buf);

    vga_write("  blocks  : ");
    vga_write_uint(h_blocks);
    vga_writeln("");

    /* --- 分页 --- */
    uint32_t identity_mb = 0, user_pde = 0;
    paging_stats(&identity_mb, &user_pde);

    vga_writeln("");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("Paging");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    vga_write("  status    : ");
    vga_set_color(paging_enabled() ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED,
                  VGA_COLOR_BLACK);
    vga_writeln(paging_enabled() ? "enabled" : "disabled");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  page dir  : ");
    vga_write_hex(paging_kernel_directory());
    vga_writeln("");
    vga_write("  identity  : 0x00000000 - ");
    vga_write_hex(identity_mb * 1024 * 1024);
    vga_write("  (");
    vga_write_uint(identity_mb);
    vga_writeln(" MB, 4MB pages, supervisor only)");
    vga_write("  user base : ");
    vga_write_hex(USER_BASE);
    vga_writeln("  (one 4MB region per process)");

    /* --- 关键物理地址 --- */
    vga_writeln("");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("Address space");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_writeln("  0x00005000   E820 memory map left by the boot sector");
    vga_writeln("  0x00007C00   boot sector");
    vga_writeln("  0x00010000   kernel image (128KB load window)");
    vga_writeln("  0x00090000   kernel stack, grows down");
    vga_writeln("  0x000B8000   VGA text buffer");

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * heap：分配器自检
 * ------------------------------------------------------------ */
static void cmd_heap(void) {
    /* 大小刻意选得零碎，覆盖分裂与合并的各种边界 */
    static const uint32_t sizes[8] = { 16, 100, 4096, 33, 8192, 512, 7, 2048 };
    void* ptrs[8];
    bool ok = true;
    bool basic;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("Heap self-test");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");

    /* 1. 连续分配 8 块大小不同的内存，各自填入不同特征值 */
    for (int i = 0; i < 8; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) {
            ok = false;
            break;
        }
        memset(ptrs[i], 0xA0 + i, sizes[i]);
    }

    /* 2. 检查有没有互相踩踏 */
    if (ok) {
        for (int i = 0; i < 8 && ok; i++) {
            uint8_t* p = (uint8_t*)ptrs[i];
            for (uint32_t k = 0; k < sizes[i]; k++) {
                if (p[k] != (uint8_t)(0xA0 + i)) {
                    ok = false;
                    break;
                }
            }
        }
    }

    for (int i = 0; i < 8; i++) {
        kfree(ptrs[i]);
    }

    vga_set_color(ok ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_writeln(ok ? "  [ OK ] 8 live allocations stayed independent"
                   : "  [FAIL] allocations overlapped - heap is corrupt");

    /* 3. 释放后应能重新拿到同样大的块，最终合并成一整块 */
    basic = heap_self_test();
    vga_set_color(basic ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_writeln(basic ? "  [ OK ] freed blocks coalesce back into one"
                      : "  [FAIL] coalescing did not restore the heap");

    uint32_t h_total = 0, h_used = 0, h_free = 0, h_blocks = 0;
    heap_stats(&h_total, &h_used, &h_free, &h_blocks);

    vga_writeln("");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  total     : ");
    print_size32(h_total);
    vga_putchar('\n');
    vga_write("  in use    : ");
    print_size32(h_used);
    vga_putchar('\n');
    vga_write("  free      : ");
    print_size32(h_free);
    vga_putchar('\n');

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_write("  blocks    : ");
    vga_write_uint(h_blocks);
    vga_writeln("   (1 means everything merged back)");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * diskinfo：ATA 设备探测结果
 * ------------------------------------------------------------ */
static void cmd_diskinfo(void) {
    uint32_t count = ata_init();   /* 重新探测一次，顺便刷新统计 */
    char buf[24];

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("ATA devices");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");

    if (count == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("  no ATA device detected");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        const ata_device_t* d = ata_get_device(i);

        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_write("  [");
        vga_write_uint(i);
        vga_write("] ");
        vga_writeln(d->channel == ATA_PRIMARY ? "primary" : "secondary");
        vga_write("      drive   : ");
        vga_writeln(d->drive == ATA_MASTER ? "master" : "slave");

        vga_write("      model   : ");
        vga_writeln(d->model[0] ? d->model : "(unknown)");

        vga_write("      serial  : ");
        vga_writeln(d->serial[0] ? d->serial : "(unknown)");

        format_size((uint64_t)d->sectors * ATA_SECTOR_SIZE, buf, sizeof(buf));
        vga_write("      capacity: ");
        print_padded(buf, 10);
        vga_write("  ");
        vga_write_uint(d->sectors);
        vga_writeln(" sectors (512 B each)");

        vga_write("      LBA     : ");
        vga_set_color(d->lba_supported ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED,
                      VGA_COLOR_BLACK);
        vga_writeln(d->lba_supported ? "supported" : "not supported");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  sectors read   : ");
    vga_write_uint(ata_sectors_read());
    vga_writeln("");
    vga_write("  sectors written: ");
    vga_write_uint(ata_sectors_written());
    vga_writeln("");

    /* 实打实读一次 0 号扇区，验证 PIO 读通路是通的 */
    static uint8_t sector[ATA_SECTOR_SIZE];
    vga_writeln("");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_write("  reading LBA 0 to verify PIO... ");

    if (ata_read_sectors(0, 1, sector)) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_write("OK");
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_write("  boot signature ");
        vga_write_hex((uint32_t)sector[510] | ((uint32_t)sector[511] << 8));
        vga_writeln(sector[510] == 0x55 && sector[511] == 0xAA
                        ? "  (valid)" : "  (unexpected!)");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_write("FAILED: ");
        vga_writeln(ata_last_error());
    }
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * ls [路径]：列出某个目录，不给路径就列根目录
 * ------------------------------------------------------------ */
static void cmd_ls(const char* args) {
    char path[HNEOFS_PATH_MAX];
    char buf[24];
    uint32_t dir = HNEOFS_ROOT;
    uint32_t n = 0;

    get_arg(args, 0, path, sizeof(path));

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_write("HNeoFS  ");
    vga_writeln(path[0] ? path : "/");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");

    if (!hneofs_mounted()) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("  filesystem is not mounted");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    /* 给了路径就把它解析成目录下标 */
    if (path[0]) {
        int32_t idx = hneofs_resolve(path);

        if (idx < 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_write("  no such directory: ");
            vga_writeln(path);
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            return;
        }
        if (hneofs_file((uint32_t)idx)->type != HNEOFS_TYPE_DIR) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_write("  not a directory: ");
            vga_writeln(path);
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            return;
        }
        dir = (uint32_t)idx;
    }

    if (hneofs_child_count(dir) == 0) {
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_writeln("  (empty)");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("  name                 size      kind");

    for (;;) {
        int32_t idx = hneofs_child_at(dir, n);
        const hneofs_file_t* f;

        if (idx < 0) {
            break;
        }
        f = hneofs_file((uint32_t)idx);
        if (!f) {
            break;
        }
        n++;

        /* 可执行文件用亮绿色，目录用亮青色 */
        if (f->type == HNEOFS_TYPE_DIR) {
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        } else {
            vga_set_color((f->flags & HNEOFS_FLAG_EXEC) ? VGA_COLOR_LIGHT_GREEN
                                                        : VGA_COLOR_WHITE,
                          VGA_COLOR_BLACK);
        }
        print_padded(f->name, 21);

        if (f->type == HNEOFS_TYPE_DIR) {
            vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
            vga_write("       -  ");
            vga_writeln("dir");
        } else {
            format_size(f->size, buf, sizeof(buf));
            print_padded(buf, 10);
            vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
            vga_writeln((f->flags & HNEOFS_FLAG_EXEC) ? "exec" : "data");
        }
    }

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  ");
    vga_write_uint(n);
    vga_writeln(" entries");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * cat <name>：把文件内容打印出来
 * ------------------------------------------------------------ */
static void cmd_cat(const char* args) {
    char name[HNEOFS_PATH_MAX];
    const hneofs_file_t* f;
    uint8_t* buffer;
    int32_t got;

    get_arg(args, 0, name, sizeof(name));

    if (name[0] == '\0') {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_writeln("usage: cat <path>");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    if (!hneofs_mounted()) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("filesystem is not mounted");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    f = hneofs_lookup(name);
    if (!f) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_write("no such file: ");
        vga_writeln(name);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }
    if (f->type == HNEOFS_TYPE_DIR) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_write("is a directory: ");
        vga_writeln(name);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    /* 多申请一个字节用来放字符串结尾 */
    buffer = (uint8_t*)kmalloc(f->size + 1);
    if (!buffer) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("out of memory reading the file");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    got = hneofs_read_file(f, buffer, f->size + 1);
    if (got < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("disk read failed");
        kfree(buffer);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    buffer[got] = '\0';

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write((const char*)buffer);
    vga_putchar('\n');

    kfree(buffer);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * fsstat：超级块内容与空间占用
 * ------------------------------------------------------------ */
static void cmd_fsstat(void) {
    const hneofs_super_t* sb = hneofs_super();
    char buf[24];

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("HNeoFS superblock");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");

    if (!sb) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("  filesystem is not mounted");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  magic        : ");
    vga_write_hex(sb->magic);
    vga_writeln("  ('LVSF')");

    vga_write("  version      : ");
    vga_write_uint(sb->version);
    vga_writeln("");

    vga_write("  label        : ");
    vga_writeln(sb->label);

    vga_write("  block size   : ");
    vga_write_uint(sb->block_size);
    vga_writeln(" bytes");

    vga_write("  total blocks : ");
    vga_write_uint(sb->total_blocks);
    vga_writeln("");

    vga_write("  files        : ");
    vga_write_uint(sb->file_count);
    vga_writeln("");

    vga_write("  table at LBA : ");
    vga_write_uint(sb->table_lba);
    vga_writeln("");

    vga_write("  data at LBA  : ");
    vga_write_uint(sb->data_lba);
    vga_writeln("");

    /* 已用 = 从数据区起点到第一个空闲扇区。
     * 这里全部用 32 位运算：64 位除法会生成对 __udivdi3 的调用，
     * 而我们是 -nostdlib 链接的，没有 libgcc 可链。
     * 卷容量只有几 MB，32 位完全够用。
     */
    uint32_t capacity = sb->total_blocks * sb->block_size;
    uint32_t freebytes = hneofs_free_bytes();
    uint32_t used = (capacity > freebytes) ? (capacity - freebytes) : 0;

    vga_writeln("");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("  disk usage");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    format_size((uint64_t)capacity, buf, sizeof(buf));
    vga_write("    capacity : ");
    vga_writeln(buf);

    format_size((uint64_t)used, buf, sizeof(buf));
    vga_write("    used     : ");
    vga_writeln(buf);

    format_size((uint64_t)freebytes, buf, sizeof(buf));
    vga_write("    free     : ");
    vga_writeln(buf);

    /* 用 33 个字符画一条占用条，够直观就行 */
    uint32_t pct = capacity ? ((used * 100u) / capacity) : 0;

    vga_write("    [");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    uint32_t filled = pct / 3;   /* 100% -> 33 格 */
    for (uint32_t i = 0; i < 33; i++) {
        vga_putchar(i < filled ? '#' : ' ');
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("] ");
    vga_write_uint(pct);
    vga_writeln("%");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * exec <program>：把磁盘上的可执行文件加载到 ring 3 运行
 *
 * process_run 会一直阻塞到程序调用 exit，然后返回它的退出码，
 * 所以这里看起来就像普通的一次函数调用。
 * ------------------------------------------------------------ */
static void cmd_exec(const char* args) {
    char name[HNEOFS_PATH_MAX];
    char full[HNEOFS_PATH_MAX];
    int rc;
    int len;

    get_arg(args, 0, name, sizeof(name));

    if (name[0] == '\0') {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_writeln("usage: exec <program>");
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_writeln("  e.g. exec hello     runs /bin/hello.lxe");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    /* 不带路径就默认去 /bin 找；不带扩展名就自动补 .lxe */
    if (strchr(name, '/') != NULL) {
        strncpy(full, name, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    } else {
        strncpy(full, "/bin/", sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
        strncat(full, name, sizeof(full) - strlen(full) - 1);
    }

    len = (int)strlen(full);
    if (len < 4 || strcmp(full + len - 4, ".lxe") != 0) {
        if (len + 4 < (int)sizeof(full)) {
            full[len++] = '.';
            full[len++] = 'l';
            full[len++] = 'x';
            full[len++] = 'e';
            full[len]   = '\0';
        }
    }

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_write("loading ");
    vga_write(full);
    vga_writeln(" into ring 3 ...");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    rc = process_run(full, args);

    if (rc >= 0) {
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_write("[");
        vga_write(full);
        vga_write(" exited with code ");
        vga_write_uint((uint32_t)rc);
        vga_writeln("]");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_write("cannot run ");
    vga_write(name);
    vga_write(": ");

    switch (rc) {
        case -1: vga_writeln("no such file on the volume"); break;
        case -2: vga_writeln("filesystem is not mounted"); break;
        case -3: vga_writeln("out of kernel memory"); break;
        case -4: vga_writeln("disk read failed"); break;
        case -5: vga_writeln("not a valid LXE (bad header or too big)"); break;
        default: vga_writeln("unknown error"); break;
    }
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * taskmgr：任务管理器
 * ------------------------------------------------------------ */
static const char* state_name(proc_state_t s) {
    switch (s) {
        case PROC_READY:   return "ready";
        case PROC_RUNNING: return "running";
        case PROC_ZOMBIE:  return "exited";
        default:           return "?";
    }
}

static void cmd_taskmgr(void) {
    char buf[24];
    uint32_t count = 0;
    uint32_t total_kernel = 0;
    uint32_t total_user = 0;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("Task manager");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------------------------");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("  pid  name             state     kind    memory    cpu   syscalls");

    for (uint32_t i = 0; i < PROCESS_MAX; i++) {
        process_t* p = process_get(i);
        process_t* cur;

        if (!p) {
            continue;
        }
        count++;
        cur = process_current();

        /* 正在 CPU 上的任务用亮绿色高亮 */
        if (p == cur) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        } else if (p->state == PROC_ZOMBIE) {
            vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        } else {
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }

        /* pid 右对齐到 4 列 */
        vga_putchar(' ');
        if (p->pid < 10) {
            vga_write("  ");
        } else if (p->pid < 100) {
            vga_putchar(' ');
        }
        vga_write_uint(p->pid);
        vga_write("  ");

        print_padded(p->name, 17);

        vga_write(state_name(p->state));
        print_padded("", 10 - (int)strlen(state_name(p->state)));

        vga_write(p->is_user ? "user    " : "kernel  ");

        format_size((uint64_t)p->memory_used, buf, sizeof(buf));
        print_padded(buf, 10);

        vga_write_uint(p->cpu_ticks);
        print_padded("", 6 - (int)(p->cpu_ticks < 10 ? 1 :
                                  p->cpu_ticks < 100 ? 2 :
                                  p->cpu_ticks < 1000 ? 3 :
                                  p->cpu_ticks < 10000 ? 4 : 5));

        vga_write_uint(p->syscall_count);
        vga_putchar('\n');

        if (p->is_user) {
            total_user += p->memory_used;
        } else {
            total_kernel += p->kernel_stack_size;
        }
    }

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------------------------");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  tasks          : ");
    vga_write_uint(count);
    vga_write(" alive, ");
    vga_write_uint(process_total_created());
    vga_writeln(" created since boot");

    format_size((uint64_t)total_user, buf, sizeof(buf));
    vga_write("  user memory    : ");
    vga_writeln(buf);

    format_size((uint64_t)total_kernel, buf, sizeof(buf));
    vga_write("  kernel stacks  : ");
    vga_writeln(buf);

    vga_write("  syscalls total : ");
    vga_write_uint(syscall_total());
    vga_writeln("");

    /* 页框占用情况，和 mem 命令里的数字对得上 */
    format_size((uint64_t)pmm_free_frames() * PAGE_SIZE, buf, sizeof(buf));
    vga_write("  free frames    : ");
    vga_writeln(buf);

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("");
    vga_writeln("  commands: spawn <prog> starts a program in the background,");
    vga_writeln("            kill <pid> terminates one");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * kill <pid>
 * ------------------------------------------------------------ */
static void cmd_kill(const char* args) {
    char buf[16];
    int pid = 0;
    int rc;

    get_arg(args, 0, buf, sizeof(buf));

    if (buf[0] == '\0') {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_writeln("usage: kill <pid>");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    for (int i = 0; buf[i]; i++) {
        if (!isdigit(buf[i])) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_writeln("pid must be a number");
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            return;
        }
        pid = pid * 10 + (buf[i] - '0');
    }

    rc = process_kill((uint32_t)pid);

    if (rc == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_write("killed pid ");
        vga_write_uint((uint32_t)pid);
        vga_writeln("");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_write("cannot kill ");
        vga_write_uint((uint32_t)pid);
        vga_write(": ");
        switch (rc) {
            case -1: vga_writeln("no such process"); break;
            case -2: vga_writeln("the shell cannot be killed"); break;
            case -3: vga_writeln("a process cannot kill itself"); break;
            case -4: vga_writeln("it has already exited"); break;
            default: vga_writeln("unknown error"); break;
        }
    }
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * spawn <program>：后台启动，立刻回到提示符
 * ------------------------------------------------------------ */
static void cmd_spawn(const char* args) {
    char name[HNEOFS_PATH_MAX];
    char full[HNEOFS_PATH_MAX];
    int len;
    int rc;

    get_arg(args, 0, name, sizeof(name));

    if (name[0] == '\0') {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_writeln("usage: spawn <program>");
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_writeln("  starts the program in the background and returns immediately");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    /* 和 exec 一样：不带路径就去 /bin 找 */
    if (strchr(name, '/') != NULL) {
        strncpy(full, name, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    } else {
        strncpy(full, "/bin/", sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
        strncat(full, name, sizeof(full) - strlen(full) - 1);
    }

    len = (int)strlen(full);
    if (len < 4 || strcmp(full + len - 4, ".lxe") != 0) {
        if (len + 4 < (int)sizeof(full)) {
            full[len++] = '.';
            full[len++] = 'l';
            full[len++] = 'x';
            full[len++] = 'e';
            full[len]   = '\0';
        }
    }

    rc = process_spawn(full, args);

    if (rc >= 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_write("started ");
        vga_write(name);
        vga_write(" as pid ");
        vga_write_uint((uint32_t)rc);
        vga_writeln("");
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_writeln("it now shares the CPU with the shell - try 'taskmgr'");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_write("cannot start ");
        vga_writeln(name);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
}

/* ------------------------------------------------------------
 * mouse：PS/2 鼠标状态
 * ------------------------------------------------------------ */
static void cmd_mouse(void) {
    int32_t x = 0, y = 0;
    uint8_t btn = 0;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("PS/2 mouse");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  device      : ");
    vga_set_color(mouse_present() ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED,
                  VGA_COLOR_BLACK);
    vga_writeln(mouse_present() ? "detected on the auxiliary port"
                                : "not detected");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  wheel       : ");
    vga_set_color(mouse_has_wheel() ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_YELLOW,
                  VGA_COLOR_BLACK);
    vga_writeln(mouse_has_wheel() ? "yes (IntelliMouse mode, 4-byte packets)"
                                  : "no (plain 3-byte packets)");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    mouse_get_state(&x, &y, &btn);

    vga_write("  position    : ");
    vga_write_uint((uint32_t)x);
    vga_write(", ");
    vga_write_uint((uint32_t)y);
    vga_writeln("");

    vga_write("  buttons     : ");
    vga_write(btn & 0x01 ? "L" : "-");
    vga_write(btn & 0x02 ? "R" : "-");
    vga_write(btn & 0x04 ? "M" : "-");
    vga_writeln("");

    vga_write("  packets     : ");
    vga_write_uint(mouse_packets());
    vga_writeln("");

    vga_write("  wheel events: ");
    vga_write_uint(mouse_wheel_events());
    vga_writeln("");

    /* --- 回滚缓冲状态 --- */
    vga_writeln("");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("Screen scrollback");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("--------------------------------------------------------");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_write("  buffered    : ");
    vga_write_uint(vga_scrollback_lines());
    vga_writeln(" lines");

    vga_write("  scrolled    : ");
    vga_write_uint(vga_scrollback_depth());
    vga_writeln(" lines back from the live screen");

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("");
    vga_writeln("  scroll wheel   : up/down through history");
    vga_writeln("  PageUp/PageDown: one screen at a time");
    vga_writeln("  Home / End     : jump to the oldest line / back to live");
    vga_writeln("  any other key  : back to the live screen");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ------------------------------------------------------------
 * screendump：把实时画面（内存副本）打出来
 *
 * 串口日志只是线性的字符流，光标定位、擦除、插入删除这些操作
 * 在日志里看不出来。想确认渲染结果就必须直接读屏幕内容。
 * 默认只打文本，加 colors 参数再打一遍配色。
 * ------------------------------------------------------------ */
static void cmd_screendump(const char* args) {
    char want_colors[8];
    /* 必须先整屏快照再打印：
     * 打印本身会让屏幕滚动，边打边读的话后面的行读到的已经是
     * 滚动之后的内容，位置全错。
     */
    static uint16_t snap[VGA_HEIGHT][VGA_WIDTH];

    get_arg(args, 0, want_colors, sizeof(want_colors));

    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            snap[y][x] = vga_get_cell(y, x);
        }
    }

    /* 每行总共 80 个字符，正好占满一行不会折行：
     * 3 个字符行号 + 76 个格子 + 1 个收尾竖线
     */
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("   0         1         2         3         4         5         6         7");
    vga_writeln("   0123456789012345678901234567890123456789012345678901234567890123456789012 345");

    for (int y = 0; y < VGA_HEIGHT; y++) {
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        if (y < 10) {
            vga_putchar(' ');
        }
        vga_write_uint((uint32_t)y);
        vga_putchar('|');

        for (int x = 0; x < VGA_WIDTH - 4; x++) {
            uint16_t cell = snap[y][x];
            char ch = (char)(cell & 0xFF);

            vga_set_color((vga_color)((cell >> 8) & 0x0F),
                          (vga_color)((cell >> 12) & 0x0F));
            vga_putchar((ch >= 32 && ch < 127) ? ch : '.');
        }

        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_putchar('|');
        vga_putchar('\n');
    }

    /* colors 参数：把每一行的配色做行程编码打出来，用来验证 SGR 效果 */
    if (strcmp(want_colors, "colors") == 0) {
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_writeln("");
        vga_writeln("attribute runs (row: cols fg/bg), only for non-default cells:");

        for (int y = 0; y < VGA_HEIGHT; y++) {
            uint8_t run_attr = (uint8_t)(snap[y][0] >> 8);
            int run_start = 0;
            bool printed = false;

            for (int x = 1; x <= VGA_WIDTH; x++) {
                uint8_t attr = (x < VGA_WIDTH)
                             ? (uint8_t)(snap[y][x] >> 8)
                             : (uint8_t)~run_attr;   /* 哨兵，强制收尾 */

                if (attr == run_attr) {
                    continue;
                }

                /* 只报告非默认配色的片段，其余太吵 */
                if (run_attr != 0x07) {
                    if (!printed) {
                        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
                        vga_write("  row ");
                        if (y < 10) {
                            vga_putchar(' ');
                        }
                        vga_write_uint((uint32_t)y);
                        vga_write(": ");
                        printed = true;
                    }
                    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    vga_write_uint((uint32_t)run_start);
                    vga_write("-");
                    vga_write_uint((uint32_t)(x - 1));
                    vga_write(" ");
                    vga_write_uint((uint32_t)(run_attr & 0x0F));
                    vga_write("/");
                    vga_write_uint((uint32_t)(run_attr >> 4));
                    vga_write("   ");
                }

                run_attr  = attr;
                run_start = x;
            }

            if (printed) {
                vga_putchar('\n');
            }
        }
    }

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

static void cmd_color(const char* args) {
    char fg_name[24];
    char bg_name[24];

    get_arg(args, 0, fg_name, sizeof(fg_name));
    get_arg(args, 1, bg_name, sizeof(bg_name));

    if (fg_name[0] == '\0') {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_writeln("usage: color <foreground> <background>");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_writeln("e.g.   color lightgreen black");
        return;
    }

    int fg = color_from_name(fg_name);
    if (fg < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_write("unknown colour: ");
        vga_writeln(fg_name);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    int bg = VGA_COLOR_BLACK;
    if (bg_name[0] != '\0') {
        bg = color_from_name(bg_name);
        if (bg < 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_write("unknown background colour: ");
            vga_writeln(bg_name);
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            return;
        }
    }

    vga_set_color((vga_color)fg, (vga_color)bg);
    vga_writeln("palette updated. type 'clear' to repaint the whole screen.");
}

static void cmd_history(void) {
    if (history_count == 0) {
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_writeln("(no history yet)");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (int i = 0; i < history_count; i++) {
        vga_write("  ");
        vga_write_uint((uint32_t)(i + 1));
        vga_write("  ");
        vga_writeln(history[i]);
    }
}

static void cmd_banner(void);

static void cmd_reboot(void) {
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_writeln("rebooting...");

    /* 给串口和屏幕一点时间把上面的字发完 */
    for (volatile int i = 0; i < 1000000; i++) {
        /* 故意空转 */
    }

    outb(0x64, 0xFE);   /* 通过键盘控制器拉复位线 */

    disable_interrupts();
    for (;;) {
        cpu_halt();
    }
}

static void cmd_halt(void) {
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_writeln("system halted. you can close the virtual machine now.");

    disable_interrupts();
    for (;;) {
        cpu_halt();
    }
}

/* ------------------------------------------------------------
 * 命令分发
 * ------------------------------------------------------------ */
static void shell_execute(char* line) {
    char* cmd = skip_spaces(line);

    if (cmd[0] == '\0') {
        return;   /* 空行：只换一个新提示符 */
    }

    /* 切出命令名与其后的参数 */
    char* args = cmd;
    while (*args && *args != ' ' && *args != '\t') {
        args++;
    }
    if (*args != '\0') {
        *args = '\0';
        args = skip_spaces(args + 1);
    }

    /* ------------------------------------------------------------
     * 先看 /bin 下有没有同名程序。
     *
     * 这就是"每条命令都是一个可执行文件"的落点：敲 ls 的时候，
     * Shell 先去找 /bin/ls.lxe，找到就把控制权交给 ring 3 的那个程序；
     * 找不到才回退到内核内置命令。
     *
     * 命令名里如果带 '/'，就当成完整路径直接用，不再拼 /bin 前缀。
     * ------------------------------------------------------------ */
    {
        char program[HNEOFS_PATH_MAX];
        int n = 0;

        if (strchr(cmd, '/') != NULL) {
            for (const char* p = cmd; *p && n < (int)sizeof(program) - 1; p++) {
                program[n++] = *p;
            }
            program[n] = '\0';
        } else {
            const char* prefix = "/bin/";

            for (const char* p = prefix; *p && n < (int)sizeof(program) - 6; p++) {
                program[n++] = *p;
            }
            for (const char* p = cmd; *p && n < (int)sizeof(program) - 5; p++) {
                program[n++] = *p;
            }
            program[n++] = '.';
            program[n++] = 'l';
            program[n++] = 'x';
            program[n++] = 'e';
            program[n]   = '\0';
        }

        if (hneofs_mounted() && hneofs_lookup(program)) {
            int rc = process_run(program, args);

            if (rc < 0) {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                vga_write("cannot run ");
                vga_writeln(program);
                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            }
            return;
        }
    }

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "clear") == 0) {
        vga_clear();
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(args);
    } else if (strcmp(cmd, "about") == 0) {
        cmd_about();
    } else if (strcmp(cmd, "version") == 0) {
        cmd_version();
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "ticks") == 0) {
        cmd_ticks();
    } else if (strcmp(cmd, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(cmd, "heap") == 0) {
        cmd_heap();
    } else if (strcmp(cmd, "diskinfo") == 0) {
        cmd_diskinfo();
    } else if (strcmp(cmd, "mouse") == 0) {
        cmd_mouse();
    } else if (strcmp(cmd, "screendump") == 0) {
        cmd_screendump(args);
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(args);
    } else if (strcmp(cmd, "cat") == 0) {
        cmd_cat(args);
    } else if (strcmp(cmd, "fsstat") == 0) {
        cmd_fsstat();
    } else if (strcmp(cmd, "exec") == 0) {
        cmd_exec(args);
    } else if (strcmp(cmd, "spawn") == 0) {
        cmd_spawn(args);
    } else if (strcmp(cmd, "taskmgr") == 0 || strcmp(cmd, "tasks") == 0) {
        cmd_taskmgr();
    } else if (strcmp(cmd, "kill") == 0) {
        cmd_kill(args);
    } else if (strcmp(cmd, "color") == 0) {
        cmd_color(args);
    } else if (strcmp(cmd, "colors") == 0) {
        cmd_colors();
    } else if (strcmp(cmd, "history") == 0) {
        cmd_history();
    } else if (strcmp(cmd, "banner") == 0) {
        cmd_banner();
    } else if (strcmp(cmd, "panic") == 0) {
        kernel_panic(args[0] ? args : "panic triggered from the shell");
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(cmd, "halt") == 0) {
        cmd_halt();
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_write("unknown command: ");
        vga_writeln(cmd);
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_writeln("type 'help' for the list of commands.");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }
}

/* ------------------------------------------------------------
 * 启动横幅
 * ------------------------------------------------------------ */
static void cmd_banner(void) {
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_writeln("");
    vga_writeln("  ==========================================");
    vga_writeln("   _   _   _   _                     ____");
    vga_writeln("  | | | | | \\ | |   ___    ___      / ___|");
    vga_writeln("  | |_| | |  \\| |  / _ \\  / _ \\    | |");
    vga_writeln("  |  _  | | |\\  | |  __/ | (_) |   | |___");
    vga_writeln("  |_| |_| |_| \\_|  \\___|  \\___/     \\____|");
    vga_writeln("");
    vga_write("        " KERNEL_NAME " v" KERNEL_VERSION "  -  32-bit x86 kernel");
    vga_writeln("");
    vga_writeln("  ==========================================");
    vga_writeln("");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* 内核启动时调用一次 */
void shell_print_banner(void) {
    cmd_banner();
}

/* ------------------------------------------------------------
 * Shell 主循环
 * ------------------------------------------------------------ */
void shell_run(void) {
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_writeln("Type 'help' for the command list.");
    vga_writeln("Up/Down arrows browse the command history.");
    vga_writeln("");

    for (;;) {
        shell_prompt();
        char* line = shell_readline();
        history_add(line);
        shell_execute(line);
    }
}
