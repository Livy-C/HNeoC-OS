#include "../include/idt.h"
#include "../include/ports.h"
#include "../include/vga.h"
#include "../include/paging.h"
#include "../include/process.h"

/* 256 个中断门 */
#define IDT_ENTRIES 256

/* 门类型标志 */
#define IDT_FLAG_PRESENT  0x80
#define IDT_FLAG_RING0    0x00
#define IDT_FLAG_INT_GATE 0x0E   /* 32 位中断门 */
#define IDT_KERNEL_CODE   0x08   /* GDT 中的内核代码段选择子 */

static idt_entry_t idt_entries[IDT_ENTRIES];
static idt_ptr_t   idt_ptr;

/* C 语言中断处理函数表 */
static isr_t interrupt_handlers[IDT_ENTRIES];

/* 由 kernel/arch.asm 提供的存根地址表 */
extern uint32_t interrupt_stub_table[];
extern uint32_t interrupt_stub_count;

/* CPU 异常名称表，用于崩溃时打印可读信息 */
static const char* exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Unknown", "Unknown", "Unknown", "Unknown", "Unknown",
    "Unknown", "Unknown", "Unknown",
    "Security Exception",
    "Unknown"
};

/* 设置一个中断门 */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags) {
    idt_entries[num].base_low  = base & 0xFFFF;
    idt_entries[num].base_high = (base >> 16) & 0xFFFF;
    idt_entries[num].selector  = selector;
    idt_entries[num].always0   = 0;
    idt_entries[num].flags     = flags;
}

/* 注册 C 语言中断处理函数 */
void register_interrupt_handler(uint8_t num, isr_t handler) {
    interrupt_handlers[num] = handler;
}

/* 打印一个 32 位十六进制数（崩溃诊断用） */
static void vga_write_hex32(uint32_t value) {
    const char* digits = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = digits[(value >> ((7 - i) * 4)) & 0xF];
    }
    buf[10] = '\0';
    vga_write(buf);
}

/* 由 isr_common_stub 调用的总入口。
 *
 * 返回值是"下一个要运行的任务的内核栈指针"，0 表示继续跑当前任务。
 * 汇编里的存根拿到非 0 值就会把 esp 换过去，随后的 popa / iret 弹出
 * 的就是那个任务的寄存器现场 —— 上下文切换就是这么完成的。
 */
uint32_t isr_handler(registers_t* regs) {
    /* 中断号 0-31 属于 CPU 异常。 */
    if (regs->int_no < 32) {
        process_t* p = process_current();

        /* 发生在 ring 3 的异常是用户程序自己的 bug（空指针、除零、越界），
         * 没有任何理由让整个系统陪葬：把这个进程杀掉，照常调度下一个任务，
         * Shell 拿回提示符就好。
         * 内核态异常仍然停机 —— 那是内核自己的问题，硬撑下去只会更糟。
         */
        if ((regs->cs & 3) == 3 && p && p->pid != 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_write("process '");
            vga_write(p->name);
            vga_write("' killed: ");
            vga_writeln(exception_messages[regs->int_no]);
            vga_write("   EIP ");
            vga_write_hex32(regs->eip);
            if (regs->int_no == 14) {
                vga_write("   CR2 ");
                vga_write_hex32(paging_fault_address());
            }
            vga_writeln("");
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

            process_exit_current(-1);

            /* force_switch 已经由 process_exit_current 置位，所以这个非时钟中断
             * 也会真的换人；返回的是下一个任务的内核栈指针，不会再回到这里。
             */
            return sched_tick(regs);
        }

        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
        vga_clear();
        vga_writeln("");
        vga_writeln("   *** CPU EXCEPTION ***");
        vga_writeln("");
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_RED);
        vga_write("   ");
        vga_writeln(exception_messages[regs->int_no]);
        vga_write("   Interrupt: ");
        vga_write_hex32(regs->int_no);
        vga_writeln("");
        vga_write("   Error code: ");
        vga_write_hex32(regs->err_code);
        vga_writeln("");
        vga_write("   EIP: ");
        vga_write_hex32(regs->eip);
        vga_writeln("");
        vga_write("   CS : ");
        vga_write_hex32(regs->cs);
        vga_write("   EFLAGS: ");
        vga_write_hex32(regs->eflags);
        vga_writeln("");
        vga_write("   SS : ");
        vga_write_hex32(regs->ss);
        vga_write("   ESP: ");
        vga_write_hex32(regs->useresp);
        vga_writeln("");
        vga_write("   ESP0(frame): ");
        vga_write_hex32((uint32_t)regs);
        vga_writeln("");

        /* 页错误要额外打印出错地址（CR2）和错误码含义 */
        if (regs->int_no == 14) {
            uint32_t err = regs->err_code;

            vga_write("   CR2 (faulting address): ");
            vga_write_hex32(paging_fault_address());
            vga_writeln("");
            vga_write("   cause: ");
            vga_writeln((err & 0x1) ? "protection violation" : "page not present");
            vga_write("   access: ");
            vga_writeln((err & 0x2) ? "write" : "read");
            vga_write("   mode: ");
            vga_writeln((err & 0x4) ? "user" : "supervisor");
        }

        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
        vga_writeln("");
        vga_writeln("   System halted.");

        disable_interrupts();
        for (;;) {
            cpu_halt();
        }
    }

    /* 硬件中断：交给注册的处理函数 */
    if (interrupt_handlers[regs->int_no] != NULL) {
        interrupt_handlers[regs->int_no](regs);
    }

    /* 交给调度器决定要不要切走。只有时钟中断会真正触发抢占，
     * 其余中断它一律返回 0。
     */
    return sched_tick(regs);
}

/* 初始化并加载 IDT */
void idt_init(void) {
    idt_ptr.limit = sizeof(idt_entry_t) * IDT_ENTRIES - 1;
    idt_ptr.base  = (uint32_t)&idt_entries;

    /* 先把所有表项清零，未使用的中断会走兜底存根 */
    uint8_t* raw = (uint8_t*)&idt_entries;
    for (uint32_t i = 0; i < sizeof(idt_entries); i++) {
        raw[i] = 0;
    }
    for (uint32_t i = 0; i < IDT_ENTRIES; i++) {
        interrupt_handlers[i] = NULL;
    }

    uint32_t stub_count = interrupt_stub_count;

    /* 0..47 使用专用存根（异常 + IRQ） */
    for (uint32_t i = 0; i < stub_count - 1 && i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i, interrupt_stub_table[i], IDT_KERNEL_CODE,
                     IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_INT_GATE);
    }

    /* 其余全部指向兜底存根，避免未预期中断导致三重故障重启 */
    uint32_t default_stub = interrupt_stub_table[stub_count - 1];
    for (uint32_t i = stub_count - 1; i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i, default_stub, IDT_KERNEL_CODE,
                     IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_INT_GATE);
    }

    /* 加载 IDT */
    __asm__ __volatile__("lidt %0" : : "m"(idt_ptr));
}
