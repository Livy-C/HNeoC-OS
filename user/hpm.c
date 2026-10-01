/* hpm - HNeoC 的包管理器
 * 一个 ring 3 程序，用法照着 apt 来：
 *
 *   hpm                    用法 + 仓库/已装概况
 *   hpm list               已装的包
 *   hpm remove <包名>       卸载（删掉它装出来的文件）
 *   hpm files <包名>        这个包装了哪些文件
 *   hpm verify             检查已装文件是不是都还在
 *   hpm update             扫一遍仓库目录，重建索引
 *
 * HNeoC 没有网络，所以"源"就是磁盘上的 /var/hpm/repo 目录，里面放着
 * 一批 .hnpkg 文件和一个纯文本索引。hpm update 相当于 apt update，
 * 只不过它是把目录重新扫一遍而不是去下载。
 *
 * 包格式见 user/lib/include/hnpkg.h，打包器在宿主机上是 tools/mkhnpkg.ps1。
 *
 * 为什么需要 chmod 系统调用：hpm 把 .lxe 写进 /bin 的时候，新建的文件
 * 一律是默认的 0644，没有执行位，非 root 用户跑不起来 —— 装完必须
 * 按包里的 mode 补上权限。这和真实发行版里 dpkg 要恢复文件权限是一回事。
 */

#include <hneoc.h>
#include <hnpkg.h>

/* 包头/表项的布局必须和 hnpkg.h 完全一致，下面这些是编译期断言：
 * 布局一旦被改错，这里直接编不过，而不是等到装包时错位。
 *
 * 只断言 sizeof 是不够的：hnpkg_header_t 的 reserved[] 是"剩下的都归我"
 * （HNPKG_HEADER_SIZE - 8 - sizeof(hnpkg_info_t) - 4），所以把
 * HNPKG_NAME_MAX 改小一格，后面所有字段都往前挪，而 sizeof 还是整整 256，
 * 断言照样过 —— 然后每一个字段都读错。所以字段的**偏移**也要一个个钉住；
 * 下面的数字就是宿主机那侧 tools/mkhnpkg.ps1 里 $OFF_* / $EOFF_* 算出来的
 * 那组值，也是 build.ps1 读包头时用的字面量。 */
typedef char hpm_check_hdr[(sizeof(hnpkg_header_t) == HNPKG_HEADER_SIZE) ? 1 : -1];
typedef char hpm_check_ent[(sizeof(hnpkg_entry_t) == HNPKG_ENTRY_SIZE) ? 1 : -1];

_Static_assert(offsetof(hnpkg_header_t, magic) == 0,   "hnpkg header: magic must be at offset 0");
_Static_assert(offsetof(hnpkg_header_t, version) == 4, "hnpkg header: version must be at offset 4");
_Static_assert(offsetof(hnpkg_header_t, info.name) == 8,          "hnpkg header: name must be at offset 8");
_Static_assert(offsetof(hnpkg_header_t, info.version) == 40,      "hnpkg header: version field must be at offset 40");
_Static_assert(offsetof(hnpkg_header_t, info.depends) == 56,      "hnpkg header: depends must be at offset 56");
_Static_assert(offsetof(hnpkg_header_t, info.summary) == 104,     "hnpkg header: summary must be at offset 104");
_Static_assert(offsetof(hnpkg_header_t, info.file_count) == 200,  "hnpkg header: file_count must be at offset 200");
_Static_assert(offsetof(hnpkg_header_t, info.data_offset) == 204, "hnpkg header: data_offset must be at offset 204");
_Static_assert(offsetof(hnpkg_header_t, info.total_size) == 208,  "hnpkg header: total_size must be at offset 208");
_Static_assert(offsetof(hnpkg_header_t, info.flags) == 212,       "hnpkg header: flags must be at offset 212");
_Static_assert(offsetof(hnpkg_header_t, table_offset) == 216,     "hnpkg header: table_offset must be at offset 216");

_Static_assert(offsetof(hnpkg_entry_t, path) == 0,      "hnpkg entry: path must be at offset 0");
_Static_assert(offsetof(hnpkg_entry_t, offset) == 48,   "hnpkg entry: offset must be at offset 48");
_Static_assert(offsetof(hnpkg_entry_t, size) == 52,     "hnpkg entry: size must be at offset 52");
_Static_assert(offsetof(hnpkg_entry_t, mode) == 56,     "hnpkg entry: mode must be at offset 56");
_Static_assert(offsetof(hnpkg_entry_t, reserved) == 60, "hnpkg entry: reserved must be at offset 60");

#define MAX_RECS      32
#define MAX_FILES     32
#define CMD_BUF_CAP   (16 * 1024)     /* 索引/数据库文本最大长度 */
#define PATH_CAP      160
#define COPY_CHUNK    512

/* 仓库里的一条索引记录 */
typedef struct {
    char name[HNPKG_NAME_MAX];
    char version[HNPKG_VER_MAX];
    char file[64];                    /* 包文件名，如 hello-1.0.hnpkg */
    char depends[HNPKG_DEPENDS_MAX];
    char summary[HNPKG_SUMMARY_MAX];
} rec_t;

/* 装出来的文件路径。
 *
 * 放全局而不是放栈上：install_rec 是递归的（依赖链），每层都摊一份
 * 5KB 的数组会直接把 32KB 的用户栈吃穿。实测就是这么炸的 ——
 * 装 examples（依赖 hello/tools）时内核报
 *   process 'hpm' killed: Page Fault   CR2 0x403FB3D8
 * 而当时栈底是 0x403FC000：出错地址就在栈底下面 3KB。
 */
static char file_paths[MAX_FILES][PATH_CAP];

/* 索引记录也在全局：一条 rec_t 是 256 字节，32 条就是 8KB，
 * 每个命令都在栈上摆一份同样没必要。 */
static rec_t recs[MAX_RECS];

/* 本次命令里已经装过的包，避免依赖成环时反复装 */
static char installed_marks[MAX_RECS][HNPKG_NAME_MAX];
static int  installed_count = 0;

/* ------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------ */

static void copy_str(char* dst, const char* src, int max) {
    int i = 0;

    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void trim_eol(char* s) {
    int n = (int)strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

/* 去掉行首空格 */
static char* ltrim(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void err(const char* what, const char* detail) {
    term_set_fg(TERM_LIGHT_RED);
    fputs("hpm: ", STDERR_FILENO);
    fputs(what, STDERR_FILENO);
    if (detail && detail[0]) {
        fputs(": ", STDERR_FILENO);
        fputs(detail, STDERR_FILENO);
    }
    fputc('\n', STDERR_FILENO);
    term_reset();
}

/* 把整个文件读成以 '\0' 结尾的字符串。失败返回 NULL。
 * 这个文件系统的 readfile 一次性读完，索引/数据库这种小文件够用。 */
static char* slurp(const char* path, int cap) {
    char* buf = (char*)malloc((size_t)cap + 1);
    int n;

    if (!buf) {
        return NULL;
    }
    n = readfile(path, buf, (size_t)cap);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* 按 '\n' 切行，返回下一行的开头；顺手把行尾的 \r\n 去掉。
 * 传 NULL 表示还没开始。 */
static char* next_line(char* p) {
    char* nl;

    if (!p) {
        return NULL;
    }
    nl = strchr(p, '\n');
    if (nl) {
        *nl = '\0';
        return nl + 1;
    }
    return (*p) ? p + strlen(p) : NULL;
}

/* 把一行按 TAB 切成最多 max 个字段（原地改写，返回实际字段数）。
 * 注意只切前 max-1 个 TAB：最后一个字段里可以带 TAB。 */
static int split_tab(char* line, char** fields, int max) {
    int n = 1;
    char* p;

    fields[0] = line;
    for (p = line; *p; p++) {
        if (*p == '\t' && n < max) {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

/* 组装 "目录/名字"，目录为空或者 '/' 时结果就是 "/名字" */
static void join_path(char* out, int max, const char* dir, const char* name) {
    int n = 0;

    if (!dir || dir[0] == '\0' || strcmp(dir, "/") == 0) {
        out[n++] = '/';
    } else {
        while (dir[n] && n < max - 2) {
            out[n] = dir[n];
            n++;
        }
        if (n > 0 && out[n - 1] != '/') {
            out[n++] = '/';
        }
    }
    for (int i = 0; name[i] && n < max - 1; i++) {
        out[n++] = name[i];
    }
    out[n] = '\0';
}

/* 逐级建目录："a/b/c" 会依次 mkdir a、a/b、a/b/c。
 * 已经存在的目录会让 mkdir 返回错误，这里一律忽略 —— 真正的失败
 * 会在后面打开文件时暴露出来，并给出更准确的消息。 */
static void mkdir_parents(const char* path) {
    char tmp[PATH_CAP];
    int i;

    copy_str(tmp, path, sizeof(tmp));
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp);
            tmp[i] = '/';
        }
    }
    mkdir(tmp);
}

/* 只建路径里的目录部分（最后一段是文件名） */
static void mkdir_parent_of(const char* path) {
    char tmp[PATH_CAP];
    int i;
    int cut = -1;

    copy_str(tmp, path, sizeof(tmp));
    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == '/') {
            cut = i;
        }
    }
    if (cut <= 0) {
        return;                     /* 就在根目录下，不用建 */
    }
    tmp[cut] = '\0';
    mkdir_parents(tmp);
}

/* ------------------------------------------------------------
 * 仓库索引
 * ------------------------------------------------------------ */

static void rec_clear(rec_t* r) {
    memset(r, 0, sizeof(*r));
}

/* 读索引，返回记录数；仓库还不存在时返回 -1 */
static int index_load(rec_t* recs, int max) {
    char* buf = slurp(HPM_REPO_INDEX, CMD_BUF_CAP);
    char* p;
    int n = 0;

    if (!buf) {
        return -1;
    }

    p = buf;
    while (p && *p && n < max) {
        char* line = p;
        char* fields[HPM_INDEX_FIELDS];
        int nf;

        p = next_line(p);
        line = ltrim(line);
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        nf = split_tab(line, fields, HPM_INDEX_FIELDS);
        if (nf < 4) {
            continue;
        }
        rec_clear(&recs[n]);
        copy_str(recs[n].name, fields[0], HNPKG_NAME_MAX);
        copy_str(recs[n].version, fields[1], HNPKG_VER_MAX);
        copy_str(recs[n].file, fields[2], (int)sizeof(recs[n].file));
        copy_str(recs[n].depends, fields[3], HNPKG_DEPENDS_MAX);
        if (nf >= 5) {
            copy_str(recs[n].summary, fields[4], HNPKG_SUMMARY_MAX);
        }
        if (recs[n].name[0]) {
            n++;
        }
    }

    free(buf);
    return n;
}

/* 按包名找索引记录，找不到返回 -1 */
static int index_find(rec_t* recs, int n, const char* name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(recs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* 索引按包名排序：listdir 给出的顺序是目录项顺序，不好看也不好搜 */
static void index_sort(rec_t* recs, int n) {
    for (int i = 1; i < n; i++) {
        rec_t key = recs[i];
        int j = i - 1;

        while (j >= 0 && strcmp(recs[j].name, key.name) > 0) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = key;
    }
}

/* 读一个 .hnpkg 的包头，顺便校验。成功返回 0 */
static int read_header(const char* pkgpath, hnpkg_header_t* h, int* out_fd) {
    int fd = open(pkgpath, O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    if (read(fd, h, HNPKG_HEADER_SIZE) != (int)HNPKG_HEADER_SIZE) {
        close(fd);
        return -1;
    }
    if (h->magic != HNPKG_MAGIC) {
        close(fd);
        return -1;
    }
    if (h->version != HNPKG_VERSION) {
        close(fd);
        return -1;
    }
    /* 表必须紧跟在包头后面：table_offset 恒为 HNPKG_HEADER_SIZE。
     * 这一条不成立的话，表项根本不在我们刚读进来的这 256 字节之后，
     * 后面按 file_count 去读表就是在读别人的数据。 */
    if (h->table_offset != HNPKG_HEADER_SIZE) {
        close(fd);
        return -1;
    }
    /* 表是变长的，先卡住 file_count 的上限，下面的乘法才不会溢出。 */
    if (h->info.file_count > HNPKG_MAX_FILES) {
        close(fd);
        return -1;
    }
    /* data_offset 至少要把 file_count 项的表放下（表正好跟着包头，
     * 所以下限是 HNPKG_HEADER_SIZE + 64 * file_count）。少一个字节就说明
     * 表已经长到数据区里去了，读出来的表项和数据全错位。 */
    if (h->info.data_offset <
        (uint32_t)HNPKG_HEADER_SIZE + (uint32_t)HNPKG_ENTRY_SIZE * h->info.file_count) {
        close(fd);
        return -1;
    }
    /* 数据起点不能超过文件本身，否则每个文件的 offset 都指到文件外面。 */
    if (h->info.data_offset > h->info.total_size) {
        close(fd);
        return -1;
    }
    if (out_fd) {
        *out_fd = fd;
    } else {
        close(fd);
    }
    return 0;
}

/* ------------------------------------------------------------
 * 已装数据库 /var/hpm/installed
 *
 * 纯文本，一行一条：
 *   p <包名> <版本>
 *   f <包名> <路径>
 * ------------------------------------------------------------ */

static int db_pkg_installed(const char* name) {
    char* buf = slurp(HPM_DB_FILE, CMD_BUF_CAP);
    char* p;
    int found = 0;

    if (!buf) {
        return 0;
    }
    p = buf;
    while (p && *p) {
        char* line = ltrim(p);

        p = next_line(p);
        if (line[0] == HPM_DB_PKG && line[1] == ' ') {
            char* nm = ltrim(line + 2);
            char* sp = strchr(nm, ' ');

            /* 先把行切成"包名"和"版本"再比。
             * 这里不能图省事写 strncmp(nm, name, HNPKG_NAME_MAX)：那是要求
             * 前 32 字节完全相同，而 nm 后面还跟着 " 1.0"，永远比不相等 ——
             * 症状就是 hpm list 看得到那个包、hpm files 却说"没装"。 */
            if (sp) {
                *sp = '\0';
            }
            if (strcmp(nm, name) == 0) {
                found = 1;
                break;
            }
        }
    }
    free(buf);
    return found;
}

/* 取已装版本，取不到返回 0 */
static int db_get_version(const char* name, char* out, int max) {
    char* buf = slurp(HPM_DB_FILE, CMD_BUF_CAP);
    char* p;
    int found = 0;

    if (!buf) {
        return 0;
    }
    p = buf;
    while (p && *p) {
        char* line = ltrim(p);

        p = next_line(p);
        if (line[0] == HPM_DB_PKG && line[1] == ' ') {
            /* 名字前也要 ltrim，和 db_pkg_installed 一致：数据库是纯文本，
             * 手工编辑会出现 "p  hello 1.0" 这种两个空格的行。直接取
             * line + 2 的话 nm 就是 " hello"，第一个空格被当成"名字和版本
             * 的分隔符"切掉，比出来是个空串 —— 症状是"装是装了，但版本取
             * 不到"。 */
            char* nm = ltrim(line + 2);
            char* sp = strchr(nm, ' ');

            if (sp) {
                *sp = '\0';
                if (strcmp(nm, name) == 0) {
                    copy_str(out, ltrim(sp + 1), max);
                    trim_eol(out);
                    found = 1;
                    break;
                }
            }
        }
    }
    free(buf);
    return found;
}

/* 列出某个包装出来的文件，返回个数 */
static int db_list_files(const char* name, char paths[][PATH_CAP], int max) {
    char* buf = slurp(HPM_DB_FILE, CMD_BUF_CAP);
    char* p;
    int n = 0;

    if (!buf) {
        return 0;
    }
    p = buf;
    while (p && *p && n < max) {
        char* line = ltrim(p);

        p = next_line(p);
        if (line[0] == HPM_DB_FILE_ && line[1] == ' ') {
            /* 同样先 ltrim 名字："f  hello bin/hi.lxe" 这种多一个空格的行
             * 也要能认出来，否则 hpm files 会说什么都没装。 */
            char* nm = ltrim(line + 2);
            char* sp = strchr(nm, ' ');

            if (sp) {
                *sp = '\0';
                if (strcmp(nm, name) == 0) {
                    copy_str(paths[n], ltrim(sp + 1), PATH_CAP);
                    trim_eol(paths[n]);
                    n++;
                }
            }
        }
    }
    free(buf);
    return n;
}

static int db_write_line(const char* line, int append) {
    int fd = open(HPM_DB_FILE, O_WRONLY | O_CREAT |
                                (append ? O_APPEND : O_TRUNC));
    int rc;

    if (fd < 0) {
        err("cannot write the package database", HPM_DB_FILE);
        return -1;
    }
    rc = (write(fd, line, strlen(line)) == (int)strlen(line)) ? 0 : -1;
    fsync(fd);
    close(fd);
    return rc;
}

static int db_add(const char* name, const char* version) {
    char line[128];

    snprintf(line, sizeof(line), "p %s %s\n", name, version);
    return db_write_line(line, 1);
}

static int db_add_file(const char* name, const char* path) {
    char line[PATH_CAP + HNPKG_NAME_MAX + 8];

    snprintf(line, sizeof(line), "f %s %s\n", name, path);
    return db_write_line(line, 1);
}

/* 把数据库里属于 name 的行全部删掉，其余原样写回 */
static int db_remove(const char* name) {
    char* buf = slurp(HPM_DB_FILE, CMD_BUF_CAP);
    char* out;
    char* p;
    int fd;

    if (!buf) {
        return 0;                       /* 没有数据库就等于没有记录 */
    }
    out = (char*)malloc((size_t)CMD_BUF_CAP + 1);
    if (!out) {
        free(buf);
        return -1;
    }
    out[0] = '\0';

    p = buf;
    while (p && *p) {
        char* line = ltrim(p);
        int keep = 1;

        p = next_line(p);

        if ((line[0] == HPM_DB_PKG || line[0] == HPM_DB_FILE_) && line[1] == ' ') {
            /* 名字前先 ltrim，理由同 db_pkg_installed / db_get_version：
             * 手工编辑过的 "p  hello 1.0" 里名字前面多一个空格，不 ltrim
             * 就永远比不相等 —— 于是 hpm remove 说删掉了，其实一行都没删。 */
            char* nm = ltrim(line + 2);
            char* sp = strchr(nm, ' ');

            if (sp) {
                *sp = '\0';
                if (strcmp(nm, name) == 0) {
                    keep = 0;
                } else {
                    *sp = ' ';
                }
            }
        }
        if (keep && line[0]) {
            strcat(out, line);
            strcat(out, "\n");
        }
    }

    fd = open(HPM_DB_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        err("cannot rewrite the package database", HPM_DB_FILE);
        free(out);
        free(buf);
        return -1;
    }
    if (out[0]) {
        write(fd, out, strlen(out));
    }
    fsync(fd);
    close(fd);
    free(out);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------
 * 解包 / 安装
 * ------------------------------------------------------------ */

/* 把包里的一段数据拷出来写成 target 文件 */
static int copy_range(int fd, uint32_t off, uint32_t size, const char* target) {
    char chunk[COPY_CHUNK];
    uint32_t left = size;
    int out;

    out = open(target, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        return out;
    }
    if (lseek(fd, (int)off, SEEK_SET) < 0) {
        close(out);
        return -1;
    }
    while (left > 0) {
        int want = (left > sizeof(chunk)) ? (int)sizeof(chunk) : (int)left;
        int got = read(fd, chunk, (size_t)want);

        if (got <= 0) {
            close(out);
            return -1;
        }
        if (write(out, chunk, (size_t)got) != got) {
            close(out);
            return -1;
        }
        left -= (uint32_t)got;
    }
    fsync(out);
    close(out);
    return 0;
}

/* 展开一个包：把表里每个文件写到 /<path>，并按 mode 设权限。
 * installed[] 用来回传这次装出来的路径（写进数据库用）。
 * 返回装出来的文件数，失败返回负数。 */
static int unpack(const char* pkgpath, char installed[][PATH_CAP], int max) {
    hnpkg_header_t* h;
    hnpkg_entry_t*  tbl;
    char target[PATH_CAP];
    int fd = -1;
    int n = 0;

    h   = (hnpkg_header_t*)malloc(HNPKG_HEADER_SIZE);
    tbl = (hnpkg_entry_t*)malloc(sizeof(hnpkg_entry_t) * HNPKG_MAX_FILES);
    if (!h || !tbl) {
        free(h);
        free(tbl);
        err("out of memory", NULL);
        return -1;
    }

    if (read_header(pkgpath, h, &fd) != 0) {
        err("not a usable package (bad header)", pkgpath);
        free(h);
        free(tbl);
        return -1;
    }

    /* 表紧跟在包头后面 */
    if (lseek(fd, (int)h->table_offset, SEEK_SET) < 0 ||
        read(fd, tbl, sizeof(hnpkg_entry_t) * h->info.file_count)
            != (int)(sizeof(hnpkg_entry_t) * h->info.file_count)) {
        err("package table is truncated", pkgpath);
        close(fd);
        free(h);
        free(tbl);
        return -1;
    }

    for (uint32_t i = 0; i < h->info.file_count; i++) {
        hnpkg_entry_t* e = &tbl[i];

        /* 每个偏移都必须落在包里，坏包不许把内核缓冲区读穿 */
        if ((uint32_t)e->offset + e->size > h->info.total_size ||
            h->info.data_offset + (uint32_t)e->offset + e->size >
                h->info.total_size) {
            err("package entry points outside the file", e->path);
            close(fd);
            free(h);
            free(tbl);
            return -1;
        }

        join_path(target, sizeof(target), "/", e->path);
        mkdir_parent_of(target);

        if (copy_range(fd, h->info.data_offset + (uint32_t)e->offset,
                       e->size, target) != 0) {
            err("cannot write", target);
            err("(is this account allowed to write there?)", NULL);
            close(fd);
            free(h);
            free(tbl);
            return -1;
        }

        /* 关键一步：新建文件默认 0644，不补执行位的话 .lxe 跑不起来 */
        if (e->mode && chmod(target, (int)e->mode) != 0) {
            err("cannot set permissions on", target);
        }

        printf("  + %-32s %6u B  mode 0%o\n", target, e->size, e->mode);
        if (n < max) {
            copy_str(installed[n], target, PATH_CAP);
        }
        n++;
    }

    close(fd);
    free(h);
    free(tbl);
    return n;
}

/* 在一次 hpm 运行里记下"已经处理过"的包名 */
static int mark_done(const char* name) {
    for (int i = 0; i < installed_count; i++) {
        if (strcmp(installed_marks[i], name) == 0) {
            return 1;
        }
    }
    if (installed_count < MAX_RECS) {
        copy_str(installed_marks[installed_count++], name, HNPKG_NAME_MAX);
    }
    return 0;
}

/* 递归装一个包：先按 depends 把依赖装完，再装自己。
 * 返回值：1 = 装了，0 = 已经装过，负数 = 失败 */
static int install_rec(rec_t* recs, int nrec, const char* name, int depth) {
    char pkgpath[PATH_CAP];
    const rec_t* rec;
    int idx;
    int nfiles;

    if (depth > 8) {
        err("dependency chain is too deep", name);
        return -1;
    }

    if (mark_done(name)) {
        return 0;                       /* 这次命令里刚处理过 */
    }

    idx = index_find(recs, nrec, name);
    if (idx < 0) {
        err("no such package", name);
        return -1;
    }
    rec = &recs[idx];

    if (db_pkg_installed(name)) {
        char have[HNPKG_VER_MAX];
        char line[128];

        db_get_version(name, have, sizeof(have));
        snprintf(line, sizeof(line), "%s %s", name, have);
        term_set_fg(TERM_DARK_GREY);
        printf("  = %-28s already installed (%s)\n", name,
               have[0] ? have : "?");
        term_reset();
        return 0;
    }

    /* --- 先把依赖装掉 --- */
    if (rec->depends[0]) {
        char deps[HNPKG_DEPENDS_MAX];
        char* p;

        copy_str(deps, rec->depends, sizeof(deps));
        p = deps;
        while (*p) {
            char* comma = strchr(p, ',');
            char* dep = ltrim(p);

            if (comma) {
                *comma = '\0';
            }
            trim_eol(dep);
            /* 去掉尾部空格 */
            for (int k = (int)strlen(dep) - 1; k >= 0 && dep[k] == ' '; k--) {
                dep[k] = '\0';
            }
            if (dep[0]) {
                int rc = install_rec(recs, nrec, dep, depth + 1);

                if (rc < 0) {
                    return -1;
                }
            }
            if (!comma) {
                break;
            }
            p = comma + 1;
        }
    }

    /* --- 再装自己 --- */
    join_path(pkgpath, sizeof(pkgpath), HPM_REPO_DIR, rec->file);

    term_set_fg(TERM_LIGHT_CYAN);
    printf("installing %s %s\n", rec->name, rec->version);
    term_reset();

    nfiles = unpack(pkgpath, file_paths, MAX_FILES);
    if (nfiles < 0) {
        return -1;
    }

    db_add(rec->name, rec->version);
    for (int i = 0; i < nfiles && i < MAX_FILES; i++) {
        db_add_file(rec->name, file_paths[i]);
    }

    term_set_fg(TERM_LIGHT_GREEN);
    printf("  installed %s %s (%d files)\n", rec->name, rec->version, nfiles);
    term_reset();
    return 1;
}

/* ------------------------------------------------------------
 * 命令
 * ------------------------------------------------------------ */

static void usage(void) {
    puts("hpm - the HNeoC package manager");
    puts("");
    puts("  hpm list                installed packages");
    puts("  hpm avail               packages in the repository");
    puts("  hpm search <word>       search names and summaries");
    puts("  hpm info <package>      details of one package");
    puts("  hpm install <pkg>...    install (dependencies first)");
    puts("  hpm remove <pkg>        uninstall and delete its files");
    puts("  hpm files <package>     what did it install");
    puts("  hpm verify              check installed files still exist");
    puts("  hpm update              rescan the repository, rebuild the index");
    puts("");
    printf("repository : %s\n", HPM_REPO_DIR);
    printf("database   : %s\n", HPM_DB_FILE);
}

static int cmd_list(void) {
    char* buf = slurp(HPM_DB_FILE, CMD_BUF_CAP);
    char* p;
    int n = 0;

    if (!buf) {
        puts("no packages installed yet");
        return 0;
    }

    puts("installed packages");
    puts("--------------------------------------------------");
    p = buf;
    while (p && *p) {
        char* line = ltrim(p);

        p = next_line(p);
        if (line[0] == HPM_DB_PKG && line[1] == ' ') {
            char* nm = line + 2;
            char* sp = strchr(nm, ' ');

            if (sp) {
                *sp = '\0';
                printf("  %-24s %s\n", nm, ltrim(sp + 1));
                n++;
            }
        }
    }
    puts("--------------------------------------------------");
    printf("  %d package(s)\n", n);
    free(buf);
    return 0;
}

static int cmd_avail(void) {
    int n = index_load(recs, MAX_RECS);

    if (n <= 0) {
        err("no repository index", HPM_REPO_INDEX);
        puts("  try: hpm update");
        return 1;
    }
    index_sort(recs, n);

    puts("available packages");
    puts("--------------------------------------------------");
    for (int i = 0; i < n; i++) {
        int have = db_pkg_installed(recs[i].name);

        term_set_fg(have ? TERM_DARK_GREY : TERM_WHITE);
        printf("  %c %-14s %-6s %s\n", have ? 'i' : ' ',
               recs[i].name, recs[i].version, recs[i].summary);
        term_reset();
    }
    puts("--------------------------------------------------");
    printf("  %d package(s)   ('i' = already installed)\n", n);
    return 0;
}

static int cmd_search(const char* word) {
    int n;
    int hits = 0;

    if (!word || !word[0]) {
        err("usage: hpm search <word>", NULL);
        return 1;
    }
    n = index_load(recs, MAX_RECS);
    if (n <= 0) {
        err("no repository index", HPM_REPO_INDEX);
        return 1;
    }

    for (int i = 0; i < n; i++) {
        int match = 0;

        if (strstr(recs[i].name, word)) {
            match = 1;
        }
        if (strstr(recs[i].summary, word)) {
            match = 1;
        }
        if (match) {
            term_set_fg(TERM_LIGHT_GREEN);
            printf("  %-14s %-6s ", recs[i].name, recs[i].version);
            term_reset();
            printf("%s\n", recs[i].summary);
            hits++;
        }
    }
    printf("  %d match(es) for '%s'\n", hits, word);
    return hits ? 0 : 0;
}

static int cmd_info(const char* name) {
    char pkgpath[PATH_CAP];
    int n;
    int idx;

    if (!name || !name[0]) {
        err("usage: hpm info <package>", NULL);
        return 1;
    }
    n = index_load(recs, MAX_RECS);
    if (n <= 0) {
        err("no repository index", HPM_REPO_INDEX);
        return 1;
    }
    idx = index_find(recs, n, name);
    if (idx < 0) {
        err("no such package", name);
        return 1;
    }

    join_path(pkgpath, sizeof(pkgpath), HPM_REPO_DIR, recs[idx].file);

    printf("package   : %s\n", recs[idx].name);
    printf("version   : %s\n", recs[idx].version);
    printf("summary   : %s\n", recs[idx].summary);
    printf("depends   : %s\n", recs[idx].depends[0] ? recs[idx].depends : "(none)");
    printf("file      : %s\n", pkgpath);
    printf("installed : %s\n", db_pkg_installed(name) ? "yes" : "no");

    /* 直接从包头里读一遍，作为"索引说的"和"包里写的"对得上的证据 */
    {
        hnpkg_header_t* h = (hnpkg_header_t*)malloc(HNPKG_HEADER_SIZE);

        if (h) {
            if (read_header(pkgpath, h, NULL) == 0) {
                printf("in package: %s %s, %u file(s), %u bytes\n",
                       h->info.name, h->info.version,
                       h->info.file_count, h->info.total_size);
            } else {
                err("cannot read the package file", pkgpath);
            }
            free(h);
        }
    }
    return 0;
}

static int cmd_install(int argc, char** argv) {
    int n;
    int failed = 0;

    if (argc == 0) {
        err("usage: hpm install <package>...", NULL);
        return 1;
    }
    if (!is_admin()) {
        err("only an administrator can install packages", NULL);
        return 1;
    }

    n = index_load(recs, MAX_RECS);
    if (n <= 0) {
        err("no repository index", HPM_REPO_INDEX);
        puts("  try: hpm update");
        return 1;
    }

    installed_count = 0;
    for (int i = 0; i < argc; i++) {
        if (install_rec(recs, n, argv[i], 0) < 0) {
            failed = 1;
        }
    }
    return failed;
}

static int cmd_remove(const char* name) {
    int n;

    if (!name || !name[0]) {
        err("usage: hpm remove <package>", NULL);
        return 1;
    }
    if (!is_admin()) {
        err("only an administrator can remove packages", NULL);
        return 1;
    }
    if (!db_pkg_installed(name)) {
        err("package is not installed", name);
        return 1;
    }

    n = db_list_files(name, file_paths, MAX_FILES);
    printf("removing %s (%d file(s))\n", name, n);
    for (int i = 0; i < n; i++) {
        if (unlink(file_paths[i]) == 0) {
            printf("  - %s\n", file_paths[i]);
        } else {
            term_set_fg(TERM_LIGHT_RED);
            printf("  ? %s (not removed)\n", file_paths[i]);
            term_reset();
        }
    }
    db_remove(name);

    term_set_fg(TERM_LIGHT_GREEN);
    printf("  removed %s\n", name);
    term_reset();
    puts("  (empty directories are left behind)");
    return 0;
}

static int cmd_files(const char* name) {
    int n;

    if (!name || !name[0]) {
        err("usage: hpm files <package>", NULL);
        return 1;
    }
    if (!db_pkg_installed(name)) {
        err("package is not installed", name);
        return 1;
    }
    n = db_list_files(name, file_paths, MAX_FILES);
    printf("%s owns %d file(s)\n", name, n);
    for (int i = 0; i < n; i++) {
        printf("  %s\n", file_paths[i]);
    }
    return 0;
}

static int cmd_verify(void) {
    char* buf = slurp(HPM_DB_FILE, CMD_BUF_CAP);
    char* p;
    int total = 0;
    int missing = 0;

    if (!buf) {
        puts("nothing installed, nothing to verify");
        return 0;
    }

    puts("verifying installed files");
    p = buf;
    while (p && *p) {
        char* line = ltrim(p);

        p = next_line(p);
        if (line[0] == HPM_DB_FILE_ && line[1] == ' ') {
            char* nm = line + 2;
            char* sp = strchr(nm, ' ');
            int fd;

            if (!sp) {
                continue;
            }
            *sp = '\0';
            sp = ltrim(sp + 1);
            trim_eol(sp);
            total++;

            fd = open(sp, O_RDONLY);
            if (fd < 0) {
                term_set_fg(TERM_LIGHT_RED);
                printf("  missing: %s (owned by %s)\n", sp, nm);
                term_reset();
                missing++;
            } else {
                close(fd);
            }
        }
    }
    printf("  %d file(s) checked, %d missing\n", total, missing);
    free(buf);
    return missing ? 1 : 0;
}

/* 扫仓库目录，重建索引。
 * 相当于 apt update，只是源在本地：读每个 .hnpkg 的包头就够了，
 * 不用把整包读进来。 */
static int cmd_update(void) {
    char* out = (char*)malloc((size_t)CMD_BUF_CAP);
    int used = 0;
    int count = 0;
    int fd;

    if (!is_admin()) {
        err("only an administrator can update the repository index", NULL);
        return 1;
    }
    if (!out) {
        err("out of memory", NULL);
        return 1;
    }

    used += snprintf(out + used, (size_t)(CMD_BUF_CAP - used),
                     "# HNeoC package index - regenerated by 'hpm update'\n"
                     "# name\tversion\tfile\tdepends\tsummary\n");

    for (int i = 0; ; i++) {
        char name[48];
        char pkgpath[PATH_CAP];
        hnpkg_header_t* h;
        int r = listdir(HPM_REPO_DIR, i, name, sizeof(name));

        if (r < 0) {
            break;
        }
        if (IS_DIR(r)) {
            continue;
        }
        if (!strstr(name, HPM_EXT)) {
            continue;
        }

        h = (hnpkg_header_t*)malloc(HNPKG_HEADER_SIZE);
        if (!h) {
            break;
        }
        join_path(pkgpath, sizeof(pkgpath), HPM_REPO_DIR, name);
        if (read_header(pkgpath, h, NULL) == 0) {
            int wrote = snprintf(out + used, (size_t)(CMD_BUF_CAP - used),
                                 "%s\t%s\t%s\t%s\t%s\n",
                                 h->info.name, h->info.version, name,
                                 h->info.depends, h->info.summary);

            if (wrote > 0 && used + wrote < CMD_BUF_CAP) {
                used += wrote;
                count++;
                printf("  + %-14s %-6s %s\n", h->info.name, h->info.version, name);
            }
        } else {
            err("skipping unusable package", name);
        }
        free(h);
    }

    fd = open(HPM_REPO_INDEX, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        err("cannot write the index", HPM_REPO_INDEX);
        free(out);
        return 1;
    }
    write(fd, out, (size_t)used);
    fsync(fd);
    close(fd);
    free(out);

    term_set_fg(TERM_LIGHT_GREEN);
    printf("index rebuilt: %d package(s)\n", count);
    term_reset();
    return 0;
}

/* 不带参数的 hpm：用法 + 一眼能看懂的概况 */
static int cmd_status(void) {
    char* db;
    int navail = index_load(recs, MAX_RECS);
    int ninst = 0;

    usage();
    puts("");

    printf("available  : %d package(s)\n", navail > 0 ? navail : 0);

    db = slurp(HPM_DB_FILE, CMD_BUF_CAP);
    if (db) {
        char* p = db;

        while (p && *p) {
            char* line = ltrim(p);

            p = next_line(p);
            if (line[0] == HPM_DB_PKG && line[1] == ' ') {
                ninst++;
            }
        }
        free(db);
    }
    printf("installed  : %d package(s)\n", ninst);
    printf("who am i   : uid %d%s\n", getuid(), is_admin() ? " (admin)" : "");
    return 0;
}

/* ------------------------------------------------------------
 * 入口：拆参数
 * ------------------------------------------------------------ */

int main(void) {
    char args[256];
    char* argv[8];
    int argc = 0;
    char* p;

    getargs(args, sizeof(args));

    /* 按空格切成 argv（不需要引号处理：包名里不会有空格） */
    p = args;
    while (*p && argc < 8) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }

    if (argc == 0) {
        return cmd_status();
    }

    if (strcmp(argv[0], "list") == 0) {
        return cmd_list();
    }
    if (strcmp(argv[0], "avail") == 0) {
        return cmd_avail();
    }
    if (strcmp(argv[0], "search") == 0) {
        return cmd_search(argc > 1 ? argv[1] : NULL);
    }
    if (strcmp(argv[0], "info") == 0) {
        return cmd_info(argc > 1 ? argv[1] : NULL);
    }
    if (strcmp(argv[0], "install") == 0) {
        return cmd_install(argc - 1, argv + 1);
    }
    if (strcmp(argv[0], "remove") == 0 || strcmp(argv[0], "uninstall") == 0) {
        return cmd_remove(argc > 1 ? argv[1] : NULL);
    }
    if (strcmp(argv[0], "files") == 0) {
        return cmd_files(argc > 1 ? argv[1] : NULL);
    }
    if (strcmp(argv[0], "verify") == 0) {
        return cmd_verify();
    }
    if (strcmp(argv[0], "update") == 0) {
        return cmd_update();
    }
    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "-h") == 0 ||
        strcmp(argv[0], "--help") == 0) {
        usage();
        return 0;
    }

    err("unknown command", argv[0]);
    usage();
    return 1;
}
