; ============================================================
; HNeoC OS 引导扇区（必须正好 512 字节）
;   BIOS 把本扇区加载到 0x7C00，我们负责：
;     1. 用 INT 15h/E820 探测物理内存布局，结果放在 0x5000
;     2. 用 INT 13h 扩展读把内核读到物理地址 0x10000（最多 128KB）
;     3. 建立 GDT，切到 32 位保护模式
;     4. 跳转到内核入口 kernel_entry
;
;   只支持 INT 13h 扩展读（LBA），不再提供 CHS 回退路径：
;   512 字节实在放不下，而且 VirtualBox/现代 BIOS 都支持扩展读。
;   如果以后要支持软盘镜像启动，得把加载器改成两阶段引导。
; ============================================================

[BITS 16]
[ORG 0x7C00]

E820_ADDR      equ 0x5000      ; 内存布局缓冲区：前 4 字节是条目数，之后是条目
E820_MAX       equ 32          ; 最多记录 32 条

KERNEL_SEG     equ 0x1000      ; 内核物理地址 = 0x1000 << 4 = 0x10000
CHUNK_SECTORS  equ 64          ; 每块 64 扇区 = 32KB
KERNEL_CHUNKS  equ 4           ; 4 块 = 128KB 上限

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00             ; 栈放在引导扇区下方，向下增长
    sti

    mov [boot_drive], dl       ; BIOS 在 DL 里传入了启动设备号

    mov si, msg_boot
    call print

    call detect_memory
    call load_kernel
    call switch_to_pm

    jmp $                      ; 正常情况下到不了这里

; ------------------------------------------------------------
; 打印以 0 结尾的字符串（DS:SI）
; ------------------------------------------------------------
print:
    pusha
    mov ah, 0x0E
.next:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .next
.done:
    popa
    ret

; ------------------------------------------------------------
; INT 15h / EAX=E820：探测物理内存布局
;   结果写到 E820_ADDR：dword 条目数，之后每条 24 字节
;   { u64 base, u64 length, u32 type, u32 acpi }
; ------------------------------------------------------------
detect_memory:
    pusha
    xor ax, ax
    mov es, ax                 ; ES:DI 指向缓冲区
    mov dword [E820_ADDR], 0   ; 先清零条目数，探测失败时内核会退回默认值
    mov di, E820_ADDR + 4
    xor ebx, ebx               ; 续读标识，第一次必须为 0
    mov word [e820_count], 0
    mov word [e820_left], E820_MAX
    ; 注意：不能用 CX/loop 当计数器，因为每次 INT 15h 前都要把
    ; ECX 设成 24，会覆盖 CL
.next:
    mov eax, 0xE820
    mov edx, 0x534D4150        ; 'SMAP'
    mov ecx, 24
    int 0x15
    jc .done                   ; CF=1 表示已经读完了
    cmp eax, 0x534D4150        ; 部分 BIOS 不支持，返回的 EAX 不是 'SMAP'
    jne .done
    inc word [e820_count]
    add di, 24
    dec word [e820_left]
    jz .done
    test ebx, ebx              ; EBX=0 表示没有更多条目
    jnz .next
.done:
    movzx eax, word [e820_count]
    mov [E820_ADDR], eax
    popa
    ret

; ------------------------------------------------------------
; 把内核读入内存
;   分成 KERNEL_CHUNKS 块，每块 32KB 放在独立的 64KB 段里，
;   这样目标偏移从 0 开始、永远不会跨越段边界
; ------------------------------------------------------------
load_kernel:
    mov si, msg_loading
    call print

    mov ah, 0x41               ; 检查是否支持扩展读
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error

.chunk_loop:
    call set_chunk_segment     ; AX = 本块的段地址
    mov [dap_segment], ax

    mov ax, [chunk_index]      ; dap_lba = 1 + chunk_index * 64
    mov cl, 6
    shl ax, cl
    inc ax
    movzx eax, ax
    mov [dap_lba], eax
    mov dword [dap_lba + 4], 0

    call lba_read

    inc word [chunk_index]
    mov ax, [chunk_index]
    cmp ax, KERNEL_CHUNKS
    jb .chunk_loop
    ret

; ------------------------------------------------------------
; 当前块的段地址 = KERNEL_SEG + chunk_index * 0x800
; （0x800 段 = 32KB，正好是一块的大小）
; ------------------------------------------------------------
set_chunk_segment:
    mov ax, [chunk_index]
    mov cl, 11                 ; * 2048
    shl ax, cl
    add ax, KERNEL_SEG
    ret

; ------------------------------------------------------------
; LBA 读：按 DAP 描述读一块，失败自动重试
; ------------------------------------------------------------
lba_read:
    mov byte [retries], 3
.retry:
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    jnc .done
    xor ah, ah                 ; 出错先复位磁盘控制器
    mov dl, [boot_drive]
    int 0x13
    dec byte [retries]
    jnz .retry
    jmp disk_error
.done:
    ret

; ------------------------------------------------------------
; 失败处理
; ------------------------------------------------------------
disk_error:
    mov si, msg_error
    call print
    cli
    hlt
    jmp $

; ------------------------------------------------------------
; 切换到 32 位保护模式
; ------------------------------------------------------------
switch_to_pm:
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0               ; 打开 CR0 的 PE 位
    or eax, 0x1
    mov cr0, eax

    jmp CODE_SEG:init_pm       ; 远跳转以刷新 CS

; ------------------------------------------------------------
; 全局描述符表：一个空描述符 + 平坦的代码段和数据段
; ------------------------------------------------------------
gdt_start:
    dd 0x0
    dd 0x0

gdt_code:                      ; ring0 代码段，基址 0，界限 4GB
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

gdt_data:                      ; ring0 数据段，基址 0，界限 4GB
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start   ; = 0x08
DATA_SEG equ gdt_data - gdt_start   ; = 0x10

; ------------------------------------------------------------
; 32 位保护模式入口
; ------------------------------------------------------------
[BITS 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, 0x90000           ; 临时栈，内核入口会重新设置
    mov esp, ebp

    jmp KERNEL_SEG * 0x10      ; 跳转到内核入口 kernel_entry

.hang:
    cli
    hlt
    jmp .hang

; ------------------------------------------------------------
; 数据
; ------------------------------------------------------------
dap:
    db 0x10                    ; 结构大小
    db 0                      ; 保留
dap_count:
    dw CHUNK_SECTORS           ; 要读的扇区数
dap_offset:
    dw 0x0000                  ; 目标偏移
dap_segment:
    dw KERNEL_SEG              ; 目标段
dap_lba:
    dq 1                       ; 起始 LBA

boot_drive:   db 0
retries:      db 0
chunk_index:  dw 0
e820_count:   dw 0
e820_left:    dw 0

msg_boot:     db 'HNeoC OS', 13, 10, 0
msg_loading:  db 'Loading', 13, 10, 0
msg_error:    db 'Disk error', 13, 10, 0

times 510-($-$$) db 0
dw 0xAA55                      ; 引导扇区魔数
