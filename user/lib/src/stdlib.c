#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <hneoc.h>

/* ============================================================
 * 内存分配
 *
 * 建在 sbrk 之上：双向链表 + 首次适配 + 相邻块合并，
 * 结构和内核堆完全一样。
 * ============================================================ */

typedef struct block {
    size_t        size;      /* 数据区字节数 */
    uint32_t      magic;
    struct block* next;
    struct block* prev;
    uint32_t      free;
    uint32_t      reserved;
} block_t;

#define BLOCK_MAGIC 0x554C4942u   /* 'BILU' */
#define HEADER_SIZE ((size_t)sizeof(block_t))
#define ALIGN8(x)   (((x) + 7u) & ~7u)

/* 一次向内核要多少：太小会频繁 sbrk，太大浪费地址空间 */
#define HEAP_CHUNK (64u * 1024u)

static block_t* heap_head = NULL;

static block_t* grow_heap(size_t need) {
    size_t bytes = HEAP_CHUNK;
    block_t* b;
    block_t* tail;

    if (bytes < need + HEADER_SIZE) {
        bytes = need + HEADER_SIZE;
        bytes = (bytes + 4095u) & ~4095u;   /* 按页向上取整 */
    }

    b = (block_t*)sbrk((int)bytes);
    if (!b) {
        return NULL;
    }

    b->size     = bytes - HEADER_SIZE;
    b->magic    = BLOCK_MAGIC;
    b->free     = 1;
    b->reserved = 0;
    b->next     = NULL;
    b->prev     = NULL;

    if (!heap_head) {
        heap_head = b;
        return b;
    }

    tail = heap_head;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = b;
    b->prev    = tail;

    /* 和前面那块空闲区合并，减少碎片 */
    if (tail->free) {
        tail->size += HEADER_SIZE + b->size;
        tail->next  = NULL;
        b->magic    = 0;
        return tail;
    }
    return b;
}

void* malloc(size_t size) {
    size_t need;
    block_t* b;

    if (size == 0) {
        return NULL;
    }
    need = ALIGN8(size);

    for (b = heap_head; b; b = b->next) {
        if (!b->free || b->size < need) {
            continue;
        }
        /* 剩下的够放一个头和一点数据才值得切 */
        if (b->size >= need + HEADER_SIZE + 16) {
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
        return (uint8_t*)b + HEADER_SIZE;
    }

    /* 现有空间都不够，向内核再要一块再重试 */
    if (!grow_heap(need)) {
        return NULL;
    }
    return malloc(size);
}

void* calloc(size_t count, size_t size) {
    size_t total = count * size;
    void* p = malloc(total);

    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void free(void* ptr) {
    block_t* b;

    if (!ptr) {
        return;
    }
    b = (block_t*)((uint8_t*)ptr - HEADER_SIZE);

    if (b->magic != BLOCK_MAGIC || b->free) {
        return;   /* 不是我们发出去的指针，或者重复释放 */
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
        n->magic = 0;
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
}

void* realloc(void* ptr, size_t size) {
    block_t* b;
    void* fresh;

    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    b = (block_t*)((uint8_t*)ptr - HEADER_SIZE);
    if (b->magic != BLOCK_MAGIC) {
        return NULL;
    }
    if (b->size >= size) {
        return ptr;   /* 现有空间够用，原地不动 */
    }

    fresh = malloc(size);
    if (!fresh) {
        return NULL;
    }
    memcpy(fresh, ptr, b->size);
    free(ptr);
    return fresh;
}

/* ============================================================
 * 其它
 * ============================================================ */

int atoi(const char* s) {
    int v = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    while (isdigit((int)*s)) {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

long atol(const char* s) {
    return (long)atoi(s);
}

int abs(int v) {
    return v < 0 ? -v : v;
}

void exit(int code) {
    (void)__syscall1(SYS_EXIT, code);
    for (;;) {
        /* exit 系统调用不会返回 */
    }
}
