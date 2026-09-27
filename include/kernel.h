#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

/* 内核版本信息 */
#define KERNEL_VERSION "0.3.0"
#define KERNEL_NAME    "HNeoC OS"

/* 内核主函数（由 kernel/arch.asm 的 kernel_entry 调用） */
void kernel_main(void);

/* 内核恐慌：打印错误信息并永久停机 */
void kernel_panic(const char* message);

#endif /* KERNEL_H */
