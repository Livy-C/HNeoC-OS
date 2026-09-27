#include "../include/paging.h"
#include "../include/pmm.h"
#include "../include/string.h"

/* 页目录必须 4KB 对齐。这里不依赖链接器的对齐保证
 * （objcopy 生成的裸二进制里对齐不一定可靠），
 * 而是多留 4KB 出来，运行时手工对齐。
 */
static uint8_t  pd_storage[PAGE_4KB_SIZE + PAGE_4KB_SIZE];
static uint32_t* page_directory = NULL;
static bool     enabled = false;

void paging_init(void) {
    /* 运行时把页目录对齐到 4KB 边界 */
    uint32_t addr = (uint32_t)pd_storage;
    addr = (addr + PAGE_4KB_SIZE - 1) & PAGE_ADDR_MASK;
    page_directory = (uint32_t*)addr;

    /* 先全部清成"不存在"，未映射的地址访问就会触发缺页异常，
     * 而不是默默读到随机内存
     */
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0;
    }

    /* 恒等映射前 64MB：虚拟地址 == 物理地址。
     * 内核、VGA 显存、页框分配器交出来的内存都在这个范围里，
     * 所以内核不需要做任何地址转换。
     * 这些项不带 PAGE_USER，ring 3 访问会直接触发缺页。
     */
    for (int i = 0; i < KERNEL_IDENTITY_PDES; i++) {
        page_directory[i] = (uint32_t)(i * PAGE_4MB_SIZE)
                          | PAGE_PRESENT | PAGE_WRITE | PAGE_4MB;
    }

    /* 用户区（1GB 处）的页目录项留空，等创建进程时再挂页表 */

    /* 打开 CR4.PSE，让页目录项里的 PS 位真的生效为 4MB 大页 */
    __asm__ __volatile__(
        "mov %%cr4, %%eax\n"
        "or  $0x00000010, %%eax\n"
        "mov %%eax, %%cr4\n"
        : : : "eax");

    /* 写 CR3 装载页目录 */
    __asm__ __volatile__("mov %0, %%cr3" : : "r"((uint32_t)page_directory) : "memory");

    /* 最后打开 CR0.PG。这一句执行完，下一条指令就已经在分页模式下了。
     * 因为映射是恒等的，取指不会出问题。
     */
    __asm__ __volatile__(
        "mov %%cr0, %%eax\n"
        "or  $0x80000000, %%eax\n"
        "mov %%eax, %%cr0\n"
        : : : "eax", "memory");

    enabled = true;
}

bool paging_enabled(void) {
    return enabled;
}

uint32_t* paging_create_directory(void) {
    if (!page_directory) {
        return NULL;
    }

    uint32_t* pd = (uint32_t*)pmm_alloc_page();
    if (!pd) {
        return NULL;
    }

    /* 把内核的恒等映射整份复制过去。
     * 这样用户程序跑起来之后，内核代码、数据、VGA 显存依然可访问，
     * 中断处理程序和系统调用才能正常工作。
     * 用户区那一项在主目录里本来就是空的，复制过去也是空的。
     */
    for (int i = 0; i < 1024; i++) {
        pd[i] = page_directory[i];
    }
    pd[USER_PDE_INDEX] = 0;

    return pd;
}

bool paging_attach_user_table(uint32_t* pd, uint32_t* table) {
    if (!pd || !table) {
        return false;
    }

    /* 页目录项本身也要带 USER，否则 ring 3 连页表都读不到 */
    pd[USER_PDE_INDEX] = ((uint32_t)table & PAGE_ADDR_MASK)
                       | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    return true;
}

uint32_t* paging_create_user_table(void) {
    /* 页表本身也要占一页，从页框分配器拿，天然就是 4KB 对齐的 */
    uint32_t* table = (uint32_t*)pmm_alloc_page();
    if (!table) {
        return NULL;
    }

    /* 全部标成"不存在"，用户区一开始是空的 */
    for (int i = 0; i < 1024; i++) {
        table[i] = 0;
    }
    return table;
}

bool paging_map_user_page(uint32_t* table, uint32_t vaddr, uint32_t paddr, bool writable) {
    if (!table) {
        return false;
    }
    if (vaddr < USER_BASE || vaddr >= USER_BASE + USER_REGION_SIZE) {
        return false;
    }
    if (vaddr & (PAGE_4KB_SIZE - 1)) {
        return false;   /* 必须页对齐 */
    }

    uint32_t index = (vaddr - USER_BASE) >> 12;

    table[index] = (paddr & PAGE_ADDR_MASK)
                 | PAGE_PRESENT | PAGE_USER
                 | (writable ? PAGE_WRITE : 0);
    return true;
}

void paging_switch_directory(uint32_t pd_phys) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

uint32_t paging_kernel_directory(void) {
    return (uint32_t)page_directory;
}

uint32_t paging_fault_address(void) {
    uint32_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

void paging_stats(uint32_t* identity_mb, uint32_t* user_pde_index) {
    if (identity_mb) {
        *identity_mb = KERNEL_IDENTITY_PDES * 4;
    }
    if (user_pde_index) {
        *user_pde_index = USER_PDE_INDEX;
    }
}
