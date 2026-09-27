#ifndef PORTS_H
#define PORTS_H

#include "types.h"

/* 向 I/O 端口写入一个字节 */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* 从 I/O 端口读取一个字节 */
static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ __volatile__("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* 向 I/O 端口写入一个字 */
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* 从 I/O 端口读取一个字 */
static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ __volatile__("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* 短暂延迟（向未使用的端口 0x80 写入，用于让慢速设备跟上） */
static inline void io_wait(void) {
    outb(0x80, 0);
}

/* 开中断 */
static inline void enable_interrupts(void) {
    __asm__ __volatile__("sti");
}

/* 关中断 */
static inline void disable_interrupts(void) {
    __asm__ __volatile__("cli");
}

/* 保存中断标志位并关中断，返回原来的 EFLAGS。
 * 和 irq_restore 配对使用，可以精确还原调用前的开中断状态。
 */
static inline uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

/* 恢复 irq_save 保存的中断标志 */
static inline void irq_restore(uint32_t flags) {
    __asm__ __volatile__("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
}

/* CPU 进入低功耗等待状态，直到下一个中断到来 */
static inline void cpu_halt(void) {
    __asm__ __volatile__("hlt");
}

#endif /* PORTS_H */
