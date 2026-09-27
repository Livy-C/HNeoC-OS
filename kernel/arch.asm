; ============================================================
; HNeoC OS 底层汇编代码
;   - kernel_entry : 内核真正的入口点（清 BSS -> 调用 kernel_main）
;   - isr0..isr47  : 中断服务例程存根（异常 + IRQ）
; 汇编命令：nasm -f win32 kernel/arch.asm -o build/arch.o
; ============================================================

[BITS 32]

; MinGW/PE 目标会在 C 符号前加下划线，这里用宏统一控制
%ifdef NO_UNDERSCORE
%define SYM(x) x
%else
%define SYM(x) _ %+ x
%endif

; ------------------------------------------------------------
; 内核入口（必须位于镜像最前面，地址 0x10000）
; ------------------------------------------------------------
section .entry code align=16

global SYM(kernel_entry)
extern SYM(kernel_main)
extern SYM(bss_start)
extern SYM(bss_end)

SYM(kernel_entry):
    cli                         ; 先关中断，等 IDT 装好再开
    mov esp, 0x90000            ; 设置内核栈（向下增长，空间充足）
    mov ebp, esp

    ; 清空 BSS 段（链接器脚本提供起止符号）
    mov edi, SYM(bss_start)
    mov ecx, SYM(bss_end)
    sub ecx, edi                ; 字节数
    xor eax, eax
    cld
    rep stosb

    call SYM(kernel_main)

.hang:                          ; kernel_main 若返回则永久停机
    cli
    hlt
    jmp .hang

; ------------------------------------------------------------
; 中断存根
; ------------------------------------------------------------
section .text

extern SYM(isr_handler)

; 没有错误码的异常
%macro ISR_NOERRCODE 1
global SYM(isr%1)
SYM(isr%1):
    push dword 0                ; 占位错误码，保持栈布局统一
    push dword %1               ; 中断号
    jmp isr_common_stub
%endmacro

; CPU 会自动压入错误码的异常
%macro ISR_ERRCODE 1
global SYM(isr%1)
SYM(isr%1):
    push dword %1               ; 只压中断号（错误码已在栈上）
    jmp isr_common_stub
%endmacro

; 硬件 IRQ（不需要错误码）
%macro IRQ 2
global SYM(irq%1)
SYM(irq%1):
    push dword 0
    push dword %2
    jmp isr_common_stub
%endmacro

; --- CPU 异常 0-31 ---
ISR_NOERRCODE 0     ; 除零
ISR_NOERRCODE 1     ; 调试
ISR_NOERRCODE 2     ; 不可屏蔽中断
ISR_NOERRCODE 3     ; 断点
ISR_NOERRCODE 4     ; 溢出
ISR_NOERRCODE 5     ; 越界
ISR_NOERRCODE 6     ; 非法操作码
ISR_NOERRCODE 7     ; 设备不可用
ISR_ERRCODE   8     ; 双重错误
ISR_NOERRCODE 9
ISR_ERRCODE   10    ; 无效 TSS
ISR_ERRCODE   11    ; 段不存在
ISR_ERRCODE   12    ; 栈段错误
ISR_ERRCODE   13    ; 一般保护错误
ISR_ERRCODE   14    ; 页错误
ISR_NOERRCODE 15
ISR_NOERRCODE 16    ; x87 浮点错误
ISR_ERRCODE   17    ; 对齐检查
ISR_NOERRCODE 18    ; 机器检查
ISR_NOERRCODE 19    ; SIMD 浮点错误
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30    ; 安全异常
ISR_NOERRCODE 31

; --- 硬件中断 IRQ 0-15（重映射后向量号为 32-47）---
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; --- 系统调用入口：int 0x80 ---
; 用户程序通过它陷入内核。IDT 里这一项的门类型是 trap gate 且 DPL=3，
; 所以 ring 3 可以直接调用，而且不会清掉 IF。
global SYM(isr128)
SYM(isr128):
    push dword 0
    push dword 128
    jmp isr_common_stub

; --- 未预期中断的兜底处理 ---
global SYM(isr_default)
SYM(isr_default):
    push dword 0
    push dword 0xFF
    jmp isr_common_stub

; --- 公共存根：保存现场 -> 调用 C 处理函数 -> 恢复现场 ---
isr_common_stub:
    pusha                       ; 压入 edi,esi,ebp,esp,ebx,edx,ecx,eax

    mov ax, ds                  ; 保存当前数据段
    push eax

    mov ax, 0x10                ; 切到内核数据段（GDT 第 3 项）
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                    ; 传给 C 的参数：registers_t*
    call SYM(isr_handler)
    add esp, 4                  ; 丢弃参数

    ; isr_handler 返回"下一个要运行的任务的内核栈指针"，
    ; 返回 0 表示继续跑当前任务。
    ;
    ; 这里就是上下文切换发生的地方：把 esp 换成另一个任务的栈之后，
    ; 下面的 popa / iret 弹出的就是那个任务的寄存器现场。
    ; 对它来说，就好像自己刚从这次中断里返回一样。
    test eax, eax
    jz .no_switch
    mov esp, eax
.no_switch:

    pop eax                     ; 恢复原数据段
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8                  ; 丢弃中断号和错误码
    iret

; ------------------------------------------------------------
; GDT / TSS
; ------------------------------------------------------------

; void gdt_flush(uint32_t gdt_ptr_addr)
global SYM(gdt_flush)
SYM(gdt_flush):
    mov eax, [esp + 4]
    lgdt [eax]

    mov ax, 0x10                ; ring0 数据段
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.reload_cs         ; 远跳转刷新 CS
.reload_cs:
    ret

; void tss_flush(void)
global SYM(tss_flush)
SYM(tss_flush):
    mov ax, 0x28                ; TSS 选择子
    ltr ax
    ret

; ------------------------------------------------------------
; 说明：这里曾经有 exec_save_context / exec_restore_context /
; user_enter 三个辅助函数，用来从用户程序的 exit 跳回 Shell。
; 改成抢占式多任务之后就不再需要了：
;   - 进入用户态靠调度器构造的初始栈帧 + isr_common_stub 里的 iret
;   - 退出靠 SYS_EXIT 把任务标成僵尸，再由上面的 esp 切换换走
; ------------------------------------------------------------

; ------------------------------------------------------------
; 存根地址表，供 C 代码注册 IDT 时使用
; 索引 0..31 = CPU 异常，32..47 = IRQ，48 = 兜底
; ------------------------------------------------------------
section .data

global SYM(interrupt_stub_table)

SYM(interrupt_stub_table):
%assign i 0
%rep 32
    dd SYM(isr%+i)
%assign i i+1
%endrep

%assign i 0
%rep 16
    dd SYM(irq%+i)
%assign i i+1
%endrep

    dd SYM(isr_default)

global SYM(interrupt_stub_count)
SYM(interrupt_stub_count):
    dd 49
