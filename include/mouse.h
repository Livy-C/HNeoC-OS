#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

/* PS/2 鼠标驱动
 *
 * 挂在 8042 控制器的辅助端口上，用 IRQ12。滚轮需要先做 IntelliMouse
 * 握手（依次把采样率设成 200 / 100 / 80 再读设备 ID，返回 3 就说明
 * 是带滚轮的型号），之后每个数据包是 4 个字节而不是 3 个。
 */

void mouse_init(void);

/* 是否探测到了鼠标 */
bool mouse_present(void);

/* 是否支持滚轮（IntelliMouse 模式握手成功） */
bool mouse_has_wheel(void);

/* 当前指针位置（相对移动累加得到）和按键状态 */
void mouse_get_state(int32_t* x, int32_t* y, uint8_t* buttons);

/* 累计收到的数据包数量，用来验证驱动确实在工作 */
uint32_t mouse_packets(void);
uint32_t mouse_wheel_events(void);

#endif /* MOUSE_H */
