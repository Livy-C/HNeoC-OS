#include "../include/pmm.h"
#include "../include/e820.h"

/* 页框位图：1 表示已占用，0 表示空闲。
 * 1M 个页框需要 128KB，作为 BSS 不占可执行文件空间。
 */
static uint8_t  frame_bitmap[PMM_MAX_FRAMES / 8];

/* 位图实际覆盖的页框数（由 E820 结果决定，不一定是 PMM_MAX_FRAMES） */
static uint32_t managed_frames = 0;
static uint32_t used_frames    = 0;

/* 由链接器脚本提供的内核映像末尾符号，用来把内核自身占的内存标记为已用 */
extern uint8_t bss_end;

/* ---------------- 位图基本操作 ---------------- */

static inline void bitmap_set(uint32_t frame) {
    frame_bitmap[frame >> 3] |= (uint8_t)(1 << (frame & 7));
}

static inline void bitmap_clear(uint32_t frame) {
    frame_bitmap[frame >> 3] &= (uint8_t)~(1 << (frame & 7));
}

static inline bool bitmap_test(uint32_t frame) {
    return (frame_bitmap[frame >> 3] & (uint8_t)(1 << (frame & 7))) != 0;
}

/* ---------------- 对外接口 ---------------- */

void pmm_mark_region(uint64_t base, uint64_t length, bool used) {
    if (length == 0) {
        return;
    }

    /* 起止地址都按页对齐：起点向下取整，终点向上取整，
     * 保证部分覆盖的页框整体被标记，不会把别人的数据踩掉
     */
    uint64_t first = base >> PAGE_SHIFT;
    uint64_t last  = (base + length + PAGE_SIZE - 1) >> PAGE_SHIFT;

    if (first >= managed_frames) {
        return;
    }
    if (last > managed_frames) {
        last = managed_frames;
    }

    for (uint64_t f = first; f < last; f++) {
        bool already = bitmap_test((uint32_t)f);

        if (used && !already) {
            bitmap_set((uint32_t)f);
            used_frames++;
        } else if (!used && already) {
            bitmap_clear((uint32_t)f);
            used_frames--;
        }
    }
}

void pmm_init(void) {
    const e820_map_t* map = e820_get_map();

    /* 先把整张位图当成"全部已占用"。如果后面 E820 什么都没探测到，
     * 系统至少不会误把不存在的内存分配出去。
     */
    for (uint32_t i = 0; i < sizeof(frame_bitmap); i++) {
        frame_bitmap[i] = 0xFF;
    }

    uint64_t highest = e820_highest_usable();
    if (highest == 0 || !map) {
        /* 探测失败：退回到"假定前 16MB 可用"，保证系统还能跑起来 */
        managed_frames = (16u * 1024 * 1024) / PAGE_SIZE;
    } else {
        uint64_t frames = (highest + PAGE_SIZE - 1) / PAGE_SIZE;
        if (frames > PMM_MAX_FRAMES) {
            frames = PMM_MAX_FRAMES;
        }
        managed_frames = (uint32_t)frames;
    }

    used_frames = managed_frames;

    /* 再把 E820 报告的可用区间放出来 */
    if (map) {
        for (uint32_t i = 0; i < map->count; i++) {
            if (map->entries[i].type == E820_TYPE_USABLE) {
                pmm_mark_region(map->entries[i].base, map->entries[i].length, false);
            }
        }
    } else {
        pmm_mark_region(0, 16u * 1024 * 1024, false);
    }

    /* 保留低端 1MB：里面有中断向量表、BIOS 数据区、EBDA、显存等，
     * 虽然 E820 通常只把 0x0-0x9FC00 报成可用，但整段锁住更省心
     */
    pmm_mark_region(0, 0x100000, true);

    /* 保留内核映像（从 0x10000 到 BSS 结束） */
    pmm_mark_region(0x10000, (uint64_t)(uint32_t)&bss_end - 0x10000, true);

    /* 保留引导程序写下的 E820 缓冲区 */
    pmm_mark_region(E820_ADDR, sizeof(e820_map_t), true);

    /* 保留 VGA 文本缓冲区 */
    pmm_mark_region(0xB8000, 0x8000, true);
}

/* 从 from 开始找 count 个连续空闲页框，找不到返回 -1 */
static int32_t find_free_run(uint32_t count) {
    uint32_t run = 0;

    for (uint32_t f = 0; f < managed_frames; f++) {
        if (bitmap_test(f)) {
            run = 0;
            continue;
        }
        run++;
        if (run == count) {
            return (int32_t)(f - count + 1);
        }
    }
    return -1;
}

void* pmm_alloc_pages(uint32_t count) {
    if (count == 0) {
        return NULL;
    }

    int32_t start = find_free_run(count);
    if (start < 0) {
        return NULL;
    }

    for (uint32_t i = 0; i < count; i++) {
        bitmap_set((uint32_t)start + i);
        used_frames++;
    }

    return (void*)((uint32_t)start << PAGE_SHIFT);
}

void* pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

void pmm_free_pages(void* addr, uint32_t count) {
    uint32_t frame = (uint32_t)addr >> PAGE_SHIFT;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t f = frame + i;
        if (f >= managed_frames) {
            break;
        }
        if (bitmap_test(f)) {
            bitmap_clear(f);
            used_frames--;
        }
    }
}

void pmm_free_page(void* addr) {
    pmm_free_pages(addr, 1);
}

uint32_t pmm_total_frames(void) { return managed_frames; }
uint32_t pmm_used_frames(void)  { return used_frames; }
uint32_t pmm_free_frames(void)  { return managed_frames - used_frames; }
