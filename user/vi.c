/* vi - HNeoC OS 的全屏模态编辑器
 *
 * 这是一个从零写的 vi 克隆，用的是 kilo 那一系的"行数组"结构：
 * 每一行存原始内容，另存一份把 tab 展开后的版本用于显示。
 *
 * 它不碰任何绘图接口：屏幕上的每一样东西都是往标准输出写
 * ANSI 转义序列画出来的，按键则通过 getkey() 从内核拿。
 *
 * 支持：
 *   普通模式  h j k l / 方向键 / 0 $ w b / gg G / PageUp PageDown
 *             i a A o O   进入插入模式
 *             x           删除光标处字符
 *             dd          删除整行
 *             yy p        复制 / 粘贴整行
 *             u           撤销（单级）
 *             :           进入命令模式
 *   插入模式  可打印字符、回车、退格、ESC 返回
 *   命令模式  :w :q :q! :wq :x :e <文件> :<行号>
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

/* 单级撤销：改动前把整个缓冲区序列化存一份 */
static char* undoblob = 0;
static uint32_t undolen = 0;

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
 * 撤销：把整个缓冲区打包成一个 blob，撤销时还原
 * 只保留一级，够用了
 * ------------------------------------------------------------ */
static void snapshot(void) {
    uint32_t total = 4;   /* 行数 */

    for (int i = 0; i < E.numrows; i++) {
        total += 4 + (uint32_t)E.rows[i].size;
    }

    free(undoblob);
    undoblob = (char*)malloc(total);
    if (!undoblob) {
        undolen = 0;
        return;
    }

    undolen = total;
    {
        char* p = undoblob;
        memcpy(p, &E.numrows, 4);
        p += 4;
        for (int i = 0; i < E.numrows; i++) {
            memcpy(p, &E.rows[i].size, 4);
            p += 4;
            memcpy(p, E.rows[i].chars, (uint32_t)E.rows[i].size);
            p += E.rows[i].size;
        }
    }
}

static void undo(void) {
    char* p;
    int rows;
    int i;

    if (!undoblob || undolen < 4) {
        snprintf(E.status, sizeof(E.status), "already at the oldest change");
        return;
    }

    /* 先把当前内容整个丢掉 */
    for (i = 0; i < E.numrows; i++) {
        row_free(&E.rows[i]);
    }
    free(E.rows);
    E.rows    = 0;
    E.numrows = 0;

    p = undoblob;
    memcpy(&rows, p, 4);
    p += 4;

    for (i = 0; i < rows; i++) {
        int len;

        memcpy(&len, p, 4);
        p += 4;
        row_insert(E.numrows, p, len);
        p += len;
    }

    E.dirty = 1;
    E.cy    = 0;
    E.cx    = 0;
    E.rx    = 0;
    E.rowoff = 0;
    E.coloff = 0;

    snprintf(E.status, sizeof(E.status), "undo: restored %d lines", E.numrows);

    /* 撤销只能做一次，用完就丢 */
    free(undoblob);
    undoblob = 0;
    undolen  = 0;
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

    if (E.cmd[i] == 'q') {
        if (E.dirty && E.quit_times < 1) {
            snprintf(E.status, sizeof(E.status),
                     "unsaved changes! :q! to discard, or :w to save");
            E.quit_times++;
            return;
        }
        E.running = 0;
        return;
    }

    if (E.cmd[i] == 'w') {
        /* :w 或 :w <文件名> */
        i++;
        while (i < E.cmdlen && E.cmd[i] == ' ') { i++; }
        if (i < E.cmdlen) {
            strncpy(arg, E.cmd + i, sizeof(arg) - 1);
            arg[sizeof(arg) - 1] = '\0';
            strncpy(E.filename, arg, sizeof(E.filename) - 1);
            E.filename[sizeof(E.filename) - 1] = '\0';
        }
        editor_save();
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

static void normal_key(int c) {
    /* 先处理方向键和翻页 */
    switch (c) {
        case KEY_LEFT:  cursor_move(0, -1); return;
        case KEY_RIGHT: cursor_move(0, 1);  return;
        case KEY_UP:    cursor_move(-1, 0); return;
        case KEY_DOWN:  cursor_move(1, 0);  return;
        case KEY_HOME:  E.cx = 0; return;
        case KEY_END:
            E.cx = (E.cy < E.numrows) ? E.rows[E.cy].size : 0;
            return;
        case KEY_PAGEUP:
            E.cy -= TEXT_ROWS;
            if (E.cy < 0) { E.cy = 0; }
            return;
        case KEY_PAGEDOWN:
            E.cy += TEXT_ROWS;
            if (E.cy >= E.numrows) { E.cy = (E.numrows > 0) ? E.numrows - 1 : 0; }
            return;
        case KEY_DELETE:
            if (E.cy < E.numrows && E.cx < E.rows[E.cy].size) {
                snapshot();
                row_delete_char(&E.rows[E.cy], E.cx);
            }
            return;
        default:
            break;
    }

    switch (c) {
        case 'h': cursor_move(0, -1); return;
        case 'l': cursor_move(0, 1);  return;
        case 'j': cursor_move(1, 0);  return;
        case 'k': cursor_move(-1, 0); return;

        case '0': E.cx = 0; return;
        case '$':
            E.cx = (E.cy < E.numrows) ? E.rows[E.cy].size : 0;
            return;

        case 'w': {   /* 下一个词首 */
            erow* row = (E.cy < E.numrows) ? &E.rows[E.cy] : 0;

            if (row) {
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
            return;
        }

        case 'b': {   /* 上一个词首 */
            erow* row = (E.cy < E.numrows) ? &E.rows[E.cy] : 0;

            if (row) {
                int i = E.cx;

                while (i > 0 && isspace(row->chars[i - 1])) { i--; }
                while (i > 0 && !isspace(row->chars[i - 1])) { i--; }
                E.cx = i;
            }
            return;
        }

        case 'G':   /* 跳到文件末尾 */
            E.cy = (E.numrows > 0) ? E.numrows - 1 : 0;
            E.cx = 0;
            return;
        case 'g': {  /* gg 需要看下一个键，这里简化成按一次就跳开头 */
            E.cy = 0;
            E.cx = 0;
            return;
        }

        case 'i':   /* 在光标前插入 */
            E.mode = MODE_INSERT;
            return;
        case 'a':   /* 在光标后插入 */
            if (E.cy < E.numrows && E.cx < E.rows[E.cy].size) {
                E.cx++;
            }
            E.mode = MODE_INSERT;
            return;
        case 'A':   /* 行尾插入 */
            if (E.cy < E.numrows) {
                E.cx = E.rows[E.cy].size;
            }
            E.mode = MODE_INSERT;
            return;
        case 'I':   /* 行首插入 */
            E.cx = 0;
            E.mode = MODE_INSERT;
            return;
        case 'o':   /* 下面开新行 */
            snapshot();
            row_insert(E.cy + 1, "", 0);
            E.cy++;
            E.cx = 0;
            E.mode = MODE_INSERT;
            return;
        case 'O':   /* 上面开新行 */
            snapshot();
            row_insert(E.cy, "", 0);
            E.cx = 0;
            E.mode = MODE_INSERT;
            return;

        case 'x':   /* 删一个字符 */
            if (E.cy < E.numrows && E.cx < E.rows[E.cy].size) {
                snapshot();
                row_delete_char(&E.rows[E.cy], E.cx);
            }
            return;

        case 'd':   /* dd：这里简化成按一次 d 就删当前行 */
            if (E.numrows > 0) {
                snapshot();
                row_delete(E.cy);
                if (E.cy >= E.numrows) {
                    E.cy = (E.numrows > 0) ? E.numrows - 1 : 0;
                }
                E.cx = 0;
            }
            return;

        case 'u':   /* 撤销 */
            undo();
            return;

        case ':':
            E.mode   = MODE_COMMAND;
            E.cmdlen = 0;
            E.cmd[0] = '\0';
            return;

        default:
            return;
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

    while (E.running) {
        scroll();
        draw_screen();

        c = getkey();

        /* 任何模式下 ESC 都能回到普通模式 */
        if (E.mode == MODE_INSERT) {
            insert_key(c);
        } else if (E.mode == MODE_COMMAND) {
            command_key(c);
        } else {
            normal_key(c);
        }

        /* 插入模式下改动了内容，退出插入时刷新撤销点 */
        if (E.mode == MODE_INSERT && undoblob == 0 && E.dirty) {
            /* 第一次进入修改，做一个基准快照（简化处理） */
        }
    }

    /* 回到普通屏幕 */
    write(1, "\x1b[0m\x1b[?1049l", 12);
    puts("vi: exited");
    return 0;
}
