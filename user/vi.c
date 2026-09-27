/* vi - HNeoC OS 的全屏模态编辑器
 *
 * 这是一个从零写的 vi 克隆，用的是 kilo 那一系的"行数组"结构：
 * 每一行存原始内容，另存一份把 tab 展开后的版本用于显示。
 *
 * 它不碰任何绘图接口：屏幕上的每一样东西都是往标准输出写
 * ANSI 转义序列画出来的，按键则通过 getkey() 从内核拿。
 *
 * 支持：
 *   启动      vi <文件> 直接打开（也可以用 :e <文件>）
 *   普通模式  动作    h j k l / 方向键 / 0 $ w b / gg G / PageUp PageDown
 *             计数    3j  5x  2dd  3yy ……（数字前缀）
 *             操作符  dd dw d$ d0 dj dk  删除
 *                     yy yw y$          复制进寄存器
 *                     cc cw C S         修改，改完直接进插入模式
 *                     D = d$    X = 往前删
 *             粘贴    p（光标后 / 下一行）、P（光标前 / 上一行）
 *             插入    i a A I o O
 *             其它    r 替换一个字符、J 续行、x 删字符、u 多级撤销（8 级）
 *             搜索    / 向后、? 向前、n / N 重复
 *             退出    ZZ = 存盘退出
 *   插入模式  可打印字符、回车、退格、方向键，ESC 返回
 *   命令模式  :w :w <文件> :q :q! :wq :x :e <文件> :<行号>
 */

#include "hneoc.h"

/* ------------------------------------------------------------
 * 屏幕布局（25 行）
 *   0 .. TEXT_ROWS-1   正文
 *   STATUS_ROW         状态栏
 *   MSG_ROW            消息 / 命令行
 * ------------------------------------------------------------ */
#define TEXT_ROWS   22
#define STATUS_ROW  22
#define MSG_ROW     23
#define TAB_STOP    8
#define VGA_COLS    80

#define MODE_NORMAL  0
#define MODE_INSERT  1
#define MODE_COMMAND 2
#define MODE_SEARCH  3

typedef struct {
    int   size;      /* 原始字节数（不含换行） */
    char* chars;
    int   rsize;     /* 渲染后长度（tab 已展开） */
    char* render;
} erow;

static struct {
    int   cx, cy;        /* 光标：cy 是文件行号，cx 是原始列 */
    int   rx;            /* 渲染列，用于显示 */
    int   rowoff;        /* 视口第一行 */
    int   coloff;        /* 横向滚动 */
    int   numrows;
    erow* rows;
    int   dirty;         /* 有未保存修改 */
    char  filename[64];
    char  status[96];
    char  cmd[96];
    int   cmdlen;
    int   mode;
    int   quit_times;    /* 连按几次 :q! 才真的退出 */
    int   running;
} E;

/* 多级撤销：每次改动前把整个缓冲区序列化压进环里。 */
#define UNDO_LEVELS 8

static char*    undo_blobs[UNDO_LEVELS];
static uint32_t undo_lens[UNDO_LEVELS];
static int      undo_top   = 0;   /* 下一个要写的槽 */
static int      undo_count = 0;   /* 现在有几级可撤 */

/* 待执行的命令前缀。
 *   pend_count  计数前缀，0 表示没给（1 和"没给"在这里等价）
 *   pend_op     待决操作符：0/'d'/'y'/'c' 等，加上 'g'、'Z'、'r' 这些两键命令
 */
static int pend_count = 0;
static int pend_op    = 0;

/* 寄存器（dd / yy / dw 剪切或复制出来的内容）。
 * 序列化成 [charwise:4][lines:4]，后面跟 lines 组 [len:4][bytes]。
 * charwise = 0 表示整行，粘贴时按行插；= 1 表示字符片段，粘贴时插在光标处。
 */
static char*    regblob     = 0;
static uint32_t reglen      = 0;
static int      reg_charwise = 0;
static int      reg_lines    = 0;

/* 上一次的搜索串（/ 与 ? 共用，用 n / N 重复） */
static char last_search[96];
static int  last_search_len = 0;
static int  last_dir       = 1;   /* 1 = 向后，-1 = 向前 */
static int  search_dir     = 1;   /* 当前正在输入的 / 或 ? 的方向 */

/* 往状态栏写一条消息 */
static void msg(const char* s) {
    snprintf(E.status, sizeof(E.status), "%s", s);
}

/* ------------------------------------------------------------
 * 输出缓冲：一帧的内容先攒起来，最后一次性写出去，
 * 免得每画一小段就发一次系统调用
 * ------------------------------------------------------------ */
static char obuf[8192];
static int  olen = 0;

static void oflush(void) {
    if (olen > 0) {
        write(1, obuf, (uint32_t)olen);
        olen = 0;
    }
}

static void oput(const char* s, int n) {
    for (int i = 0; i < n; i++) {
        if (olen >= (int)sizeof(obuf) - 2) {
            oflush();
        }
        obuf[olen++] = s[i];
    }
}

static void oputs(const char* s) {
    oput(s, (int)strlen(s));
}

static void oputc(char c) {
    oput(&c, 1);
}

static void oputuint(uint32_t v) {
    char tmp[12];
    int n = 0;

    if (v == 0) {
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

/* 光标绝对定位，行列从 1 开始（ANSI 约定） */
static void ocursor(int row, int col) {
    oputs("\x1b[");
    oputuint((uint32_t)(row + 1));
    oputc(';');
    oputuint((uint32_t)(col + 1));
    oputc('H');
}

/* ------------------------------------------------------------
 * 行操作
 * ------------------------------------------------------------ */

/* 把 tab 展开成空格，算出用于显示的 render 和 rsize */
static void row_update(erow* row) {
    int tabs = 0;
    int idx = 0;

    for (int i = 0; i < row->size; i++) {
        if (row->chars[i] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = (char*)malloc((uint32_t)(row->size + tabs * (TAB_STOP - 1) + 1));
    if (!row->render) {
        row->rsize = 0;
        return;
    }

    for (int i = 0; i < row->size; i++) {
        if (row->chars[i] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[i];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

/* 在第 at 行插入一行内容，长度 len */
static void row_insert(int at, const char* s, int len) {
    if (at < 0 || at > E.numrows) {
        return;
    }

    E.rows = (erow*)realloc(E.rows, sizeof(erow) * (uint32_t)(E.numrows + 1));
    if (!E.rows) {
        return;
    }

    for (int i = E.numrows; i > at; i--) {
        E.rows[i] = E.rows[i - 1];
    }

    E.rows[at].size   = len;
    E.rows[at].chars  = (char*)malloc((uint32_t)len + 1);
    E.rows[at].render = 0;
    E.rows[at].rsize  = 0;

    if (E.rows[at].chars) {
        memcpy(E.rows[at].chars, s, (uint32_t)len);
        E.rows[at].chars[len] = '\0';
    } else {
        E.rows[at].size = 0;
    }

    row_update(&E.rows[at]);
    E.numrows++;
    E.dirty = 1;
}

static void row_free(erow* row) {
    free(row->chars);
    free(row->render);
    row->chars  = 0;
    row->render = 0;
}

static void row_delete(int at) {
    if (at < 0 || at >= E.numrows) {
        return;
    }
    row_free(&E.rows[at]);
    for (int i = at; i < E.numrows - 1; i++) {
        E.rows[i] = E.rows[i + 1];
    }
    E.numrows--;
    E.dirty = 1;
}

static void row_insert_char(erow* row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }

    row->chars = (char*)realloc(row->chars, (uint32_t)row->size + 2);
    if (!row->chars) {
        return;
    }
    memcpy(row->chars + at + 1, row->chars + at, (uint32_t)(row->size - at + 1));
    row->chars[at] = (char)c;
    row->size++;
    row_update(row);
    E.dirty = 1;
}

static void row_append_string(erow* row, const char* s, int len) {
    row->chars = (char*)realloc(row->chars, (uint32_t)(row->size + len + 1));
    if (!row->chars) {
        return;
    }
    memcpy(row->chars + row->size, s, (uint32_t)len);
    row->size += len;
    row->chars[row->size] = '\0';
    row_update(row);
    E.dirty = 1;
}

static void row_delete_char(erow* row, int at) {
    if (at < 0 || at >= row->size) {
        return;
    }
    memcpy(row->chars + at, row->chars + at + 1,
           (uint32_t)(row->size - at));
    row->size--;
    row_update(row);
    E.dirty = 1;
}

/* ------------------------------------------------------------
 * 撤销：把整个缓冲区打包成一个 blob 压进环里，撤销时还原。
 * 现在保留 UNDO_LEVELS 级，u 可以一路往回走。
 * ------------------------------------------------------------ */
static void snapshot(void) {
    uint32_t total = 4;   /* 行数 */
    char* blob;
    char* p;

    for (int i = 0; i < E.numrows; i++) {
        total += 4 + (uint32_t)E.rows[i].size;
    }

    blob = (char*)malloc(total);
    if (!blob) {
        return;   /* 分配不出来就这次不做撤销点，不影响编辑 */
    }

    p = blob;
    memcpy(p, &E.numrows, 4);
    p += 4;
    for (int i = 0; i < E.numrows; i++) {
        memcpy(p, &E.rows[i].size, 4);
        p += 4;
        memcpy(p, E.rows[i].chars, (uint32_t)E.rows[i].size);
        p += E.rows[i].size;
    }

    /* 写进环；满了就顶掉最老的那一级 */
    free(undo_blobs[undo_top]);
    undo_blobs[undo_top] = blob;
    undo_lens[undo_top]  = total;
    undo_top = (undo_top + 1) % UNDO_LEVELS;
    if (undo_count < UNDO_LEVELS) {
        undo_count++;
    }
}

static void undo(void) {
    char* blob;
    uint32_t len;
    char* p;
    int rows;
    int i;
    int slot;

    if (undo_count == 0) {
        msg("already at the oldest change");
        return;
    }

    /* 取回最近一级 */
    slot = (undo_top + UNDO_LEVELS - 1) % UNDO_LEVELS;
    blob = undo_blobs[slot];
    len  = undo_lens[slot];
    if (!blob || len < 4) {
        undo_count = 0;
        msg("nothing to undo");
        return;
    }

    /* 先把当前内容整个丢掉 */
    for (i = 0; i < E.numrows; i++) {
        row_free(&E.rows[i]);
    }
    free(E.rows);
    E.rows    = 0;
    E.numrows = 0;

    p = blob;
    memcpy(&rows, p, 4);
    p += 4;

    for (i = 0; i < rows; i++) {
        int l;

        memcpy(&l, p, 4);
        p += 4;
        row_insert(E.numrows, p, l);
        p += l;
    }

    E.dirty = 1;
    E.cy    = 0;
    E.cx    = 0;
    E.rx    = 0;
    E.rowoff = 0;
    E.coloff = 0;

    /* 这一级已经用掉了 */
    undo_blobs[slot] = 0;
    undo_lens[slot]  = 0;
    free(blob);
    undo_top = slot;
    undo_count--;

    snprintf(E.status, sizeof(E.status), "undo: %d more level(s)", undo_count);
}

/* ------------------------------------------------------------
 * 文件读写
 * ------------------------------------------------------------ */
static void editor_open(const char* filename) {
    int fd;
    char* buf;
    int got;
    int start;
    int at;

    /* 先把之前的清空 */
    for (int i = 0; i < E.numrows; i++) {
        row_free(&E.rows[i]);
    }
    free(E.rows);
    E.rows    = 0;
    E.numrows = 0;

    strncpy(E.filename, filename, sizeof(E.filename) - 1);
    E.filename[sizeof(E.filename) - 1] = '\0';
    E.dirty  = 0;
    E.cy = E.cx = E.rx = 0;
    E.rowoff = E.coloff = 0;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        /* 新文件：不算错误，保存时创建 */
        snprintf(E.status, sizeof(E.status),
                 "\"%s\" is a new file", filename);
        return;
    }

    {
        int size = fstat(fd);

        if (size < 0 || size > 512 * 1024) {
            close(fd);
            snprintf(E.status, sizeof(E.status), "file too large (max 512KB)");
            return;
        }

        buf = (char*)malloc((uint32_t)size + 1);
        if (!buf) {
            close(fd);
            snprintf(E.status, sizeof(E.status), "out of memory");
            return;
        }

        got = read(fd, buf, (uint32_t)size);
        close(fd);

        if (got < 0) {
            free(buf);
            snprintf(E.status, sizeof(E.status), "read failed");
            return;
        }
        buf[got] = '\0';
    }

    /* 按行切开 */
    start = 0;
    at = 0;
    for (int i = 0; i < got; i++) {
        if (buf[i] == '\n') {
            int len = i - start;

            /* 去掉行尾的 \r，兼容 Windows 换行 */
            if (len > 0 && buf[start + len - 1] == '\r') {
                len--;
            }
            row_insert(at++, buf + start, len);
            start = i + 1;
        }
    }
    if (start < got) {
        int len = got - start;

        if (len > 0 && buf[start + len - 1] == '\r') {
            len--;
        }
        row_insert(at++, buf + start, len);
    }

    free(buf);
    E.dirty = 0;
    snprintf(E.status, sizeof(E.status),
             "\"%s\"  %d lines", filename, E.numrows);
}

static int editor_save(void) {
    int fd;
    int total = 0;
    char* buf;
    int p = 0;
    int rc;

    if (E.filename[0] == '\0') {
        snprintf(E.status, sizeof(E.status), "no filename, use :w <name>");
        return -1;
    }

    /* 先把整个文件拼成一个缓冲区 */
    for (int i = 0; i < E.numrows; i++) {
        total += E.rows[i].size + 1;   /* 每行补一个换行 */
    }

    buf = (char*)malloc((uint32_t)total + 1);
    if (!buf) {
        snprintf(E.status, sizeof(E.status), "out of memory");
        return -1;
    }

    for (int i = 0; i < E.numrows; i++) {
        memcpy(buf + p, E.rows[i].chars, (uint32_t)E.rows[i].size);
        p += E.rows[i].size;
        buf[p++] = '\n';
    }

    fd = open(E.filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        free(buf);
        snprintf(E.status, sizeof(E.status), "cannot open for writing");
        return -1;
    }

    rc = write(fd, buf, (uint32_t)p);
    free(buf);

    if (rc < 0) {
        close(fd);
        snprintf(E.status, sizeof(E.status), "write failed");
        return -1;
    }

    fsync(fd);
    close(fd);

    E.dirty = 0;
    E.quit_times = 0;
    snprintf(E.status, sizeof(E.status), "\"%s\"  %d bytes written",
             E.filename, p);
    return 0;
}

/* ------------------------------------------------------------
 * 光标与滚动
 * ------------------------------------------------------------ */

/* 从原始列算出渲染列（把 tab 展开算进去） */
static int row_cx_to_rx(const erow* row, int cx) {
    int rx = 0;

    for (int i = 0; i < cx && i < row->size; i++) {
        if (row->chars[i] == '\t') {
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
}

static void scroll(void) {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = row_cx_to_rx(&E.rows[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + TEXT_ROWS) {
        E.rowoff = E.cy - TEXT_ROWS + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + VGA_COLS) {
        E.coloff = E.rx - VGA_COLS + 1;
    }
}

static void cursor_move(int dcy, int dcx) {
    int row_len;

    E.cy += dcy;
    if (E.cy < 0) { E.cy = 0; }
    if (E.cy >= E.numrows) { E.cy = (E.numrows > 0) ? E.numrows - 1 : 0; }

    row_len = (E.cy < E.numrows) ? E.rows[E.cy].size : 0;
    if (dcx > 0 && E.cx >= row_len) {
        /* 已到行尾，换到下一行的行首 */
        if (E.cy < E.numrows - 1) {
            E.cy++;
            E.cx = 0;
        }
    } else {
        E.cx += dcx;
        if (E.cx < 0) { E.cx = 0; }
        row_len = (E.cy < E.numrows) ? E.rows[E.cy].size : 0;
        if (E.cx > row_len) { E.cx = row_len; }
    }
}

/* ------------------------------------------------------------
 * 绘制
 * ------------------------------------------------------------ */
static void draw_status(void) {
    char left[120];
    char right[40];
    int len;
    int rlen;

    snprintf(left, sizeof(left), "%.40s%s  %d/%d",
             E.filename[0] ? E.filename : "[No Name]",
             E.dirty ? " [+]" : "",
             E.numrows ? E.cy + 1 : 0, E.numrows);

    len = (int)strlen(left);
    if (len > VGA_COLS) { len = VGA_COLS; }

    ocursor(STATUS_ROW, 0);
    oputs("\x1b[7m");            /* 反显 */
    oput(left, len);
    for (int i = len; i < VGA_COLS; i++) {
        oputc(' ');
    }
    oputs("\x1b[0m");

    /* 右下角放个模式提示 */
    if (E.mode == MODE_INSERT) {
        snprintf(right, sizeof(right), "-- INSERT --");
    } else if (E.mode == MODE_COMMAND) {
        snprintf(right, sizeof(right), "-- COMMAND --");
    } else {
        snprintf(right, sizeof(right), "-- NORMAL --");
    }
    rlen = (int)strlen(right);
    ocursor(STATUS_ROW, VGA_COLS - rlen);
    oputs("\x1b[7m");
    oputs(right);
    oputs("\x1b[0m");
}

static void draw_message(void) {
    ocursor(MSG_ROW, 0);

    if (E.mode == MODE_COMMAND) {
        oputs("\x1b[0m:");
        oput(E.cmd, E.cmdlen);
    } else if (E.status[0]) {
        oputs("\x1b[33m");       /* 黄色提示 */
        oputs(E.status);
        oputs("\x1b[0m");
    }
    oputs("\x1b[K");             /* 擦到行尾 */
}

static void draw_welcome(void) {
    static const char* art[] = {
        "        vi - HNeoC OS full screen editor",
        "",
        "        :e <file>   open a file",
        "        i           insert mode",
        "        :w  :q  :wq save / quit",
        "        u           undo",
        "",
        "        ESC returns to normal mode",
    };
    int n = (int)(sizeof(art) / sizeof(art[0]));
    int top = (TEXT_ROWS - n) / 2;

    for (int y = 0; y < TEXT_ROWS; y++) {
        ocursor(y, 0);
        if (y >= top && y < top + n) {
            const char* line = art[y - top];
            int len = (int)strlen(line);

            oputs("\x1b[36m");
            oputs(line);
            oputc('\r');
            /* 用 ESC[K 擦掉这一行剩下的部分 */
            oputs("\x1b[K");
            (void)len;
        } else {
            oputs("~");
            oputs("\x1b[K");
        }
        oputs("\r\n");
    }
}

static void draw_screen(void) {
    /* 画的过程中先藏起光标，避免它到处乱跳 */
    oputs("\x1b[?25l");

    if (E.numrows == 0) {
        draw_welcome();
    } else {
        for (int y = 0; y < TEXT_ROWS; y++) {
            int filerow = y + E.rowoff;

            ocursor(y, 0);

            if (filerow >= E.numrows) {
                oputs("~");
            } else {
                erow* row = &E.rows[filerow];
                int len = row->rsize - E.coloff;

                if (len > VGA_COLS) {
                    len = VGA_COLS;
                }
                if (len > 0) {
                    /* 有内容的行用亮灰，显得层次清楚一点 */
                    oputs("\x1b[37m");
                    oput(row->render + E.coloff, len);
                }
            }
            oputs("\x1b[0m\x1b[K");
        }
    }

    draw_status();
    draw_message();

    /* 把光标放回编辑器认为的位置 */
    ocursor(E.cy - E.rowoff, E.rx - E.coloff);
    oputs("\x1b[?25h");

    oflush();
    if (E.status[0] && E.mode != MODE_COMMAND) {
        /* 提示只显示一帧 */
        E.status[0] = '\0';
    }
}

/* ------------------------------------------------------------
 * 命令模式
 * ------------------------------------------------------------ */
static void command_execute(void) {
    char arg[64];
    int i = 0;
    int n;

    E.cmd[E.cmdlen] = '\0';

    /* 跳过前导空格 */
    while (i < E.cmdlen && E.cmd[i] == ' ') { i++; }

    if (i >= E.cmdlen) {
        return;   /* 空命令 */
    }

    /* 纯数字 = 跳到第 N 行 */
    if (isdigit(E.cmd[i])) {
        n = atoi(E.cmd + i);
        if (n > 0) {
            E.cy = n - 1;
            if (E.cy >= E.numrows) { E.cy = (E.numrows > 0) ? E.numrows - 1 : 0; }
            E.cx = 0;
        }
        return;
    }

    /* ---- :q / :q! ----
     * 原来这里只看了 'q'，后半截的 '!' 根本没解析，靠一个 quit_times 计数凑数：
     * 结果 :q! 要按两次才退，而没改过也会先警告一次；更糟的是有未保存改动时
     * 第二次 :q 会静默把改动丢掉。这里老老实实把 '!' 读出来。
     */
    if (E.cmd[i] == 'q') {
        int force = (i + 1 < E.cmdlen && E.cmd[i + 1] == '!');

        if (E.dirty && !force) {
            msg("unsaved changes! use :q! to discard, or :w to save");
            return;
        }
        E.running = 0;
        return;
    }

    /* ---- :w / :wq / :w! / :w <文件名> ----
     * 原来的 w 分支把 'w' 后面剩下的东西一律当文件名，于是 :wq 会把文件
     * 存到一个叫 "q" 的文件里（README 还写着支持 :wq）。这里先把 q / ! 摘掉，
     * 只有空格后面跟的东西才算文件名。
     */
    if (E.cmd[i] == 'w') {
        int quit_after = 0;

        i++;

        if (i < E.cmdlen && E.cmd[i] == 'q') {
            i++;
            quit_after = 1;
        }
        if (i < E.cmdlen && E.cmd[i] == '!') {
            i++;
            if (i < E.cmdlen && E.cmd[i] == 'q') {
                i++;
                quit_after = 1;
            }
        }

        while (i < E.cmdlen && E.cmd[i] == ' ') { i++; }
        if (i < E.cmdlen) {
            strncpy(arg, E.cmd + i, sizeof(arg) - 1);
            arg[sizeof(arg) - 1] = '\0';
            strncpy(E.filename, arg, sizeof(E.filename) - 1);
            E.filename[sizeof(E.filename) - 1] = '\0';
        }

        if (editor_save() != 0) {
            return;             /* 存盘失败就别退出，别把改动丢了 */
        }
        if (quit_after) {
            E.running = 0;
        }
        return;
    }

    if (E.cmd[i] == 'x') {
        if (!E.dirty || editor_save() == 0) {
            E.running = 0;
        }
        return;
    }

    if (E.cmd[i] == 'e') {
        i++;
        while (i < E.cmdlen && E.cmd[i] == ' ') { i++; }
        if (i >= E.cmdlen) {
            snprintf(E.status, sizeof(E.status), "usage: :e <filename>");
            return;
        }
        strncpy(arg, E.cmd + i, sizeof(arg) - 1);
        arg[sizeof(arg) - 1] = '\0';
        editor_open(arg);
        return;
    }

    snprintf(E.status, sizeof(E.status),
             "unknown command: %s", E.cmd + i);
}

/* ------------------------------------------------------------
 * 按键处理
 * ------------------------------------------------------------ */
static void insert_key(int c) {
    if (c == 27) {                       /* ESC */
        E.mode = MODE_NORMAL;
        if (E.cx > 0) { E.cx--; }
        return;
    }

    if (c == KEY_LEFT)  { cursor_move(0, -1); return; }
    if (c == KEY_RIGHT) { cursor_move(0, 1);  return; }
    if (c == KEY_UP)    { cursor_move(-1, 0); return; }
    if (c == KEY_DOWN)  { cursor_move(1, 0);  return; }

    if (c == KEY_HOME)  { E.cx = 0; return; }
    if (c == KEY_END)   { E.cx = (E.cy < E.numrows) ? E.rows[E.cy].size : 0; return; }

    if (c == KEY_DELETE) {
        if (E.cy < E.numrows && E.cx < E.rows[E.cy].size) {
            row_delete_char(&E.rows[E.cy], E.cx);
        }
        return;
    }

    if (c == 8 || c == 127) {            /* 退格 */
        if (E.cx > 0) {
            row_delete_char(&E.rows[E.cy], E.cx - 1);
            E.cx--;
        } else if (E.cy > 0) {
            /* 和上一行合并 */
            int prev_len = E.rows[E.cy - 1].size;

            row_append_string(&E.rows[E.cy - 1], E.rows[E.cy].chars,
                              E.rows[E.cy].size);
            row_delete(E.cy);
            E.cy--;
            E.cx = prev_len;
        }
        return;
    }

    if (c == '\r' || c == '\n') {        /* 回车：拆行 */
        if (E.cy < E.numrows) {
            erow* row = &E.rows[E.cy];
            int tail = row->size - E.cx;

            row_insert(E.cy + 1, row->chars + E.cx, tail);
            row->size = E.cx;
            row->chars[E.cx] = '\0';
            row_update(row);
        } else {
            row_insert(E.numrows, "", 0);
        }
        E.cy++;
        E.cx = 0;
        return;
    }

    if (c >= 32 && c < 127) {
        if (E.cy >= E.numrows) {
            row_insert(E.numrows, "", 0);
        }
        row_insert_char(&E.rows[E.cy], E.cx, c);
        E.cx++;
    }
}

/* ------------------------------------------------------------
 * 命令状态机的零件
 *
 * vi 的命令是"计数 + 操作符 + 动作"拼出来的：3dw、2dd、5j、d$、yy、cc……
 * 所以必须记住"上一次按了什么"。这一段就是为它准备的。
 * ------------------------------------------------------------ */

/* 取一次计数并清掉；没给就算 1 */
static int take_count(void) {
    int n = (pend_count > 0) ? pend_count : 1;

    pend_count = 0;
    return n;
}

static void clear_pending(void) {
    pend_count = 0;
    pend_op    = 0;
}

/* 把光标夹回合法位置 */
static void clamp_cursor(void) {
    if (E.cy < 0) { E.cy = 0; }
    if (E.cy >= E.numrows) { E.cy = (E.numrows > 0) ? E.numrows - 1 : 0; }
    if (E.cy < E.numrows) {
        if (E.cx > E.rows[E.cy].size) { E.cx = E.rows[E.cy].size; }
    } else {
        E.cx = 0;
    }
    if (E.cx < 0) { E.cx = 0; }
}

/* 下一个词首 */
static void word_forward(void) {
    erow* row = (E.cy < E.numrows) ? &E.rows[E.cy] : 0;

    if (!row) { return; }

    {
        int i = E.cx;

        while (i < row->size && !isspace(row->chars[i])) { i++; }
        while (i < row->size && isspace(row->chars[i])) { i++; }
        if (i >= row->size && E.cy < E.numrows - 1) {
            E.cy++;
            E.cx = 0;
            while (E.cx < E.rows[E.cy].size &&
                   isspace(E.rows[E.cy].chars[E.cx])) {
                E.cx++;
            }
        } else {
            E.cx = i;
        }
    }
}

/* 上一个词首 */
static void word_backward(void) {
    erow* row = (E.cy < E.numrows) ? &E.rows[E.cy] : 0;

    if (!row) { return; }

    {
        int i = E.cx;

        while (i > 0 && isspace(row->chars[i - 1])) { i--; }
        while (i > 0 && !isspace(row->chars[i - 1])) { i--; }
        E.cx = i;
    }
}

/* j/k/G/gg 这类动作按整行算，操作符遇到它们就退化成整行操作 */
static int is_linewise_motion(int k) {
    return (k == 'j' || k == 'k' || k == 'G' || k == 'g' ||
            k == KEY_UP || k == KEY_DOWN);
}

/* 执行一次动作。返回 0 表示这不是动作键 */
static int do_motion(int k, int count) {
    int n;

    if (count < 1) { count = 1; }

    switch (k) {
        case KEY_LEFT:  case 'h':
            for (n = 0; n < count; n++) { cursor_move(0, -1); }
            return 1;
        case KEY_RIGHT: case 'l':
            for (n = 0; n < count; n++) { cursor_move(0, 1); }
            return 1;
        case KEY_UP:    case 'k':
            for (n = 0; n < count; n++) { cursor_move(-1, 0); }
            return 1;
        case KEY_DOWN:  case 'j':
            for (n = 0; n < count; n++) { cursor_move(1, 0); }
            return 1;
        case '0':
            E.cx = 0;
            return 1;
        case '$':
            E.cx = (E.cy < E.numrows) ? E.rows[E.cy].size : 0;
            return 1;
        case 'w':
            for (n = 0; n < count; n++) { word_forward(); }
            return 1;
        case 'b':
            for (n = 0; n < count; n++) { word_backward(); }
            return 1;
        case 'G':   /* 有计数就跳到那一行，没有就到末尾 */
            E.cy = (count > 1) ? (count - 1) : ((E.numrows > 0) ? E.numrows - 1 : 0);
            clamp_cursor();
            E.cx = 0;
            return 1;
        case 'g':   /* gg：回到第一行（或用计数跳到第 N 行） */
            E.cy = (count > 1) ? (count - 1) : 0;
            clamp_cursor();
            E.cx = 0;
            return 1;
        default:
            return 0;
    }
}

/* ------------------------------------------------------------
 * 寄存器：dd / yy / dw 出来的内容
 * 布局 [charwise:4][lines:4]，后面跟 lines 组 [len:4][bytes]
 * ------------------------------------------------------------ */
static void reg_set_lines(int y1, int y2) {
    uint32_t total = 8;
    char* p;
    int i;

    if (E.numrows == 0 || y1 > y2) { return; }
    if (y1 < 0) { y1 = 0; }
    if (y2 >= E.numrows) { y2 = E.numrows - 1; }

    for (i = y1; i <= y2; i++) {
        total += 4 + (uint32_t)E.rows[i].size;
    }

    free(regblob);
    regblob = (char*)malloc(total);
    if (!regblob) { reglen = 0; return; }

    reg_charwise = 0;
    reg_lines    = y2 - y1 + 1;
    reglen       = total;

    p = regblob;
    memcpy(p, &reg_charwise, 4); p += 4;
    memcpy(p, &reg_lines, 4);    p += 4;
    for (i = y1; i <= y2; i++) {
        memcpy(p, &E.rows[i].size, 4); p += 4;
        memcpy(p, E.rows[i].chars, (uint32_t)E.rows[i].size);
        p += E.rows[i].size;
    }
}

static void reg_set_chars(int y, int x1, int x2) {
    uint32_t total = 8;
    char* p;
    int len;

    if (y < 0 || y >= E.numrows) { return; }
    if (x1 < 0) { x1 = 0; }
    if (x2 > E.rows[y].size) { x2 = E.rows[y].size; }
    len = x2 - x1;
    if (len <= 0) { return; }

    total += 4 + (uint32_t)len;

    free(regblob);
    regblob = (char*)malloc(total);
    if (!regblob) { reglen = 0; return; }

    reg_charwise = 1;
    reg_lines    = 1;
    reglen       = total;

    p = regblob;
    memcpy(p, &reg_charwise, 4); p += 4;
    memcpy(p, &reg_lines, 4);    p += 4;
    memcpy(p, &len, 4);          p += 4;
    memcpy(p, E.rows[y].chars + x1, (uint32_t)len);
}

/* p / P：整行按行插，字符片段插在光标处 */
static void reg_put(int after, int count) {
    char* p;
    int charwise;
    int lines;
    int k;
    int i;

    if (!regblob || reglen < 8) {
        msg("register is empty");
        return;
    }
    if (count < 1) { count = 1; }

    p = regblob;
    memcpy(&charwise, p, 4); p += 4;
    memcpy(&lines, p, 4);    p += 4;

    snapshot();

    if (charwise) {
        int y = E.cy;
        int at;

        if (E.numrows == 0) { row_insert(0, "", 0); y = 0; }
        if (y >= E.numrows) { y = E.numrows - 1; }

        at = after ? (E.cx + 1) : E.cx;
        if (at > E.rows[y].size) { at = E.rows[y].size; }

        for (k = 0; k < count; k++) {
            char* q = p;

            for (i = 0; i < lines; i++) {
                int len;
                int j;

                memcpy(&len, q, 4); q += 4;
                for (j = 0; j < len; j++) {
                    row_insert_char(&E.rows[y], at, (unsigned char)q[j]);
                    at++;
                }
                q += len;
            }
        }
        E.cy = y;
        E.cx = (at > 0) ? (at - 1) : 0;
    } else {
        int at_line = after ? (E.cy + 1) : E.cy;

        if (E.numrows == 0) { at_line = 0; }
        if (at_line > E.numrows) { at_line = E.numrows; }

        for (k = 0; k < count; k++) {
            int ins = at_line;
            char* q = p;

            for (i = 0; i < lines; i++) {
                int len;

                memcpy(&len, q, 4); q += 4;
                row_insert(ins, q, len);
                q += len;
                ins++;
            }
            at_line += lines;
        }
        E.cy = after ? (E.cy + 1) : E.cy;
        clamp_cursor();
        E.cx = 0;
    }

    snprintf(E.status, sizeof(E.status), "pasted %d line(s)",
             charwise ? count : lines * count);
}

/* ------------------------------------------------------------
 * 操作符 d / y / c
 *   dd yy cc   —— 整行
 *   dj dk dG   —— 动作跨行，按整行算
 *   dw d$ d0 x —— 行内字符范围
 * ------------------------------------------------------------ */
static void apply_operator(int op, int key, int count) {
    int y1 = E.cy;
    int x1 = E.cx;
    int y2 = E.cy;
    int x2 = E.cx;
    int linewise = 0;
    int i;

    if (count < 1) { count = 1; }
    if (E.numrows == 0) { msg("empty buffer"); return; }

    if (key == op) {                       /* dd / yy / cc */
        linewise = 1;
        y2 = E.cy + count - 1;
        if (y2 >= E.numrows) { y2 = E.numrows - 1; }
        x2 = 0;
    } else if (is_linewise_motion(key)) {
        linewise = 1;
        if (!do_motion(key, count)) { msg("unknown motion"); return; }
        y2 = E.cy;
        if (y2 >= E.numrows) { y2 = E.numrows - 1; }
        x2 = 0;
    } else {
        int inclusive = (key == '$');

        if (!do_motion(key, count)) { msg("unknown motion"); return; }

        y2 = E.cy;
        x2 = E.cx + (inclusive ? 1 : 0);

        if (y2 != y1) {                    /* 跨行就当整行处理，省得算半行 */
            int lo = (y1 < y2) ? y1 : y2;
            int hi = (y1 > y2) ? y1 : y2;

            linewise = 1;
            y1 = lo;
            y2 = hi;
            x1 = 0;
            x2 = 0;
        }
    }

    if (linewise) {
        int n = y2 - y1 + 1;

        if (op == 'y') {
            reg_set_lines(y1, y2);
            E.cy = y1;
            E.cx = 0;
            snprintf(E.status, sizeof(E.status), "%d line(s) yanked", n);
            return;
        }

        snapshot();
        reg_set_lines(y1, y2);
        for (i = 0; i < n; i++) {
            row_delete(y1);                /* 删掉之后后面的行会往前补 */
        }
        if (E.numrows == 0) {
            row_insert(0, "", 0);          /* 至少留一行，别让编辑器空掉 */
        }
        E.cy = (y1 < E.numrows) ? y1 : (E.numrows - 1);
        E.cx = 0;

        if (op == 'c') {
            E.mode = MODE_INSERT;
            snprintf(E.status, sizeof(E.status), "%d line(s) changed", n);
        } else {
            snprintf(E.status, sizeof(E.status), "%d line(s) deleted", n);
        }
    } else {
        int a = (x1 < x2) ? x1 : x2;
        int b = (x1 < x2) ? x2 : x1;
        int n;

        if (b > E.rows[y1].size) { b = E.rows[y1].size; }
        if (b <= a) { return; }
        n = b - a;

        if (op == 'y') {
            reg_set_chars(y1, a, b);
            E.cx = a;
            snprintf(E.status, sizeof(E.status), "%d char(s) yanked", n);
            return;
        }

        snapshot();
        reg_set_chars(y1, a, b);
        for (i = 0; i < n; i++) {
            row_delete_char(&E.rows[y1], a);   /* 每次都删同一个位置 */
        }
        E.cx = a;
        clamp_cursor();

        if (op == 'c') {
            E.mode = MODE_INSERT;
            snprintf(E.status, sizeof(E.status), "%d char(s) changed", n);
        } else {
            snprintf(E.status, sizeof(E.status), "%d char(s) deleted", n);
        }
    }
}

/* ------------------------------------------------------------
 * 搜索
 * ------------------------------------------------------------ */
static int line_find_forward(int y, int from) {
    char* hit;

    if (y < 0 || y >= E.numrows) { return -1; }
    if (from < 0) { from = 0; }
    if (from > E.rows[y].size) { return -1; }

    hit = strstr(E.rows[y].chars + from, last_search);
    return hit ? (int)(hit - E.rows[y].chars) : -1;
}

static int line_find_backward(int y, int from) {
    int x;
    int best = -1;

    if (y < 0 || y >= E.numrows) { return -1; }
    if (from > E.rows[y].size) { from = E.rows[y].size; }

    for (x = 0; x <= from && x + last_search_len <= E.rows[y].size; x++) {
        if (strncmp(E.rows[y].chars + x, last_search,
                    (uint32_t)last_search_len) == 0) {
            best = x;
        }
    }
    return best;
}

static void search_do(int dir) {
    int n;

    if (last_search_len == 0) { msg("no previous search"); return; }
    if (E.numrows == 0) { msg("empty buffer"); return; }

    for (n = 0; n <= E.numrows; n++) {
        int y = E.cy + dir * n;
        int from;
        int x;

        while (y >= E.numrows) { y -= E.numrows; }
        while (y < 0) { y += E.numrows; }

        /* 第一遍从光标旁边开始，绕回来之后整行都算 */
        if (n == 0) {
            from = (dir > 0) ? (E.cx + 1) : (E.cx - 1);
        } else {
            from = (dir > 0) ? 0 : E.rows[y].size;
        }

        x = (dir > 0) ? line_find_forward(y, from) : line_find_backward(y, from);
        if (x >= 0) {
            E.cy = y;
            E.cx = x;
            clamp_cursor();
            snprintf(E.status, sizeof(E.status), "/%s", last_search);
            return;
        }
    }
    msg("pattern not found");
}

static void search_ask(int dir) {
    E.mode     = MODE_SEARCH;
    search_dir = dir;
    E.cmdlen   = 0;
    E.cmd[0]   = '\0';
}

/* ------------------------------------------------------------
 * normal 模式：计数 + 操作符 + 动作
 * ------------------------------------------------------------ */
static void normal_key(int c) {
    int cnt;
    int n;

    /* ---- 计数前缀（0 只有在计数已经开始时才是数字）---- */
    if (c >= '1' && c <= '9') {
        pend_count = pend_count * 10 + (c - '0');
        return;
    }
    if (c == '0' && pend_count > 0) {
        pend_count = pend_count * 10;
        return;
    }

    /* ---- 两键命令的第二个键 ---- */
    if (pend_op == 'g') {
        pend_op = 0;
        do_motion('g', take_count());
        return;
    }
    if (pend_op == 'Z') {
        pend_op = 0;
        if (c == 'Z') {
            if (!E.dirty || editor_save() == 0) {
                E.running = 0;
            }
        }
        return;
    }
    if (pend_op == 'r') {
        cnt = take_count();
        pend_op = 0;
        if (E.cy < E.numrows && c >= 32 && c < 127) {
            snapshot();
            for (n = 0; n < cnt && (E.cx + n) < E.rows[E.cy].size; n++) {
                E.rows[E.cy].chars[E.cx + n] = (char)c;
            }
            row_update(&E.rows[E.cy]);
            E.dirty = 1;
        }
        return;
    }
    if (pend_op == 'd' || pend_op == 'y' || pend_op == 'c') {
        int op = pend_op;

        pend_op = 0;
        if (c == 27) {                     /* ESC 取消这次操作符 */
            pend_count = 0;
            msg("");
            return;
        }
        apply_operator(op, c, take_count());
        return;
    }

    if (c == 27) {                         /* 普通模式下 ESC 清状态 */
        pend_count = 0;
        msg("");
        return;
    }

    switch (c) {
        case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
        case KEY_HOME: case KEY_END:
        case KEY_PAGEUP: case KEY_PAGEDOWN:
        case KEY_DELETE:
            cnt = take_count();
            switch (c) {
                case KEY_LEFT:  do_motion('h', cnt); return;
                case KEY_RIGHT: do_motion('l', cnt); return;
                case KEY_UP:    do_motion('k', cnt); return;
                case KEY_DOWN:  do_motion('j', cnt); return;
                case KEY_HOME:  E.cx = 0; return;
                case KEY_END:
                    E.cx = (E.cy < E.numrows) ? E.rows[E.cy].size : 0;
                    return;
                case KEY_PAGEUP:
                    E.cy -= TEXT_ROWS * cnt;
                    if (E.cy < 0) { E.cy = 0; }
                    return;
                case KEY_PAGEDOWN:
                    E.cy += TEXT_ROWS * cnt;
                    if (E.cy >= E.numrows) {
                        E.cy = (E.numrows > 0) ? E.numrows - 1 : 0;
                    }
                    return;
                default:               /* KEY_DELETE */
                    if (E.cy < E.numrows) {
                        snapshot();
                        for (n = 0; n < cnt && E.cx < E.rows[E.cy].size; n++) {
                            row_delete_char(&E.rows[E.cy], E.cx);
                        }
                    }
                    return;
            }
    }

    switch (c) {
        case 'h': case 'l': case 'j': case 'k':
        case 'w': case 'b':
        case '0': case '$':
        case 'G':
            do_motion(c, take_count());
            return;

        case 'g':                       /* gg：等第二个键 */
            pend_op = 'g';
            return;
        case 'Z':                       /* ZZ：存盘退出 */
            pend_op = 'Z';
            return;

        /* ---- 进入插入模式：先落一个撤销点，插入过程中的改动都归这一级 ---- */
        case 'i':
            snapshot();
            E.mode = MODE_INSERT;
            clear_pending();
            return;
        case 'a':
            snapshot();
            if (E.cy < E.numrows && E.cx < E.rows[E.cy].size) { E.cx++; }
            E.mode = MODE_INSERT;
            clear_pending();
            return;
        case 'A':
            snapshot();
            if (E.cy < E.numrows) { E.cx = E.rows[E.cy].size; }
            E.mode = MODE_INSERT;
            clear_pending();
            return;
        case 'I':
            snapshot();
            E.cx = 0;
            E.mode = MODE_INSERT;
            clear_pending();
            return;
        case 'o':
            snapshot();
            row_insert(E.cy + 1, "", 0);
            E.cy++;
            E.cx = 0;
            E.mode = MODE_INSERT;
            clear_pending();
            return;
        case 'O':
            snapshot();
            row_insert(E.cy, "", 0);
            E.cx = 0;
            E.mode = MODE_INSERT;
            clear_pending();
            return;

        /* ---- 删除 / 修改 ---- */
        case 'x':
            cnt = take_count();
            if (E.cy < E.numrows) {
                snapshot();
                for (n = 0; n < cnt && E.cx < E.rows[E.cy].size; n++) {
                    row_delete_char(&E.rows[E.cy], E.cx);
                }
            }
            return;
        case 'X':                       /* 往光标前面删 */
            cnt = take_count();
            if (E.cy < E.numrows) {
                snapshot();
                for (n = 0; n < cnt && E.cx > 0; n++) {
                    row_delete_char(&E.rows[E.cy], E.cx - 1);
                    E.cx--;
                }
            }
            return;
        case 'd': case 'y': case 'c':   /* 操作符：等下一个键（动作或同一个键）*/
            pend_op = c;
            return;
        case 'D':                       /* D = d$ */
            apply_operator('d', '$', take_count());
            return;
        case 'C':                       /* C = c$ */
            apply_operator('c', '$', take_count());
            return;
        case 'S':                       /* S = cc */
            apply_operator('c', 'c', take_count());
            return;
        case 'p':
            reg_put(1, take_count());
            return;
        case 'P':
            reg_put(0, take_count());
            return;
        case 'r':                       /* r：下一个键替换光标处字符 */
            pend_op = 'r';
            return;
        case 'J': {                     /* 把下一行接到本行后面 */
            cnt = take_count();
            if (E.cy < E.numrows - 1) {
                snapshot();
                for (n = 0; n < cnt && E.cy < E.numrows - 1; n++) {
                    row_append_string(&E.rows[E.cy], " ", 1);
                    row_append_string(&E.rows[E.cy], E.rows[E.cy + 1].chars,
                                      E.rows[E.cy + 1].size);
                    row_delete(E.cy + 1);
                }
                msg("lines joined");
            }
            return;
        }

        case 'u':   /* 撤销（多级） */
            undo();
            clear_pending();
            return;

        /* ---- 搜索 ---- */
        case '/':
            search_ask(1);
            clear_pending();
            return;
        case '?':
            search_ask(-1);
            clear_pending();
            return;
        case 'n':
            search_do(last_dir);
            clear_pending();
            return;
        case 'N':
            search_do(-last_dir);
            clear_pending();
            return;

        case ':':
            E.mode   = MODE_COMMAND;
            E.cmdlen = 0;
            E.cmd[0] = '\0';
            clear_pending();
            return;

        default:
            clear_pending();
            return;
    }
}

/* 搜索输入：和命令行共用缓冲区，只是方向不同 */
static void search_key(int c) {
    if (c == 27) {                 /* ESC 取消 */
        E.mode   = MODE_NORMAL;
        E.cmdlen = 0;
        return;
    }
    if (c == '\r' || c == '\n') {
        E.mode = MODE_NORMAL;
        if (E.cmdlen > 0) {
            snprintf(last_search, sizeof(last_search), "%s", E.cmd);
            last_search_len = E.cmdlen;
            last_dir        = search_dir;
        }
        if (last_search_len == 0) {
            msg("no pattern");
            return;
        }
        search_do(last_dir);
        return;
    }
    if (c == 8 || c == 127) {
        if (E.cmdlen > 0) {
            E.cmdlen--;
            E.cmd[E.cmdlen] = '\0';
        } else {
            E.mode = MODE_NORMAL;
        }
        return;
    }
    if (c >= 32 && c < 127 && E.cmdlen < (int)sizeof(E.cmd) - 1) {
        E.cmd[E.cmdlen++] = (char)c;
        E.cmd[E.cmdlen]   = '\0';
    }
}

static void command_key(int c) {
    if (c == 27) {                 /* ESC 取消 */
        E.mode   = MODE_NORMAL;
        E.cmdlen = 0;
        return;
    }
    if (c == '\r' || c == '\n') {
        E.mode = MODE_NORMAL;
        command_execute();
        return;
    }
    if (c == 8 || c == 127) {
        if (E.cmdlen > 0) {
            E.cmdlen--;
            E.cmd[E.cmdlen] = '\0';
        } else {
            E.mode = MODE_NORMAL;
        }
        return;
    }
    if (c >= 32 && c < 127 && E.cmdlen < (int)sizeof(E.cmd) - 1) {
        E.cmd[E.cmdlen++] = (char)c;
        E.cmd[E.cmdlen]   = '\0';
    }
}

/* ------------------------------------------------------------
 * 主循环
 * ------------------------------------------------------------ */
int main(void) {
    int c;

    /* 切到备用屏幕：退出时原来的画面自动恢复 */
    write(1, "\x1b[?1049h\x1b[2J\x1b[H", 15);
    write(1, "\x1b[?25h", 6);

    E.numrows = 0;
    E.rows    = 0;
    E.dirty   = 0;
    E.mode    = MODE_NORMAL;
    E.running = 1;
    E.cmdlen  = 0;
    E.cy = E.cx = E.rx = 0;
    E.rowoff = E.coloff = 0;
    E.filename[0] = '\0';
    E.status[0]   = '\0';
    E.quit_times  = 0;

    snprintf(E.status, sizeof(E.status),
             "vi for HNeoC OS - :e <file> to open, :q to quit");

    /* 命令行参数：`vi <文件>` 直接打开它。
     * 内核把命令名之后的那段文本通过 getargs 交给程序 —— 原来这里
     * 完全没读参数，所以敲 `vi motd.txt` 打开的是 [No Name] 空缓冲区。
     */
    {
        char argv[64];
        char name[64];
        int  i = 0;
        int  n = 0;

        getargs(argv, sizeof(argv));
        while (argv[i] == ' ' || argv[i] == '\t') { i++; }
        while (argv[i] && argv[i] != ' ' && argv[i] != '\t' &&
               n < (int)sizeof(name) - 1) {
            name[n++] = argv[i++];
        }
        name[n] = '\0';

        if (n > 0) {
            editor_open(name);
        }
    }

    while (E.running) {
        scroll();
        draw_screen();

        c = getkey();

        if (E.mode == MODE_INSERT) {
            insert_key(c);
        } else if (E.mode == MODE_COMMAND) {
            command_key(c);
        } else if (E.mode == MODE_SEARCH) {
            search_key(c);
        } else {
            normal_key(c);
        }
    }

    /* 回到普通屏幕 */
    write(1, "\x1b[0m\x1b[?1049l", 12);
    puts("vi: exited");
    return 0;
}
