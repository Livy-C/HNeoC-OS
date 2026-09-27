/* ansi - 演示 ANSI 转义序列
 *
 * 这个程序不碰显存，也不调用任何"画界面"的系统调用：它只是往
 * 标准输出写字节流，里面夹着 ESC 开头的控制序列，由内核的控制台
 * 解释器负责把光标移来移去、擦除、上色、限制滚动区域。
 *
 * 这正是一个真正的终端程序（比如以后要移植的 vi）的工作方式。
 *
 * 主绘制留在普通屏幕上，所以退出后可以用 screendump 检查渲染结果；
 * 最后再单独验证一下备用屏幕缓冲的切换与恢复。
 */

#include "hneoc.h"

#define CSI "\x1b["

static void w(const char* s) {
    write(1, s, str_len(s));
}

/* 光标绝对定位，行列都从 1 开始 */
static void gotoxy(int row, int col) {
    char buf[16];
    int n = 0;

    buf[n++] = 0x1B;
    buf[n++] = '[';
    if (row >= 10) { buf[n++] = (char)('0' + row / 10); }
    buf[n++] = (char)('0' + row % 10);
    buf[n++] = ';';
    if (col >= 10) { buf[n++] = (char)('0' + col / 10); }
    buf[n++] = (char)('0' + col % 10);
    buf[n++] = 'H';
    write(1, buf, (uint32_t)n);
}

static void sgr(const char* code) {
    w(CSI);
    w(code);
    w("m");
}

int main(void) {
    w(CSI "2J");       /* 清屏 */
    w(CSI "H");        /* 光标回左上角 */

    /* --- 绝对定位：四个角分别定位写入 --- */
    sgr("1;36");
    gotoxy(2, 3);
    w("ANSI escape sequences work");

    sgr("0");
    gotoxy(3, 3);
    w("------------------------------------------------------------");

    /* --- 颜色：SGR 30-37 / 90-97 --- */
    gotoxy(5, 3);
    w("normal : ");
    sgr("31"); w("red ");     sgr("32"); w("green ");
    sgr("33"); w("yellow ");  sgr("34"); w("blue ");
    sgr("35"); w("magenta "); sgr("36"); w("cyan");
    sgr("0");

    gotoxy(6, 3);
    w("bright : ");
    sgr("1;31"); w("red ");     sgr("1;32"); w("green ");
    sgr("1;33"); w("yellow ");  sgr("1;34"); w("blue ");
    sgr("1;35"); w("magenta "); sgr("1;36"); w("cyan");
    sgr("0");

    gotoxy(7, 3);
    w("reverse: ");
    sgr("7"); w(" inverted "); sgr("0");
    sgr("44;97"); w(" blue bg "); sgr("0");

    /* --- 画方框：完全靠绝对定位，没有任何绘图接口 --- */
    gotoxy(9, 3);  w("+");
    gotoxy(9, 30); w("+");
    gotoxy(12, 3); w("+");
    gotoxy(12, 30); w("+");
    for (int c = 4; c < 30; c++) {
        gotoxy(9, c);  w("-");
        gotoxy(12, c); w("-");
    }
    for (int r = 10; r < 12; r++) {
        gotoxy(r, 3);  w("|");
        gotoxy(r, 30); w("|");
    }
    sgr("33");
    gotoxy(10, 5);
    w("drawn with ESC[y;xH only");
    sgr("0");

    /* --- EL：先写长串，再从中间擦到行尾 --- */
    gotoxy(14, 3);
    w("this tail will be erased >>>>>>>>>>>>>>>>>>>>>>");
    gotoxy(14, 25);
    w(CSI "0K");
    gotoxy(14, 3);
    sgr("32");
    w("EL removed the rest of this line");
    sgr("0");

    /* --- ICH / DCH：插入和删除字符 --- */
    gotoxy(16, 3);
    w("ABCDEFGH");
    gotoxy(16, 3);
    w(CSI "3@");
    sgr("36"); w("XYZ"); sgr("0");

    gotoxy(17, 3);
    w("123456789");
    gotoxy(17, 3);
    w(CSI "3P");
    sgr("36"); w("  <- DCH removed 123"); sgr("0");

    /* --- 滚动区域：只在 19-22 行之间滚动，上面的内容不受影响 --- */
    gotoxy(19, 3);
    sgr("35"); w("scroll region 19-22:"); sgr("0");

    w(CSI "19;22r");          /* DECSTBM */

    for (int i = 1; i <= 6; i++) {
        gotoxy(22, 3);
        w("scrolling line ");
        put_int(i);
        w("      ");
    }
    putchar('\n');            /* 触发区域内的滚动 */

    w(CSI "r");               /* 恢复整屏滚动 */
    gotoxy(19, 26);
    sgr("32"); w("(only that band scrolled)"); sgr("0");

    /* --- 备用屏幕缓冲 --- */
    gotoxy(21, 3);
    sgr("33");
    w("switching to the alternate screen for 2 s...");
    sgr("0");

    sleep_ms(1200);

    w(CSI "?1049h");          /* 切到备用屏幕 */
    w(CSI "2J");
    w(CSI "H");
    sgr("1;31");
    gotoxy(11, 25);
    w("ALTERNATE SCREEN");
    gotoxy(13, 20);
    sgr("0;37");
    w("this whole screen is a scratch buffer");

    sleep_ms(2000);           /* 这 2 秒里应该只看到上面那两行 */

    w(CSI "?1049l");          /* 切回来，原来的画面应当原样恢复 */

    sgr("0");
    gotoxy(21, 3);
    sgr("1;32");
    w("back on the normal screen - the drawing above is intact");
    sgr("0");

    gotoxy(24, 3);
    w("ansi demo finished\n");

    return 0;
}
