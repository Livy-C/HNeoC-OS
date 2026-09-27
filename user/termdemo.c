/* termdemo - 演示 term.h 这套终端 API
 *
 * 它把所有 ANSI 细节都藏在 term_* 后面，全程不出现一个 "\x1b["。
 * 画的是一个会自己动的进度条加一块固定状态栏 —— 状态栏靠
 * 滚动区域隔离，下面的输出不会把它顶掉。
 */

#include <hneoc.h>

static const char* color_names[8] = {
    "black", "blue", "green", "cyan", "red", "magenta", "brown", "lightgrey"
};

static void draw_box(int top, int left, int height, int width) {
    term_gotoxy(top, left);
    term_putc('+');
    for (int i = 1; i < width - 1; i++) { term_putc('-'); }
    term_putc('+');

    for (int r = 1; r < height - 1; r++) {
        term_gotoxy(top + r, left);
        term_putc('|');
        term_gotoxy(top + r, left + width - 1);
        term_putc('|');
    }

    term_gotoxy(top + height - 1, left);
    term_putc('+');
    for (int i = 1; i < width - 1; i++) { term_putc('-'); }
    term_putc('+');
}

int main(void) {
    int rows, cols;

    term_init_fullscreen(&rows, &cols);

    /* --- 顶部保留两行给标题，底部两行给状态栏 --- */
    term_set_scroll_region(10, rows - 3);

    /* --- 静态部分 --- */
    term_reset();
    term_set_bold(1);
    term_set_fg(TERM_LIGHT_CYAN);
    term_gotoxy(1, 2);
    term_printf("term.h demo   screen is %dx%d", cols, rows);
    term_reset();

    draw_box(2, 2, 8, 40);

    term_set_fg(TERM_YELLOW);
    term_gotoxy(3, 4);
    term_puts("a box, drawn with term_gotoxy only");
    term_set_fg(TERM_LIGHT_GREY);
    term_gotoxy(4, 4);
    term_puts("no escape sequences in this file");

    /* 颜色条：直接用 TERM_* 常量 */
    term_gotoxy(6, 4);
    term_puts("colors: ");
    for (int i = 0; i < 8; i++) {
        term_set_color(i, TERM_BLACK);
        term_printf(" %s ", color_names[i]);
        term_reset();
        term_putc(' ');
    }

    /* 反显高亮 */
    term_gotoxy(8, 4);
    term_set_reverse(1);
    term_puts(" reverse video ");
    term_set_reverse(0);
    term_puts("  bold: ");
    term_set_bold(1);
    term_puts("strong");
    term_set_bold(0);

    /* --- 状态栏固定在倒数第二行：靠滚动区域隔离 --- */
    {
        int filled = 0;
        int bar_top = rows - 3;

        term_gotoxy(rows - 3, 2);
        term_set_fg(TERM_DARK_GREY);
        term_puts("output area ends here - the status line below is fixed");
        term_reset();

        for (int step = 0; step <= 40; step++) {
            /* 状态栏 */
            term_gotoxy(rows - 2, 0);
            term_set_reverse(1);
            term_printf(" progress: [%-40s] %3d%% ",
                        "", step * 100 / 40);
            term_set_reverse(0);

            /* 进度条填充部分单独描一遍，做出"长出来"的效果 */
            term_gotoxy(rows - 2, 13);
            term_set_reverse(1);
            term_set_fg(TERM_LIGHT_GREEN);
            for (int i = 0; i < step; i++) { term_putc('#'); }
            term_set_reverse(0);
            term_reset();
            term_flush();

            /* 滚动区域里也放点内容，证明区域滚动不影响状态栏 */
            if (step % 4 == 0 && filled < bar_top - 10) {
                term_gotoxy(bar_top + filled, 2);
                term_set_fg(TERM_LIGHT_GREY);
                term_printf("line %d inside the scroll region", filled);
                term_putc('\n');
                term_flush();
                filled++;
            }

            sleep_ms(60);
        }
    }

    /* --- 提示行 --- */
    term_gotoxy(rows - 1, 2);
    term_set_fg(TERM_LIGHT_GREEN);
    term_puts("press any key to exit");
    term_reset();
    term_flush();

    (void)getkey();

    term_leave_fullscreen();
    puts("termdemo: back to the shell");
    return 0;
}
