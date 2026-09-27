#include "../include/serial.h"
#include "../include/ports.h"

/* COM1 的寄存器偏移 */
#define COM1 0x3F8

#define REG_DATA        0   /* 数据寄存器（DLAB=0）/ 分频低字节（DLAB=1） */
#define REG_INT_ENABLE  1   /* 中断使能 / 分频高字节（DLAB=1） */
#define REG_FIFO_CTRL   2   /* FIFO 控制 */
#define REG_LINE_CTRL   3   /* 线路控制（bit7 = DLAB） */
#define REG_MODEM_CTRL  4   /* MODEM 控制 */
#define REG_LINE_STATUS 5   /* 线路状态 */

void serial_init(void) {
    outb(COM1 + REG_INT_ENABLE, 0x00);   /* 关闭串口中断，只用轮询发送 */
    outb(COM1 + REG_LINE_CTRL, 0x80);    /* 置位 DLAB，准备设置波特率 */
    outb(COM1 + REG_DATA, 0x03);         /* 分频值低字节：115200/3 = 38400 */
    outb(COM1 + REG_INT_ENABLE, 0x00);   /* 分频值高字节 */
    outb(COM1 + REG_LINE_CTRL, 0x03);    /* 8 位数据、无校验、1 位停止位 */
    outb(COM1 + REG_FIFO_CTRL, 0xC7);    /* 打开 FIFO，清空，阈值 14 字节 */
    outb(COM1 + REG_MODEM_CTRL, 0x0B);   /* 打开 IRQ，置位 RTS/DSR */
}

/* 发送寄存器为空（bit5）时才可以写下一个字节 */
static bool serial_tx_ready(void) {
    return (inb(COM1 + REG_LINE_STATUS) & 0x20) != 0;
}

void serial_putchar(char c) {
    /* 直接写 '\n' 在终端里只会下移一行而不回到行首，
     * 所以换行时补一个 '\r'
     */
    if (c == '\n') {
        serial_putchar('\r');
    }

    while (!serial_tx_ready()) {
        /* 等待发送缓冲腾空 */
    }
    outb(COM1 + REG_DATA, (uint8_t)c);
}

void serial_write(const char* str) {
    if (!str) return;
    while (*str) {
        serial_putchar(*str++);
    }
}

void serial_writeln(const char* str) {
    serial_write(str);
    serial_putchar('\n');
}
