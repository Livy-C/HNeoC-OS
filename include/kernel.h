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

/* 内核栈守卫。
 *
 * 内核 BSS 结束（bss_end ≈ 0x63460）和 Shell 用的引导栈（0x90000）之间
 * 只有不到 200KB，而且没有任何保护页：一旦调用链吃超了，栈会静默穿过
 * BSS、内核映像和 paging 的 pd_storage，最后压在页目录（0x24000）上 ——
 * 页目录一坏连异常都进不去，CPU 直接三重故障，什么线索都不留。
 *
 * 所以在几个关键入口量一下 esp，掉到 KERNEL_STACK_FLOOR 以下就 panic，
 * 并把入口名、当时的 esp 和返回地址打出来。
 */
#define KERNEL_STACK_FLOOR  0x00070000u
void stack_guard(const char* where);

#endif /* KERNEL_H */
