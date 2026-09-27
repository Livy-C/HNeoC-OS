#include "../include/pic.h"
#include "../include/ports.h"

/* PIC 重映射后的中断向量偏移 */
#define PIC1_OFFSET 0x20   /* 主片 -> 向量 32-39 */
#define PIC2_OFFSET 0x28   /* 从片 -> 向量 40-47 */

/* 初始化控制字 */
#define ICW1_ICW4       0x01
#define ICW1_SINGLE     0x02
#define ICW1_INTERVAL4  0x04
#define ICW1_LEVEL      0x08
#define ICW1_INIT       0x10

#define ICW4_8086       0x01
#define ICW4_AUTO       0x02
#define ICW4_BUF_SLAVE  0x08
#define ICW4_BUF_MASTER 0x0C
#define ICW4_SFNM       0x10

/* 把 8259 PIC 从 BIOS 默认的 0x08-0x0F / 0x70-0x77 重映射到
 * 0x20-0x2F，否则会和 CPU 异常向量冲突
 */
void pic_init(void) {
    /* 保存原有屏蔽字 */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* 开始初始化序列（ICW1） */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2：设置中断向量偏移 */
    outb(PIC1_DATA, PIC1_OFFSET);
    io_wait();
    outb(PIC2_DATA, PIC2_OFFSET);
    io_wait();

    /* ICW3：主从片级联关系 */
    outb(PIC1_DATA, 0x04);   /* 从片接在主片的 IRQ2 上 */
    io_wait();
    outb(PIC2_DATA, 0x02);   /* 从片的级联标识 */
    io_wait();

    /* ICW4：8086 模式 */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* 恢复屏蔽字（先全部屏蔽，由各驱动按需开放） */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    (void)mask1;
    (void)mask2;
}

/* 屏蔽指定 IRQ */
void pic_set_mask(uint8_t irq_line) {
    uint16_t port;
    uint8_t value;

    if (irq_line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq_line -= 8;
    }

    value = inb(port) | (uint8_t)(1 << irq_line);
    outb(port, value);
}

/* 解除屏蔽指定 IRQ */
void pic_clear_mask(uint8_t irq_line) {
    uint16_t port;
    uint8_t value;

    if (irq_line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq_line -= 8;
    }

    value = inb(port) & (uint8_t)~(1 << irq_line);
    outb(port, value);
}

/* 发送 EOI（中断结束）信号 */
void pic_send_eoi(uint8_t irq_line) {
    if (irq_line >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);   /* 从片先发 */
    }
    outb(PIC1_COMMAND, PIC_EOI);
}
