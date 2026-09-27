/* ============================================================
 * HNeoC OS PE 链接脚本（供 MinGW/Win32 工具链使用）
 *
 * 说明：MinGW 自带的 ld 只支持 i386pe 目标，无法生成 ELF。
 *       因此这里用 PE 格式链接，再用 objcopy -O binary 提取裸二进制。
 *       链接时需带 --image-base 0x10000 以避免 "section below image base"。
 * ============================================================ */

OUTPUT_FORMAT(pei-i386)
ENTRY(_kernel_entry)

SECTIONS
{
    /* 内核加载地址，与 boot/boot.asm 中的 KERNEL_SEG 保持一致 */
    . = 0x10000;

    /* 入口代码必须排在最前面，引导程序会直接跳到 0x10000 */
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

    /* BSS 不占文件空间，由 kernel/arch.asm 的 kernel_entry 负责清零 */
    _bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    _bss_end = .;

    /* 这些节对裸机内核没有意义，直接丢弃 */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame*)
        *(.reloc)
        *(.idata)
        *(.debug*)
        *(.comment)
    }
}
