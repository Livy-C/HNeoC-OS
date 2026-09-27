#include "../include/timer.h"
#include "../include/idt.h"
#include "../include/pic.h"
#include "../include/ports.h"

/* PIT 通道 0 数据端口与命令端口 */
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

/* PIT 基准频率 1.193182 MHz */
#define PIT_BASE_FREQUENCY 1193182

static volatile uint32_t timer_ticks = 0;

/* IRQ0 处理函数：每 10ms 触发一次 */
static void timer_callback(registers_t* regs) {
    (void)regs;
    timer_ticks++;
    pic_send_eoi(0);
}

void timer_init(void) {
    /* 计算分频值：1.193182MHz / 100Hz = 11931 */
    uint32_t divisor = PIT_BASE_FREQUENCY / TIMER_HZ;

    /* 命令字 0x36：通道0、先低字节后高字节、模式3（方波）、二进制计数 */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    register_interrupt_handler(IRQ0, timer_callback);
    pic_clear_mask(0);   /* 开放 IRQ0 */
}

uint32_t timer_get_ticks(void) {
    return timer_ticks;
}

uint32_t timer_get_uptime_seconds(void) {
    return timer_ticks / TIMER_HZ;
}
