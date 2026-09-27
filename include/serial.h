#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

/* 串口调试输出（COM1）
 *
 * 裸机调试不能靠 printf，也不方便每次都开图形界面看屏幕。
 * 把内核日志镜像一份到 COM1 之后，只要在虚拟机里把串口重定向到
 * 文件，就能在宿主机上直接读到屏幕上出现过的每一个字符：
 *
 *   VBoxManage modifyvm <vm> --uart1 0x3F8 4 \
 *                            --uartmode1 file D:\path\serial.log
 */

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char* str);
void serial_writeln(const char* str);

#endif /* SERIAL_H */
