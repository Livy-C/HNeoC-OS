#ifndef E820_H
#define E820_H

#include "types.h"

/* 引导扇区把 INT 15h/E820 的结果放在这里：
 *   dword 条目数，之后紧跟条目数组
 */
#define E820_ADDR        0x5000
#define E820_MAX_ENTRIES 32

/* E820 返回的一条物理内存区间描述 */
typedef struct {
    uint64_t base;      /* 起始物理地址 */
    uint64_t length;    /* 长度（字节） */
    uint32_t type;      /* 区间类型，见下面的常量 */
    uint32_t acpi;      /* ACPI 扩展属性，一般不用 */
} __attribute__((packed)) e820_entry_t;

/* 引导扇区写下的完整内存布局 */
typedef struct {
    uint32_t     count;
    e820_entry_t entries[E820_MAX_ENTRIES];
} __attribute__((packed)) e820_map_t;

/* E820 区间类型 */
#define E820_TYPE_USABLE        1   /* 可以被操作系统自由使用 */
#define E820_TYPE_RESERVED      2   /* 保留，不可用 */
#define E820_TYPE_ACPI_RECLAIM  3   /* ACPI 数据，回收后可用 */
#define E820_TYPE_ACPI_NVS      4   /* ACPI 非易失存储，不可用 */
#define E820_TYPE_BAD           5   /* 坏内存 */

/* 取引导扇区探测到的内存布局（可能是空的，要判断 count） */
const e820_map_t* e820_get_map(void);

/* 可用内存总和（字节） */
uint64_t e820_total_usable(void);

/* 最高可用物理地址，用来决定位图要覆盖多大范围 */
uint64_t e820_highest_usable(void);

/* 初始化时打印用：把类型号转成简短的名字 */
const char* e820_type_name(uint32_t type);

#endif /* E820_H */
