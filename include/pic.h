#ifndef PIC_H
#define PIC_H

#include "types.h"

/* 主片与从片 PIC 的 I/O 端口 */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

/* 初始化控制字 */
#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01

/* 结束中断命令 */
#define PIC_EOI      0x20

/* 把 PIC 的中断向量重映射到 0x20-0x2F */
void pic_init(void);

/* 屏蔽指定 IRQ 线 */
void pic_set_mask(uint8_t irq_line);

/* 解除屏蔽指定 IRQ 线 */
void pic_clear_mask(uint8_t irq_line);

/* 发送中断结束信号 */
void pic_send_eoi(uint8_t irq_line);

#endif /* PIC_H */
