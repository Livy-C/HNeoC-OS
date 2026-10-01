/* hncc - HNeoC 的 C 编译器
 *
 * 一个跑在 HNeoC 上的 ring 3 程序：把 .c 编译成能直接执行的 .lxe。
 *
 *   hncc hello.c                  -> hello.lxe
 *   hncc hello.c -o /tmp/a.lxe    -> 指定输出
 *
 * 它自己生成 x86 机器码，不依赖任何汇编器/链接器，这在这个系统上是
 * 可行的关键：LXE 格式是"固定加载到 0x40000000 的扁平二进制 + 40 字节
 * 头部"，没有重定位、没有段。所以编译器可以：
 *
 *   1. 把代码生成到一块缓冲区，把字符串/全局变量生成到另一块；
 *   2. 代码里引用数据的地方全部记成"重定位项"，等代码长度定下来
 *      （数据的地址 = 基址 + 代码长度 + 数据内偏移）再回头改写；
 *   3. 最后拼上 LXE 头部写文件。
 *
 * 编译流程是教科书式的：词法 -> 语法树 -> 代码生成，没有优化。
 *
 * 支持的 C 子集（刻意保守，够写真实的小程序）：
 *   类型   int / char / void、指针（任意层）、数组（可多维）
 *   语句   变量声明（可带初值）、if/else、while、do-while、for、
 *          return、break、continue、复合语句、表达式语句
 *   运算   + - * / %  == != < <= > >=  && || !  & | ^ ~ << >>  ++ --
 *          = 和复合赋值  ?:  []  ()  & * sizeof 强制类型转换
 *   其它   函数（可递归、可前置声明）、全局变量、字符串/字符字面量、
 *          单行与块注释、忽略 #include（见下）
 *
 * 不支持：浮点、long long（当 int 处理）、真正的预处理器、多文件编译、
 *         初始化列表里的嵌套数组与结构体元素。
 *         结构体只能通过指针传参/返回，也不能整体赋值（解析阶段明确报错）。
 *         typedef 必须先声明后使用。
 *
 * 运行时是"自己编译自己"：编译器内部带着一段用这个子集写的 C 源码
 * （见文件末尾的 RT_SOURCE），编译用户代码之前先把它编译进去，
 * 等于每次编译都在自测。需要系统调用的那几个（putchar/getchar/...）
 * 是编译器直接发射 int 0x80 的内建函数，所以用户程序里不需要
 * #include <hneoc.h>，也不需要链接任何库。
 *
 * 因为运行时是内建的，#include 一律被忽略（支持的少数 #define
 * 见 lexer 里的说明）—— 它没有真正的预处理器。
 */

#include <hneoc.h>
#include <lxe.h>

/* ============================================================
 * 1. 缓冲区与输出
 * ============================================================ */

#define CODE_CAP   (96 * 1024)
#define DATA_CAP   (96 * 1024)
#define SRC_CAP    (96 * 1024)
/* 语法树的缓冲区（只涨不回收）。
 *
 * 512KB 曾经够用，但运行时本身（strlen/printf/malloc/字符串函数……）
 * 就要吃掉 527KB —— 一超就是"任何文件都编译不了"，而且报的是
 * "out of compiler memory"，看着像用户的程序太大，实际是自己撑爆的。
 * 现在给到 1MB：用户区有 4MB，hncc 自身的映像 48KB + bss 132KB，够用。
 * 如果以后还超，优先想办法缩小运行时，而不是继续加这个数。 */
#define ARENA_CAP  (1024 * 1024)
#define MAX_LABELS 512
#define MAX_FIXUPS 1024
#define MAX_SYMS   128
#define MAX_LOCALS 128
#define MAX_CALL_ARGS 16     /* 函数参数最多 8 个，留一倍余量 */

static unsigned char* code;            /* 代码 */
static int code_len;

static unsigned char* data;            /* 字符串字面量 + 有初值的全局变量 */
static int data_len;

static int src_line = 1;               /* 报错用的行号 */
static const char* src_name = "?";

/* 编译出错就用它跳出来 */
static void fatal(const char* fmt, ...);
static void err(const char* what, const char* detail);
static void patch32_data(int at, uint32_t v);
static int  find_struct(const char* name);   /* add_constant 要用（它在下面定义） */

static void copy_str_local(char* dst, const char* src, int max) {
    int i = 0;

    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void emit8(int b) {
    if (code_len >= CODE_CAP) {
        fatal("generated code is too large (limit %d bytes)", CODE_CAP);
    }
    code[code_len++] = (unsigned char)b;
}

static void emit32(uint32_t v) {
    emit8((int)(v & 0xFF));
    emit8((int)((v >> 8) & 0xFF));
    emit8((int)((v >> 16) & 0xFF));
    emit8((int)((v >> 24) & 0xFF));
}

static void patch32(int at, uint32_t v) {
    code[at + 0] = (unsigned char)(v & 0xFF);
    code[at + 1] = (unsigned char)((v >> 8) & 0xFF);
    code[at + 2] = (unsigned char)((v >> 16) & 0xFF);
    code[at + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static int data_emit_bytes(const void* p, int n) {
    int at = data_len;

    if (data_len + n > DATA_CAP) {
        fatal("initialised data is too large (limit %d bytes)", DATA_CAP);
    }
    memcpy(data + data_len, p, (size_t)n);
    data_len += n;
    return at;
}

static int data_emit32(uint32_t v) {
    unsigned char b[4];

    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF);
    b[3] = (unsigned char)((v >> 24) & 0xFF);
    return data_emit_bytes(b, 4);
}

static int data_emit_zeros(int n) {
    unsigned char zeros[16];
    int at = data_len;

    memset(zeros, 0, sizeof(zeros));
    while (n > 0) {
        int chunk = (n > (int)sizeof(zeros)) ? (int)sizeof(zeros) : n;

        data_emit_bytes(zeros, chunk);
        n -= chunk;
    }
    return at;
}

/* ------------------------------------------------------------
 * x86 机器码发射器
 *
 * 只覆盖这个编译器需要的那一小撮指令。寄存器编号按 x86 约定：
 *   eax=0 ecx=1 edx=2 ebx=3 esp=4 ebp=5 esi=6 edi=7
 * ------------------------------------------------------------ */

#define R_EAX 0
#define R_ECX 1
#define R_EDX 2
#define R_EBX 3
#define R_ESP 4
#define R_EBP 5
#define R_ESI 6
#define R_EDI 7

static void e_push(int r)  { emit8(0x50 + r); }
static void e_pop(int r)   { emit8(0x58 + r); }
static void e_ret(void)    { emit8(0xC3); }
static void e_leave(void)  { emit8(0xC9); }
static void e_hlt(void)    { emit8(0xF4); }

static void e_mov_r_imm(int r, uint32_t v) {
    emit8(0xB8 + r);
    emit32(v);
}

/* mov dst, src（都是寄存器） */
static void e_mov_rr(int dst, int src) {
    emit8(0x89);
    emit8(0xC0 | (src << 3) | dst);
}

/* add/sub/and/or/xor/cmp dst, src（都是寄存器，结果留在 dst） */
static void e_alu_rr(int opcode, int dst, int src) {
    emit8(opcode);
    emit8(0xC0 | (src << 3) | dst);
}

#define ALU_ADD 0x01
#define ALU_SUB 0x29
#define ALU_AND 0x21
#define ALU_OR  0x09
#define ALU_XOR 0x31
#define ALU_CMP 0x39
#define ALU_IMUL 0xAF      /* 两字节：0F AF /r */

static void e_imul_rr(int dst, int src) {
    emit8(0x0F);
    emit8(ALU_IMUL);
    emit8(0xC0 | (dst << 3) | src);
}

static void e_neg(int r) {
    emit8(0xF7);
    emit8(0xD8 | r);
}

static void e_not(int r) {
    emit8(0xF7);
    emit8(0xD0 | r);
}

static void e_cdq(void)  { emit8(0x99); }         /* cdq：符号扩展到 edx:eax */
static void e_idiv(int r) { emit8(0xF7); emit8(0xF8 | r); }
static void e_shl_cl(void) { emit8(0xD3); emit8(0xE0); }
static void e_sar_cl(void) { emit8(0xD3); emit8(0xF8); }
static void e_test_rr(int a, int b) { emit8(0x85); emit8(0xC0 | (b << 3) | a); }

/* 0F 后缀的指令：movsx / movzx / setcc */
static void e_movsx_byte(int dst, int src) {
    emit8(0x0F); emit8(0xBE); emit8(0xC0 | (dst << 3) | src);
}
static void e_movzx_byte(int dst, int src) {
    emit8(0x0F); emit8(0xB6); emit8(0xC0 | (dst << 3) | src);
}
static void e_setcc(int cc, int r) {
    emit8(0x0F); emit8(0x90 + cc); emit8(0xC0 | r);
}

/* 条件码：和 x86 的 /cc 后缀一致 */
#define CC_E  0x4
#define CC_NE 0x5
#define CC_L  0xC
#define CC_GE 0xD
#define CC_LE 0xE
#define CC_G  0xF

/* 内存操作：基址寄存器 + 32 位位移，宽度由 opcode 决定 */
static void e_modrm_mem(int reg, int base, int32_t disp) {
    /* [base + disp32]：mod=10 */
    emit8(0x80 | (reg << 3) | base);
    emit32((uint32_t)disp);
}

/* mov r32, [base+disp] */
static void e_load32(int dst, int base, int32_t disp) {
    emit8(0x8B);
    e_modrm_mem(dst, base, disp);
}

/* mov [base+disp], r32 */
static void e_store32(int src, int base, int32_t disp) {
    emit8(0x89);
    e_modrm_mem(src, base, disp);
}

/* mov r32, [base+disp] 但只读一个字节并符号扩展 */
static void e_load8(int dst, int base, int32_t disp) {
    emit8(0x0F); emit8(0xBE);
    e_modrm_mem(dst, base, disp);
}

/* mov [base+disp], r8（寄存器的最低字节） */
static void e_store8(int src, int base, int32_t disp) {
    emit8(0x88);
    e_modrm_mem(src, base, disp);
}

/* lea r32, [base+disp] */
static void e_lea(int dst, int base, int32_t disp) {
    emit8(0x8D);
    e_modrm_mem(dst, base, disp);
}

/* mov r32, [esp] —— switch 把待比较的值压在栈顶，取它用。
 * mod=00 / rm=100 要跟一个 SIB 字节 0x24（base=esp, index=无）。 */
static void e_load_sp(int dst) {
    emit8(0x8B);
    emit8(0x04 | (dst << 3));
    emit8(0x24);
}

/* int 0x80 */
static void e_int80(void) {
    emit8(0xCD);
    emit8(0x80);
}

/* ------------------------------------------------------------
 * 跳转与重定位
 *
 * 前向跳转的目标还不知道，所以先发一个占位位移，把"要改哪里、
 * 跳到哪个标签"记下来，等标签落地再统一回填。
 * ------------------------------------------------------------ */

typedef struct {
    int at;        /* 要改的 4 字节位移在 code 里的位置 */
    int label;     /* 目标标签 */
} jump_fix_t;

static jump_fix_t jump_fix[MAX_FIXUPS];
static int jump_fix_n;

static int label_pc[MAX_LABELS];       /* -1 表示还没落地 */

/* 引用数据区（字符串、全局变量）的绝对地址，等代码长度确定后回填。
 * in_data = 1 表示这个 4 字节在数据区里（全局指针变量的初值），
 * 否则在代码区。bss = 1 表示目标在 bss 区（偏移是相对 bss 起点的）。 */
typedef struct {
    int at;        /* 4 字节地址的位置 */
    int target;    /* 目标在数据区/bss 区里的偏移 */
    int in_data;   /* 0 = 代码区，1 = 数据区 */
    int is_bss;    /* 目标是不是 bss */
    int bss_rel;   /* is_bss 时的相对偏移 */
} data_fix_t;

static data_fix_t data_fix[MAX_FIXUPS];
static int data_fix_n;

static void init_labels(void) {
    for (int i = 0; i < MAX_LABELS; i++) {
        label_pc[i] = -1;
    }
    jump_fix_n = 0;
    data_fix_n = 0;
}

static int new_label(void) {
    static int next = 0;

    if (next >= MAX_LABELS) {
        fatal("too many labels (limit %d)", MAX_LABELS);
    }
    return next++;
}

/* 记下一个需要回填的绝对地址（指到已初始化数据区） */
static void ref_data(int at, int data_off, int in_data) {
    if (data_fix_n >= MAX_FIXUPS) {
        fatal("too many data references");
    }
    data_fix[data_fix_n].at = at;
    data_fix[data_fix_n].target = data_off;
    data_fix[data_fix_n].in_data = in_data;
    data_fix[data_fix_n].is_bss = 0;
    data_fix[data_fix_n].bss_rel = 0;
    data_fix_n++;
}

/* 记下一个要指向 bss 区的绝对地址 */
static void ref_bss(int at, int bss_rel, int in_data) {
    if (data_fix_n >= MAX_FIXUPS) {
        fatal("too many data references");
    }
    data_fix[data_fix_n].at = at;
    data_fix[data_fix_n].target = 0;
    data_fix[data_fix_n].in_data = in_data;
    data_fix[data_fix_n].is_bss = 1;
    data_fix[data_fix_n].bss_rel = bss_rel;
    data_fix_n++;
}

/* mov r32, <数据区某处的绝对地址>：占位，最后回填 */
static void e_mov_r_dataaddr(int r, int data_off) {
    emit8(0xB8 + r);
    ref_data(code_len, data_off, 0);
    emit32(0);
}

static void e_mov_r_bssaddr(int r, int bss_rel) {
    emit8(0xB8 + r);
    ref_bss(code_len, bss_rel, 0);
    emit32(0);
}

/* jmp / jcc 到标签 */
static void e_jmp_label(int opcode, int opcode2, int label) {
    int at;

    if (opcode2) {
        emit8(0x0F);
        emit8(opcode2);
    } else {
        emit8(opcode);
    }
    at = code_len;
    emit32(0);                    /* 先占位 */

    /* 目标已经落地（向后引用：跳回循环开头、调用前面发射过的函数）：
     * 当场就能算位移，直接写完事。
     *
     * 这个分支不是优化，是正确性：place_label 只在标签落地的那一刻回填
     * 当时已经登记过的引用，向后引用是在那之后才发出来的，不在这里处理
     * 就永远停留在位移 0 —— 也就是"跳到下一条指令"。症状是调用某个函数
     * 变成了顺序执行，最后在别的地方空指针缺页（实测就是这么炸的：
     * 反汇编里 `e8 00 00 00 00` 明晃晃地摆着）。
     */
    if (label_pc[label] >= 0) {
        patch32(at, (uint32_t)(label_pc[label] - (at + 4)));
        return;
    }

    if (jump_fix_n >= MAX_FIXUPS) {
        fatal("too many forward jumps");
    }
    jump_fix[jump_fix_n].at = at;
    jump_fix[jump_fix_n].label = label;
    jump_fix_n++;
}

static void e_jmp(int label)         { e_jmp_label(0xE9, 0, label); }
static void e_je(int label)          { e_jmp_label(0, 0x84, label); }
static void e_jne(int label)         { e_jmp_label(0, 0x85, label); }

/* call rel32：位移的算法和 jmp 完全一样，直接复用 */
static void e_call_label(int label)  { e_jmp_label(0xE8, 0, label); }

/* add/sub esp, imm32 */
static void e_add_esp_imm(uint32_t v) { emit8(0x81); emit8(0xC4); emit32(v); }
static void e_sub_esp_imm(uint32_t v) { emit8(0x81); emit8(0xEC); emit32(v); }

/* 落标签：把之前所有指向它的跳转一次回填。
 * 位移是相对"下一条指令"，也就是 at+4。 */
static void place_label(int label) {
    label_pc[label] = code_len;
    for (int i = 0; i < jump_fix_n; i++) {
        if (jump_fix[i].label == label) {
            patch32(jump_fix[i].at, (uint32_t)(code_len - (jump_fix[i].at + 4)));
            jump_fix[i].label = -1;
        }
    }
}

/* ============================================================
 * 2. 词法分析
 * ============================================================ */

typedef enum {
    TK_EOF, TK_NUM, TK_STR, TK_CHARLIT, TK_IDENT, TK_PUNCT
} tk_kind;

typedef struct {
    tk_kind kind;
    int     num;          /* 数字 / 字符字面量的值 */
    char    text[256];    /* 标识符名 / 字符串内容 */
    char    punct[4];     /* 运算符，最多三个字符 */
    int     line;
} token_t;

static const char* lex_p;             /* 当前扫描位置 */
static token_t tok;                   /* 当前 token（向前看一个） */

/* 报错并退出。
 *
 * 用户运行时的 printf 系列是用"参数在栈上紧跟在 fmt 之后"这个约定取
 * 可变参数的（见 user/lib/src/stdio.c），这里照抄同一招：
 * 不依赖 stdarg.h（这个系统上的 libc 没有它）。 */
static void fatal(const char* fmt, ...) {
    char buf[256];
    uint32_t* args = (uint32_t*)(&fmt) + 1;

    vsnprintf(buf, sizeof(buf), fmt, args);

    term_set_fg(TERM_LIGHT_RED);
    fprintf(STDERR_FILENO, "hncc: %s:%d: %s\n", src_name, src_line, buf);
    term_reset();
    exit(1);
}

static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int is_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }

/* 跳过一个 # 指令行。
 *
 * 编译器没有真正的预处理器：#include 直接忽略（运行时是内建的），
 * #define 只认最简单的"名字 数字"形式，之后按常量替换。
 * enum 的枚举名也登记进同一张表（见 parse_enum_def）—— 在代码生成阶段
 * 枚举完全不存在，就是个常量。 */
#define MAX_CONSTS 64

static char define_names[MAX_CONSTS][64];
static int  define_values[MAX_CONSTS];
static int  define_count;

static void add_constant(const char* name, int value) {
    if (find_struct(name) >= 0) {
        fatal("'%s' is already a struct name", name);
    }
    for (int i = 0; i < define_count; i++) {
        if (strcmp(define_names[i], name) == 0) {
            define_values[i] = value;      /* 重定义就覆盖，和 #define 一样 */
            return;
        }
    }
    if (define_count >= MAX_CONSTS) {
        fatal("too many constants (#define + enum, limit %d)", MAX_CONSTS);
    }
    copy_str_local(define_names[define_count], name, sizeof(define_names[0]));
    define_values[define_count] = value;
    define_count++;
}

static void skip_directive(void) {
    char line[256];
    int n = 0;

    while (*lex_p && *lex_p != '\n') {
        if (n < (int)sizeof(line) - 1) {
            line[n++] = *lex_p;
        }
        lex_p++;
    }
    line[n] = '\0';

    /* 只处理 "#define NAME 123" */
    if (strncmp(line, "#define", 7) == 0) {
        const char* p = line + 7;
        char name[64];
        int k = 0;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        while (is_alnum(*p) && k < (int)sizeof(name) - 1) {
            name[k++] = *p++;
        }
        name[k] = '\0';
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (k > 0 && is_digit(*p)) {
            int v = 0;

            while (is_digit(*p)) {
                v = v * 10 + (*p - '0');
                p++;
            }
            add_constant(name, v);
        }
    }
}

/* 查常量表。找到返回 1 并把值写进 *out，没找到返回 0。
 *
 * 第一版是"返回 -1 表示没找到"，那样**负数常量就废了**：enum 里
 * `FAIL = -3` 会让查找结果 -3 被当成"没找到"，于是 FAIL 报
 * "undeclared variable" —— 明明就在常量表里。用输出参数把"有没有"
 * 和"值是多少"分开，负数就不再和哨兵值撞车。 */
static int lookup_define(const char* name, int* out) {
    for (int i = 0; i < define_count; i++) {
        if (strcmp(define_names[i], name) == 0) {
            *out = define_values[i];
            return 1;
        }
    }
    return 0;
}

/* 读下一个 token */
static void lex_next(void) {
    const char* p = lex_p;
    int c;

    for (;;) {
        while (is_space(*p)) {
            if (*p == '\n') {
                src_line++;
            }
            p++;
        }
        /* 注释 */
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') {
                    src_line++;
                }
                p++;
            }
            if (*p) {
                p += 2;
            }
            continue;
        }
        /* # 指令：整行处理掉 */
        if (*p == '#') {
            lex_p = p;
            skip_directive();
            p = lex_p;
            continue;
        }
        break;
    }

    lex_p = p;
    tok.line = src_line;

    c = *p;

    if (c == '\0') {
        tok.kind = TK_EOF;
        tok.text[0] = '\0';
        tok.punct[0] = '\0';
        return;
    }

    /* --- 数字 --- */
    if (is_digit(c)) {
        int v = 0;

        if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            while (is_digit(*p) || (*p >= 'a' && *p <= 'f') ||
                   (*p >= 'A' && *p <= 'F')) {
                int d = is_digit(*p) ? (*p - '0')
                                     : ((*p | 0x20) - 'a' + 10);
                v = v * 16 + d;
                p++;
            }
        } else {
            while (is_digit(*p)) {
                v = v * 10 + (*p - '0');
                p++;
            }
        }
        /* 允许 10u / 10L 这类后缀，直接忽略 */
        while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') {
            p++;
        }
        lex_p = p;
        tok.kind = TK_NUM;
        tok.num = v;
        return;
    }

    /* --- 字符串 --- */
    if (c == '"') {
        int n = 0;

        p++;
        while (*p && *p != '"') {
            int ch = *p++;

            if (ch == '\\' && *p) {
                int e = *p++;

                switch (e) {
                    case 'n':  ch = '\n'; break;
                    case 't':  ch = '\t'; break;
                    case 'r':  ch = '\r'; break;
                    case '0':  ch = '\0'; break;
                    case '\\': ch = '\\'; break;
                    case '"':  ch = '"';  break;
                    case 'x': {
                        int v = 0;
                        int k = 0;

                        while (k < 2 && ((*p >= '0' && *p <= '9') ||
                                         (*p >= 'a' && *p <= 'f') ||
                                         (*p >= 'A' && *p <= 'F'))) {
                            v = v * 16 + (is_digit(*p) ? (*p - '0')
                                                       : ((*p | 0x20) - 'a' + 10));
                            p++;
                            k++;
                        }
                        ch = v;
                        break;
                    }
                    default:   ch = e; break;
                }
            }
            if (n < (int)sizeof(tok.text) - 1) {
                tok.text[n++] = (char)ch;
            }
        }
        if (*p == '"') {
            p++;
        }
        tok.text[n] = '\0';
        tok.num = n;                  /* 记录真实长度（内容里可能有 \0） */
        lex_p = p;
        tok.kind = TK_STR;
        return;
    }

    /* --- 字符字面量 --- */
    if (c == '\'') {
        int ch;

        p++;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n':  ch = '\n'; break;
                case 't':  ch = '\t'; break;
                case 'r':  ch = '\r'; break;
                case '0':  ch = '\0'; break;
                case '\\': ch = '\\'; break;
                case '\'': ch = '\''; break;
                default:   ch = *p;   break;
            }
            p++;
        } else {
            ch = *p ? *p++ : 0;
        }
        if (*p == '\'') {
            p++;
        }
        lex_p = p;
        tok.kind = TK_CHARLIT;
        tok.num = ch;
        return;
    }

    /* --- 标识符 / 关键字（关键字由语法分析处理） --- */
    if (is_alpha(c)) {
        int n = 0;

        while (is_alnum(*p) && n < (int)sizeof(tok.text) - 1) {
            tok.text[n++] = *p++;
        }
        tok.text[n] = '\0';
        lex_p = p;
        tok.kind = TK_IDENT;
        return;
    }

    /* --- 运算符：贪心地匹配最长的 --- */
    {
        static const char* three[] = { "<<=", ">>=", "...", NULL };
        static const char* two[] = {
            "==", "!=", "<=", ">=", "&&", "||", "++", "--",
            "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "->",
            NULL
        };

        for (int i = 0; three[i]; i++) {
            if (strncmp(p, three[i], 3) == 0) {
                copy_str_local(tok.punct, three[i], 4);
                tok.kind = TK_PUNCT;
                lex_p = p + 3;
                return;
            }
        }
        for (int i = 0; two[i]; i++) {
            if (p[0] == two[i][0] && p[1] == two[i][1]) {
                copy_str_local(tok.punct, two[i], 4);
                tok.kind = TK_PUNCT;
                lex_p = p + 2;
                return;
            }
        }
        tok.punct[0] = (char)c;
        tok.punct[1] = '\0';
        tok.kind = TK_PUNCT;
        lex_p = p + 1;
    }
}

/* ---- 语法分析用的小工具 ---- */

static int is_punct(const char* s) {
    return tok.kind == TK_PUNCT && strcmp(tok.punct, s) == 0;
}

static int is_kw(const char* s) {
    return tok.kind == TK_IDENT && strcmp(tok.text, s) == 0;
}

static void expect_punct(const char* s) {
    if (!is_punct(s)) {
        fatal("expected '%s' but found '%s'", s,
              tok.kind == TK_EOF ? "end of file" : tok.punct[0] ? tok.punct : tok.text);
    }
    lex_next();
}

static int accept_punct(const char* s) {
    if (is_punct(s)) {
        lex_next();
        return 1;
    }
    return 0;
}

static int accept_kw(const char* s) {
    if (is_kw(s)) {
        lex_next();
        return 1;
    }
    return 0;
}

static void expect_ident(char* out, int max) {
    if (tok.kind != TK_IDENT) {
        fatal("expected an identifier");
    }
    copy_str_local(out, tok.text, max);
    lex_next();
}

/* 变量的地址：局部变量用 [ebp-disp]，全局变量用绝对地址 */
#define SYM_LOCAL  1
#define SYM_GLOBAL 2
#define SYM_PARAM  3

struct sym {
    char name[64];
    int  kind;          /* SYM_* */
    int  offset;        /* 局部/参数：相对 ebp；全局：数据区偏移 */
    int  type;          /* TY_* */
    int  base;          /* 元素类型（指针/数组用） */
    int  ptr;           /* 指针层数 */
    int  nelem;         /* 数组元素个数（0 = 不是数组） */
    int  size;          /* 占多少字节 */
    int  func_index;    /* SYM_FUNC 用：函数下标 */
    int  is_bss;        /* 全局：在 bss 区吗 */
    int  defined;       /* 全局变量/函数是否已经定义 */
    int  is_struct;     /* 是结构体类型吗 */
    int  sidx;          /* is_struct 时：结构体下标 */
};

/* ---- 类型 ---- */
#define TY_VOID 0
#define TY_CHAR 1
#define TY_INT  2

/* 变量的地址 -> eax（ND_DECL 手上有的是符号，没有节点，所以单独一个入口） */
static void gen_addr_var(struct sym* s);

/* ============================================================
 * 3. 缓冲分配器与语法树
 *
 * 编译器只跑一次就退出，所以不做 free：所有节点都从一块大缓冲区里
 * 顺序切出来。少一个内存管理 bug，也少一个排查方向。
 * ============================================================ */

static char* arena;
static int arena_used;
static int arena_size;

static void* nalloc(int n) {
    char* p;

    n = (n + 7) & ~7;
    if (arena_used + n > arena_size) {
        fatal("out of compiler memory (needed %d bytes, have %d)",
              arena_used + n, arena_size);
    }
    p = arena + arena_used;
    arena_used += n;
    memset(p, 0, (size_t)n);
    return p;
}

typedef struct {
    int base;        /* TY_VOID / TY_CHAR / TY_INT */
    int ptr;         /* 指针层数 */
    int dims[4];     /* 数组各维长度 */
    int ndims;
    int is_struct;   /* 是结构体吗（0 表示不是 —— 这样 memset 出来的默认值就对了） */
    int sidx;        /* is_struct 时：结构体在结构体表里的下标 */
} vtype_t;

/* ---- 结构体表 ----
 *
 * 字段偏移在定义时就算好（每个字段 4 字节对齐，整体大小也向上取整到 4）。
 * 之所以不做紧凑排列：这个编译器只处理 int / char / 指针，
 * 对齐到 4 之后所有字段的访存都是对齐的，反汇编出来也好读。
 */
#define MAX_STRUCTS 32
#define MAX_FIELDS  16

typedef struct {
    char    name[32];
    int     nfields;
    char    fname[MAX_FIELDS][24];
    vtype_t ftype[MAX_FIELDS];
    int     offset[MAX_FIELDS];
    int     size;
} struct_def_t;

static struct_def_t structs[MAX_STRUCTS];
static int struct_n;

static int find_struct(const char* name) {
    for (int i = 0; i < struct_n; i++) {
        if (strcmp(structs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* 前向声明：`struct node;`。
 *
 * 登记一个还没有字段的结构体名（大小 0）。这样互相引用的两个结构体可以写：
 *     struct b;                 // 先用名字
 *     struct a { struct b* p; };
 *     struct b { struct a* q; };
 * 但"按值"使用一个还没定义完的结构体要在 declare_symbol 里报错 ——
 * 大小是 0，静默放过去会生成错代码。
 */
static void declare_struct_forward(const char* name) {
    if (find_struct(name) >= 0) {
        return;                    /* 已经声明过或者已经定义过 */
    }
    if (struct_n >= MAX_STRUCTS) {
        fatal("too many struct types (limit %d)", MAX_STRUCTS);
    }
    memset(&structs[struct_n], 0, sizeof(structs[struct_n]));
    copy_str_local(structs[struct_n].name, name, sizeof(structs[0].name));
    struct_n++;
}

static int base_size(int base) {
    if (base == TY_CHAR) {
        return 1;
    }
    if (base == TY_VOID) {
        return 1;                 /* void* 的算术按 1 字节走，和 GCC 一样 */
    }
    return 4;
}

/* ---- typedef 表 ----
 *
 * 名字 -> 类型。只有出现在"类型该出现的位置"时才当类型看（和 C 一样），
 * 所以这张表只被 parse_type / is_type_kw 查，不会和变量名、函数名打架。
 * 这也意味着 typedef 必须先声明后使用 —— 在单文件编译器里这是可以接受的。
 */
#define MAX_TYPEDEFS 32

typedef struct {
    char    name[32];
    vtype_t type;
} typedef_def_t;

static typedef_def_t typedefs[MAX_TYPEDEFS];
static int typedef_n;
static int anon_struct_n;   /* 匿名结构体的编号（typedef struct { ... }） */

static int find_typedef(const char* name) {
    for (int i = 0; i < typedef_n; i++) {
        if (strcmp(typedefs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void add_typedef(const char* name, vtype_t t) {
    int ti = find_typedef(name);

    if (ti >= 0) {
        fatal("'%s' is already a typedef", name);
    }
    if (typedef_n >= MAX_TYPEDEFS) {
        fatal("too many typedefs (limit %d)", MAX_TYPEDEFS);
    }
    copy_str_local(typedefs[typedef_n].name, name,
                   sizeof(typedefs[typedef_n].name));
    typedefs[typedef_n].type = t;
    typedef_n++;
}

static int type_size(vtype_t t) {
    int n;

    if (t.ptr > 0) {
        return 4;
    }
    if (t.is_struct) {
        if (t.sidx < 0 || t.sidx >= struct_n) {
            fatal("internal: bad struct index %d", t.sidx);
        }
        /* 前向声明过的结构体只有名字、没有大小。原来这里默默返回 0：
         * `sizeof(struct fwd)` 得到 0，按它算出来的数组和栈槽全是 0 字节，
         * 写进去就踩到别的东西。C 里这就是"类型不完整"，必须报错。 */
        if (structs[t.sidx].nfields == 0) {
            fatal("struct '%s' is incomplete here (only forward-declared); "
                  "its size is unknown", structs[t.sidx].name);
        }
        n = structs[t.sidx].size;
    } else {
        n = base_size(t.base);
    }
    for (int i = 0; i < t.ndims; i++) {
        n *= t.dims[i];
    }
    return n;
}

/* 结构体的某个字段 */
static int field_offset(vtype_t t, const char* name) {
    const struct_def_t* d;

    if (!t.is_struct) {
        return -1;
    }
    d = &structs[t.sidx];
    for (int i = 0; i < d->nfields; i++) {
        if (strcmp(d->fname[i], name) == 0) {
            return d->offset[i];
        }
    }
    return -1;
}

static vtype_t field_type(vtype_t t, const char* name) {
    const struct_def_t* d = &structs[t.sidx];

    for (int i = 0; i < d->nfields; i++) {
        if (strcmp(d->fname[i], name) == 0) {
            return d->ftype[i];
        }
    }
    fatal("no member named '%s'", name);
    return t;
}

/* 去掉一层：指针 -> 下一层指针或基类型；数组 -> 少一维 */
static vtype_t type_element(vtype_t t) {
    if (t.ptr > 0) {
        t.ptr--;
        return t;
    }
    if (t.ndims > 0) {
        for (int i = 1; i < t.ndims; i++) {
            t.dims[i - 1] = t.dims[i];
        }
        t.ndims--;
        return t;
    }
    fatal("cannot take an element of this type");
    return t;
}

static int type_is_ptr_like(vtype_t t) {
    return t.ptr > 0 || t.ndims > 0;
}

static vtype_t type_int(void) {
    vtype_t t;

    memset(&t, 0, sizeof(t));
    t.base = TY_INT;
    return t;
}

static vtype_t type_char(void) {
    vtype_t t;

    memset(&t, 0, sizeof(t));
    t.base = TY_CHAR;
    return t;
}

static vtype_t type_ptr_to(vtype_t t) {
    vtype_t r = t;

    if (r.ndims > 0) {
        /* int a[10] 退化成的指针：指向元素 */
        r = type_element(r);
    }
    r.ptr++;
    return r;
}

/* ---- 节点 ---- */
typedef enum {
    ND_NUM, ND_STR, ND_VAR, ND_BIN, ND_ASSIGN, ND_CALL, ND_DEREF, ND_ADDR,
    ND_MEMBER, ND_INITLIST,
    ND_COND, ND_CAST, ND_NEG, ND_BITNOT, ND_LOGNOT, ND_PREINC, ND_PREDEC,
    ND_POSTINC, ND_POSTDEC,
    ND_BLOCK, ND_IF, ND_WHILE, ND_FOR, ND_DOWHILE, ND_RETURN, ND_BREAK,
    ND_CONTINUE, ND_DECL, ND_EXPRSTMT, ND_EMPTY,
    ND_SWITCH, ND_CASE, ND_DEFAULT
} nd_kind;

struct node;

typedef struct node {
    int     kind;
    int     op;            /* ND_BIN/ND_ASSIGN：运算符的 token 字符串下标 */
    char    opstr[4];
    int     num;           /* ND_NUM 的值 */
    int     data_off;      /* ND_STR：数据区偏移（代码生成时分配） */
    int     str_len;
    char    str[256];
    int     data_ready;    /* ND_STR：是否已经在数据区里放过一份 */
    char    name[64];      /* ND_VAR / ND_CALL；ND_MEMBER：字段名 */
    struct sym* sym;       /* ND_VAR / ND_DECL：变量；ND_CALL：函数用 func_index */
    int     func_index;
    int     field_off;     /* ND_MEMBER：字段在结构体里的偏移 */
    int     via_ptr;       /* ND_MEMBER：是 p->f（1）还是 a.f（0） */
    int     label;         /* ND_CASE / ND_DEFAULT：这个分支的代码标签 */
    struct node* a;
    struct node* b;
    struct node* c;
    struct node* d;
    struct node* next;     /* 语句链、参数链 */
    vtype_t ty;
} node_t;

/* 全局符号：函数 + 全局变量 */
#define MAX_FUNCS 64

struct sym_func {
    char name[64];
    vtype_t ret;
    int  nparams;
    char pname[8][32];
    vtype_t ptype[8];
    node_t* body;          /* NULL = 只有声明 */
    int  label;            /* 代码标签（延迟分配） */
    int  defined;
    int  is_builtin;       /* 内建函数：编译器直接发射系统调用，没有函数体 */
    int  builtin_nr;       /* 内建函数对应的系统调用号；-1 表示特殊处理 */
    int  builtin_mode;     /* 0 普通，1 = putchar 那种特殊形式，2 = 之后要 hlt */
    int  is_varargs;       /* 参数表里有 "..."：调用时不再核对参数个数 */
    struct sym* param_sym[8];  /* 形参在局部表里的符号（代码生成时给它们定偏移） */
};

static struct sym  globals[MAX_SYMS];
static int         global_n;
static struct sym_func funcs[MAX_FUNCS];
static int         func_n;

/* 当前函数的局部变量。
 *
 * 这里存的是**指针**，符号本身从 arena 里分配：locals 这张表每个函数
 * 都会被重置，但语法树上引用的是符号本体，如果符号本体也放在这张表里，
 * 解析下一个函数时就会把上一个函数的局部变量覆盖掉，代码生成阶段
 * 拿到的偏移全是错的。
 */
static struct sym* local_tab[MAX_LOCALS];
static int         local_n;

/* 全局变量声明（按出现顺序，代码生成时用来排数据区） */
static node_t* global_decls[MAX_SYMS];
static int     global_decl_n;

/* 正在解析全局作用域吗（parse_declaration 靠它区分全局/局部） */
static int     parsing_global = 1;

static struct sym* find_global(const char* name) {
    for (int i = 0; i < global_n; i++) {
        if (strcmp(globals[i].name, name) == 0) {
            return &globals[i];
        }
    }
    return NULL;
}

static struct sym* find_local(const char* name) {
    for (int i = 0; i < local_n; i++) {
        if (strcmp(local_tab[i]->name, name) == 0) {
            return local_tab[i];
        }
    }
    return NULL;
}

static struct sym* find_var(const char* name) {
    struct sym* s = find_local(name);

    return s ? s : find_global(name);
}

static int find_func(const char* name) {
    for (int i = 0; i < func_n; i++) {
        if (strcmp(funcs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static node_t* node_new(int kind) {
    node_t* n = (node_t*)nalloc(sizeof(node_t));

    n->kind = kind;
    n->ty = type_int();
    return n;
}

/* ============================================================
 * 4. 语法分析
 *
 * 递归下降，一个优先级一个函数，和 C 的运算符优先级表一一对应。
 * ============================================================ */

static node_t* parse_expr(void);
static node_t* parse_assign(void);
static node_t* parse_unary(void);
static node_t* parse_postfix(void);
static node_t* parse_stmt(void);
static node_t* parse_block(void);
static vtype_t parse_type(void);
static node_t* parse_declaration(int eat_semi);
static void parse_array_dims(vtype_t* t);
static void parse_struct_def(const char* name, int eat_semi);
static void parse_enum_def(const char* name);
static int  parse_case_const(void);

static int is_type_kw(void) {
    /* typedef 名也算类型名 —— 语法上"这个位置能出现类型吗"是靠它判断的，
     * 所以 `MyInt x;` 才会被当成声明而不是表达式语句。 */
    if (tok.kind == TK_IDENT && find_typedef(tok.text) >= 0) {
        return 1;
    }
    return is_kw("int") || is_kw("char") || is_kw("void") || is_kw("struct") ||
           is_kw("enum") ||
           is_kw("static") || is_kw("const") || is_kw("unsigned") ||
           is_kw("signed") || is_kw("long") || is_kw("short");
}

/* 这些词一律接受、但基本忽略。
 *
 * 为什么接受：真实 C 代码里到处是 `static int x;`、`unsigned char c;`、
 * `long n;`，不接受的话一大半现成代码根本进不了编译器。
 * 为什么忽略：这个编译器只有 32 位 int 和 8 位 char，没有真正的位宽区分，
 * 也没有跨文件链接（所以 static 在这里没有额外含义），更没有 const 检查。
 * 接受它们是为了让代码能编译，不是假装支持 —— 这一点写在 README 里。
 */
static int is_qualifier_kw(void) {
    return is_kw("static") || is_kw("const") || is_kw("unsigned") ||
           is_kw("signed") || is_kw("long") || is_kw("short");
}

static vtype_t parse_type(void) {
    vtype_t t;
    int saw_qualifier = 0;

    memset(&t, 0, sizeof(t));
    while (is_qualifier_kw()) {
        saw_qualifier = 1;
        lex_next();
    }

    if (accept_kw("int")) {
        t.base = TY_INT;
    } else if (accept_kw("char")) {
        t.base = TY_CHAR;
    } else if (accept_kw("void")) {
        t.base = TY_VOID;
    } else if (is_kw("struct")) {
        char sname[32];
        int si;

        lex_next();
        expect_ident(sname, sizeof(sname));
        si = find_struct(sname);
        if (si < 0) {
            /* 结构体必须先定义再用：没有前向声明，也不做"用到了再补"的推断。
             * 报错比默默当成 int 好得多。 */
            fatal("unknown struct '%s' (define it before using it)", sname);
        }
        t.is_struct = 1;
        t.sidx = si;
    } else if (is_kw("enum")) {
        /* `enum Color c;` —— 枚举类型的变量就是 int（枚举名只是常量）。 */
        char ename[32];

        lex_next();
        expect_ident(ename, sizeof(ename));
        t.base = TY_INT;
    } else if (tok.kind == TK_IDENT && find_typedef(tok.text) >= 0) {
        /* typedef 出来的名字 */
        t = typedefs[find_typedef(tok.text)].type;
        lex_next();
    } else if (saw_qualifier) {
        t.base = TY_INT;               /* `unsigned x;` 这种省略写法 */
    } else {
        fatal("expected a type (int / char / void / struct)");
    }
    while (accept_punct("*")) {
        t.ptr++;
    }
    return t;
}

/* struct T { ... }; —— 调用方已经把 "struct T" 和 '{' 都看过了。
 *
 * eat_semi 是给 typedef 用的：`typedef struct { ... } Pair;` 里 `}` 后面
 * 直接就是别名，没有分号（分号在别名后面）。原来这里写死 expect_punct(";")，
 * 于是匿名结构体 typedef 报 "expected ';' but found '}'"，指到结构体
 * 结尾那一行上，完全看不出是 typedef 的事。 */
static void parse_struct_def(const char* name, int eat_semi)
{
    int si = find_struct(name);
    struct_def_t* d;
    int off = 0;

    if (si < 0) {
        if (struct_n >= MAX_STRUCTS) {
            fatal("too many struct types (limit %d)", MAX_STRUCTS);
        }
        si = struct_n++;
        memset(&structs[si], 0, sizeof(structs[si]));
        copy_str_local(structs[si].name, name, sizeof(structs[si].name));
    } else if (structs[si].nfields > 0) {
        fatal("struct '%s' is already defined", name);
    }
    d = &structs[si];

    /* 这里已经把结构体登记进表里了，所以 "struct T* next;" 这种自引用
     * 指针字段能解析 —— 指针大小是 4，不需要知道结构体本身多大。 */
    lex_next();                        /* 吃掉 '{' */

    while (!is_punct("}")) {
        vtype_t base = parse_type();

        if (tok.kind == TK_EOF) {
            fatal("unexpected end of file inside struct '%s'", name);
        }

        for (;;) {
            char fn[24];
            vtype_t ft = base;

            expect_ident(fn, sizeof(fn));
            if (is_punct("[")) {
                parse_array_dims(&ft);
            }
            if (d->nfields >= MAX_FIELDS) {
                fatal("struct '%s' has too many fields (limit %d)",
                      name, MAX_FIELDS);
            }
            if (ft.is_struct && ft.ptr == 0 && ft.sidx == si) {
                fatal("struct '%s' cannot contain itself (use a pointer)", name);
            }

            while ((off % 4) != 0) {   /* 每个字段 4 字节对齐 */
                off++;
            }
            copy_str_local(d->fname[d->nfields], fn, sizeof(d->fname[0]));
            d->ftype[d->nfields]  = ft;
            d->offset[d->nfields] = off;
            off += type_size(ft);
            d->nfields++;

            if (!accept_punct(",")) {
                break;
            }
        }
        expect_punct(";");
    }
    lex_next();                        /* 吃掉 '}' */
    if (eat_semi) {
        expect_punct(";");
    }

    while ((off % 4) != 0) {           /* 整体大小也向上取整到 4 */
        off++;
    }
    d->size = off;
}

/* enum Name { A, B = 5, C }; —— 调用方已经把 "enum Name" 和 '{' 都看过了。
 *
 * 枚举名当成常量登记进 #define 那张表（见 add_constant），所以代码生成
 * 阶段完全看不到枚举：`if (c == RED)` 里的 RED 在解析时就被换成 0。
 * 不给枚举建类型表，是因为这个子集里枚举只用来起名字。 */
static void parse_enum_def(const char* name)
{
    int value = 0;

    lex_next();                        /* 吃掉 '{' */

    while (!is_punct("}")) {
        char ename[64];
        int  neg = 0;

        expect_ident(ename, sizeof(ename));

        if (accept_punct("=")) {
            if (accept_punct("-")) {
                neg = 1;
            }
            if (tok.kind != TK_NUM) {
                fatal("enum '%s': the value of '%s' must be a number",
                      name, ename);
            }
            value = (int)tok.num;
            if (neg) {
                value = -value;
            }
            lex_next();
        }

        add_constant(ename, value);
        value = value + 1;

        if (!accept_punct(",")) {
            break;
        }
    }
    expect_punct("}");
    expect_punct(";");
}

/* 二元：都从子层拿左值，遇到对应的运算符就继续往上套 */
static node_t* bin_node(node_t* lhs, node_t* rhs, const char* op, vtype_t ty) {
    node_t* n = node_new(ND_BIN);

    n->a = lhs;
    n->b = rhs;
    copy_str_local(n->opstr, op, sizeof(n->opstr));
    n->ty = ty;
    return n;
}

static node_t* parse_multiplicative(void) {
    node_t* n = parse_unary();

    for (;;) {
        if (is_punct("*") || is_punct("/") || is_punct("%")) {
            char op[4];
            node_t* rhs;

            copy_str_local(op, tok.punct, sizeof(op));
            lex_next();
            rhs = parse_unary();
            n = bin_node(n, rhs, op, type_int());
        } else {
            return n;
        }
    }
}

/* 指针 + 整数要按元素大小缩放：p+1 前进一个元素，不是一字节。
 * 这一步在语法分析里就把缩放节点插进去，代码生成只认缩放后的整数。 */
static node_t* scale_pointer_operand(node_t* n, vtype_t ptr_ty) {
    node_t* scale = node_new(ND_NUM);
    node_t* mul;

    scale->num = type_size(type_element(ptr_ty));
    if (scale->num == 1) {
        return n;
    }
    mul = bin_node(n, scale, "*", type_int());
    return mul;
}

static node_t* parse_additive(void) {
    node_t* n = parse_multiplicative();

    for (;;) {
        if (is_punct("+") || is_punct("-")) {
            char op[4];
            node_t* rhs;
            vtype_t rty;

            copy_str_local(op, tok.punct, sizeof(op));
            lex_next();
            rhs = parse_multiplicative();

            if (type_is_ptr_like(n->ty) && !type_is_ptr_like(rhs->ty)) {
                rhs = scale_pointer_operand(rhs, n->ty);
                rty = n->ty;
            } else if (type_is_ptr_like(rhs->ty) && !type_is_ptr_like(n->ty) &&
                       op[0] == '+') {
                rhs = scale_pointer_operand(n, rhs->ty);
                rty = rhs->ty;
            } else if (op[0] == '-' && type_is_ptr_like(n->ty) &&
                       type_is_ptr_like(rhs->ty)) {
                /* 指针相减：先算字节差，再除以元素大小 */
                int esize = type_size(type_element(n->ty));

                n = bin_node(n, rhs, "-", type_int());
                if (esize > 1) {
                    node_t* d = node_new(ND_NUM);

                    d->num = esize;
                    n = bin_node(n, d, "/", type_int());
                }
                continue;
            } else {
                rty = type_int();
            }
            n = bin_node(n, rhs, op, rty);
        } else {
            return n;
        }
    }
}

static node_t* parse_shift(void) {
    node_t* n = parse_additive();

    while (is_punct("<<") || is_punct(">>")) {
        char op[4];
        node_t* rhs;

        copy_str_local(op, tok.punct, sizeof(op));
        lex_next();
        rhs = parse_additive();
        n = bin_node(n, rhs, op, type_int());
    }
    return n;
}

static node_t* parse_relational(void) {
    node_t* n = parse_shift();

    while (is_punct("<") || is_punct(">") || is_punct("<=") || is_punct(">=")) {
        char op[4];
        node_t* rhs;

        copy_str_local(op, tok.punct, sizeof(op));
        lex_next();
        rhs = parse_shift();
        n = bin_node(n, rhs, op, type_int());
    }
    return n;
}

static node_t* parse_equality(void) {
    node_t* n = parse_relational();

    while (is_punct("==") || is_punct("!=")) {
        char op[4];
        node_t* rhs;

        copy_str_local(op, tok.punct, sizeof(op));
        lex_next();
        rhs = parse_relational();
        n = bin_node(n, rhs, op, type_int());
    }
    return n;
}

static node_t* parse_bitand(void) {
    node_t* n = parse_equality();

    while (is_punct("&")) {
        node_t* rhs;

        lex_next();
        rhs = parse_equality();
        n = bin_node(n, rhs, "&", type_int());
    }
    return n;
}

static node_t* parse_bitxor(void) {
    node_t* n = parse_bitand();

    while (is_punct("^")) {
        node_t* rhs;

        lex_next();
        rhs = parse_bitand();
        n = bin_node(n, rhs, "^", type_int());
    }
    return n;
}

static node_t* parse_bitor(void) {
    node_t* n = parse_bitxor();

    while (is_punct("|")) {
        node_t* rhs;

        lex_next();
        rhs = parse_bitxor();
        n = bin_node(n, rhs, "|", type_int());
    }
    return n;
}

static node_t* parse_logand(void) {
    node_t* n = parse_bitor();

    while (is_punct("&&")) {
        node_t* rhs;

        lex_next();
        rhs = parse_bitor();
        n = bin_node(n, rhs, "&&", type_int());
    }
    return n;
}

static node_t* parse_logor(void) {
    node_t* n = parse_logand();

    while (is_punct("||")) {
        node_t* rhs;

        lex_next();
        rhs = parse_logand();
        n = bin_node(n, rhs, "||", type_int());
    }
    return n;
}

static node_t* parse_conditional(void) {
    node_t* cond = parse_logor();

    if (is_punct("?")) {
        node_t* n = node_new(ND_COND);

        lex_next();
        n->a = cond;
        n->b = parse_expr();
        expect_punct(":");
        n->c = parse_conditional();
        n->ty = n->b->ty;
        return n;
    }
    return cond;
}

/* 赋值是右结合的：a = b = c 要解析成 a = (b = c) */
static node_t* parse_assign(void) {
    node_t* lhs = parse_conditional();
    static const char* ops[] = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", NULL
    };

    if (tok.kind == TK_PUNCT) {
        for (int i = 0; ops[i]; i++) {
            if (strcmp(tok.punct, ops[i]) == 0) {
                node_t* n = node_new(ND_ASSIGN);
                char op[4];

                copy_str_local(op, tok.punct, sizeof(op));
                lex_next();
                n->a = lhs;
                n->b = parse_assign();
                copy_str_local(n->opstr, op, sizeof(n->opstr));
                n->ty = lhs->ty;

                /* 整个结构体赋值要生成一块 memcpy，这个编译器不做：
                 * 明确拒绝，比生成半截拷贝安全。逐字段抄或者用指针。 */
                if (lhs->ty.is_struct && lhs->ty.ptr == 0) {
                    fatal("assigning whole structs is not supported "
                          "(assign the members, or use a pointer)");
                }
                return n;
            }
        }
    }
    return lhs;
}

static node_t* parse_expr(void) {
    node_t* n = parse_assign();

    while (is_punct(",")) {
        node_t* n2 = node_new(ND_BIN);
        node_t* rhs;

        lex_next();
        rhs = parse_assign();
        /* 逗号表达式的值取右边，左边只为了副作用 */
        n2->a = n;
        n2->b = rhs;
        copy_str_local(n2->opstr, ",", sizeof(n2->opstr));
        n2->ty = rhs->ty;
        n = n2;
    }
    return n;
}

/* 函数调用的实参表 */
static node_t* parse_args(void) {
    node_t* head = NULL;
    node_t* tail = NULL;

    if (is_punct(")")) {
        return NULL;
    }
    for (;;) {
        node_t* a = parse_assign();

        if (tail) {
            tail->next = a;
        } else {
            head = a;
        }
        tail = a;
        if (!accept_punct(",")) {
            break;
        }
    }
    return head;
}

static int count_args(node_t* a) {
    int n = 0;

    while (a) {
        n++;
        a = a->next;
    }
    return n;
}

static node_t* parse_unary(void) {
    if (accept_punct("-")) {
        node_t* n = node_new(ND_NEG);

        n->a = parse_unary();
        n->ty = n->a->ty;
        return n;
    }
    if (accept_punct("+")) {
        return parse_unary();
    }
    if (accept_punct("!")) {
        node_t* n = node_new(ND_LOGNOT);

        n->a = parse_unary();
        return n;
    }
    if (accept_punct("~")) {
        node_t* n = node_new(ND_BITNOT);

        n->a = parse_unary();
        return n;
    }
    if (accept_punct("&")) {
        node_t* n = node_new(ND_ADDR);

        n->a = parse_unary();
        n->ty = type_ptr_to(n->a->ty);
        return n;
    }
    if (accept_punct("*")) {
        node_t* n = node_new(ND_DEREF);

        n->a = parse_unary();
        if (!type_is_ptr_like(n->a->ty)) {
            fatal("cannot dereference a value that is not a pointer");
        }
        n->ty = type_element(n->a->ty);
        return n;
    }
    if (accept_punct("++")) {
        node_t* n = node_new(ND_PREINC);

        n->a = parse_unary();
        n->ty = n->a->ty;
        return n;
    }
    if (accept_punct("--")) {
        node_t* n = node_new(ND_PREDEC);

        n->a = parse_unary();
        n->ty = n->a->ty;
        return n;
    }
    if (accept_kw("sizeof")) {
        node_t* n = node_new(ND_NUM);

        if (is_punct("(") && (tok.kind == TK_PUNCT)) {
            /* sizeof(type) 还是 sizeof(expr)？看括号里第一个词是不是类型 */
            const char* save_p = lex_p;
            token_t save_tok = tok;
            int save_line = src_line;

            lex_next();
            if (is_type_kw()) {
                vtype_t t = parse_type();

                while (accept_punct("*")) {
                    t.ptr++;
                }
                expect_punct(")");
                n->num = type_size(t);
                return n;
            }
            /* 不是类型：把 token 流倒回去，按表达式处理 */
            lex_p = save_p;
            tok = save_tok;
            src_line = save_line;
        }
        {
            node_t* e = parse_unary();

            n->num = type_size(e->ty);
            return n;
        }
    }
    /* 强制类型转换：(int)x / (char*)p —— 只有括号里是类型名才算转换，
     * 因为类型关键字就 int/char/void 三个，不会有歧义 */
    if (is_punct("(")) {
        const char* save_p = lex_p;
        token_t save_tok = tok;
        int save_line = src_line;

        lex_next();
        if (is_type_kw()) {
            vtype_t t = parse_type();
            node_t* n;

            expect_punct(")");
            n = node_new(ND_CAST);
            n->a = parse_unary();
            n->ty = t;
            return n;
        }
        lex_p = save_p;
        tok = save_tok;
        src_line = save_line;
    }
    return parse_postfix();
}

static node_t* parse_primary(void) {
    /* 数字 */
    if (tok.kind == TK_NUM) {
        node_t* n = node_new(ND_NUM);

        n->num = tok.num;
        n->ty = type_int();
        lex_next();
        return n;
    }
    /* 字符字面量：当 int 用 */
    if (tok.kind == TK_CHARLIT) {
        node_t* n = node_new(ND_NUM);

        n->num = tok.num;
        n->ty = type_int();
        lex_next();
        return n;
    }
    /* 字符串字面量：类型是 char*，内容进数据区 */
    if (tok.kind == TK_STR) {
        node_t* n = node_new(ND_STR);

        n->str_len = tok.num;
        memcpy(n->str, tok.text, (size_t)tok.num);
        n->str[tok.num] = '\0';
        n->ty = type_char();
        n->ty.ptr = 1;
        lex_next();
        return n;
    }
    /* 标识符 */
    if (tok.kind == TK_IDENT) {
        char name[64];
        int cval = 0;
        int is_const;

        copy_str_local(name, tok.text, sizeof(name));

        is_const = lookup_define(name, &cval);
        lex_next();

        /* 宏常量 / 枚举名（值可以是负数，见 lookup_define 的注释） */
        if (is_const && !is_punct("(")) {
            node_t* n = node_new(ND_NUM);

            n->num = cval;
            return n;
        }

        if (!is_punct("(")) {
            struct sym* s = find_var(name);
            node_t* n = node_new(ND_VAR);

            if (!s) {
                fatal("undeclared variable '%s'", name);
            }
            copy_str_local(n->name, name, sizeof(n->name));
            n->sym = s;
            n->ty = s->type == TY_CHAR ? type_char() : type_int();
            n->ty.ptr = s->ptr;
            n->ty.base = s->base;
            n->ty.ndims = 0;
            /* 结构体信息也要带上：漏了它，"struct point* p" 里的 p 在表达式里
             * 就变成"不是结构体"，p->x 会报 "'->' needs a pointer to a struct"
             * —— 明明声明的类型是对的。符号表里存了，这里必须一起搬过来。 */
            n->ty.is_struct = s->is_struct;
            n->ty.sidx = s->sidx;
            if (s->nelem > 0) {
                n->ty.ndims = 1;
                n->ty.dims[0] = s->nelem;
            }
            return n;
        }

        /* 函数调用 */
        {
            node_t* n = node_new(ND_CALL);
            int fi = find_func(name);

            copy_str_local(n->name, name, sizeof(n->name));
            n->func_index = fi;
            lex_next();               /* 吃掉 '(' */
            n->a = parse_args();
            expect_punct(")");

            if (fi >= 0) {
                n->ty = funcs[fi].ret;
                if (funcs[fi].is_varargs) {
                    /* 变参函数只要求"至少给够固定参数" */
                    if (count_args(n->a) < funcs[fi].nparams) {
                        fatal("'%s' needs at least %d argument(s) but got %d",
                              name, funcs[fi].nparams, count_args(n->a));
                    }
                } else if (count_args(n->a) != funcs[fi].nparams) {
                    fatal("'%s' expects %d argument(s) but got %d", name,
                          funcs[fi].nparams, count_args(n->a));
                }
            } else {
                /* 内建函数在代码生成时才查表；这里先给 int 返回类型，
                 * 真正用到返回值的场合由 builtin 表决定 */
                n->ty = type_int();
            }
            return n;
        }
    }

    if (accept_punct("(")) {
        node_t* n = parse_expr();

        expect_punct(")");
        return n;
    }

    fatal("unexpected token in an expression");
    return NULL;
}

static node_t* parse_postfix(void) {
    node_t* n = parse_primary();

    for (;;) {
        if (is_punct("[")) {
            node_t* idx;
            node_t* add;

            lex_next();
            idx = parse_expr();
            expect_punct("]");

            if (!type_is_ptr_like(n->ty)) {
                fatal("subscripted value is not an array or pointer");
            }
            /* a[i] 等价于 *(a + i)，缩放交给指针加法那套规则 */
            add = bin_node(n, scale_pointer_operand(idx, n->ty), "+", n->ty);
            n = node_new(ND_DEREF);
            n->a = add;
            n->ty = type_element(add->ty);
        } else if (is_punct(".") || is_punct("->")) {
            /* 成员访问：a.f 和 p->f。
             *
             * 两者的区别只在"结构体地址从哪来"：a.f 取变量 a 的地址，
             * p->f 用 p 里存的那个地址。所以节点里用 via_ptr 记一下，
             * 代码生成时按需取地址/取值即可。 */
            int via_ptr = (tok.punct[0] == '-');
            char fname[24];
            vtype_t sty;
            node_t* m;
            int off;

            lex_next();
            expect_ident(fname, sizeof(fname));

            sty = n->ty;
            if (via_ptr) {
                if (sty.ptr == 0 || !sty.is_struct) {
                    fatal("'->' needs a pointer to a struct");
                }
                sty.ptr--;                 /* 指向的结构体类型 */
            }
            if (!sty.is_struct || sty.ptr != 0) {
                fatal("'.' needs a struct (got something else)");
            }
            off = field_offset(sty, fname);
            if (off < 0) {
                fatal("struct '%s' has no member named '%s'",
                      structs[sty.sidx].name, fname);
            }

            m = node_new(ND_MEMBER);
            m->a = n;
            m->via_ptr = via_ptr;
            m->field_off = off;
            copy_str_local(m->name, fname, sizeof(m->name));
            m->ty = field_type(sty, fname);
            n = m;
        } else if (is_punct("++")) {
            node_t* p = node_new(ND_POSTINC);

            lex_next();
            p->a = n;
            p->ty = n->ty;
            n = p;
        } else if (is_punct("--")) {
            node_t* p = node_new(ND_POSTDEC);

            lex_next();
            p->a = n;
            p->ty = n->ty;
            n = p;
        } else {
            return n;
        }
    }
}

/* ---- 变量声明 ---- */

/* 把声明记进符号表。scope=SYM_GLOBAL 时进全局表，否则进当前函数。
 * 返回符号指针，offset/是否 bss 由代码生成阶段填。 */
static struct sym* declare_symbol(const char* name, vtype_t t, int kind) {
    struct sym* s;

    if (kind == SYM_GLOBAL) {
        if (find_global(name)) {
            fatal("'%s' is already declared", name);
        }
        if (global_n >= MAX_SYMS) {
            fatal("too many globals (limit %d)", MAX_SYMS);
        }
        s = &globals[global_n++];
    } else {
        if (find_local(name)) {
            fatal("'%s' is already declared in this function", name);
        }
        if (local_n >= MAX_LOCALS) {
            fatal("too many locals in one function (limit %d)", MAX_LOCALS);
        }
        s = (struct sym*)nalloc(sizeof(struct sym));
        local_tab[local_n++] = s;
    }

    memset(s, 0, sizeof(*s));
    copy_str_local(s->name, name, sizeof(s->name));

    /* 按值使用一个只做了前向声明的结构体：大小还是 0，静默放过去会生成
     * 错代码（比如给它分配 0 字节栈空间），所以在这里拦下来。
     * 指向它的指针没问题 —— 指针就是 4 字节，不需要知道结构体多大。 */
    if (t.is_struct && t.ptr == 0 && structs[t.sidx].nfields == 0) {
        fatal("struct '%s' is only declared, not defined yet "
              "(use a pointer, or define it before using it by value)",
              structs[t.sidx].name);
    }

    s->kind   = kind;
    s->base   = t.base;
    s->ptr    = t.ptr;
    s->nelem  = (t.ndims > 0) ? t.dims[0] : 0;
    s->type   = t.base;
    s->size   = type_size(t);
    s->is_struct = t.is_struct;
    s->sidx      = t.sidx;
    return s;
}

/* 解析 "[N]"。N 可以留空（`char s[] = "abc"`）：那时 dims 记 0，
 * 由初始化列表/字符串的个数回填（见 parse_declaration）。
 * 最多支持 4 维。 */
static void parse_array_dims(vtype_t* t) {
    while (is_punct("[")) {
        lex_next();
        if (t->ndims >= 4) {
            fatal("at most 4 array dimensions are supported");
        }
        if (t->ndims == 0 && is_punct("]")) {
            t->dims[t->ndims++] = 0;        /* 留空，等初始化列表补 */
            lex_next();
            continue;
        }
        if (tok.kind != TK_NUM) {
            fatal("array size must be a number (or empty, with an initialiser)");
        }
        t->dims[t->ndims++] = tok.num;
        lex_next();
        expect_punct("]");
    }
}

/* 一条声明：可能是结构体/枚举定义、全局变量、函数原型或函数定义。
 *
 * 变量声明返回一个 ND_DECL 的链（"int a = 1, b;" 是两个节点），
 * 函数原型和函数定义返回 NULL —— 它们不需要可执行代码。 */
static node_t* parse_declaration(int eat_semi) {
    vtype_t base;
    char name[64];
    int is_global = parsing_global;

    /* typedef 定义：`typedef int MyInt;`、`typedef struct node Node;`、
     * `typedef struct node { ... } Node;`。
     * 它只往 typedef 表里登记一个名字，不产生任何代码。 */
    if (is_kw("typedef")) {
        vtype_t tt;

        lex_next();

        /* `typedef struct Name { ... } Alias;` —— 先把结构体定义吃掉。
         *
         * 这里必须自己把别名表列完，不能吃掉定义后再交给 parse_type()：
         * parse_struct_def() 连结尾的 ';' 一起吃了，词法位置已经越过
         * `struct Name`，parse_type() 只会看到别名（一个还不存在的
         * typedef 名），于是报 "expected a type" —— 第一版就是这样，
         * `typedef struct point { int x; } Point;` 根本编译不过。
         *
         * 顺带支持匿名的 `typedef struct { ... } Alias;`：没有名字就造一个
         * 内部名字（$anonN），反正用户代码里也用不到它。 */
        if (is_kw("struct")) {
            const char* save_p    = lex_p;
            token_t     save_tok  = tok;
            int         save_line = src_line;
            char        sname[32];
            int         is_def    = 0;
            int         have_st   = 0;

            lex_next();
            if (tok.kind == TK_IDENT) {
                copy_str_local(sname, tok.text, sizeof(sname));
                lex_next();
                if (is_punct("{")) {
                    is_def = 1;
                }
            } else if (is_punct("{")) {
                /* 匿名结构体：造一个内部名字（这里没有 stdio，手写十进制） */
                char* np = sname;
                int   v  = ++anon_struct_n;

                *np++ = '$';
                *np++ = 'a';
                *np++ = 'n';
                *np++ = 'o';
                *np++ = 'n';
                if (v >= 10) {
                    char digits[12];
                    int  k = 0;

                    while (v > 0) {
                        digits[k++] = (char)('0' + v % 10);
                        v /= 10;
                    }
                    while (k > 0) {
                        *np++ = digits[--k];
                    }
                } else {
                    *np++ = (char)('0' + v);
                }
                *np = '\0';
                is_def = 1;
            }
            if (is_def) {
                vtype_t st;

                parse_struct_def(sname, 0);    /* 结尾的 ';' 不在这里 */
                have_st = 1;

                memset(&st, 0, sizeof(st));
                st.is_struct = 1;
                st.sidx      = find_struct(sname);
                if (st.sidx < 0) {
                    fatal("internal: struct '%s' just defined is missing", sname);
                }

                if (accept_punct(";")) {
                    /* `typedef struct Name { ... };` —— 其实没有别名，
                     * 等价于一条普通的结构体定义。 */
                    return NULL;
                }
                for (;;) {
                    vtype_t t2 = st;
                    char    tname[32];

                    expect_ident(tname, sizeof(tname));
                    while (accept_punct("*")) {
                        t2.ptr++;
                    }
                    if (is_punct("[")) {
                        parse_array_dims(&t2);
                    }
                    add_typedef(tname, t2);
                    if (!accept_punct(",")) {
                        break;
                    }
                }
                expect_punct(";");
                return NULL;
            }
            if (!have_st) {
                lex_p    = save_p;
                tok      = save_tok;
                src_line = save_line;
            }
        }

        tt = parse_type();

        {
            int first_alias = 1;

            for (;;) {
                vtype_t t2 = tt;
                char tname[32];

                /* 逗号后面的声明符要自己带自己的 '*'：
                 * `typedef int A, *B;` 里 B 是指针，而
                 * `typedef int *A, B;` 里 B 只是 int —— 原来两处都错，
                 * 因为 t2 每次都从 tt 原样复制（tt 带着第一个声明符的星号）。 */
                if (!first_alias) {
                    t2.ptr = 0;
                }
                while (accept_punct("*")) {
                    t2.ptr++;
                }
                first_alias = 0;

                expect_ident(tname, sizeof(tname));
                if (is_punct("[")) {
                    parse_array_dims(&t2);
                }
                add_typedef(tname, t2);

                if (!accept_punct(",")) {
                    break;
                }
            }
        }
        expect_punct(";");
        return NULL;
    }

    /* 先看是不是 "enum Color { ... };" 这种定义（和下面的 struct 一样，
     * 只看当前 token 分不清定义和 "enum Color c;"，要往后看一个）。 */
    if (is_kw("enum")) {
        const char* save_p    = lex_p;
        token_t     save_tok  = tok;
        int         save_line = src_line;
        char        ename[32];
        int         is_def    = 0;

        lex_next();
        if (tok.kind == TK_IDENT) {
            copy_str_local(ename, tok.text, sizeof(ename));
            lex_next();
            if (is_punct("{")) {
                is_def = 1;
            }
        }
        if (is_def) {
            parse_enum_def(ename);
            return NULL;
        }
        lex_p    = save_p;
        tok      = save_tok;
        src_line = save_line;
    }

    /* 先看是不是 "struct T { ... };" 这种定义，或者 "struct T;" 前向声明。
     * 只看当前 token 分不清 "struct T {" 和 "struct T x;"，所以要往后看一个：
     * 词法器只有一个 token 的前看量，用"存下位置再退回来"的老办法。 */
    if (is_kw("struct")) {
        const char* save_p    = lex_p;
        token_t     save_tok  = tok;
        int         save_line = src_line;
        char        sname[32];
        int         is_def    = 0;
        int         is_fwd    = 0;

        lex_next();
        if (tok.kind == TK_IDENT) {
            copy_str_local(sname, tok.text, sizeof(sname));
            lex_next();
            if (is_punct("{")) {
                is_def = 1;
            } else if (is_punct(";")) {
                is_fwd = 1;
            }
        }
        if (is_def) {
            parse_struct_def(sname, 1);    /* 吃掉 body 和结尾的 ';' */
            return NULL;
        }
        if (is_fwd) {
            lex_next();                    /* 吃掉 ';' */
            declare_struct_forward(sname);
            return NULL;
        }
        lex_p    = save_p;
        tok      = save_tok;
        src_line = save_line;
    }

    base = parse_type();

    if (tok.kind != TK_IDENT) {
        fatal("expected a variable or function name");
    }
    expect_ident(name, sizeof(name));

    /* 函数？ */
    if (is_punct("(")) {
        int fi;
        struct sym_func* f;

        if (!is_global) {
            fatal("nested functions are not supported ('%s' is inside a block)",
                  name);
        }

        fi = find_func(name);
        if (fi < 0) {
            if (func_n >= MAX_FUNCS) {
                fatal("too many functions (limit %d)", MAX_FUNCS);
            }
            fi = func_n++;
            memset(&funcs[fi], 0, sizeof(funcs[fi]));
            copy_str_local(funcs[fi].name, name, sizeof(funcs[fi].name));
            funcs[fi].label = -1;
            funcs[fi].builtin_nr = -1;
        }
        f = &funcs[fi];

        if (f->body) {
            fatal("function '%s' is defined twice", name);
        }
        if (f->builtin_nr >= 0 || f->is_builtin) {
            fatal("'%s' is a built-in function and cannot be redefined", name);
        }
        f->ret = base;
        f->nparams = 0;

        /* 结构体只能通过指针传递/返回。
         * 按值传递要生成一份拷贝，这个编译器的调用约定不做这件事 ——
         * 与其生成错的代码，不如在这里明确拒绝。 */
        if (base.is_struct && base.ptr == 0) {
            fatal("'%s' returns a struct by value; use a pointer instead", name);
        }

        lex_next();                       /* 吃掉 '(' */
        if (!is_punct(")")) {
            /* "void" 有两种意思：
             *   int f(void)        -> 没有参数
             *   void* memset(...)  -> void 是个类型，后面还有指针和参数名
             * 只看当前 token 是不够的（两个都从 void 开始），得往后看一个。
             * 词法器只有一个 token 的前看量，所以这里用"存下位置再退回来"
             * 的办法（和 parse_unary 里区分转换与括号是同一招）。
             */
            int no_params = 0;

            if (is_kw("void")) {
                const char* save_p    = lex_p;
                token_t     save_tok  = tok;
                int         save_line = src_line;

                lex_next();
                if (is_punct(")")) {
                    no_params = 1;        /* 就是 "void)" */
                } else {
                    lex_p    = save_p;
                    tok      = save_tok;
                    src_line = save_line;
                }
            }

            if (!no_params) {
                for (;;) {
                    vtype_t pt;
                    char pn[32];

                    /* "..." 结束固定参数，之后是可变参数。
                     * 有它之后调用点不再核对参数个数（见 parse_primary）。 */
                    if (is_punct("...")) {
                        lex_next();
                        f->is_varargs = 1;
                        break;
                    }

                    pt = parse_type();
                    expect_ident(pn, sizeof(pn));
                    if (is_punct("[")) {
                        parse_array_dims(&pt);
                    }
                    if (pt.is_struct && pt.ptr == 0) {
                        fatal("parameter '%s' is a struct by value; use a pointer",
                              pn);
                    }
                    if (f->nparams >= 8) {
                        fatal("at most 8 parameters are supported");
                    }
                    copy_str_local(f->pname[f->nparams], pn, 32);
                    f->ptype[f->nparams] = pt;
                    f->nparams++;
                    if (!accept_punct(",")) {
                        break;
                    }
                }
            }
        }
        expect_punct(")");

        if (is_punct(";")) {
            lex_next();
            return NULL;                  /* 原型 */
        }
        if (is_punct("{")) {
            node_t* block;

            /* 参数先进局部表，函数体里就能按名字直接用。
             * 这里把符号指针也存进函数表：代码生成阶段要给它们
             * 填 [ebp+8+4i] 这个偏移，而语法树里引用的是同一批符号。 */
            local_n = 0;
            for (int i = 0; i < f->nparams; i++) {
                f->param_sym[i] = declare_symbol(f->pname[i], f->ptype[i],
                                                 SYM_PARAM);
            }
            parsing_global = 0;
            block = parse_block();
            parsing_global = 1;
            f->body = block;
            f->defined = 1;
            return NULL;
        }
        fatal("expected ';' or '{' after the parameter list");
    }

    /* --- 变量 --- */
    {
        node_t* head = NULL;
        node_t* tail = NULL;

        for (;;) {
            vtype_t t = base;
            struct sym* s;
            node_t* n;
            int inferred = 0;

            parse_array_dims(&t);
            if (t.ndims > 0 && t.dims[0] == 0) {
                inferred = 1;              /* `int a[] = {...}`：长度等下补 */
            }
            s = declare_symbol(name, t, is_global ? SYM_GLOBAL : SYM_LOCAL);

            n = node_new(ND_DECL);
            copy_str_local(n->name, name, sizeof(n->name));
            n->sym = s;

            if (accept_punct("=")) {
                if (is_punct("{")) {
                    /* 初始化列表：逐个表达式，用 next 串起来 */
                    node_t* list = node_new(ND_INITLIST);
                    node_t* itail = NULL;
                    int count = 0;

                    lex_next();            /* 吃掉 '{' */
                    while (!is_punct("}")) {
                        node_t* e = parse_assign();

                        if (itail) {
                            itail->next = e;
                        } else {
                            list->a = e;
                        }
                        itail = e;
                        count++;
                        if (!accept_punct(",")) {
                            break;
                        }
                    }
                    expect_punct("}");
                    if (count == 0) {
                        fatal("empty initialiser list");
                    }
                    list->num = count;     /* 元素个数 */
                    n->a = list;

                    if (inferred) {
                        t.dims[0] = count;
                        s->nelem = count;
                        s->size = type_size(t);
                    } else if (count > t.dims[0]) {
                        fatal("too many initialisers for '%s' (%d > %d)",
                              name, count, t.dims[0]);
                    }
                } else if (tok.kind == TK_STR) {
                    n->a = parse_primary();

                    if (inferred) {
                        /* `char s[] = "abc"` -> 长度是 strlen + 1 */
                        t.dims[0] = n->a->str_len + 1;
                        s->nelem = t.dims[0];
                        s->size = type_size(t);
                    }
                } else {
                    n->a = parse_assign();
                }
            } else if (inferred) {
                fatal("'%s' has no size and no initialiser", name);
            }

            if (tail) {
                tail->next = n;
            } else {
                head = n;
            }
            tail = n;

            if (is_global) {
                if (global_decl_n >= MAX_SYMS) {
                    fatal("too many globals (limit %d)", MAX_SYMS);
                }
                global_decls[global_decl_n++] = n;
            }

            if (accept_punct(",")) {
                /* 逗号后面的声明符自己带自己的 '*'（parse_type 只吃掉了
                 * 第一个声明符前面那批）：
                 *   `int *a, b;`  b 是 int（原来被当成 int* 了）
                 *   `int a, *b;`  b 是 int*（原来 `int a, *b;` 直接报错，
                 *                 因为这里紧接着就 expect_ident） */
                base.ptr = 0;
                while (accept_punct("*")) {
                    base.ptr++;
                }
                expect_ident(name, sizeof(name));
                continue;
            }
            if (eat_semi) {
                expect_punct(";");
            }
            return head;
        }
    }
}

/* case 的常量表达式。
 *
 * 支持：十进制/十六进制字面量、字符常量、常量表里的名字（enum 成员、
 * #define），以及它们之间的一元 +- 和二元 +-。
 * 不支持 '* / ( )' —— 那几个在 case 里本来就少见，宁可报错也别算错，
 * 而"只认数字字面量"更糟：`case RED:` 这种写法直接编译不过。
 */
static int parse_case_const_term(void) {
    int v = 0;
    int neg = 0;

    for (;;) {
        if (accept_punct("-")) {
            neg = !neg;
        } else if (accept_punct("+")) {
            /* 一元正号，没用 */
        } else {
            break;
        }
    }

    if (tok.kind == TK_NUM || tok.kind == TK_CHARLIT) {
        v = (int)tok.num;
        lex_next();
    } else if (tok.kind == TK_IDENT) {
        if (!lookup_define(tok.text, &v)) {
            fatal("'case' value '%s' is not a constant "
                  "(enum member or #define)", tok.text);
        }
        lex_next();
    } else {
        fatal("'case' needs a constant value");
    }
    return neg ? -v : v;
}

static int parse_case_const(void) {
    int v = parse_case_const_term();

    for (;;) {
        if (accept_punct("+")) {
            v += parse_case_const_term();
        } else if (accept_punct("-")) {
            v -= parse_case_const_term();
        } else {
            break;
        }
    }
    return v;
}

static node_t* parse_stmt(void) {
    /* 声明。`typedef` 也走这一支：块里面的 typedef 只看 is_type_kw() 是
     * 看不出来的（typedef 不是类型名），于是它会被当成表达式语句，
     * 报"未声明的变量 typedef" —— 报错信息完全指不到点子上。 */
    if (is_type_kw() || is_kw("typedef")) {
        return parse_declaration(1);
    }
    /* 复合语句 */
    if (is_punct("{")) {
        return parse_block();
    }
    /* 空语句 */
    if (accept_punct(";")) {
        return node_new(ND_EMPTY);
    }
    if (accept_kw("if")) {
        node_t* n = node_new(ND_IF);

        expect_punct("(");
        n->a = parse_expr();
        expect_punct(")");
        n->b = parse_stmt();
        if (accept_kw("else")) {
            n->c = parse_stmt();
        }
        return n;
    }
    if (accept_kw("while")) {
        node_t* n = node_new(ND_WHILE);

        expect_punct("(");
        n->a = parse_expr();
        expect_punct(")");
        n->b = parse_stmt();
        return n;
    }
    if (accept_kw("do")) {
        node_t* n = node_new(ND_DOWHILE);

        n->b = parse_stmt();
        if (!accept_kw("while")) {
            fatal("expected 'while' after the body of 'do'");
        }
        expect_punct("(");
        n->a = parse_expr();
        expect_punct(")");
        expect_punct(";");
        return n;
    }
    if (accept_kw("for")) {
        node_t* n = node_new(ND_FOR);

        expect_punct("(");
        /* 初始化：可以是声明，也可以是表达式，也可以空着 */
        if (is_type_kw()) {
            n->a = parse_declaration(1);
        } else if (accept_punct(";")) {
            n->a = NULL;
        } else {
            node_t* e = node_new(ND_EXPRSTMT);

            e->a = parse_expr();
            expect_punct(";");
            n->a = e;
        }
        if (!is_punct(";")) {
            n->b = parse_expr();
        }
        expect_punct(";");
        if (!is_punct(")")) {
            n->c = parse_expr();
        }
        expect_punct(")");
        n->d = parse_stmt();
        return n;
    }
    if (accept_kw("return")) {
        node_t* n = node_new(ND_RETURN);

        if (!is_punct(";")) {
            n->a = parse_expr();
        }
        expect_punct(";");
        return n;
    }
    if (accept_kw("break")) {
        expect_punct(";");
        return node_new(ND_BREAK);
    }
    if (accept_kw("continue")) {
        expect_punct(";");
        return node_new(ND_CONTINUE);
    }

    /* switch (expr) 语句体
     *
     * case / default 是"标签"，作为普通语句挂在语句链上（见下面那两个
     * 分支），代码生成时先扫一遍链把标签都找出来。 */
    if (accept_kw("switch")) {
        node_t* n = node_new(ND_SWITCH);

        expect_punct("(");
        n->a = parse_expr();
        expect_punct(")");
        n->b = parse_stmt();
        return n;
    }

    if (accept_kw("case")) {
        node_t* n = node_new(ND_CASE);

        n->label = -1;
        n->num = parse_case_const();
        expect_punct(":");
        /* 标签后面跟着的语句挂到 next 上，这样标签就"混"在语句链里，
         * 一个 switch 体扫一遍就能按顺序找到所有分支（也支持
         * "case 1: case 2: stmt;" 这种连着写）。 */
        n->b = parse_stmt();
        if (n->b) {
            n->next = n->b;
        }
        n->b = NULL;
        return n;
    }

    if (accept_kw("default")) {
        node_t* n = node_new(ND_DEFAULT);

        n->label = -1;
        expect_punct(":");
        n->b = parse_stmt();
        if (n->b) {
            n->next = n->b;
        }
        n->b = NULL;
        return n;
    }

    /* 表达式语句 */
    {
        node_t* n = node_new(ND_EXPRSTMT);

        n->a = parse_expr();
        expect_punct(";");
        return n;
    }
}

/* 复合语句：把里面的语句串成链表 */
static node_t* parse_block(void) {
    node_t* head = NULL;
    node_t* tail = NULL;

    expect_punct("{");
    while (!is_punct("}")) {
        node_t* s;

        if (tok.kind == TK_EOF) {
            fatal("unexpected end of file inside a block");
        }
        s = parse_stmt();
        if (!s) {
            continue;              /* 声明本身没有可执行的语句 */
        }
        if (tail) {
            tail->next = s;
        } else {
            head = s;
        }
        /* 一个"语句"可能是一条链：`int a = 1, b = 2;` 返回的是两个 ND_DECL，
         * case 标签也把后面那条语句挂在 next 上。tail 必须推进到链尾，
         * 否则下一条语句会覆盖链中间 —— 那样 b 的初始化会被整条丢掉，
         * 而且完全看不出来。
         *
         * 注意推进之后**不能**再写一句 `tail = s;`（顺序反了就等于
         * 白推）。这个修复第一版就是被一句多余赋值抵消掉的：switch 的
         * case 体、`int a=1, b=2` 的第二个声明符照样被丢，而且完全看
         * 不出来 —— 少了一句代码，多了一次默认 0 的赋值。 */
        tail = s;
        while (tail->next) {
            tail = tail->next;
        }
    }
    lex_next();

    if (!head) {
        head = node_new(ND_EMPTY);
    }
    {
        node_t* blk = node_new(ND_BLOCK);

        blk->a = head;
        return blk;
    }
}

/* ============================================================
 * 5. 代码生成
 *
 * 策略是最朴素的"栈机"：每个表达式算完把结果留在 eax，
 * 二元运算把左边压栈、右边算完再弹回来。没有寄存器分配，
 * 也没有任何优化 —— 换来的是一眼能看懂、出错了能对着反汇编找。
 * ============================================================ */

/* 数据区里"已初始化"部分的高水位；bss 部分紧接着它。
 * bss 里全局变量的偏移先按"相对 bss 起点"记，等代码生成完
 * （数据长度不会再变）再折算成真正的数据区偏移。 */
static int bss_rel_len;

/* 数据区里的一个字要指向另一处数据（全局变量 char* s = "abc"; ） */
static void data_ref_word(int at_off, int target_off) {
    ref_data(at_off, target_off, 1);
}

static vtype_t sym_vtype(const struct sym* s) {
    vtype_t t;

    memset(&t, 0, sizeof(t));
    t.base = s->base;
    t.ptr = s->ptr;
    t.is_struct = s->is_struct;
    t.sidx = s->sidx;
    if (s->nelem > 0) {
        t.ndims = 1;
        t.dims[0] = s->nelem;
    }
    return t;
}

static void gen_expr(node_t* n);
static void gen_addr(node_t* n);
static void gen_stmt(node_t* n);

/* [eax] 里的值读进 eax，宽度按类型 */
static void gen_load(vtype_t t) {
    if (t.ptr == 0 && t.ndims == 0 && t.base == TY_CHAR) {
        e_load8(R_EAX, R_EAX, 0);
    } else {
        e_load32(R_EAX, R_EAX, 0);
    }
}

/* ecx 存进 [eax]，宽度按类型 */
static void gen_store(vtype_t t) {
    if (t.ptr == 0 && t.ndims == 0 && t.base == TY_CHAR) {
        e_store8(R_ECX, R_EAX, 0);
    } else {
        e_store32(R_ECX, R_EAX, 0);
    }
}

/* 变量的地址 -> eax */
static void gen_addr(node_t* n) {
    struct sym* s;

    switch (n->kind) {
        case ND_VAR:
            s = n->sym;
            if (s->kind == SYM_GLOBAL) {
                if (s->is_bss) {
                    e_mov_r_bssaddr(R_EAX, s->offset);
                } else {
                    e_mov_r_dataaddr(R_EAX, s->offset);
                }
            } else {
                e_lea(R_EAX, R_EBP, s->offset);
            }
            return;

        case ND_DEREF:
            /* *p 的地址就是 p 的值 */
            gen_expr(n->a);
            return;

        case ND_MEMBER:
            /* a.f  ->  取 a 的地址，再加上字段偏移
             * p->f ->  取 p 的值（就是结构体地址），再加上字段偏移 */
            if (n->via_ptr) {
                gen_expr(n->a);
            } else {
                gen_addr(n->a);
            }
            if (n->field_off != 0) {
                e_mov_r_imm(R_ECX, (uint32_t)n->field_off);
                e_alu_rr(ALU_ADD, R_EAX, R_ECX);
            }
            return;

        default:
            fatal("this expression cannot be used as an assignment target");
    }
}

/* 字符串字面量：第一次用到时把它放进数据区，之后复用 */
static void gen_string_literal(node_t* n) {
    if (!n->data_ready) {
        unsigned char* p = (unsigned char*)nalloc(n->str_len + 1);

        memcpy(p, n->str, (size_t)n->str_len);
        p[n->str_len] = 0;
        n->data_off = data_emit_bytes(p, n->str_len + 1);
        n->data_ready = 1;
    }
    e_mov_r_dataaddr(R_EAX, n->data_off);
}

/* 前置/后置 ++ --。
 *
 * 增量是编译期常量（指针就按元素大小缩放），所以这里不需要任何
 * 运行时乘法，只要一次加法、一次存回。 */
static void gen_incdec(node_t* n, int delta, int post) {
    int step = 1;
    int change;

    if (type_is_ptr_like(n->a->ty)) {
        step = type_size(type_element(n->a->ty));
    }
    change = delta * step;
    if (change == 0) {
        change = delta;                    /* void* 之类退化的情况 */
    }

    gen_addr(n->a);                        /* eax = 地址 */
    e_push(R_EAX);                         /* [addr] */
    gen_load(n->a->ty);                    /* eax = 旧值 */
    e_mov_r_imm(R_ECX, (uint32_t)change);
    e_alu_rr(ALU_ADD, R_EAX, R_ECX);       /* eax = 新值 */
    e_push(R_EAX);                         /* [addr][new] */
    e_pop(R_ECX);                          /* ecx = 新值 */
    e_pop(R_EAX);                          /* eax = 地址 */
    e_push(R_ECX);                         /* [new]：先放着，等会儿当结果 */
    gen_store(n->a->ty);                   /* [eax] = ecx */
    e_pop(R_EAX);                          /* eax = 新值 */

    if (post) {
        e_mov_r_imm(R_ECX, (uint32_t)change);
        e_alu_rr(ALU_SUB, R_EAX, R_ECX);   /* 后置：结果是旧值 */
    }
}

/* ---- 循环栈：break / continue 要知道跳到哪 ---- */
static int break_labels[32];
static int continue_labels[32];
static int loop_depth;

/* ---- 当前函数的尾声标签（return 跳到这里） ---- */
static int cur_epilogue = -1;

static void gen_binop(node_t* n) {
    const char* op = n->opstr;

    /* 短路求值的两个 */
    if (strcmp(op, "&&") == 0) {
        int lfalse = new_label();
        int lend = new_label();

        gen_expr(n->a);
        e_test_rr(R_EAX, R_EAX);
        e_je(lfalse);
        gen_expr(n->b);
        e_test_rr(R_EAX, R_EAX);
        e_je(lfalse);
        e_mov_r_imm(R_EAX, 1);
        e_jmp(lend);
        place_label(lfalse);
        e_mov_r_imm(R_EAX, 0);
        place_label(lend);
        return;
    }
    if (strcmp(op, "||") == 0) {
        int ltrue = new_label();
        int lend = new_label();

        gen_expr(n->a);
        e_test_rr(R_EAX, R_EAX);
        e_jne(ltrue);
        gen_expr(n->b);
        e_test_rr(R_EAX, R_EAX);
        e_jne(ltrue);
        e_mov_r_imm(R_EAX, 0);
        e_jmp(lend);
        place_label(ltrue);
        e_mov_r_imm(R_EAX, 1);
        place_label(lend);
        return;
    }
    if (strcmp(op, ",") == 0) {
        gen_expr(n->a);
        gen_expr(n->b);
        return;
    }

    gen_expr(n->a);
    e_push(R_EAX);
    gen_expr(n->b);
    e_mov_rr(R_ECX, R_EAX);
    e_pop(R_EAX);

    if (strcmp(op, "+") == 0) {
        e_alu_rr(ALU_ADD, R_EAX, R_ECX);
    } else if (strcmp(op, "-") == 0) {
        e_alu_rr(ALU_SUB, R_EAX, R_ECX);
    } else if (strcmp(op, "*") == 0) {
        e_imul_rr(R_EAX, R_ECX);
    } else if (strcmp(op, "/") == 0) {
        e_cdq();
        e_idiv(R_ECX);
    } else if (strcmp(op, "%") == 0) {
        e_cdq();
        e_idiv(R_ECX);
        e_mov_rr(R_EAX, R_EDX);            /* 余数在 edx */
    } else if (strcmp(op, "&") == 0) {
        e_alu_rr(ALU_AND, R_EAX, R_ECX);
    } else if (strcmp(op, "|") == 0) {
        e_alu_rr(ALU_OR, R_EAX, R_ECX);
    } else if (strcmp(op, "^") == 0) {
        e_alu_rr(ALU_XOR, R_EAX, R_ECX);
    } else if (strcmp(op, "<<") == 0) {
        e_shl_cl();
    } else if (strcmp(op, ">>") == 0) {
        e_sar_cl();
    } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
               strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
               strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
        int cc = CC_E;

        e_alu_rr(ALU_CMP, R_EAX, R_ECX);
        if (strcmp(op, "==") == 0)      cc = CC_E;
        else if (strcmp(op, "!=") == 0) cc = CC_NE;
        else if (strcmp(op, "<") == 0)  cc = CC_L;
        else if (strcmp(op, "<=") == 0) cc = CC_LE;
        else if (strcmp(op, ">") == 0)  cc = CC_G;
        else                            cc = CC_GE;

        e_setcc(cc, R_EAX);
        e_movzx_byte(R_EAX, R_EAX);
    } else {
        fatal("internal: unhandled binary operator '%s'", op);
    }
}

/* 赋值（含复合赋值）。lhs 的类型决定存几个字节。 */
static void gen_assign(node_t* n) {
    const char* op = n->opstr;
    int simple = (strcmp(op, "=") == 0);

    gen_addr(n->a);                        /* eax = 左值地址 */
    e_push(R_EAX);

    if (simple) {
        gen_expr(n->b);                    /* eax = 右值 */
        e_mov_rr(R_ECX, R_EAX);
        e_pop(R_EAX);
        e_push(R_ECX);                     /* 存一份结果当表达式的值 */
        gen_store(n->a->ty);
        e_pop(R_EAX);
        return;
    }

    /* 复合赋值：把老值读出来参与运算 */
    gen_expr(n->b);                        /* eax = 右值 */
    e_mov_rr(R_ECX, R_EAX);
    e_pop(R_EAX);                          /* eax = 地址 */
    e_push(R_EAX);                         /* 地址留着写回 */
    gen_load(n->a->ty);                    /* eax = 老值 */
    e_push(R_ECX);                         /* 右值 */
    e_mov_rr(R_EDX, R_EAX);                /* edx = 老值 */

    if (strcmp(op, "+=") == 0) {
        e_alu_rr(ALU_ADD, R_EAX, R_ECX);
    } else if (strcmp(op, "-=") == 0) {
        e_alu_rr(ALU_SUB, R_EAX, R_ECX);
    } else if (strcmp(op, "*=") == 0) {
        e_imul_rr(R_EAX, R_ECX);
    } else if (strcmp(op, "/=") == 0) {
        e_cdq();
        e_idiv(R_ECX);
    } else if (strcmp(op, "%=") == 0) {
        e_cdq();
        e_idiv(R_ECX);
        e_mov_rr(R_EAX, R_EDX);
    } else if (strcmp(op, "&=") == 0) {
        e_alu_rr(ALU_AND, R_EAX, R_ECX);
    } else if (strcmp(op, "|=") == 0) {
        e_alu_rr(ALU_OR, R_EAX, R_ECX);
    } else if (strcmp(op, "^=") == 0) {
        e_alu_rr(ALU_XOR, R_EAX, R_ECX);
    } else if (strcmp(op, "<<=") == 0) {
        e_shl_cl();
    } else if (strcmp(op, ">>=") == 0) {
        e_sar_cl();
    } else {
        fatal("internal: unhandled assignment operator '%s'", op);
    }

    e_pop(R_ECX);                          /* ecx = 右值，不再需要 */
    e_push(R_EAX);                         /* 结果是新值 */
    e_pop(R_ECX);                          /* ecx = 新值 */
    e_pop(R_EAX);                          /* eax = 地址 */
    e_push(R_ECX);
    gen_store(n->a->ty);
    e_pop(R_EAX);
}

/* 内建函数：直接发射 int 0x80 */
static void gen_builtin_call(node_t* n, struct sym_func* f) {
    node_t* args[4];
    int na = 0;

    for (node_t* a = n->a; a; a = a->next) {
        if (na < 4) {
            args[na] = a;
        }
        na++;
    }

    if (f->builtin_mode == 1) {
        /* putchar：把字符压栈，栈顶就是那个字节缓冲区 */
        gen_expr(args[0]);
        e_push(R_EAX);
        e_mov_rr(R_ECX, R_ESP);            /* ecx = &c */
        e_mov_r_imm(R_EBX, 1);             /* fd = 1 */
        e_mov_r_imm(R_EDX, 1);             /* len = 1 */
        e_mov_r_imm(R_EAX, 1);             /* SYS_WRITE */
        e_int80();
        e_add_esp_imm(4);
        return;
    }

    /* 参数从右往左压，弹出来正好是 ebx/ecx/edx/esi */
    for (int i = na - 1; i >= 0; i--) {
        gen_expr(args[i]);
        e_push(R_EAX);
    }
    if (na > 0) e_pop(R_EBX);
    if (na > 1) e_pop(R_ECX);
    if (na > 2) e_pop(R_EDX);
    if (na > 3) e_pop(R_ESI);

    e_mov_r_imm(R_EAX, (uint32_t)f->builtin_nr);
    e_int80();
    if (f->builtin_mode == 2) {
        e_hlt();                           /* exit 不该返回 */
    }
}

static void gen_call(node_t* n) {
    struct sym_func* f;
    int na = count_args(n->a);

    if (n->func_index < 0) {
        fatal("call to undefined function '%s'", n->name);
    }
    f = &funcs[n->func_index];

    if (f->is_builtin) {
        gen_builtin_call(n, f);
        return;
    }
    if (!f->defined) {
        fatal("function '%s' is declared but never defined", n->name);
    }

    /* 实参从右往左压栈。
     *
     * MAX_CALL_ARGS 这个上限必须**报错**，不能悄悄截断：原来超过 16 个
     * 实参时只压了 16 个，收尾却按 na 调整 esp —— 一次调用把栈指针拨到
     * 半空，返回地址都错位了。 */
    if (na > MAX_CALL_ARGS) {
        fatal("too many arguments in the call to '%s' (%d > %d)",
              n->name, na, MAX_CALL_ARGS);
    }
    {
        node_t* args[MAX_CALL_ARGS];
        int k = 0;

        for (node_t* a = n->a; a; a = a->next) {
            args[k++] = a;
        }
        for (int i = k - 1; i >= 0; i--) {
            gen_expr(args[i]);
            e_push(R_EAX);
        }
    }

    e_call_label(f->label);
    if (na > 0) {
        e_add_esp_imm(na * 4);
    }
}

static void gen_expr(node_t* n) {
    switch (n->kind) {
        case ND_NUM:
            e_mov_r_imm(R_EAX, (uint32_t)n->num);
            return;

        case ND_STR:
            gen_string_literal(n);
            return;

        case ND_VAR:
            gen_addr(n);

            /* 只有"数组名"是把地址当值用（退化成指针）；别的都要取值 ——
             * **指针变量也一样**：char* s 作为值用的时候，要读的是它里面
             * 存的那个地址，不是 s 自己所在的位置。
             *
             * 最初写成"只要类型是指针类就不取值"，于是 s[i] 变成了
             * *(&s + i)：读出来的是栈上那个指针变量的字节。症状很好认 ——
             * 打印出来的是那串地址的可见字节（比如 $ 或 ?@），因为地址的
             * 第 2 个字节常是 0x1A/0x16，打印到那儿就遇到 '\0' 停了；
             * 而下标恰好取到 0x1A 时会显示成 26 —— 那正是数据区地址的第二字节。
             */
            if (n->ty.ndims == 0) {
                gen_load(n->ty);
            }
            return;

        case ND_DEREF:
            gen_expr(n->a);
            gen_load(n->ty);
            return;

        case ND_MEMBER:
            /* 和 ND_VAR 一个道理：数组字段的名字当地址用，别的都要取值，
             * 指针字段也要把它存的地址读出来。整个结构体不能当值用
             * （解析阶段已经拒绝赋值了），所以到这里的都是能取的值。 */
            gen_addr(n);
            if (n->ty.ndims == 0) {
                gen_load(n->ty);
            }
            return;

        case ND_ADDR:
            gen_addr(n->a);
            return;

        case ND_BIN:
            gen_binop(n);
            return;

        case ND_ASSIGN:
            gen_assign(n);
            return;

        case ND_CALL:
            gen_call(n);
            return;

        case ND_NEG:
            gen_expr(n->a);
            e_neg(R_EAX);
            return;

        case ND_BITNOT:
            gen_expr(n->a);
            e_not(R_EAX);
            return;

        case ND_LOGNOT:
            gen_expr(n->a);
            e_test_rr(R_EAX, R_EAX);
            e_setcc(CC_E, R_EAX);
            e_movzx_byte(R_EAX, R_EAX);
            return;

        case ND_CAST:
            gen_expr(n->a);
            if (n->ty.ptr == 0 && n->ty.ndims == 0 && n->ty.base == TY_CHAR) {
                e_movsx_byte(R_EAX, R_EAX);    /* 截成 char 并符号扩展 */
            }
            return;

        case ND_PREINC:  gen_incdec(n, 1, 0);  return;
        case ND_PREDEC:  gen_incdec(n, -1, 0); return;
        case ND_POSTINC: gen_incdec(n, 1, 1);  return;
        case ND_POSTDEC: gen_incdec(n, -1, 1); return;

        case ND_COND: {
            int lelse = new_label();
            int lend = new_label();

            gen_expr(n->a);
            e_test_rr(R_EAX, R_EAX);
            e_je(lelse);
            gen_expr(n->b);
            e_jmp(lend);
            place_label(lelse);
            gen_expr(n->c);
            place_label(lend);
            return;
        }

        default:
            fatal("internal: cannot generate code for node kind %d", n->kind);
    }
}

static void gen_block(node_t* n) {
    for (node_t* s = n->a; s; s = s->next) {
        gen_stmt(s);
    }
}

static void gen_stmt(node_t* n) {
    switch (n->kind) {
        case ND_BLOCK:
            gen_block(n);
            return;

        case ND_EMPTY:
            return;

        case ND_EXPRSTMT:
            gen_expr(n->a);
            return;

        case ND_DECL:
            if (n->a) {
                /* 初始化列表：数组逐个元素赋值（元素可以是任意表达式，
                 * 因为这是一条语句，不像全局那样要求常量） */
                if (n->a->kind == ND_INITLIST) {
                    vtype_t at_ty = sym_vtype(n->sym);
                    vtype_t el_ty = type_element(at_ty);
                    int esize = type_size(el_ty);
                    int i = 0;

                    for (node_t* e = n->a->a; e; e = e->next) {
                        gen_addr_var(n->sym);
                        if (i * esize != 0) {
                            e_mov_r_imm(R_ECX, (uint32_t)(i * esize));
                            e_alu_rr(ALU_ADD, R_EAX, R_ECX);
                        }
                        e_push(R_EAX);
                        gen_expr(e);
                        e_mov_rr(R_ECX, R_EAX);
                        e_pop(R_EAX);
                        gen_store(el_ty);
                        i++;
                    }
                    return;
                }

                /* 字符串初始化局部 char 数组：逐字节抄。
                 * （第一版这里直接报错，现在真正生成代码 —— 写法常见，
                 * 而且逐字节抄一共就几条指令。）
                 *
                 * 判断类型必须看**符号**（sym_vtype），不能看 n->ty：
                 * ND_DECL 节点上的 ty 从来没设过，默认是 int —— 第一版
                 * 就是栽在这儿，字符串初始化走了"当指针存 4 字节"那条路，
                 * lbuf[0] 变成了地址的低字节，puts 打出来一个不可见字符。 */
                if (n->a->kind == ND_STR) {
                    vtype_t dt = sym_vtype(n->sym);

                    if (dt.ndims > 0 && dt.ptr == 0 && dt.base == TY_CHAR) {
                        int len = n->a->str_len + 1;      /* 含结尾的 0 */
                        int room = type_size(dt);

                        if (len > room) {
                            fatal("initialiser for '%s' does not fit (%d > %d)",
                                  n->name, len, room);
                        }
                        for (int i = 0; i < len; i++) {
                            /* 目标地址 -> eax */
                            gen_addr_var(n->sym);
                            if (i != 0) {
                                e_mov_r_imm(R_ECX, (uint32_t)i);
                                e_alu_rr(ALU_ADD, R_EAX, R_ECX);
                            }
                            e_push(R_EAX);
                            /* 源字节：字符串字面量基址 + i，读一个字节 */
                            gen_string_literal(n->a);
                            if (i != 0) {
                                e_mov_r_imm(R_ECX, (uint32_t)i);
                                e_alu_rr(ALU_ADD, R_EAX, R_ECX);
                            }
                            e_load8(R_EAX, R_EAX, 0);
                            e_mov_rr(R_ECX, R_EAX);
                            e_pop(R_EAX);
                            e_store8(R_ECX, R_EAX, 0);
                        }
                        return;
                    }
                }

                /* 结构体不能从另一个结构体初始化（要逐字段抄，或者用指针） */
                {
                    vtype_t dt = sym_vtype(n->sym);

                    if (dt.is_struct && dt.ptr == 0) {
                        fatal("cannot initialise the struct '%s' from an "
                              "expression; assign its members instead", n->name);
                    }
                }

                /* 其它情况：等于一次赋值 */
                gen_addr_var(n->sym);
                e_push(R_EAX);
                gen_expr(n->a);
                e_mov_rr(R_ECX, R_EAX);
                e_pop(R_EAX);
                gen_store(sym_vtype(n->sym));
            }
            return;

        case ND_IF: {
            int lelse = new_label();
            int lend = new_label();

            gen_expr(n->a);
            e_test_rr(R_EAX, R_EAX);
            e_je(lelse);
            gen_stmt(n->b);
            if (n->c) {
                e_jmp(lend);
                place_label(lelse);
                gen_stmt(n->c);
                place_label(lend);
            } else {
                place_label(lelse);
            }
            return;
        }

        case ND_WHILE: {
            int ltop = new_label();
            int lend = new_label();

            if (loop_depth >= 32) {
                fatal("loops are nested too deeply");
            }
            break_labels[loop_depth] = lend;
            continue_labels[loop_depth] = ltop;
            loop_depth++;

            place_label(ltop);
            gen_expr(n->a);
            e_test_rr(R_EAX, R_EAX);
            e_je(lend);
            gen_stmt(n->b);
            e_jmp(ltop);
            place_label(lend);

            loop_depth--;
            return;
        }

        case ND_DOWHILE: {
            int ltop = new_label();
            int lcont = new_label();
            int lend = new_label();

            if (loop_depth >= 32) {
                fatal("loops are nested too deeply");
            }
            break_labels[loop_depth] = lend;
            continue_labels[loop_depth] = lcont;
            loop_depth++;

            place_label(ltop);
            gen_stmt(n->b);
            place_label(lcont);
            gen_expr(n->a);
            e_test_rr(R_EAX, R_EAX);
            e_jne(ltop);
            place_label(lend);

            loop_depth--;
            return;
        }

        case ND_FOR: {
            int ltop = new_label();
            int lstep = new_label();
            int lend = new_label();

            if (n->a) {
                /* 初始化（可能是声明）。`for (int i = 0, j = 3; ...)` 的
                 * 初始化是一条 ND_DECL 链：只生成链头的话，后面的名字虽然
                 * 有栈槽（assign_locals 会发）却没有初始化代码 —— 值不确定。 */
                for (node_t* in = n->a; in; in = in->next) {
                    gen_stmt(in);
                }
            }
            if (loop_depth >= 32) {
                fatal("loops are nested too deeply");
            }
            break_labels[loop_depth] = lend;
            continue_labels[loop_depth] = lstep;
            loop_depth++;

            place_label(ltop);
            if (n->b) {
                gen_expr(n->b);
                e_test_rr(R_EAX, R_EAX);
                e_je(lend);
            }
            gen_stmt(n->d);
            place_label(lstep);
            if (n->c) {
                gen_expr(n->c);
            }
            e_jmp(ltop);
            place_label(lend);

            loop_depth--;
            return;
        }

        case ND_RETURN:
            if (n->a) {
                gen_expr(n->a);
            }
            e_jmp(cur_epilogue);
            return;

        case ND_BREAK:
            if (loop_depth == 0) {
                fatal("'break' outside of a loop or switch");
            }
            e_jmp(break_labels[loop_depth - 1]);
            return;

        case ND_CONTINUE:
            /* switch 也占一层（为了 break），但它不是循环：
             * 它那层的 continue 目标被设成 -1，落到这里就是"continue
             * 在 switch 里但外面没有循环"，那才是真的错。 */
            if (loop_depth == 0 || continue_labels[loop_depth - 1] < 0) {
                fatal("'continue' outside of a loop");
            }
            e_jmp(continue_labels[loop_depth - 1]);
            return;

        case ND_SWITCH: {
            /* 布局：
             *     <求值>                    eax = 待比较的值
             *     push eax                  留在栈顶，比较时从 [esp] 取
             *     cmp [esp], 值N / je 分支N  每个 case 一条
             *     jmp <default 或 cleanup>  都没匹配
             *   分支N: ...                  break 跳 cleanup
             *   cleanup:
             *     add esp, 4                丢掉临时值
             *
             * continue 的坑：switch 里面写 continue，必须先把栈顶那 4 字节
             * 临时值弹掉再跳回外层循环，否则每转一圈漏 4 字节，循环几百次
             * 就把栈写穿了。所以这一层的 continue 目标不是外层循环的
             * 标签，而是一小段蹦床（trampoline）：
             *     add esp, 4
             *     jmp <外层 continue>
             * 蹦床放在 cleanup 之后，用一条 jmp 跳过，保证不会被顺序执行到。
             */
            node_t* body;
            node_t* s;
            int cleanup = new_label();
            int no_match = cleanup;
            int outer_cont = (loop_depth > 0) ? continue_labels[loop_depth - 1] : -1;
            int lcont = -1;

            if (!n->b) {
                fatal("internal: switch without a body");
            }
            body = (n->b->kind == ND_BLOCK) ? n->b->a : n->b;

            /* 第一遍：给每个分支分配标签（-1 表示还没分配），
             * 顺便查重复的 case 值和重复的 default —— 重复了就是死代码，
             * 而且第一个分支永远匹配，写错的人自己看不出来。 */
            for (s = body; s; s = s->next) {
                if (s->kind == ND_CASE || s->kind == ND_DEFAULT) {
                    for (node_t* p = body; p != s; p = p->next) {
                        if (s->kind == ND_CASE && p->kind == ND_CASE &&
                            p->num == s->num) {
                            fatal("duplicate 'case %d'", s->num);
                        }
                        if (s->kind == ND_DEFAULT && p->kind == ND_DEFAULT) {
                            fatal("duplicate 'default' in one switch");
                        }
                    }
                    if (s->label < 0) {
                        s->label = new_label();
                    }
                }
            }
            for (s = body; s; s = s->next) {
                if (s->kind == ND_DEFAULT) {
                    no_match = s->label;
                }
            }

            gen_expr(n->a);
            e_push(R_EAX);

            for (s = body; s; s = s->next) {
                if (s->kind == ND_CASE) {
                    e_load_sp(R_EAX);
                    e_mov_r_imm(R_ECX, (uint32_t)s->num);
                    e_alu_rr(ALU_CMP, R_EAX, R_ECX);
                    e_je(s->label);
                }
            }
            e_jmp(no_match);

            /* 语句体：遇到标签就落标签，其它照常生成 */
            if (loop_depth >= 32) {
                fatal("switch/loops are nested too deeply");
            }
            break_labels[loop_depth] = cleanup;
            if (outer_cont >= 0) {
                lcont = new_label();
                continue_labels[loop_depth] = lcont;
            } else {
                continue_labels[loop_depth] = -1;
            }
            loop_depth++;

            for (s = body; s; s = s->next) {
                if (s->kind == ND_CASE || s->kind == ND_DEFAULT) {
                    place_label(s->label);
                    continue;
                }
                gen_stmt(s);
            }

            loop_depth--;
            place_label(cleanup);
            e_add_esp_imm(4);          /* 丢掉那个临时值 */
            if (lcont >= 0) {
                int lskip = new_label();

                e_jmp(lskip);          /* 别顺序走进蹦床 */
                place_label(lcont);
                e_add_esp_imm(4);      /* 丢掉那个临时值 */
                e_jmp(outer_cont);
                place_label(lskip);
            }
            return;
        }

        case ND_CASE:
        case ND_DEFAULT:
            fatal("'case'/'default' must be at the top level of a switch body");
            return;

        default:
            fatal("internal: cannot generate code for statement kind %d", n->kind);
    }
}

/* gen_addr 的变量专用版本（ND_DECL 手上有的是符号而不是节点） */
static void gen_addr_var(struct sym* s) {
    if (s->kind == SYM_GLOBAL) {
        if (s->is_bss) {
            e_mov_r_bssaddr(R_EAX, s->offset);
        } else {
            e_mov_r_dataaddr(R_EAX, s->offset);
        }
    } else {
        e_lea(R_EAX, R_EBP, s->offset);
    }
}

/* 预扫：把函数体里每个局部变量安排到栈帧里。
 * 必须先扫完再发序言，因为 sub esp, N 里的 N 就是这里算出来的。 */
static void assign_locals(node_t* s, int* used) {
    for (; s; s = s->next) {
        switch (s->kind) {
            case ND_DECL: {
                int size = type_size(sym_vtype(s->sym));

                size = (size + 3) & ~3;
                *used += size;
                s->sym->offset = -*used;
                break;
            }
            case ND_BLOCK:
                assign_locals(s->a, used);
                break;
            case ND_IF:
                assign_locals(s->b, used);
                assign_locals(s->c, used);
                break;
            case ND_WHILE:
            case ND_DOWHILE:
                assign_locals(s->b, used);
                break;
            case ND_SWITCH:
                /* switch 也是"带子语句的语句"：漏掉这一支，写在 switch 体里的
                 * 局部变量就永远没有栈槽（frame 算成 0），赋值会落到 [ebp+0]
                 * 上，把调用者的 ebp 踩掉 —— 静默的错代码。 */
                assign_locals(s->b, used);
                break;
            case ND_CASE:
            case ND_DEFAULT:
                /* case/default 后面挂的语句在 next 上，外层循环会走到；
                 * 但它自己的 b（如果有）也要扫。 */
                assign_locals(s->b, used);
                break;
            case ND_FOR:
                if (s->a && s->a->kind == ND_DECL) {
                    /* `for (int i = 0, j = 3; ...)` 是一条 ND_DECL 链，
                     * 每个声明符都要槽位（原来只处理链头，j 就没槽了）。 */
                    for (node_t* d = s->a; d; d = d->next) {
                        int size = type_size(sym_vtype(d->sym));

                        size = (size + 3) & ~3;
                        *used += size;
                        d->sym->offset = -*used;
                    }
                } else if (s->a) {
                    assign_locals(s->a, used);
                }
                assign_locals(s->d, used);
                break;
            default:
                break;
        }
    }
}

/* ============================================================
 * 6. 内建函数：编译器直接发射系统调用
 *
 * 用户程序里写 putchar('x') 就行 —— 不需要 #include，也不需要链接
 * 任何库。这几个函数是"编译器认识的"，调用点直接展开成 int 0x80。
 *
 * 参数走 ebx/ecx/edx/esi，和内核 syscall_handler 的约定一致。
 * ============================================================ */

typedef struct {
    const char* name;
    int nr;          /* 系统调用号 */
    int nargs;
    int ret_int;     /* 1 = 返回值是 eax，0 = 无返回值 */
    int mode;        /* 见 gen_builtin_call：0 普通 / 1 putchar / 2 exit */
} builtin_def_t;

static const builtin_def_t builtin_defs[] = {
    { "putchar",  -1, 1, 1, 1 },   /* 参数拿栈顶当缓冲区，见 gen_builtin_call */
    { "getchar",   2, 0, 1, 0 },
    { "exit",      0, 1, 0, 2 },
    { "write",     1, 3, 1, 0 },
    { "sleep_ms",  5, 1, 0, 0 },
    { "uptime",    6, 0, 1, 0 },
    { "ticks",     7, 0, 1, 0 },
    { "readfile", 10, 3, 1, 0 },
    { "listdir",  11, 4, 1, 0 },
    { "open",     12, 2, 1, 0 },
    { "close",    13, 1, 1, 0 },
    { "read",     14, 3, 1, 0 },
    { "lseek",    15, 3, 1, 0 },
    { "unlink",   16, 1, 1, 0 },
    { "fstat",    17, 1, 1, 0 },
    { "fsync",    18, 1, 1, 0 },
    { "getkey",   19, 0, 1, 0 },
    { "winsize",  20, 0, 1, 0 },
    { "sbrk",     21, 1, 1, 0 },
    { "mkdir",    22, 1, 1, 0 },
    { "getargs",  23, 2, 1, 0 },
    { "getcwd",   24, 2, 1, 0 },
    { "chmod",    25, 2, 1, 0 },
    { "whoami",   26, 0, 1, 0 },
    { "getpid",    3, 0, 1, 0 },
    { "yield",     4, 0, 0, 0 },
    { NULL, 0, 0, 0, 0 }
};

static void register_builtins(void) {
    for (int i = 0; builtin_defs[i].name; i++) {
        const builtin_def_t* b = &builtin_defs[i];
        struct sym_func* f;

        if (func_n >= MAX_FUNCS) {
            fatal("too many functions");
        }
        f = &funcs[func_n];
        memset(f, 0, sizeof(*f));
        copy_str_local(f->name, b->name, sizeof(f->name));
        f->ret = type_int();
        if (!b->ret_int) {
            f->ret.base = TY_VOID;         /* exit / yield / sleep_ms 没有返回值 */
        }
        f->nparams = b->nargs;
        f->is_builtin = 1;
        f->builtin_nr = b->nr;
        f->builtin_mode = b->mode;
        f->defined = 1;
        f->label = -1;
        for (int k = 0; k < b->nargs; k++) {
            copy_str_local(f->pname[k], "arg", 32);
            f->ptype[k] = type_int();
        }
        func_n++;
    }
}

/* ============================================================
 * 7. 全局变量的数据布局
 *
 * 有初值的进数据区（跟代码一起写进文件），没初值的进 bss 区
 * （只在 LXE 头部记一个长度，加载时清零）—— 一个 char buf[65536];
 * 不会让 .lxe 白白胖 64KB。
 * ============================================================ */

/* 把全局初值折叠成常数；不是常数就报错（这个子集不支持动态初始化） */
static int const_eval(node_t* n, int* ok) {
    if (!n) {
        *ok = 0;
        return 0;
    }
    if (n->kind == ND_NUM) {
        *ok = 1;
        return n->num;
    }
    if (n->kind == ND_NEG) {
        int v = const_eval(n->a, ok);

        return -v;
    }
    if (n->kind == ND_BITNOT) {
        int v = const_eval(n->a, ok);

        return ~v;
    }
    if (n->kind == ND_CAST) {
        return const_eval(n->a, ok);
    }
    if (n->kind == ND_BIN) {
        int aok = 0;
        int bok = 0;
        int a = const_eval(n->a, &aok);
        int b = const_eval(n->b, &bok);

        if (!aok || !bok) {
            *ok = 0;
            return 0;
        }
        *ok = 1;
        if (strcmp(n->opstr, "+") == 0)  return a + b;
        if (strcmp(n->opstr, "-") == 0)  return a - b;
        if (strcmp(n->opstr, "*") == 0)  return a * b;
        if (strcmp(n->opstr, "/") == 0)  return b ? a / b : 0;
        if (strcmp(n->opstr, "%") == 0)  return b ? a % b : 0;
        if (strcmp(n->opstr, "&") == 0)  return a & b;
        if (strcmp(n->opstr, "|") == 0)  return a | b;
        if (strcmp(n->opstr, "^") == 0)  return a ^ b;
        if (strcmp(n->opstr, "<<") == 0) return a << b;
        if (strcmp(n->opstr, ">>") == 0) return a >> b;
    }
    *ok = 0;
    return 0;
}

/* 字符串字面量进数据区，返回它的数据区偏移 */
static int intern_string(const char* s, int len) {
    int at = data_emit_bytes(s, len + 1);   /* 含结尾的 '\0' */

    data[data_len - 1] = 0;
    return at;
}

static void layout_globals(void) {
    for (int i = 0; i < global_decl_n; i++) {
        node_t* d = global_decls[i];
        struct sym* s = d->sym;
        vtype_t t = sym_vtype(s);

        /* 4 字节对齐：x86 允许非对齐访问，但让每个 int / 指针都落在
         * 自己的边界上，反汇编出来也好读。 */
        while (data_len % 4) {
            unsigned char z = 0;

            data_emit_bytes(&z, 1);
        }

        if (!d->a) {
            /* 没初值：bss */
            int size = type_size(t);

            size = (size + 3) & ~3;
            s->is_bss = 1;
            s->offset = bss_rel_len;
            bss_rel_len += size;
            continue;
        }

        if (d->a->kind == ND_INITLIST) {
            /* 全局数组的初始化列表：元素必须是常量，直接铺进数据区，
             * 没写到的位置留 0（data_emit_zeros 已经把整块清零了）。 */
            vtype_t el_ty = type_element(t);
            int esize = type_size(el_ty);
            int total = type_size(t);
            int at = data_emit_zeros(total);
            int i = 0;

            s->offset = at;
            for (node_t* e = d->a->a; e && i < d->a->num; e = e->next, i++) {
                int ok = 0;
                int v = const_eval(e, &ok);

                if (!ok) {
                    fatal("initialiser for '%s' must be a constant", s->name);
                }
                if (esize == 1) {
                    data[at + i] = (unsigned char)v;
                } else if (esize == 4) {
                    patch32_data(at + i * 4, (uint32_t)v);
                } else {
                    fatal("struct elements in an initialiser list are not "
                          "supported (assign the members instead)");
                }
            }
            continue;
        }

        if (d->a->kind == ND_STR) {
            int slen = d->a->str_len;
            int str_off = intern_string(d->a->str, slen);

            if (t.ptr > 0) {
                /* char* s = "abc";  数据里存一个地址 */
                int at = data_emit32(0);

                s->offset = at;
                data_ref_word(at, str_off);
            } else {
                /* char buf[N] = "abc";  内容直接铺进去，不够的补 0 */
                int total = type_size(t);
                int at = data_emit_zeros(total);

                s->offset = at;
                if (slen > total) {
                    fatal("initialiser for '%s' is longer than the array", s->name);
                }
                memcpy(data + at, d->a->str, (size_t)slen);
            }
            continue;
        }

        /* 数字常量 */
        {
            int ok = 0;
            int v = const_eval(d->a, &ok);

            if (!ok) {
                fatal("global initialiser for '%s' must be a constant", s->name);
            }
            if (t.ptr > 0 || base_size(t.base) == 4) {
                s->offset = data_emit32((uint32_t)v);
            } else {
                unsigned char b = (unsigned char)v;

                s->offset = data_emit_bytes(&b, 1);
            }
        }
    }
}

/* ============================================================
 * 8. 函数发射
 * ============================================================ */

static void gen_function(int fi) {
    struct sym_func* f = &funcs[fi];
    int frame = 0;
    int epi;

    if (!f->body) {
        return;
    }

    /* 形参按 cdecl 就在 [ebp+8+4i]。
     * 注意这里改的是解析阶段建好的那批符号对象本身 —— 语法树里
     * ND_VAR 存的就是它们的指针，另建一份新的等于让所有引用都落空。 */
    for (int i = 0; i < f->nparams; i++) {
        if (f->param_sym[i]) {
            f->param_sym[i]->offset = 8 + 4 * i;
            f->param_sym[i]->kind = SYM_PARAM;
        }
    }
    assign_locals(f->body->a, &frame);
    frame = (frame + 15) & ~15;

    if (f->label < 0) {
        f->label = new_label();
    }
    place_label(f->label);

    e_push(R_EBP);
    e_mov_rr(R_EBP, R_ESP);
    if (frame > 0) {
        e_sub_esp_imm((uint32_t)frame);
    }

    epi = new_label();
    cur_epilogue = epi;
    loop_depth = 0;

    gen_block(f->body);

    /* 没有 return 就直接走到末尾：返回 0 */
    e_mov_r_imm(R_EAX, 0);
    place_label(epi);
    e_leave();
    e_ret();

    cur_epilogue = -1;
}

/* ============================================================
 * 9. 自带运行时（用这个子集写的 C 源码） *
 * 编译用户代码之前先把它编译一遍：既是"标准库"，也是每次编译
 * 都跑一遍的自测。保留字前缀 rt_ 是运行时自己用的全局变量。
 * ============================================================ */

static const char* RT_SOURCE =
    "char* rt_hexdigits = \"0123456789abcdef\";\n"
    "char* rt_heap_base = 0;\n"
    "int rt_heap_used = 0;\n"
    "int rt_heap_have = 0;\n"
    "\n"
    "int strlen(char* s) {\n"
    "    int n = 0;\n"
    "    while (s[n] != 0) { n = n + 1; }\n"
    "    return n;\n"
    "}\n"
    "\n"
    "int strcmp(char* a, char* b) {\n"
    "    int i = 0;\n"
    "    while (a[i] != 0 && a[i] == b[i]) { i = i + 1; }\n"
    "    return a[i] - b[i];\n"
    "}\n"
    "\n"
    "int strncmp(char* a, char* b, int n) {\n"
    "    int i = 0;\n"
    "    while (i < n && a[i] != 0 && a[i] == b[i]) { i = i + 1; }\n"
    "    if (i == n) { return 0; }\n"
    "    return a[i] - b[i];\n"
    "}\n"
    "\n"
    "char* strcpy(char* d, char* s) {\n"
    "    int i = 0;\n"
    "    while (s[i] != 0) { d[i] = s[i]; i = i + 1; }\n"
    "    d[i] = 0;\n"
    "    return d;\n"
    "}\n"
    "\n"
    "char* strncpy(char* d, char* s, int n) {\n"
    "    int i = 0;\n"
    "    while (i < n && s[i] != 0) { d[i] = s[i]; i = i + 1; }\n"
    "    while (i < n) { d[i] = 0; i = i + 1; }\n"
    "    return d;\n"
    "}\n"
    "\n"
    "void* memset(void* p, int c, int n) {\n"
    "    char* q = (char*)p;\n"
    "    int i = 0;\n"
    "    while (i < n) { q[i] = (char)c; i = i + 1; }\n"
    "    return p;\n"
    "}\n"
    "\n"
    "void* memcpy(void* d, void* s, int n) {\n"
    "    char* dd = (char*)d;\n"
    "    char* ss = (char*)s;\n"
    "    int i = 0;\n"
    "    while (i < n) { dd[i] = ss[i]; i = i + 1; }\n"
    "    return d;\n"
    "}\n"
    "\n"
    "int isdigit(int c) { return c >= '0' && c <= '9'; }\n"
    "int isspace(int c) { return c == ' ' || c == 9 || c == 10 || c == 13; }\n"
    "\n"
    "int atoi(char* s) {\n"
    "    int i = 0;\n"
    "    int v = 0;\n"
    "    int neg = 0;\n"
    "    while (isspace(s[i])) { i = i + 1; }\n"
    "    if (s[i] == '-') { neg = 1; i = i + 1; }\n"
    "    while (isdigit(s[i])) { v = v * 10 + (s[i] - '0'); i = i + 1; }\n"
    "    return neg ? 0 - v : v;\n"
    "}\n"
    "\n"
    "int puts(char* s) {\n"
    "    int i = 0;\n"
    "    while (s[i] != 0) { putchar(s[i]); i = i + 1; }\n"
    "    putchar(10);\n"
    "    return 0;\n"
    "}\n"
    "\n"
    "void print_int(int v) {\n"
    "    char buf[16];\n"
    "    int i = 0;\n"
    "    int neg = 0;\n"
    "    if (v < 0) { neg = 1; v = 0 - v; }\n"
    "    if (v == 0) { putchar('0'); return; }\n"
    "    while (v > 0) {\n"
    "        buf[i] = (char)('0' + v % 10);\n"
    "        i = i + 1;\n"
    "        v = v / 10;\n"
    "    }\n"
    "    if (neg) { putchar('-'); }\n"
    "    while (i > 0) { i = i - 1; putchar(buf[i]); }\n"
    "}\n"
    "\n"
    "void print_hex(int v) {\n"
    "    char buf[16];\n"
    "    int i = 0;\n"
    "    int u = v;\n"
    "    int left = 0;\n"
    "    if (u < 0) { left = 8; }\n"
    "    if (u == 0) { putchar('0'); return; }\n"
    "    while (i < 15 && (u > 0 || left > 0)) {\n"
    "        buf[i] = rt_hexdigits[u & 15];\n"
    "        u = u >> 4;\n"
    "        i = i + 1;\n"
    "        if (left > 0) { left = left - 1; }\n"
    "    }\n"
    "    while (i > 0) { i = i - 1; putchar(buf[i]); }\n"
    "}\n"
    "\n"
    "void* malloc(int n) {\n"
    "    char* p;\n"
    "    int need = 0;\n"
    "    if (n <= 0) { return 0; }\n"
    "    n = (n + 7) & (0 - 8);\n"
    "    if (rt_heap_base == 0) {\n"
    "        rt_heap_base = (char*)sbrk(0);\n"
    "        if (rt_heap_base == 0) { return 0; }\n"
    "    }\n"
    "    if (rt_heap_used + n > rt_heap_have) {\n"
    "        need = rt_heap_used + n - rt_heap_have;\n"
    "        need = (need + 4095) / 4096 * 4096;\n"
    "        if (sbrk(need) == 0) { return 0; }\n"
    "        rt_heap_have = rt_heap_have + need;\n"
    "    }\n"
    "    p = rt_heap_base + rt_heap_used;\n"
    "    rt_heap_used = rt_heap_used + n;\n"
    "    return (void*)p;\n"
    "}\n"
    "\n"
    "void free(void* p) {\n"
    "    p = p;\n"
    "}\n"
    "\n"
    "/* ---- 字符串函数 ---- */\n"
    "\n"
    "char* strcat(char* d, char* s) {\n"
    "    int i = strlen(d);\n"
    "    int j = 0;\n"
    "    while (s[j] != 0) { d[i] = s[j]; i = i + 1; j = j + 1; }\n"
    "    d[i] = 0;\n"
    "    return d;\n"
    "}\n"
    "\n"
    "char* strchr(char* s, int c) {\n"
    "    int i = 0;\n"
    "    while (s[i] != 0) {\n"
    "        if (s[i] == c) { return s + i; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    if (c == 0) { return s + i; }\n"
    "    return 0;\n"
    "}\n"
    "\n"
    "char* strrchr(char* s, int c) {\n"
    "    int i = 0;\n"
    "    char* hit = 0;\n"
    "    while (s[i] != 0) {\n"
    "        if (s[i] == c) { hit = s + i; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    if (c == 0) { return s + i; }\n"
    "    return hit;\n"
    "}\n"
    "\n"
    "char* strstr(char* hay, char* needle) {\n"
    "    int i = 0;\n"
    "    if (needle[0] == 0) { return hay; }\n"
    "    while (hay[i] != 0) {\n"
    "        int j = 0;\n"
    "        while (needle[j] != 0 && hay[i + j] == needle[j]) { j = j + 1; }\n"
    "        if (needle[j] == 0) { return hay + i; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"
    "\n"
    "int memcmp(void* a, void* b, int n) {\n"
    "    char* x = (char*)a;\n"
    "    char* y = (char*)b;\n"
    "    int i = 0;\n"
    "    while (i < n) {\n"
    "        if (x[i] != y[i]) { return x[i] - y[i]; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"
    "\n"
    "/* ---- 字符串与格式化输出 ---- */\n"
    "\n"
    "void putstr(char* s) {\n"
    "    int i = 0;\n"
    "    while (s[i] != 0) { putchar(s[i]); i = i + 1; }\n"
    "}\n"
    "\n"
    "void print_uint(int v) {\n"
    "    char buf[16];\n"
    "    int i = 0;\n"
    "    if (v < 0) { print_int(v); return; }\n"
    "    if (v == 0) { putchar('0'); return; }\n"
    "    while (v > 0 && i < 15) {\n"
    "        buf[i] = (char)('0' + v % 10);\n"
    "        i = i + 1;\n"
    "        v = v / 10;\n"
    "    }\n"
    "    while (i > 0) { i = i - 1; putchar(buf[i]); }\n"
    "}\n"
    "\n"
    "/* printf：变参是靠从参数在栈上的位置往后数实现的（不能直接写引号，\n"
    " * 这是嵌在 C 字符串里的源码）。\n"
    " * 调用约定是参数从右往左压栈，所以第一个参数在最低地址，\n"
    " * 紧跟其后的字就是第一个可变参数 —— 和 user/lib 里那套是同一个技巧，\n"
    " * 只不过这里是用 hncc 自己编译出来的代码。\n"
    " * 支持 %d %u %x %c %s %%，返回值恒为 0（不统计字符数）。\n"
    " */\n"
    "int printf(char* fmt, ...) {\n"
    "    int* args;\n"
    "    int i = 0;\n"
    "\n"
    "    args = &fmt;\n"
    "    args = args + 1;\n"
    "\n"
    "    while (fmt[i] != 0) {\n"
    "        if (fmt[i] != '%') {\n"
    "            putchar(fmt[i]);\n"
    "            i = i + 1;\n"
    "            continue;\n"
    "        }\n"
    "        i = i + 1;\n"
    "        if (fmt[i] == 'd') { print_int(*args); args = args + 1; }\n"
    "        else if (fmt[i] == 'u') { print_uint(*args); args = args + 1; }\n"
    "        else if (fmt[i] == 'x') { print_hex(*args); args = args + 1; }\n"
    "        else if (fmt[i] == 'c') { putchar(*args); args = args + 1; }\n"
    "        else if (fmt[i] == 's') { putstr((char*)*args); args = args + 1; }\n"
    "        else if (fmt[i] == '%') { putchar('%'); }\n"
    "        else if (fmt[i] == 0) { break; }\n"
    "        else { putchar('%'); putchar(fmt[i]); }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

/* ============================================================
 * 10. 收尾：回填、写 LXE 文件
 * ============================================================ */

#define USER_BASE 0x40000000u

static void patch_all(void) {
    /* 代码里的绝对地址：基址 + 代码长度 + 数据区偏移 */
    for (int i = 0; i < data_fix_n; i++) {
        uint32_t addr;
        int at_in_data = data_fix[i].in_data;

        if (data_fix[i].is_bss) {
            addr = USER_BASE + (uint32_t)code_len + (uint32_t)data_len +
                   (uint32_t)data_fix[i].bss_rel;
        } else {
            addr = USER_BASE + (uint32_t)code_len +
                   (uint32_t)data_fix[i].target;
        }
        if (at_in_data) {
            patch32_data(data_fix[i].at, addr);
        } else {
            patch32(data_fix[i].at, addr);
        }
    }
}

/* 数据区里的 4 字节（全局指针初值）也要回填 */
static void patch32_data(int at, uint32_t v) {
    data[at + 0] = (unsigned char)(v & 0xFF);
    data[at + 1] = (unsigned char)((v >> 8) & 0xFF);
    data[at + 2] = (unsigned char)((v >> 16) & 0xFF);
    data[at + 3] = (unsigned char)((v >> 24) & 0xFF);
}

/* 入口桩：call main; mov ebx, eax; xor eax, eax; int 0x80; hlt
 * （和 user/crt0.asm 干的活一样，只是这里是自己发机器码） */
static void emit_entry_stub(void) {
    int main_fi = find_func("main");

    if (main_fi < 0) {
        fatal("this program has no main() function");
    }
    if (!funcs[main_fi].body) {
        fatal("main() is declared but never defined");
    }
    if (funcs[main_fi].label < 0) {
        funcs[main_fi].label = new_label();
    }

    e_call_label(funcs[main_fi].label);    /* E8 rel32 */
    e_mov_rr(R_EBX, R_EAX);                /* 89 C3 */
    e_alu_rr(ALU_XOR, R_EAX, R_EAX);       /* 31 C0 */
    e_int80();                             /* CD 80 */
    e_hlt();                               /* F4 */
}

static int write_file_all(int fd, const unsigned char* p, int n) {
    int done = 0;

    while (done < n) {
        int w = write(fd, p + done, (size_t)(n - done));

        if (w <= 0) {
            return -1;
        }
        done += w;
    }
    return 0;
}

static int write_lxe(const char* path, const char* prog_name) {
    lxe_header_t h;
    int fd;

    memset(&h, 0, sizeof(h));
    h.magic    = LXE_MAGIC;
    h.version  = LXE_VERSION;
    h.entry    = 0;
    h.size     = (uint32_t)(code_len + data_len);
    h.bss_size = (uint32_t)bss_rel_len;
    h.flags    = LXE_FLAG_CONSOLE;
    copy_str_local(h.name, prog_name, (int)sizeof(h.name));

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        err("cannot create the output file", path);
        return -1;
    }
    if (write_file_all(fd, (const unsigned char*)&h, (int)sizeof(h)) != 0 ||
        write_file_all(fd, code, code_len) != 0 ||
        write_file_all(fd, data, data_len) != 0) {
        close(fd);
        err("write failed", path);
        return -1;
    }
    fsync(fd);
    close(fd);

    /* 刚建出来的文件是默认的 0644，没有执行位 —— 非 root 用户就跑不起来。
     * 编译器的产物应该是可直接执行的，所以这里自己补上。 */
    if (chmod(path, 0x1ED /* 0755 */) != 0) {
        err("compiled, but cannot set the execute bit on", path);
    }
    return 0;
}

static void err(const char* what, const char* detail) {
    term_set_fg(TERM_LIGHT_RED);
    fputs("hncc: ", STDERR_FILENO);
    fputs(what, STDERR_FILENO);
    if (detail && detail[0]) {
        fputs(": ", STDERR_FILENO);
        fputs(detail, STDERR_FILENO);
    }
    fputc('\n', STDERR_FILENO);
    term_reset();
}

static void usage(void) {
    puts("hncc - the HNeoC C compiler");
    puts("");
    puts("  hncc <source.c> [-o <output.lxe>]");
    puts("");
    puts("Compiles a C subset straight to a runnable .lxe (x86 machine code).");
    puts("putchar / getchar / open / read / write / sbrk / ... are built in,");
    puts("so no headers and no libraries are needed.");
    puts("");
    puts("Supported: int char void, pointers, arrays, functions,");
    puts("           if/else while do-while for return break continue,");
    puts("           the usual operators, string and char literals.");
    puts("Not supported: struct, float, long long, varargs, real preprocessing.");
}

/* ------------------------------------------------------------
 * 解析一段源码（运行时的内建源码或者用户的文件都走这里）
 * ------------------------------------------------------------ */
static void parse_program(void) {
    parsing_global = 1;
    lex_next();
    while (tok.kind != TK_EOF) {
        parse_declaration(1);
    }
}

int main(void) {
    char args[256];
    char src_path[128];
    char out_path[128];
    char prog_name[16];
    char* src_buf;
    int src_len;
    int have_out = 0;
    int i = 0;
    int n = 0;

    /* --- 参数 --- */
    getargs(args, sizeof(args));
    src_path[0] = '\0';
    out_path[0] = '\0';

    while (args[i]) {
        while (args[i] == ' ' || args[i] == '\t') {
            i++;
        }
        if (!args[i]) {
            break;
        }
        if (args[i] == '-' && args[i + 1] == 'o') {
            i += 2;
            while (args[i] == ' ' || args[i] == '\t') {
                i++;
            }
            n = 0;
            while (args[i] && args[i] != ' ' && args[i] != '\t' &&
                   n < (int)sizeof(out_path) - 1) {
                out_path[n++] = args[i++];
            }
            out_path[n] = '\0';
            have_out = 1;
            continue;
        }
        if (src_path[0] == '\0') {
            n = 0;
            while (args[i] && args[i] != ' ' && args[i] != '\t' &&
                   n < (int)sizeof(src_path) - 1) {
                src_path[n++] = args[i++];
            }
            src_path[n] = '\0';
            continue;
        }
        err("unexpected argument", args + i);
        return 1;
    }

    if (src_path[0] == '\0') {
        usage();
        return 1;
    }

    /* 输出名：默认把 .c 换成 .lxe */
    if (!have_out) {
        int k = 0;

        while (src_path[k] && k < (int)sizeof(out_path) - 6) {
            out_path[k] = src_path[k];
            k++;
        }
        if (k >= 2 && out_path[k - 1] == 'c' && out_path[k - 2] == '.') {
            k -= 2;
        }
        out_path[k++] = '.';
        out_path[k++] = 'l';
        out_path[k++] = 'x';
        out_path[k++] = 'e';
        out_path[k] = '\0';
    }
    /* 程序名（LXE 头部里那个）取输出文件的主名 */
    {
        int k = 0;
        int last = -1;

        while (out_path[k]) {
            if (out_path[k] == '/') {
                last = k;
            }
            k++;
        }
        copy_str_local(prog_name, out_path + last + 1, sizeof(prog_name));
        for (int j = 0; prog_name[j]; j++) {
            if (prog_name[j] == '.') {
                prog_name[j] = '\0';
                break;
            }
        }
    }

    /* --- 读源码 --- */
    src_buf = (char*)malloc(SRC_CAP);
    code    = (unsigned char*)malloc(CODE_CAP);
    data    = (unsigned char*)malloc(DATA_CAP);
    arena   = (char*)malloc(ARENA_CAP);
    if (!src_buf || !code || !data || !arena) {
        err("out of memory", NULL);
        return 1;
    }
    arena_size = ARENA_CAP;

    src_len = readfile(src_path, src_buf, SRC_CAP - 1);
    if (src_len < 0) {
        err("cannot read the source file", src_path);
        return 1;
    }
    src_buf[src_len] = '\0';

    /* --- 编译 --- */
    init_labels();
    register_builtins();

    /* 几个到处都在用的名字，直接当内建常量给出来。
     * 用户的 #define 或变量会覆盖它们（常量表先于变量查找），
     * 所以想用同名的变量就自己 #define 一下。 */
    add_constant("NULL", 0);
    add_constant("true", 1);
    add_constant("false", 0);

    src_name = "<hncc runtime>";
    lex_p = RT_SOURCE;
    src_line = 1;
    parse_program();

    src_name = src_path;
    lex_p = src_buf;
    src_line = 1;
    parse_program();

    /* 布局：先全局数据，再函数体 */
    layout_globals();

    code_len = 0;
    for (int fi = 0; fi < func_n; fi++) {
        if (funcs[fi].body && funcs[fi].label < 0) {
            funcs[fi].label = new_label();
        }
    }
    emit_entry_stub();
    for (int fi = 0; fi < func_n; fi++) {
        gen_function(fi);
    }

    /* 代码生成完了，不允许还有没回填的跳转/调用。
     *
     * 这一条是补上一个真实踩过的坑：向后引用的位移如果没算，机器码里就是
     * `e8 00 00 00 00`（跳到下一条指令），编译照样"成功"，直到运行起来
     * 空指针缺页才发现。宁可在这里当场报错。
     */
    for (int i = 0; i < jump_fix_n; i++) {
        if (jump_fix[i].label >= 0) {
            fatal("internal error: jump/call to label %d was never resolved",
                  jump_fix[i].label);
        }
    }

    patch_all();

    if (write_lxe(out_path, prog_name) != 0) {
        return 1;
    }

    printf("compiled %s\n", src_path);
    printf("  code   : %d bytes\n", code_len);
    printf("  data   : %d bytes\n", data_len);
    printf("  bss    : %d bytes\n", bss_rel_len);
    printf("  output : %s\n", out_path);
    return 0;
}
