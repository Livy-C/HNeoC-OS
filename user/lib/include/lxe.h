#ifndef _LXE_H
#define _LXE_H

#include "stdint.h"

/* ============================================================
 * LXE —— HNeoC eXecutable（用户态看到的版本）
 *
 * 内核里的同一份定义在 include/lxe.h。之所以要两份：内核用 types.h，
 * 用户程序用 lib/include 下的 stdint.h，两边不共享头文件目录。
 * 字段布局必须一模一样，改一个就要改另一个。
 *
 * 文件结构：40 字节头部 + 代码和数据。整体被加载到 USER_BASE，
 * 头部里的 entry 是相对基址的入口偏移（链接脚本把入口排在映像最前面，
 * 所以正常编译出来的程序 entry = 0）。
 *
 * hncc 编译器自己就按这个布局写文件，不经过任何工具链。
 * ============================================================ */

#define LXE_MAGIC    0x0045584Cu   /* 'LXE\0' 小端 */
#define LXE_VERSION  1u

#define LXE_FLAG_CONSOLE 0x0001u   /* 需要控制台，运行前清屏 */

#define USER_BASE_VADDR 0x40000000u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t entry;        /* 入口偏移，相对加载基址 */
    uint32_t size;         /* 代码 + 已初始化数据的字节数 */
    uint32_t bss_size;     /* 需要额外清零的字节数 */
    uint32_t flags;
    char     name[16];
} __attribute__((packed)) lxe_header_t;

#define LXE_HEADER_SIZE ((uint32_t)sizeof(lxe_header_t))

#endif /* _LXE_H */
