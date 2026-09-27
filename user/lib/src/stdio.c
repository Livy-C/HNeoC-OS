#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <hneoc.h>

/* ============================================================
 * 格式化输出的核心
 *
 * printf / fprintf / snprintf 共用同一个格式化函数，区别只在于
 * 字符往哪儿送：一个"水槽"要么写进调用者的缓冲区，要么攒在
 * 内部缓冲里等满了再用一次 write 发出去。
 *
 * 参数表用裸指针手动前进模拟：我们没带 stdarg.h。
 * x86 cdecl 下可变参数在栈上连续排布，所以 &fmt + 1 就是第一个。
 * ============================================================ */

#define PRINTF_BUF 256

typedef struct {
    char   buf[PRINTF_BUF];   /* 写 fd 时的攒批缓冲 */
    size_t len;
    int    fd;                /* >= 0：写这个 fd；-1：写调用者缓冲区 */
    char*  out;
    size_t outmax;
    size_t outlen;
    size_t total;             /* 已经"产出"的字符总数，含被截断的 */
} sink_t;

static void sink_putc(sink_t* s, char c) {
    s->total++;

    if (s->fd >= 0) {
        s->buf[s->len++] = c;
        if (s->len >= sizeof(s->buf)) {
            write(s->fd, s->buf, s->len);
            s->len = 0;
        }
    } else {
        if (s->outlen + 1 < s->outmax) {
            s->out[s->outlen++] = c;
            s->out[s->outlen]  = '\0';
        }
    }
}

static void sink_flush(sink_t* s) {
    if (s->fd >= 0 && s->len > 0) {
        write(s->fd, s->buf, s->len);
        s->len = 0;
    }
}

static void sink_puts(sink_t* s, const char* str, int max) {
    int i = 0;

    while (str[i] && (max < 0 || i < max)) {
        sink_putc(s, str[i]);
        i++;
    }
}

/* 带宽度和对齐的字符串输出。
 * 少了这个，"%-40s" 里的 40 个空格会被整段丢掉，
 * 后面靠长度对齐的界面就全歪了。
 */
static void sink_puts_padded(sink_t* s, const char* str, int max,
                             int width, int left_align) {
    int n = 0;

    while (str[n] && (max < 0 || n < max)) {
        n++;
    }

    if (!left_align) {
        for (int i = n; i < width; i++) {
            sink_putc(s, ' ');
        }
    }
    for (int i = 0; i < n; i++) {
        sink_putc(s, str[i]);
    }
    if (left_align) {
        for (int i = n; i < width; i++) {
            sink_putc(s, ' ');
        }
    }
}

static void sink_put_uint(sink_t* s, uint32_t v, int base, int upper,
                          int width, int left_align, char pad) {
    char tmp[33];
    int n = 0;
    int digits = 0;
    uint32_t t = v;

    if (t == 0) {
        tmp[n++] = '0';
    } else {
        while (t > 0 && n < 32) {
            uint32_t d = t % (uint32_t)base;

            tmp[n++] = (char)(d < 10 ? '0' + d
                                     : (upper ? 'A' : 'a') + (d - 10));
            t /= (uint32_t)base;
        }
    }
    digits = n;

    if (!left_align) {
        for (int i = digits; i < width; i++) {
            sink_putc(s, pad);
        }
    }
    while (n > 0) {
        sink_putc(s, tmp[--n]);
    }
    if (left_align) {
        for (int i = digits; i < width; i++) {
            sink_putc(s, pad);
        }
    }
}

static void sink_put_int(sink_t* s, int32_t v, int width, int left_align,
                         char pad) {
    if (v < 0) {
        sink_putc(s, '-');
        if (width > 0) {
            width--;
        }
        sink_put_uint(s, (uint32_t)(-v), 10, 0, width, left_align, pad);
    } else {
        sink_put_uint(s, (uint32_t)v, 10, 0, width, left_align, pad);
    }
}

/* 核心：返回产出的字符总数 */
static int format(sink_t* s, const char* fmt, uint32_t* args) {
    int argi = 0;

    while (*fmt) {
        int width = 0;
        int precision = -1;
        int left_align = 0;
        char pad = ' ';

        if (*fmt != '%') {
            sink_putc(s, *fmt++);
            continue;
        }
        fmt++;

        /* 标志位 */
        for (;;) {
            if (*fmt == '-') { left_align = 1; fmt++; }
            else if (*fmt == '0') { pad = '0'; fmt++; }
            else { break; }
        }

        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }

        /* 精度：%.40s 这种写法一定要支持，否则参数会整体错位 */
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt++ - '0');
            }
        }

        /* 长度修饰符这里不需要区分，32 位平台上直接忽略 */
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            fmt++;
        }

        switch (*fmt) {
            case 'd':
            case 'i':
                sink_put_int(s, (int32_t)args[argi++], width, left_align, pad);
                break;
            case 'u':
                sink_put_uint(s, args[argi++], 10, 0, width, left_align, pad);
                break;
            case 'x':
                sink_put_uint(s, args[argi++], 16, 0, width, left_align, pad);
                break;
            case 'X':
                sink_put_uint(s, args[argi++], 16, 1, width, left_align, pad);
                break;
            case 'o':
                sink_put_uint(s, args[argi++], 8, 0, width, left_align, pad);
                break;
            case 'p':
                sink_puts(s, "0x", -1);
                sink_put_uint(s, args[argi++], 16, 0, 8, 0, '0');
                break;
            case 's': {
                const char* str = (const char*)args[argi++];

                if (!str) {
                    str = "(null)";
                }
                sink_puts_padded(s, str, precision, width, left_align);
                break;
            }
            case 'c':
                sink_putc(s, (char)args[argi++]);
                break;
            case '%':
                sink_putc(s, '%');
                break;
            case '\0':
                sink_flush(s);
                return (int)s->total;
            default:
                sink_putc(s, *fmt);
                break;
        }
        fmt++;
    }

    sink_flush(s);
    return (int)s->total;
}

/* ============================================================
 * 对外接口
 * ============================================================ */

int vsnprintf(char* buf, size_t max, const char* fmt, void* args) {
    sink_t s;

    s.fd = -1;
    s.out = buf;
    s.outmax = max;
    s.outlen = 0;
    s.total = 0;

    if (max == 0) {
        return 0;
    }
    buf[0] = '\0';

    return format(&s, fmt, (uint32_t*)args);
}

int snprintf(char* buf, size_t max, const char* fmt, ...) {
    uint32_t* args = (uint32_t*)(&fmt) + 1;
    return vsnprintf(buf, max, fmt, args);
}

int fprintf(int fd, const char* fmt, ...) {
    uint32_t* args = (uint32_t*)(&fmt) + 1;
    sink_t s;

    s.fd = fd;
    s.len = 0;
    s.out = NULL;
    s.outmax = 0;
    s.outlen = 0;
    s.total = 0;

    return format(&s, fmt, args);
}

int printf(const char* fmt, ...) {
    uint32_t* args = (uint32_t*)(&fmt) + 1;
    sink_t s;

    s.fd = STDOUT_FILENO;
    s.len = 0;
    s.out = NULL;
    s.outmax = 0;
    s.outlen = 0;
    s.total = 0;

    return format(&s, fmt, args);
}

int putchar(int c) {
    char ch = (char)c;

    write(STDOUT_FILENO, &ch, 1);
    return c;
}

int fputc(int c, int fd) {
    char ch = (char)c;

    write(fd, &ch, 1);
    return c;
}

int puts(const char* s) {
    size_t n = strlen(s);

    /* 一次写完字符串再补换行，比逐字符快得多 */
    if (n > 0) {
        write(STDOUT_FILENO, s, n);
    }
    write(STDOUT_FILENO, "\n", 1);
    return (int)n + 1;
}

int fputs(const char* s, int fd) {
    size_t n = strlen(s);

    if (n > 0) {
        write(fd, s, n);
    }
    return (int)n;
}
