#ifndef PAGING_H
#define PAGING_H

#include "types.h"

/* 页表项 / 页目录项的标志位 */
#define PAGE_PRESENT  0x001
#define PAGE_WRITE    0x002
#define PAGE_USER     0x004
#define PAGE_ACCESSED 0x020
#define PAGE_DIRTY    0x040
#define PAGE_4MB      0x080   /* 只对页目录项有效：4MB 大页 */
#define PAGE_ADDR_MASK 0xFFFFF000u

/* 页大小 */
#define PAGE_4KB_SIZE 0x1000
#define PAGE_4MB_SIZE 0x400000

/* 内核恒等映射的范围：前 64MB。16 个 4MB 大页就够，
 * 用大页的好处是只需要一个页目录，不用为内核再建 16 张页表。
 */
#define KERNEL_IDENTITY_PDES 16

/* 用户程序统一加载到这个虚拟地址（1GB 处） */
#define USER_BASE       0x40000000u
/* 每个用户程序最多能用 4MB（正好一个页表覆盖的范围） */
#define USER_REGION_SIZE PAGE_4MB_SIZE
#define USER_MAX_PAGES   (USER_REGION_SIZE / PAGE_4KB_SIZE)

/* 用户页表在页目录中的下标 */
#define USER_PDE_INDEX   (USER_BASE >> 22)

/* 打开分页。调用之后虚拟地址 == 物理地址（恒等映射），
 * 但用户区的页目录项已经预留好，可以挂每个进程自己的页表。
 */
void paging_init(void);

/* 分页是否已经启用 */
bool paging_enabled(void);

/* 建立一张新的页目录：内核恒等映射项从主目录复制过来，
 * 用户区的页目录项留空，等 paging_attach_user_table 挂上。
 * 返回的地址是物理地址，同时也是内核可直接访问的指针。
 */
uint32_t* paging_create_directory(void);

/* 把一个用户页表挂到页目录的用户区项上 */
bool paging_attach_user_table(uint32_t* pd, uint32_t* table);

/* 为一个用户进程建立页表，返回页表的物理地址（也就是内核可直接访问的指针）。
 * 表里所有项都是"不存在"，需要调用者自己用 paging_map_user_page 填。
 */
uint32_t* paging_create_user_table(void);

/* 在用户页表里建立 vaddr -> paddr 的映射。
 * vaddr 必须落在 USER_BASE .. USER_BASE+4MB 之间。
 */
bool paging_map_user_page(uint32_t* table, uint32_t vaddr, uint32_t paddr, bool writable);

/* 切换地址空间（写 CR3）。传入页目录的物理地址 */
void paging_switch_directory(uint32_t pd_phys);

/* 当前内核页目录的物理地址 */
uint32_t paging_kernel_directory(void);

/* 读 CR2（最近一次缺页的线性地址），供异常处理打印 */
uint32_t paging_fault_address(void);

/* 统计：内核页目录占了多少项，用户区的页目录项下标是多少 */
void paging_stats(uint32_t* identity_mb, uint32_t* user_pde_index);

#endif /* PAGING_H */
