#ifndef HEAP_H
#define HEAP_H

#include "types.h"

/* 内核堆：在物理内存管理器之上提供 kmalloc / kfree
 *
 * 分配器结构是经典的双向链表 + 首次适配 + 相邻块合并。
 * 每个块前面有一个头部记录大小和空闲标志，返回给调用者的是头部之后的数据区。
 */

/* 堆的初始大小（页数），不够时可以再扩 */
#define HEAP_INITIAL_PAGES 1024   /* 4MB */

void heap_init(void);

/* 分配 size 字节，返回 8 字节对齐的指针；失败返回 NULL */
void* kmalloc(size_t size);

/* 分配并清零 */
void* kzalloc(size_t size);

/* 释放，传入 NULL 是安全的 */
void kfree(void* ptr);

/* 统计信息，供 Shell 的 mem 命令显示 */
void heap_stats(uint32_t* total_bytes, uint32_t* used_bytes,
                uint32_t* free_bytes, uint32_t* block_count);

/* 简单的自检：做一轮分配/释放并返回是否全部符合预期 */
bool heap_self_test(void);

#endif /* HEAP_H */
