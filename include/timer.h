#ifndef TIMER_H
#define TIMER_H

#include "types.h"

/* 定时器中断频率（赫兹） */
#define TIMER_HZ 100

/* 初始化 PIT 定时器（注册 IRQ0 处理函数） */
void timer_init(void);

/* 自启动以来经过的定时器滴答数 */
uint32_t timer_get_ticks(void);

/* 自启动以来经过的秒数 */
uint32_t timer_get_uptime_seconds(void);

#endif /* TIMER_H */
