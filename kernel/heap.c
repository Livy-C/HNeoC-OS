#include "../include/heap.h"
#include "../include/pmm.h"
#include "../include/string.h"
#include "../include/ports.h"

/* 每个内存块前面的头部。24 字节，正好是 8 的倍数，
 * 这样返回给调用者的数据区始终是 8 字节对齐的。
 */
typedef struct block {
    uint32_t      size;       /* 数据区字节数（不含头部） */
    uint32_t      magic;      /* 头部校验值，用来发现越界写和重复释放 */
    struct block* next;
    struct block* prev;
    uint32_t      free;       /* 1 = 空闲 */
    uint32_t      reserved;   /* 补齐到 24 字节 */
} block_t;

#define BLOCK_MAGIC   0x4C4F5342u   /* 'LOSB' */
#define HEADER_SIZE   ((uint32_t)sizeof(block_t))
#define ALIGN8(x)     (((x) + 7u) & ~7u)

/* 分裂时剩余空间至少要能放下一个头部加 16 字节数据，否则不值得分裂 */
#define MIN_SPLIT     (HEADER_SIZE + 16u)

static block_t* heap_head  = NULL;
static uint32_t heap_total = 0;   /* 堆区总字节数 */

void heap_init(void) {
    /* 向物理内存管理器要一段连续页框。内存碎片化时大块可能拿不到，
     * 所以从大到小依次尝试。
     */
    static const uint32_t sizes_in_pages[] = {
        HEAP_INITIAL_PAGES, 512, 256, 64
    };

    uint8_t* region = NULL;
    uint32_t pages  = 0;

    for (uint32_t i = 0; i < sizeof(sizes_in_pages) / sizeof(sizes_in_pages[0]); i++) {
        region = (uint8_t*)pmm_alloc_pages(sizes_in_pages[i]);
        if (region) {
            pages = sizes_in_pages[i];
            break;
        }
    }

    if (!region) {
        heap_head  = NULL;
        heap_total = 0;
        return;
    }

    heap_total = pages * PAGE_SIZE;

    /* 整段内存做成一整块空闲区 */
    heap_head            = (block_t*)region;
    heap_head->size      = heap_total - HEADER_SIZE;
    heap_head->magic     = BLOCK_MAGIC;
    heap_head->next      = NULL;
    heap_head->prev      = NULL;
    heap_head->free      = 1;
    heap_head->reserved  = 0;
}

void* kmalloc(size_t size) {
    void* result = NULL;
    uint32_t flags;

    if (heap_head == NULL || size == 0) {
        return NULL;
    }

    uint32_t need = ALIGN8((uint32_t)size);

    /* 分配过程中必须关中断：调度器回收僵尸进程时会调 kfree，
     * 如果正好插在链表指针更新到一半的地方，堆就坏了。
     */
    flags = irq_save();

    for (block_t* b = heap_head; b != NULL; b = b->next) {
        if (b->magic != BLOCK_MAGIC) {
            goto out;   /* 头部被破坏了，拒绝继续分配 */
        }
        if (!b->free || b->size < need) {
            continue;
        }

        /* 剩余空间够大就切一块出去，避免大块被小请求整块占住 */
        if (b->size >= need + MIN_SPLIT) {
            block_t* rest = (block_t*)((uint8_t*)b + HEADER_SIZE + need);

            rest->size     = b->size - need - HEADER_SIZE;
            rest->magic    = BLOCK_MAGIC;
            rest->free     = 1;
            rest->reserved = 0;
            rest->prev     = b;
            rest->next     = b->next;

            if (b->next) {
                b->next->prev = rest;
            }
            b->next = rest;
            b->size = need;
        }

        b->free = 0;
        result = (uint8_t*)b + HEADER_SIZE;
        goto out;
    }

out:
    irq_restore(flags);
    return result;
}

void* kzalloc(size_t size) {
    void* p = kmalloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void kfree(void* ptr) {
    uint32_t flags;

    if (ptr == NULL || heap_head == NULL) {
        return;
    }

    block_t* b = (block_t*)((uint8_t*)ptr - HEADER_SIZE);

    flags = irq_save();

    if (b->magic != BLOCK_MAGIC) {
        irq_restore(flags);
        return;   /* 不是我们发出去的指针，直接忽略 */
    }
    if (b->free) {
        irq_restore(flags);
        return;   /* 重复释放 */
    }

    b->free = 1;

    /* 与后一块合并 */
    if (b->next && b->next->free && b->next->magic == BLOCK_MAGIC) {
        block_t* n = b->next;
        b->size += HEADER_SIZE + n->size;
        b->next = n->next;
        if (n->next) {
            n->next->prev = b;
        }
        n->magic = 0;   /* 让悬空引用立刻失效 */
    }

    /* 与前一块合并 */
    if (b->prev && b->prev->free && b->prev->magic == BLOCK_MAGIC) {
        block_t* p = b->prev;
        p->size += HEADER_SIZE + b->size;
        p->next = b->next;
        if (b->next) {
            b->next->prev = p;
        }
        b->magic = 0;
    }

    irq_restore(flags);
}

void heap_stats(uint32_t* total_bytes, uint32_t* used_bytes,
                uint32_t* free_bytes, uint32_t* block_count) {
    uint32_t used = 0;
    uint32_t freeb = 0;
    uint32_t count = 0;

    for (block_t* b = heap_head; b != NULL; b = b->next) {
        if (b->magic != BLOCK_MAGIC) {
            break;
        }
        count++;
        if (b->free) {
            freeb += b->size;
        } else {
            used += b->size;
        }
    }

    if (total_bytes) *total_bytes = heap_total;
    if (used_bytes)  *used_bytes  = used;
    if (free_bytes)  *free_bytes  = freeb;
    if (block_count) *block_count = count;
}

bool heap_self_test(void) {
    /* 分配几块，写入特征值，释放后再分配同样大小检查是否复用了空间 */
    void* a = kmalloc(100);
    void* b = kmalloc(4096);
    void* c = kmalloc(64);

    if (!a || !b || !c) {
        kfree(a); kfree(b); kfree(c);
        return false;
    }

    memset(a, 0xAA, 100);
    memset(b, 0xBB, 4096);
    memset(c, 0xCC, 64);

    /* 检查写进去的数据没有被别人踩掉 */
    uint8_t* pa = (uint8_t*)a;
    uint8_t* pb = (uint8_t*)b;
    uint8_t* pc = (uint8_t*)c;

    for (int i = 0; i < 100; i++) {
        if (pa[i] != 0xAA) { kfree(a); kfree(b); kfree(c); return false; }
    }
    for (int i = 0; i < 4096; i++) {
        if (pb[i] != 0xBB) { kfree(a); kfree(b); kfree(c); return false; }
    }
    for (int i = 0; i < 64; i++) {
        if (pc[i] != 0xCC) { kfree(a); kfree(b); kfree(c); return false; }
    }

    kfree(a);
    kfree(b);
    kfree(c);

    /* 释放后应该能重新申请到同样大的块（相邻块已合并回一整块） */
    void* d = kmalloc(4096);
    if (!d) {
        return false;
    }
    kfree(d);

    /* 所有块都释放之后，应该合并成唯一的一个空闲块 */
    uint32_t blocks = 0;
    heap_stats(NULL, NULL, NULL, &blocks);
    return blocks == 1;
}
