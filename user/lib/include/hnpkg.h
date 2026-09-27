#ifndef _HNPKG_H
#define _HNPKG_H

#include "stdint.h"

/* ============================================================
 * .hnpkg —— HNeoC 的软件包格式
 *
 * 一个包就是一个文件：256 字节包头 + 文件表 + 连续的数据区。
 * 布局和 HNeoFS 自己是同一个思路（"表 + 连续数据"），因为两边都只需要
 * 顺序读一遍：宿主机打包时顺序写出来，OS 上安装时顺序读进去。
 *
 *   [0        .. 255]  包头
 *   [256      ..    ]  文件表，HNPKG_MAX_FILES 项，每项 64 字节
 *   [data_offset .. ]  文件数据（所有文件首尾相接，表里的 offset 相对这里）
 *
 * 文件表里的 path 是**相对根的路径**（"bin/hi.lxe"、"share/hello/about.txt"），
 * 安装时直接拼在 "/" 后面；mode 是 HNeoFS 的权限位，安装时用 chmod 设回去，
 * 否则装出来的程序没有执行位，非 root 用户跑不起来。
 *
 * 设计上刻意不做压缩、不做段内重定位、不做签名：包的内容会被原样写到
 * 目标路径上，所以"包里是什么、装完就是什么"，出问题可以用 hpm files
 * 和 hpm verify 一眼看穿。
 *
 * 宿主机那侧的打包器是 tools/mkhnpkg.ps1 —— 改这个文件里的偏移量时
 * 一定要同步改它，两边靠这份布局对上。
 * ============================================================ */

#define HNPKG_MAGIC      0x4B504E48u   /* 'HNPK' 小端 */
#define HNPKG_VERSION    1u

#define HNPKG_HEADER_SIZE 256
#define HNPKG_PATH_MAX    48           /* 安装路径最大长度（含结尾 0） */
#define HNPKG_NAME_MAX    32
#define HNPKG_VER_MAX     16
#define HNPKG_DEPENDS_MAX 48           /* 逗号分隔的包名列表 */
#define HNPKG_SUMMARY_MAX 96
#define HNPKG_MAX_FILES   32

/* 包头标志 */
#define HNPKG_FLAG_ADMIN 0x0001u       /* 只有管理员能安装 */

/* 包头的"信息段"（紧跟在 magic/version 之后） */
typedef struct {
    char     name[HNPKG_NAME_MAX];         /* 包名 */
    char     version[HNPKG_VER_MAX];       /* 版本字符串，只用来显示和比较相等 */
    char     depends[HNPKG_DEPENDS_MAX];   /* 逗号分隔的依赖包名，空串表示没有 */
    char     summary[HNPKG_SUMMARY_MAX];   /* 一句话说明，hpm search 用 */
    uint32_t file_count;                   /* 文件表里有多少项 */
    uint32_t data_offset;                  /* 数据区起点（本文件内的偏移） */
    uint32_t total_size;                   /* 整个包文件的字节数 */
    uint32_t flags;                        /* HNPKG_FLAG_* */
} __attribute__((packed)) hnpkg_info_t;

/* 整个包头，正好 HNPKG_HEADER_SIZE 字节 */
typedef struct {
    uint32_t      magic;
    uint32_t      version;
    hnpkg_info_t  info;
    uint32_t      table_offset;            /* 恒为 HNPKG_HEADER_SIZE */
    uint8_t       reserved[HNPKG_HEADER_SIZE - 8 - sizeof(hnpkg_info_t) - 4];
} __attribute__((packed)) hnpkg_header_t;

/* 文件表项，64 字节 */
typedef struct {
    char     path[HNPKG_PATH_MAX];   /* 相对根的安装路径，如 "bin/hi.lxe" */
    uint32_t offset;                 /* 相对 data_offset 的文件内容偏移 */
    uint32_t size;                   /* 文件字节数 */
    uint32_t mode;                   /* HNeoFS 权限位，如 0755 / 0644 */
    uint32_t reserved;               /* 凑满 64 字节 */
} __attribute__((packed)) hnpkg_entry_t;

/* ------------------------------------------------------------
 * 本地仓库与已装数据库
 *
 * HNeoC 没有网络，所以"仓库"就是磁盘上的一个目录，hpm update
 * 扫一遍目录里的 .hnpkg 重新生成索引 —— 相当于 apt 的 update，
 * 只是源在本地。
 * ------------------------------------------------------------ */

#define HPM_REPO_DIR   "/var/hpm/repo"
#define HPM_REPO_INDEX "/var/hpm/repo/index"
#define HPM_DB_FILE    "/var/hpm/installed"
#define HPM_EXT        ".hnpkg"

/* 索引文件是纯文本，一行一个包，字段用 TAB 分开：
 *   name<TAB>version<TAB>filename<TAB>depends<TAB>summary
 * 和真实的 Packages 文件比它简陋得多，但 hpm search 只需要扫一遍。
 */
#define HPM_INDEX_FIELDS 5

/* 已装数据库也是纯文本，一行一条记录：
 *   p <name> <version>        装了这个包
 *   f <name> <path>           这个包装出来的一个文件
 * 删包时按 name 过滤掉这些行再写回去，简单到不用解析器。
 */
#define HPM_DB_PKG  'p'
#define HPM_DB_FILE_ 'f'

#endif /* _HNPKG_H */
