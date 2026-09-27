#ifndef GDT_H
#define GDT_H

#include "types.h"

/* ============================================================
 * 内核自己的 GDT
 *
 * 引导扇区里那张 GDT 只有 ring 0 的两个段，要跑用户态程序就必须
 * 由内核重新建立一张，并且加入：
 *   - 用户代码段（DPL=3）和用户数据段（DPL=3）
 *   - 一个 TSS。ring 3 触发中断时 CPU 需要从 TSS 里取 ring 0 的
 *     SS:ESP，没有 TSS 就会三重故障直接重启。
 *
 * 段选择子布局（和引导扇区保持一致，内核代码/数据仍是 0x08 / 0x10）：
 *   0x00 空描述符
 *   0x08 ring0 代码段
 *   0x10 ring0 数据段
 *   0x18 ring3 代码段  -> 选择子 0x1B
 *   0x20 ring3 数据段  -> 选择子 0x23
 *   0x28 TSS
 * ============================================================ */

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x1B   /* 0x18 | RPL 3 */
#define GDT_USER_DATA   0x23   /* 0x20 | RPL 3 */
#define GDT_TSS         0x28

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

/* 32 位任务状态段。我们只用得到 esp0 / ss0 两个字段：
 * 从 ring 3 陷入 ring 0 时，CPU 会把栈切到这里。
 */
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;          /* ring 0 栈指针 */
    uint32_t ss0;           /* ring 0 栈段 */
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

/* 建立并加载 GDT，同时装好 TSS */
void gdt_init(void);

/* 设置 ring 3 陷入内核时使用的栈（每个进程有自己的内核栈） */
void tss_set_kernel_stack(uint32_t esp0);

/* TSS 的地址，调试用 */
const tss_entry_t* tss_get(void);

#endif /* GDT_H */
