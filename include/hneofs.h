#ifndef HNEOFS_H
#define HNEOFS_H

#include "types.h"

/* ============================================================
 * HNeoFS —— HNeoC OS 的极简文件系统
 *
 * 设计目标是"能读懂、好调试"，不是性能：
 *   - 没有 inode，没有位图，没有日志
 *   - 文件表就是一张定长数组，直接读进内存
 *   - 文件在磁盘上连续存放，起始 LBA + 长度就能定位
 *
 * 目录是一棵树，但结构极简：每个目录项有一个 parent 字段指向
 * 父目录在文件表里的下标，根目录用 HNEOFS_ROOT 表示。路径查找
 * 就是沿着 parent 一级一级往下比对名字。
 *
 * 磁盘上的布局（从 1MB 处开始，给内核留足空间）：
 *
 *   LBA 2048                超级块（512 字节）
 *   LBA 2049 - 2064         文件表（16 个扇区 = 128 个目录项）
 *   LBA 2065 - ...          文件数据区，按目录项顺序连续排布
 * ============================================================ */

#define HNEOFS_MAGIC        0x53464E48u   /* 'HNFS' 小端 */
#define HNEOFS_VERSION      2u

#define HNEOFS_BLOCK_SIZE   512
#define HNEOFS_START_LBA    2048          /* 1MB 处 */
#define HNEOFS_TABLE_LBA    (HNEOFS_START_LBA + 1)
/* 目录表定长 16 个扇区 = 128 项。镜像里现在装着 48 个文件，而跑一轮回归
 * 还要在 /share/hncc 下现编出十来个 .lxe，8 个扇区（64 项）已经不够：
 * hpm 一装包就会把目录占满，hncc 接着报 "cannot create the output file"。 */
#define HNEOFS_TABLE_SECTORS 16
#define HNEOFS_DATA_LBA     (HNEOFS_TABLE_LBA + HNEOFS_TABLE_SECTORS)

#define HNEOFS_NAME_MAX     32
#define HNEOFS_PATH_MAX     128
#define HNEOFS_MAX_FILES    (HNEOFS_TABLE_SECTORS * HNEOFS_BLOCK_SIZE / 64)

/* 没有父目录 = 就在根目录下 */
#define HNEOFS_ROOT         0xFFFFFFFFu

/* 目录项类型 */
#define HNEOFS_TYPE_FILE    0
#define HNEOFS_TYPE_DIR     1

/* 目录项标志 */
#define HNEOFS_FLAG_EXEC    0x0001   /* 可以直接执行 */
#define HNEOFS_FLAG_TEXT    0x0002   /* 纯文本，cat 友好 */

/* 权限位。借用 Unix 的三组三位写法，但这里只区分"属主"和"其他人"
 * （没有用户组的概念），所以中间那三位（组）不参与判断。
 *   0755 = 属主可读写执行，其他人可读可执行
 *   0644 = 属主可读写，其他人只读
 */
#define HNEOFS_MODE_OWNER_EXEC   0x040u   /* 0100 */
#define HNEOFS_MODE_OWNER_WRITE  0x080u   /* 0200 */
#define HNEOFS_MODE_OWNER_READ   0x100u   /* 0400 */
#define HNEOFS_MODE_OTHER_EXEC   0x001u   /* 0001 */
#define HNEOFS_MODE_OTHER_WRITE  0x002u   /* 0002 */
#define HNEOFS_MODE_OTHER_READ   0x004u   /* 0004 */

/* 权限检查里"想要什么"的编码。
 *
 * 注意：这不是掩码，是位序号 2/1/0 上的值 —— hneofs_access 会把它左移 6 位
 * 去对属主的那三位、直接拿去对"其他人"的那三位，所以读=4、写=2、执行=1，
 * 顺序和 Unix 三位一样。想给掩码的地方用上面的 HNEOFS_MODE_*，两者别混。
 */
#define HNEOFS_ACCESS_EXEC   1u   /* 0100/0001 位 */
#define HNEOFS_ACCESS_WRITE  2u   /* 0200/0002 位 */
#define HNEOFS_ACCESS_READ   4u   /* 0400/0004 位 */

/* 新建文件/目录时用的默认权限 */
#define HNEOFS_MODE_DEFAULT_FILE  0x1A4u   /* 0644 */
#define HNEOFS_MODE_DEFAULT_DIR   0x1EDu   /* 0755 */

/* 打开标志 */
#define HNEOFS_O_RDONLY  0x0000
#define HNEOFS_O_WRONLY  0x0001
#define HNEOFS_O_RDWR    0x0002
#define HNEOFS_O_CREAT   0x0100
#define HNEOFS_O_TRUNC   0x0200
#define HNEOFS_O_APPEND  0x0400

/* 错误码（负值） */
#define HNEOFS_ERR_NOENT   (-1)   /* 文件不存在 */
#define HNEOFS_ERR_EXIST   (-2)   /* 已存在 */
#define HNEOFS_ERR_NOSPC   (-3)   /* 空间不够 */
#define HNEOFS_ERR_FULL    (-4)   /* 目录项用完了 */
#define HNEOFS_ERR_NAME    (-5)   /* 名字不合法 */
#define HNEOFS_ERR_IO      (-6)   /* 磁盘读写失败 */
#define HNEOFS_ERR_MOUNT   (-7)   /* 文件系统没挂载 */
#define HNEOFS_ERR_NOTDIR  (-8)   /* 中间某一段不是目录 */
#define HNEOFS_ERR_ISDIR   (-9)   /* 是目录，不能这样操作 */
#define HNEOFS_ERR_NOTEMPTY (-10) /* 目录非空，不能删 */
#define HNEOFS_ERR_PERM     (-11) /* 权限不够 */

/* 目录项：64 字节，一个扇区正好放 8 个 */
typedef struct {
    char     name[HNEOFS_NAME_MAX];  /* 单个路径分量（不含 '/'），空串表示这个槽位是空的 */
    uint32_t start_lba;              /* 文件数据起始扇区 */
    uint32_t size;                   /* 文件字节数；目录恒为 0 */
    uint32_t flags;                  /* HNEOFS_FLAG_* */
    uint32_t entry_offset;           /* 可执行文件的入口偏移，通常为 0 */
    uint32_t type;                   /* HNEOFS_TYPE_FILE / HNEOFS_TYPE_DIR */
    uint32_t parent;                 /* 父目录的下标；HNEOFS_ROOT 表示根目录下 */
    uint32_t mode;                   /* 权限位，见下面的 HNEOFS_MODE_* */
    uint32_t uid;                    /* 属主 uid，0 = root */
} __attribute__((packed)) hneofs_file_t;

/* 超级块：512 字节 */
typedef struct {
    uint32_t magic;          /* 必须是 HNEOFS_MAGIC */
    uint32_t version;        /* 格式版本 */
    uint32_t block_size;     /* 块大小，固定 512 */
    uint32_t total_blocks;   /* 文件系统区域总扇区数 */
    uint32_t file_count;     /* 文件表里已用到第几项（高水位，中间可能有空洞） */
    uint32_t table_lba;      /* 文件表起始 LBA */
    uint32_t data_lba;       /* 数据区起始 LBA */
    uint32_t free_lba;       /* 提示值，供 fsstat 显示 */
    char     label[32];      /* 卷标 */
    uint8_t  reserved[512 - 8 * 4 - 32];
} __attribute__((packed)) hneofs_super_t;

/* ------------------------------------------------------------
 * 挂载与基本信息
 * ------------------------------------------------------------ */
bool hneofs_mount(void);
bool hneofs_mounted(void);
const hneofs_super_t* hneofs_super(void);

/* 文件表里用到的高水位（下标 0 .. count-1 都可能存在，也可能被删空） */
uint32_t hneofs_count(void);

/* 按下标取目录项；下标越界或者槽位是空的都返回 NULL */
const hneofs_file_t* hneofs_file(uint32_t index);
hneofs_file_t* hneofs_file_mut(uint32_t index);

/* 按路径查下标，找不到返回 -1。path 可以是 "/bin/ls" 或者 "bin/ls"，
 * 都从根目录开始算（内核 Shell 没有工作目录的概念）
 */
int32_t hneofs_resolve(const char* path);

/* 把目录下标还原成绝对路径（Shell 提示符、cd 用）。失败返回 false */
bool hneofs_path_of(uint32_t index, char* out, uint32_t max);

/* 按路径取目录项；等价于先 resolve 再 file */
const hneofs_file_t* hneofs_lookup(const char* path);

/* ------------------------------------------------------------
 * 目录遍历
 * ------------------------------------------------------------ */

/* 在第 dir_index 个目录下找名为 name 的子项，返回下标；找不到 -1。
 * dir_index 传 HNEOFS_ROOT 表示在根目录下找
 */
int32_t hneofs_find_child(uint32_t dir_index, const char* name);

/* 遍历目录的第 n 个子项（n 从 0 开始），返回下标；没有更多返回 -1 */
int32_t hneofs_child_at(uint32_t dir_index, uint32_t n);

/* 目录里有多少个子项 */
uint32_t hneofs_child_count(uint32_t dir_index);

/* ------------------------------------------------------------
 * 读取
 * ------------------------------------------------------------ */
/* 检查 uid 对目录项 f 有没有 want（上面那几个模式位之一或组合）的权限。
 * uid 0（root）一律放行；属主看高三位，其他人看低三位。
 */
bool hneofs_access(const hneofs_file_t* f, uint32_t uid, uint32_t want);

int32_t hneofs_read_at(const hneofs_file_t* f, uint32_t offset,
                       void* buffer, uint32_t size);
int32_t hneofs_read_file(const hneofs_file_t* f, void* buffer, uint32_t buffer_size);

/* ------------------------------------------------------------
 * 写入侧
 *
 * 文件在磁盘上仍然是连续存放的，所以"分配"就是找一段够长的
 * 连续空闲扇区。空闲区不单独记位图，而是从现有文件的范围推出来。
 *
 * 注意：删除不会搬动其它目录项，只在原地留下一个空洞。
 * 这样所有下标都是稳定的，子目录的 parent 链接不会失效。
 * ------------------------------------------------------------ */

/* 把内存里的文件表写回磁盘 */
bool hneofs_sync(void);

/* 单个路径分量是否合法（非空、不太长、不含 '/'） */
bool hneofs_name_ok(const char* name);

/* 创建文件。path 的父目录必须已经存在。
 * 成功返回新目录项的下标，失败返回负的错误码
 */
int32_t hneofs_create(const char* path);

/* 创建目录。成功返回目录项下标，失败返回负的错误码 */
int32_t hneofs_mkdir(const char* path);

/* 删除文件或空目录；不存在返回 false */
bool hneofs_unlink(const char* path);

/* 整块覆盖写：不存在就创建。返回写入的字节数，失败返回负的错误码 */
int32_t hneofs_write_file(const char* path, const void* data, uint32_t size);

/* 从 offset 处写，必要时扩容并保留原内容 */
int32_t hneofs_write_at(uint32_t index, uint32_t offset,
                        const void* data, uint32_t size);

/* 改变文件长度 */
bool hneofs_truncate(uint32_t index, uint32_t new_size);

/* 剩余可用字节数 */
uint32_t hneofs_free_bytes(void);

#endif /* HNEOFS_H */
