#ifndef PMM_H
#define PMM_H

#include "types.h"

/* 物理内存按 4KB 页框管理 */
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12

/* 最多管理 4GB 内存：4GB / 4KB = 1M 个页框，位图 128KB */
#define PMM_MAX_FRAMES  (1024 * 1024)

/* 扫描 E820 结果，建立页框位图 */
void pmm_init(void);

/* 分配 / 释放单个页框，返回物理地址（未启用分页前也就是可直接使用的指针） */
void* pmm_alloc_page(void);
void  pmm_free_page(void* addr);

/* 分配 / 释放连续多个页框 */
void* pmm_alloc_pages(uint32_t count);
void  pmm_free_pages(void* addr, uint32_t count);

/* 把一段物理区间标记为已用或可用（地址会向下/向上对齐到页边界） */
void pmm_mark_region(uint64_t base, uint64_t length, bool used);

/* 统计信息 */
uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_free_frames(void);

#endif /* PMM_H */
