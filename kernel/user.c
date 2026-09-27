#include "../include/user.h"
#include "../include/hneofs.h"
#include "../include/process.h"
#include "../include/keyboard.h"
#include "../include/vga.h"
#include "../include/string.h"

/* ============================================================
 * 账户与登录
 *
 * 账户表放在磁盘上的 /etc/passwd，登录时整份读进这块缓冲区再逐行解析。
 * 文件很小（几十行），不值得为它做索引。
 * ============================================================ */

#define PASSWD_MAX   2048
#define PASSWD_PATH  "/etc/passwd"
#define HOME_ROOT    "/home"

static char passwd_buf[PASSWD_MAX];
static char current_name[USER_NAME_MAX] = "?";

/* ------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------ */

/* djb2。故意选最朴素的散列：这里的目标只是"别把口令明文写在盘上" */
uint32_t user_hash_password(const char* s) {
    uint32_t h = 5381;

    while (s && *s) {
        h = (h << 5) + h + (uint32_t)(uint8_t)(*s);
        s++;
    }
    return h;
}

static void hex8(char* out, uint32_t v) {
    const char* d = "0123456789abcdef";

    for (int i = 7; i >= 0; i--) {
        out[i] = d[v & 0xF];
        v >>= 4;
    }
    out[8] = '\0';
}

static uint32_t parse_u32(const char* s) {
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/* 读一行键盘输入。echo=0 时只回显 '*'（口令用） */
static void read_line(char* out, uint32_t max, bool echo) {
    uint32_t n = 0;

    if (max == 0) {
        return;
    }
    out[0] = '\0';

    for (;;) {
        int c = keyboard_getchar();

        if (c == '\r' || c == '\n') {
            out[n] = '\0';
            vga_writeln("");
            return;
        }
        if (c == 8 || c == 127) {                  /* 退格 */
            if (n > 0) {
                n--;
                out[n] = '\0';
                vga_write("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && n + 1 < max) {
            out[n++] = (char)c;
            vga_putchar(echo ? (char)c : '*');
        }
    }
}

/* ------------------------------------------------------------
 * 账户表
 * ------------------------------------------------------------ */

/* 把 /etc/passwd 读进静态缓冲区。返回字节数，失败返回 -1 */
static int passwd_load(void) {
    const hneofs_file_t* f = hneofs_lookup(PASSWD_PATH);
    int32_t got;

    if (!f || f->type != HNEOFS_TYPE_FILE) {
        return -1;
    }
    if (f->size >= PASSWD_MAX) {
        return -1;
    }

    got = hneofs_read_file(f, passwd_buf, PASSWD_MAX);
    if (got < 0) {
        return -1;
    }
    passwd_buf[got] = '\0';
    return (int)got;
}

/* 取一个冒号分隔的字段；返回值指向字段之后 */
static const char* next_field(const char* p, char* out, uint32_t max) {
    uint32_t n = 0;

    while (*p && *p != ':' && *p != '\n' && *p != '\r') {
        if (n + 1 < max) {
            out[n++] = *p;
        }
        p++;
    }
    out[n] = '\0';
    if (*p == ':') {
        p++;
    }
    return p;
}

/* 逐行找账户。传 NULL 表示那个字段不关心 */
static bool find_user(const char* name,
                      char* hash_out, uint32_t hash_max,
                      uint32_t* uid_out,
                      char* home_out, uint32_t home_max,
                      bool* admin_out) {
    const char* p = passwd_buf;

    while (*p) {
        char f_name[USER_NAME_MAX];
        char f_hash[16];
        char f_uid[12];
        char f_home[USER_HOME_MAX];
        char f_admin[4];
        const char* q;

        /* 跳过空行、空白和注释 */
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#') {
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (*p == '\0') {
            break;
        }

        q = p;
        q = next_field(q, f_name,  sizeof(f_name));
        q = next_field(q, f_hash,  sizeof(f_hash));
        q = next_field(q, f_uid,   sizeof(f_uid));
        q = next_field(q, f_home,  sizeof(f_home));
        q = next_field(q, f_admin, sizeof(f_admin));

        if (strcmp(f_name, name) == 0) {
            if (hash_out) {
                strncpy(hash_out, f_hash, hash_max - 1);
                hash_out[hash_max - 1] = '\0';
            }
            if (uid_out) {
                *uid_out = parse_u32(f_uid);
            }
            if (home_out) {
                strncpy(home_out, f_home, home_max - 1);
                home_out[home_max - 1] = '\0';
            }
            if (admin_out) {
                *admin_out = (f_admin[0] == '1');
            }
            return true;
        }

        while (*q && *q != '\n') {   /* 走到这一行末尾 */
            q++;
        }
        p = q;
    }
    return false;
}

/* 下一个可用的 uid：现有最大值 + 1，至少 1000 */
static uint32_t next_uid(void) {
    const char* p = passwd_buf;
    uint32_t max = 999;

    while (*p) {
        char f_name[USER_NAME_MAX];
        char f_hash[16];
        char f_uid[12];
        char f_home[USER_HOME_MAX];
        char f_admin[4];
        const char* q;
        uint32_t u;

        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { p++; }
        if (*p == '#') {
            while (*p && *p != '\n') { p++; }
            continue;
        }
        if (*p == '\0') { break; }

        q = p;
        q = next_field(q, f_name,  sizeof(f_name));
        q = next_field(q, f_hash,  sizeof(f_hash));
        q = next_field(q, f_uid,   sizeof(f_uid));
        q = next_field(q, f_home,  sizeof(f_home));
        q = next_field(q, f_admin, sizeof(f_admin));

        u = parse_u32(f_uid);
        if (u > max) {
            max = u;
        }

        while (*q && *q != '\n') { q++; }
        p = q;
    }
    return max + 1;
}

/* ------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------ */

const char* user_name(void) {
    return current_name;
}

uint32_t user_uid_of_name(const char* name) {
    uint32_t uid = 0;

    if (passwd_load() < 0) {
        return 0xFFFFFFFFu;
    }
    if (find_user(name, 0, 0, &uid, 0, 0, 0)) {
        return uid;
    }
    return 0xFFFFFFFFu;
}

bool user_home_of(const char* name, char* out, uint32_t max) {
    if (passwd_load() < 0) {
        return false;
    }
    return find_user(name, 0, 0, 0, out, max, 0);
}

bool user_is_admin(void) {
    process_t* p = process_current();

    if (p && p->uid == 0) {
        return true;      /* root 永远是管理员 */
    }
    if (!p) {
        return false;
    }
    {
        bool admin = false;

        if (passwd_load() < 0) {
            return false;
        }
        /* 用 uid 反查不方便，这里直接拿当前用户名查 */
        if (find_user(current_name, 0, 0, 0, 0, 0, &admin)) {
            return admin;
        }
    }
    return false;
}

void user_list(void) {
    const char* p;

    if (passwd_load() < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("  cannot read " PASSWD_PATH);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("  name             uid   home                 role");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_writeln("  --------------------------------------------------------");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    p = passwd_buf;
    while (*p) {
        char f_name[USER_NAME_MAX];
        char f_hash[16];
        char f_uid[12];
        char f_home[USER_HOME_MAX];
        char f_admin[4];
        const char* q;

        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { p++; }
        if (*p == '#') {
            while (*p && *p != '\n') { p++; }
            continue;
        }
        if (*p == '\0') { break; }

        q = p;
        q = next_field(q, f_name,  sizeof(f_name));
        q = next_field(q, f_hash,  sizeof(f_hash));
        q = next_field(q, f_uid,   sizeof(f_uid));
        q = next_field(q, f_home,  sizeof(f_home));
        q = next_field(q, f_admin, sizeof(f_admin));

        vga_write("  ");
        vga_write(f_name);
        for (uint32_t i = (uint32_t)strlen(f_name); i < 17; i++) {
            vga_putchar(' ');
        }
        vga_write(f_uid);
        for (uint32_t i = (uint32_t)strlen(f_uid); i < 6; i++) {
            vga_putchar(' ');
        }
        vga_write(f_home);
        for (uint32_t i = (uint32_t)strlen(f_home); i < 21; i++) {
            vga_putchar(' ');
        }
        vga_writeln((f_admin[0] == '1') ? "admin" : "user");

        while (*q && *q != '\n') { q++; }
        p = q;
    }
}

bool user_login(void) {
    char name[USER_NAME_MAX];
    char pw[32];
    char stored[16];
    char want[9];
    char home[USER_HOME_MAX];
    uint32_t uid = 0;
    bool admin = false;
    int tries;

    current_name[0] = '?';
    current_name[1] = '\0';

    if (passwd_load() < 0) {
        /* 没有账户表就别把系统锁在门外，直接当 root 进去 */
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_writeln("no " PASSWD_PATH " - continuing as root without login");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return true;
    }

    for (tries = 0; tries < 3; tries++) {
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_writeln("HNeoC OS login");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        vga_write("login: ");
        read_line(name, sizeof(name), true);

        vga_write("Password: ");
        read_line(pw, sizeof(pw), false);

        if (name[0] &&
            find_user(name, stored, sizeof(stored), &uid, home, sizeof(home), &admin)) {
            hex8(want, user_hash_password(pw));

            if (strcmp(want, stored) == 0) {
                process_t* p = process_current();

                strncpy(current_name, name, USER_NAME_MAX - 1);
                current_name[USER_NAME_MAX - 1] = '\0';

                if (p) {
                    p->uid   = uid;                     /* 权限检查就看这两个 */
                    p->admin = admin;
                }
                process_set_cwd((home[0] != '\0') ? home : "/");

                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                vga_write("welcome, ");
                vga_write(name);
                vga_writeln(admin ? " (admin)" : "");
                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                return true;
            }
        }

        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("login incorrect");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    }

    vga_writeln("too many attempts");
    return false;
}

int user_add(const char* name, const char* password, bool admin) {
    const hneofs_file_t* f;
    int32_t idx;
    char hash[9];
    char line[160];
    char uidbuf[12];
    char home[USER_HOME_MAX];
    uint32_t uid;
    uint32_t len = 0;
    const char* s;

    if (!name || name[0] == '\0' || strlen(name) >= USER_NAME_MAX) {
        return -1;
    }
    for (s = name; *s; s++) {                 /* 名字里不能有分隔符和空白 */
        if (*s == ':' || *s == '/' || *s == ' ' || *s == '\t') {
            return -1;
        }
    }

    if (passwd_load() < 0) {
        return -3;
    }
    if (find_user(name, 0, 0, 0, 0, 0, 0)) {
        return -2;                            /* 已经有这个账户了 */
    }

    hex8(hash, user_hash_password(password ? password : ""));
    uid = next_uid();

    /* 给新用户开一个 home 目录；开不出来就退回 /home */
    {
        char path[USER_HOME_MAX + 8];
        uint32_t n = 0;

        for (s = HOME_ROOT; *s && n < sizeof(path) - USER_NAME_MAX - 2; s++) {
            path[n++] = *s;
        }
        path[n++] = '/';
        for (s = name; *s && n < sizeof(path) - 1; s++) {
            path[n++] = *s;
        }
        path[n] = '\0';

        if (hneofs_mkdir(path) >= 0) {
            /* 新建目录的属主默认是"创建它的进程"，也就是 root。
             * 但这是给新用户的 home，必须改成他自己 —— 否则他连自己的
             * 目录都写不进去（root 建的目录归 root）。 */
            int32_t hi = hneofs_resolve(path);
            hneofs_file_t* he = (hi >= 0) ? hneofs_file_mut((uint32_t)hi) : 0;

            if (he) {
                he->uid = uid;
                hneofs_sync();
            }
            strncpy(home, path, sizeof(home) - 1);
            home[sizeof(home) - 1] = '\0';
        } else {
            strncpy(home, HOME_ROOT, sizeof(home) - 1);
            home[sizeof(home) - 1] = '\0';
        }
    }

    /* 拼一行：name:hash:uid:home:admin */
    for (s = name; *s; s++) { line[len++] = *s; }
    line[len++] = ':';
    for (s = hash; *s; s++) { line[len++] = *s; }
    line[len++] = ':';
    {
        int d = 0;

        if (uid == 0) {
            uidbuf[d++] = '0';
        } else {
            char tmp[12];
            int t = 0;

            while (uid > 0 && t < 11) {
                tmp[t++] = (char)('0' + (uid % 10));
                uid /= 10;
            }
            while (t > 0) {
                uidbuf[d++] = tmp[--t];
            }
        }
        uidbuf[d] = '\0';
    }
    for (s = uidbuf; *s; s++) { line[len++] = *s; }
    line[len++] = ':';
    for (s = home; *s; s++) { line[len++] = *s; }
    line[len++] = ':';
    line[len++] = admin ? '1' : '0';
    line[len++] = '\n';

    /* 追加：写到文件当前大小的位置 */
    idx = hneofs_resolve(PASSWD_PATH);
    if (idx < 0) {
        return -3;
    }
    f = hneofs_file((uint32_t)idx);
    if (!f) {
        return -3;
    }
    if (hneofs_write_at((uint32_t)idx, f->size, line, len) < 0) {
        return -3;
    }
    return 0;
}

bool user_add_interactive(const char* name, bool admin) {
    char pw[32];
    char again[32];
    int rc;

    vga_write("new password for ");
    vga_write(name);
    vga_write(": ");
    read_line(pw, sizeof(pw), false);

    vga_write("repeat: ");
    read_line(again, sizeof(again), false);

    if (strcmp(pw, again) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_writeln("passwords do not match");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return false;
    }

    rc = user_add(name, pw, admin);
    if (rc == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_write("created account ");
        vga_writeln(name);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        return true;
    }

    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_writeln(rc == -2 ? "that account already exists"
                         : "cannot create the account");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    return false;
}
