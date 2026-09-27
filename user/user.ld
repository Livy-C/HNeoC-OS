/* ============================================================
 * 用户程序的链接脚本
 *
 * 所有用户程序都加载到同一个虚拟地址 USER_BASE（0x40000000）。
 * 每个进程有自己的一张页表，所以同一个虚拟地址在不同进程里指向
 * 不同的物理页框，互不干扰。
 *
 * _ubss_start / _ubss_end 由打包脚本读取，用来算出 LXE 头部里的
 * bss_size —— objcopy 生成的裸二进制不含 .bss，加载器需要知道
 * 还要额外清零多少字节。
 * ============================================================ */

OUTPUT_FORMAT(pei-i386)
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    /* 入口必须排在最前面 */
    .entry : {
        *(.entry)
    }

    .text : {
        *(.text)
        *(.text.*)
    }

    .rdata : {
        *(.rdata)
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    _ubss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    _ubss_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame*)
        *(.reloc)
        *(.idata)
        *(.debug*)
        *(.comment)
    }
}
