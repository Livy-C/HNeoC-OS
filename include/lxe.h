#ifndef LXE_H
#define LXE_H

#include "types.h"

/* ============================================================
 * LXE —— HNeoC eXecutable
 *
 * 一个非常像 DOS 的 .COM/.EXE 的扁平可执行格式：没有重定位，
 * 没有段，没有动态链接。文件开头 40 字节是头部，后面直接跟
 * 代码和数据，整体被加载到 USER_BASE（0x40000000）。
 *
 * 之所以不用 ELF：我们只有一个 MinGW 交叉工具链，生成 PE 目标，
 * 自己解析自己的格式比跟 ELF 较劲省事得多，也更好调试。
 * ============================================================ */

#define LXE_MAGIC    0x0045584Cu   /* 'LXE\0' 小端 */
#define LXE_VERSION  1u

/* 头部标志 */
#define LXE_FLAG_CONSOLE  0x0001   /* 需要控制台，运行前会清屏 */

typedef struct {
    uint32_t magic;        /* 必须是 LXE_MAGIC */
    uint32_t version;      /* 格式版本 */
    uint32_t entry;        /* 入口偏移，相对加载基址 */
    uint32_t size;         /* 代码 + 已初始化数据的字节数 */
    uint32_t bss_size;     /* 需要额外清零的字节数（.bss） */
    uint32_t flags;        /* LXE_FLAG_* */
    char     name[16];     /* 程序名，仅用于显示 */
} __attribute__((packed)) lxe_header_t;   /* 40 字节 */

#define LXE_HEADER_SIZE ((uint32_t)sizeof(lxe_header_t))

#endif /* LXE_H */
