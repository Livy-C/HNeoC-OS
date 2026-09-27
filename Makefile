# ============================================================
#  HNeoC OS Makefile
#
#  这个 Makefile 面向 Linux / WSL / MSYS2 环境，需要真正的 ELF
#  交叉工具链（i686-elf-gcc 或带 elf_i386 支持的 binutils）。
#
#  Windows + MinGW 请改用：
#      .\build.ps1 -VmName "HNeoC"
#  因为 MinGW 自带的 ld 只支持 i386pe，无法生成 ELF。
# ============================================================

AS      = nasm
CC      = i686-elf-gcc
LD      = i686-elf-ld
OBJCOPY = i686-elf-objcopy

BUILD_DIR   = build
BOOT_DIR    = boot
KERNEL_DIR  = kernel
DRIVERS_DIR = drivers
LIB_DIR     = lib
INCLUDE_DIR = include

ASFLAGS  = -f elf32
BOOTFLAGS = -f bin
CFLAGS   = -m32 -std=gnu11 -ffreestanding -fno-pic -fno-builtin \
           -fno-stack-protector -fno-asynchronous-unwind-tables \
           -nostdlib -nostdinc -Wall -Wextra -c -I$(INCLUDE_DIR)
LDFLAGS  = -m elf_i386 -T linker.ld -e kernel_entry

KERNEL_SOURCES  = $(wildcard $(KERNEL_DIR)/*.c)
DRIVER_SOURCES  = $(wildcard $(DRIVERS_DIR)/*.c)
LIB_SOURCES     = $(wildcard $(LIB_DIR)/*.c)

KERNEL_OBJECTS  = $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(KERNEL_SOURCES)))
DRIVER_OBJECTS  = $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(DRIVER_SOURCES)))
LIB_OBJECTS     = $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(LIB_SOURCES)))

ARCH_OBJECT     = $(BUILD_DIR)/arch.o
ALL_OBJECTS     = $(ARCH_OBJECT) $(KERNEL_OBJECTS) $(DRIVER_OBJECTS) $(LIB_OBJECTS)

BOOT_BIN   = $(BUILD_DIR)/boot.bin
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
OS_IMAGE   = $(BUILD_DIR)/hneoc-os.img

# 引导扇区最多加载 64KB 内核，超出就必须同时调整 boot/boot.asm
KERNEL_MAX = 65536

all: $(OS_IMAGE)

$(OS_IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	@echo "  [6/6] 生成启动镜像"
	@dd if=/dev/zero of=$(OS_IMAGE) bs=512 count=2880 status=none
	@dd if=$(BOOT_BIN) of=$(OS_IMAGE) conv=notrunc status=none
	@dd if=$(KERNEL_BIN) of=$(OS_IMAGE) bs=512 seek=1 conv=notrunc status=none
	@echo "        $(OS_IMAGE)"

$(BOOT_BIN): $(BOOT_DIR)/boot.asm
	@echo "  [1/6] 汇编引导扇区"
	@mkdir -p $(BUILD_DIR)
	$(AS) $(BOOTFLAGS) $< -o $@

$(ARCH_OBJECT): $(KERNEL_DIR)/arch.asm
	@echo "  [2/6] 汇编内核入口与中断存根"
	@mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(DRIVERS_DIR)/%.c
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(LIB_DIR)/%.c
	$(CC) $(CFLAGS) $< -o $@

$(KERNEL_ELF): $(ALL_OBJECTS)
	@echo "  [4/6] 链接内核"
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJECTS)

$(KERNEL_BIN): $(KERNEL_ELF)
	@echo "  [5/6] 提取裸二进制"
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -c%s $@); \
	 if [ $$size -gt $(KERNEL_MAX) ]; then \
	   echo "        内核 $${size} 字节，超过 $(KERNEL_MAX) 字节上限"; exit 1; \
	 fi; \
	 echo "        $(KERNEL_BIN) ($$size 字节)"

clean:
	rm -rf $(BUILD_DIR)/*

run-qemu: $(OS_IMAGE)
	qemu-system-i386 -drive format=raw,file=$(OS_IMAGE) -serial stdio

help:
	@echo "HNeoC OS 构建系统"
	@echo ""
	@echo "  make            构建启动镜像"
	@echo "  make clean      清理构建产物"
	@echo "  make run-qemu   在 QEMU 中运行（串口输出到终端）"
	@echo "  make help       显示这份帮助"
	@echo ""
	@echo "Windows 用户请改用 build.ps1"

.PHONY: all clean run-qemu help
