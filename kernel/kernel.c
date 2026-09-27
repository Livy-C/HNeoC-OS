#include "../include/kernel.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/idt.h"
#include "../include/pic.h"
#include "../include/keyboard.h"
#include "../include/mouse.h"
#include "../include/timer.h"
#include "../include/shell.h"
#include "../include/ports.h"
#include "../include/serial.h"
#include "../include/e820.h"
#include "../include/pmm.h"
#include "../include/heap.h"
#include "../include/paging.h"
#include "../include/ata.h"
#include "../include/hneofs.h"
#include "../include/gdt.h"
#include "../include/process.h"
#include "../include/syscall.h"

/* 注意：所有显示在屏幕上的字符串都必须使用 ASCII。
 * VGA 文本模式用的是 CP437 字符集，没有汉字字形，写中文只会显示成乱码。
 * 代码注释可以继续用中文，界面文字一律用英文。
 */

/* 打印一行 [ OK ] 形式的启动日志 */
static void boot_step(const char* name) {
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_write("  [ ");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_write("OK");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_write(" ] ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_writeln(name);
}

/* 内核主入口：由 kernel/arch.asm 中的 kernel_entry 调用
 * 此时已经处于 32 位保护模式，BSS 已被清零
 */
void kernel_main(void) {
    /* 1. 先把串口调好，这样后面所有屏幕输出都会被镜像一份到 COM1，
     *    方便在宿主机上做无图形界面的调试
     */
    serial_init();

    /* 2. 显示子系统 */
    vga_init();
    vga_clear();
    shell_print_banner();

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_writeln("Initializing kernel subsystems...");
    vga_writeln("");

    /* 3. GDT：必须由内核重建，加入 ring3 段和 TSS，
     *    否则用户态程序一触发中断就会三重故障重启
     */
    gdt_init();
    boot_step("GDT rebuilt (ring0/ring3 segments + TSS)");

    /* 4. 中断描述符表（必须在开中断之前装好） */
    idt_init();
    boot_step("Interrupt descriptor table (IDT, 256 gates)");

    /* 4. 可编程中断控制器：把硬件中断重映射到 0x20-0x2F */
    pic_init();
    boot_step("Programmable interrupt controller (8259A remapped)");

    /* 5. 定时器：IRQ0，100Hz */
    timer_init();
    boot_step("System timer (PIT, 100 Hz)");

    /* 6. 键盘：IRQ1 */
    keyboard_init();
    boot_step("PS/2 keyboard driver (IRQ1)");

    /* 7. 鼠标：IRQ12，主要用来支持滚轮翻看滚过去的输出 */
    mouse_init();
    if (mouse_present()) {
        boot_step(mouse_has_wheel()
                      ? "PS/2 mouse (IRQ12, scroll wheel enabled)"
                      : "PS/2 mouse (IRQ12, no scroll wheel)");
    } else {
        boot_step("PS/2 mouse (not present)");
    }

    /* 8. 串口调试通道：COM1 */
    boot_step("Serial debug console (COM1, 38400 baud)");

    /* 8. 内存管理：先用 E820 结果建立物理页框位图，再在上面开出内核堆 */
    pmm_init();
    boot_step("Physical memory manager (4KB frames, bitmap)");

    heap_init();
    boot_step("Kernel heap (kmalloc / kfree)");

    /* 9. 打开分页。恒等映射前 64MB，用户区（1GB 处）留给进程自己挂页表 */
    paging_init();
    boot_step("Paging enabled (identity map 0-64MB, 4MB pages)");

    /* 10. 探测 ATA 磁盘，之后文件系统就架在它上面 */
    ata_init();
    boot_step("ATA disk driver (PIO mode, primary channel)");

    /* 11. 挂载文件系统。失败不算致命错误，Shell 里的 ls / cat 会提示未挂载 */
    hneofs_mount();

    /* 12. 进程与系统调用：0x80 号门必须在开中断之前注册好 */
    process_init();
    syscall_init();
    boot_step("Process table and int 0x80 syscall gate");

    /* 13. 打开中断，之后键盘和定时器开始工作 */
    enable_interrupts();
    boot_step("Interrupts enabled");

    vga_writeln("");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_write(KERNEL_NAME " v" KERNEL_VERSION);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_writeln(" boot complete.");
    vga_writeln("");

    /* 9. 交给 Shell，正常情况下永不返回 */
    shell_run();

    /* 理论上到不了这里 */
    kernel_panic("shell returned unexpectedly");
}

/* 内核恐慌：发生无法恢复的错误时调用 */
void kernel_panic(const char* message) {
    disable_interrupts();

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    vga_clear();
    vga_writeln("");
    vga_writeln("  *** KERNEL PANIC ***");
    vga_writeln("");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_RED);
    vga_write("  Error: ");
    vga_writeln(message);
    vga_writeln("");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    vga_writeln("  System halted. Reboot the virtual machine.");

    for (;;) {
        cpu_halt();
    }
}
