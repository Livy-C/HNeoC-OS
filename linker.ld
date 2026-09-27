/* ============================================================
 * HNeoC OS 链接脚本（ELF 交叉工具链版本，如 i686-elf-gcc）
 * 使用 MinGW 时请改用 linker-pe.ld
 * ============================================================ */

OUTPUT_FORMAT(elf32-i386)
ENTRY(kernel_entry)

SECTIONS
{
    /* 内核加载地址，与 boot/boot.asm 中的 KERNEL_SEG 保持一致 */
    . = 0x10000;

    .entry : {
        *(.entry)
    }

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    bss_end = .;
}
