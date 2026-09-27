#include "../include/gdt.h"
#include "../include/string.h"

/* 6 个描述符：空 + ring0 代码 + ring0 数据 + ring3 代码 + ring3 数据 + TSS */
#define GDT_ENTRIES 6

static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdt_ptr;
static tss_entry_t tss;

/* 由 kernel/arch.asm 提供：重新加载段寄存器并 ltr */
extern void gdt_flush(uint32_t gdt_ptr_addr);
extern void tss_flush(void);

/* 填一个普通段描述符 */
static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t granularity) {
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    /* 粒度字节的高 4 位是标志：G=4KB 粒度, D/B=32 位, L=0, AVL=0 */
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (granularity & 0xF0));

    gdt[index].access      = access;
}

void gdt_init(void) {
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint32_t)&gdt;

    /* 0x00 空描述符 */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* 0x08 ring0 代码段：P=1 DPL=0 S=1 type=1010(可执行/可读) */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);

    /* 0x10 ring0 数据段：P=1 DPL=0 S=1 type=0010(可读写) */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);

    /* 0x18 ring3 代码段：DPL=3 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);

    /* 0x20 ring3 数据段：DPL=3 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);

    /* 0x28 TSS：P=1 DPL=0 type=1001(32 位可用 TSS) */
    memset(&tss, 0, sizeof(tss));
    tss.ss0        = GDT_KERNEL_DATA;
    tss.esp0       = 0;
    tss.iomap_base = sizeof(tss);   /* 指向 TSS 末尾 = 没有 IO 权限位图 */
    gdt_set_entry(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

const tss_entry_t* tss_get(void) {
    return &tss;
}
