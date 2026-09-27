#ifndef IDT_H
#define IDT_H

#include "types.h"

/* 中断描述符表项（8 字节） */
typedef struct {
    uint16_t base_low;   /* 处理函数地址的低 16 位 */
    uint16_t selector;   /* 代码段选择子 */
    uint8_t  always0;    /* 保留，必须为 0 */
    uint8_t  flags;      /* 类型和权限标志 */
    uint16_t base_high;  /* 处理函数地址的高 16 位 */
} __attribute__((packed)) idt_entry_t;

/* 交给 lidt 指令的结构（6 字节） */
typedef struct {
    uint16_t limit;      /* IDT 大小 - 1 */
    uint32_t base;       /* IDT 起始地址 */
} __attribute__((packed)) idt_ptr_t;

/* 中断发生时压入栈的寄存器快照
 * 布局必须与 kernel/isr.asm 中 isr_common 的压栈顺序严格一致
 */
typedef struct {
    uint32_t ds;                                      /* 数据段选择子 */
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;  /* pusha 压入的通用寄存器 */
    uint32_t int_no, err_code;                        /* 中断号与错误码 */
    uint32_t eip, cs, eflags, useresp, ss;            /* CPU 自动压入的现场 */
} registers_t;

/* 中断处理函数类型 */
typedef void (*isr_t)(registers_t* regs);

/* 中断号定义 */
#define IRQ0  32   /* 定时器 */
#define IRQ1  33   /* 键盘 */
#define IRQ2  34
#define IRQ3  35
#define IRQ4  36
#define IRQ5  37
#define IRQ6  38
#define IRQ7  39
#define IRQ8  40
#define IRQ9  41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44   /* 鼠标 */
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

/* 初始化 IDT 并加载 */
void idt_init(void);

/* 设置单个门描述符 */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags);

/* 注册 C 语言中断处理函数 */
void register_interrupt_handler(uint8_t num, isr_t handler);

/* 由 isr_common_stub 调用的总入口。
 * 返回下一个任务的内核栈指针（0 表示不切换），汇编存根据此完成上下文切换。
 */
uint32_t isr_handler(registers_t* regs);

#endif /* IDT_H */
