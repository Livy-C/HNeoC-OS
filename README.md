# HNeoC OS

一个从零写起的类 Unix 小型操作系统，x86 32 位，用来学习操作系统原理。

**当前版本：0.3.0**

它已经不是一个玩具 Demo 了：有物理内存管理、分页、ATA 磁盘驱动、自己的文件
系统、ring 3 用户态、系统调用、抢占式多任务调度，以及一个任务管理器。
Shell 里的每条命令都是磁盘上一个独立的可执行文件。

```
hneoc> ls
HNeoFS directory
--------------------------------------------------
  name                 size      kind
  bg.lxe               244     B  exec
  hello.lxe            1140    B  exec
  ls.lxe               1364    B  exec
  readme.txt           707     B  data
  ver.lxe              1020    B  exec
--------------------------------------------------
  5 files

hneoc> taskmgr
Task manager
--------------------------------------------------------------------------
  pid  name             state     kind    memory    cpu   syscalls
   0  shell            running   kernel  0 B       1170  0
   3  bg               ready     user    16 KB     198   4
--------------------------------------------------------------------------
  tasks          : 2 alive, 4 created since boot
  syscalls total : 235
  free frames    : 27 MB
```

---

## 快速开始（Windows + MinGW）

```powershell
.\build.ps1 -VmName "HNeoC"   # 构建，并自动把新磁盘挂到虚拟机上
.\run.ps1                      # 构建 + 无头启动 + 打印屏幕内容
.\run.ps1 -Type "ls"           # 启动后自动敲一条命令
.\run.ps1 -Stop                # 关机
```

然后打开 VirtualBox 图形界面启动 `HNeoC` 虚拟机即可交互。

### 环境依赖

| 组件 | 位置 | 说明 |
|------|------|------|
| NASM | `.\tools\nasm.exe` | 汇编引导扇区和中断存根 |
| MinGW-w64 **i686** | `D:\mingw32\bin` | 必须是 32 位版本 |
| VirtualBox | `D:\VB` | 可选，用于生成 VDI 和自动化测试 |

> ⚠️ MinGW 必须是 **i686（32 位）**。用 `gcc -dumpmachine` 检查，
> 输出 `i686-w64-mingw32` 才对。

---

## 架构

### 启动流程

```
BIOS
 └─ 引导扇区 (boot/boot.asm, 512 字节)
     ├─ INT 15h / E820 探测物理内存，结果写到 0x5000
     ├─ INT 13h 扩展读把内核读到 0x10000（分 4 块，每块 32KB，最多 128KB）
     ├─ 建立 GDT，切到 32 位保护模式
     └─ 跳转 kernel_entry
         └─ kernel/kernel.c 的 kernel_main
```

### 内核子系统初始化顺序

| # | 子系统 | 说明 |
|---|--------|------|
| 1 | 串口 COM1 | 屏幕输出镜像一份，方便无图形界面调试 |
| 2 | VGA 文本驱动 | 80×25、16 色、硬件光标 |
| 3 | **GDT + TSS** | 内核重建，加入 ring 3 段和 TSS |
| 4 | IDT | 256 个门，异常诊断、IRQ、int 0x80 |
| 5 | 8259A PIC | 重映射到 0x20-0x2F |
| 6 | PIT 定时器 | IRQ0，100 Hz |
| 7 | PS/2 键盘 | IRQ1，环形缓冲，Shift / CapsLock，方向键 |
| 7b | PS/2 鼠标 | IRQ12，IntelliMouse 滚轮握手；滚轮用来翻看历史输出 |
| 8 | **物理内存管理器** | E820 → 4KB 页框位图 |
| 9 | **内核堆** | kmalloc / kfree，首次适配 + 相邻合并 |
| 10 | **分页** | 恒等映射 0-64MB（4MB 大页） |
| 11 | **ATA 驱动** | PIO 模式，LBA28 |
| 12 | **HNeoFS** | 自定义文件系统 |
| 13 | **进程 + 系统调用** | 进程表、int 0x80 门 |
| 14 | 开中断 | |

### 内存布局

```
0x00005000           E820 内存布局（引导扇区写入）
0x00007C00           引导扇区
0x00010000-0x0002FFFF 内核映像（128KB 装载窗口，含 BSS）
0x00090000           内核栈，向下增长
0x000B8000           VGA 文本缓冲区
0x00400000           内核堆（从页框分配器拿 4MB）
...
0x40000000           用户程序加载基址（每进程 4MB 用户区）
```

### 用户地址空间

每个进程有**自己的页目录**（内核恒等映射项从主目录复制），用户区的
页目录项指向自己的页表：

```
虚拟地址              用途             权限
0x00000000-0x03FFFFFF  内核恒等映射     ring 0
0x40000000-0x400FFFFF  用户程序映像     ring 3 可读写
0x403FF000-0x403FFFFF  用户栈（16KB）   ring 3 可读写
```

内核代码在所有地址空间里都映射在同一位置，所以切换 CR3 的过程中
内核始终可执行。用户程序碰不到 0-64MB 的内核区域（那部分页目录项
没有 USER 位）。

### 上下文切换

切换发生在中断返回路径上。`isr_handler` 返回"下一个任务的内核栈指针"，
汇编存根把 `esp` 一换，后面的 `popa` / `iret` 弹出的就是那个任务的现场：

```asm
    push esp
    call isr_handler          ; 返回下一个任务的 esp，0 表示不切换
    add esp, 4
    test eax, eax
    jz .no_switch
    mov esp, eax              ; ← 上下文切换就在这一句
.no_switch:
    pop eax
    mov ds, ax  ...
    popa
    add esp, 8
    iret
```

新任务的初始栈是手工构造的一份"假现场"，`iret` 之后 CPU 就带着
指定的 `eip / cs / eflags / esp / ss` 跳进 ring 3。

### HNeoFS 磁盘布局

```
LBA 0                   引导扇区
LBA 1-256               内核（128KB）
LBA 2048                超级块（魔数 'LVSF'）
LBA 2049-2052           文件表（4 个扇区 = 32 个 64 字节目录项）
LBA 2053-...            文件数据，按目录项顺序连续排布
```

没有 inode、没有分配位图、没有日志。文件就是"起始 LBA + 长度"，
挂载时把整张文件表读进内存。不支持删除和覆盖，改内容就重新打包镜像。

### LXE 可执行格式

40 字节头部 + 扁平代码数据，整体加载到 `0x40000000`：

```c
typedef struct {
    uint32_t magic;        /* 'LXE\0' */
    uint32_t version;
    uint32_t entry;        /* 入口偏移 */
    uint32_t size;         /* 代码 + 已初始化数据 */
    uint32_t bss_size;     /* 需要额外清零的字节数 */
    uint32_t flags;
    char     name[16];
} lxe_header_t;
```

不用 ELF 的理由：手上只有 MinGW 的 PE 目标，自己定义格式比跟 ELF 较劲省事。

---

## Shell 命令

**磁盘优先**：敲任意命令时，Shell 先找磁盘上有没有 `<命令名>.lxe`，
找到就交给 ring 3 的那个程序；找不到才回退到内核内置命令。

### 磁盘上的程序（ring 3）

| 命令 | 文件 | 说明 |
|------|------|------|
| `ls` | `ls.lxe` | 列出文件系统目录 |
| `ver` | `ver.lxe` | 版本与运行环境信息 |
| `hello` | `hello.lxe` | 最小的用户态示例，验证 bss 清零和系统调用 |
| `sysinfo` | `sysinfo.lxe` | 实时刷新运行时长，按 q 退出 |
| `bg` | `bg.lxe` | 安静的背景任务，用来观察 taskmgr |
| `ansi` | `ansi.lxe` | ANSI 转义序列演示：定位、颜色、擦除、滚动区域、备用屏幕 |
| `vi` | `vi.lxe` | 全屏模态编辑器（见下） |
| `wtest` | `wtest.lxe` | 可写文件系统自测：创建、写入、定位读、追加、删除 |
| `systest` | `systest.lxe` | 系统调用边界自测：readfile 的 max、lseek 的偏移量（故意越界） |
| `fault` | `fault.lxe` | 故意触发用户态缺页，验证内核只杀进程而不停机 |
| `spin` | `spin.lxe` | 前台死循环，用来验证 Ctrl+C 能把跑飞的程序拉回来 |
| `bigio` | `bigio.lxe` | 一次写完 2MB，验证长 I/O 期间中断没被关死、写入没被截断 |
| `note` | `note.lxe` | 往 notes.txt 追加一行，用来验证数据真的落盘 |
| `termdemo` | `termdemo.lxe` | term.h 终端 API 示例：固定状态栏 + 滚动区域 + 进度条 |

### vi：一个能用的全屏编辑器

`user/vi.c` 是从零写的 vi 克隆（约 800 行），用的是 kilo 那一系的
"行数组"结构：每行存原始内容，另存一份把 tab 展开后的版本用于显示。

```text
hneoc> vi
（欢迎屏，~ 表示空行）
hneoc> :e motd.txt
（文件内容 + 状态栏 -- NORMAL --）
        motd.txt  9/9                                  -- NORMAL --
```

| 类别 | 按键 |
|------|------|
| 移动 | `h` `j` `k` `l`、方向键、`0` `$`、`w` `b`、`gg` `G`、PageUp / PageDown |
| 进入插入 | `i` `a` `A` `I` `o` `O` |
| 删除 | `x` 删字符、`d` 删整行 |
| 撤销 | `u`（单级） |
| 命令 | `:w` `:q` `:q!` `:x` `:e <文件>` `:w <文件>` `:<行号>` |

它是怎么跑起来的：切到备用屏幕（`ESC[?1049h`）拿到整块屏幕，
用 ANSI 转义序列定位和擦除、反显画状态栏，按键走 `getkey()`，
文件读写走 `open/read/write/lseek/fsync`，动态内存走 `sbrk`。
退出时切回普通屏幕，Shell 的画面原样恢复。

实测（打开 → 跳末尾 → 开新行 → 输入 → 保存 → 退出 → 用 cat 核对）：

```text
hneoc> cat edited.txt
Welcome to HNeoC OS.
...
Type 'help' for the list of commands.
edited by vi in ring 3        ← 编辑器里新加的那行
```

### 用户态 C API

`user/lib/` 下是一套标准命名、分层清楚的用户态 C 库。写程序时优先用它，
而不是直接拼系统调用。

```
user/lib/include/          用户程序包含这个目录
  stdint.h stddef.h stdbool.h      基本类型
  string.h  ctype.h                字符串 / 字符分类
  stdlib.h                         malloc / realloc / free / atoi / exit
  stdio.h                          printf / snprintf / puts / putchar / getchar
  unistd.h                         open read write close lseek unlink
                                   sbrk getpid uptime sleep_ms getkey
  fcntl.h   打开标志
  errno.h   错误码常量
  term.h    终端 / 全屏应用 API
  hneoc.h  方便头：一次全拉进来，附带系统调用号 + __syscallN 原语
user/lib/src/              实现，自动被链进每个用户程序
```

三层结构：

1. **最底层** `hneoc.h` 里的 `SYS_*` 和 `__syscall0/1/2/3` —— 只有标准接口
   没覆盖到的能力才直接用
2. **中间层** 标准命名的库，写起来和普通 C 一样
3. **最上面** `term.h` —— 专门为全屏程序做的终端层

`term.h` 是这套 API 里最有价值的一块，它把 ANSI 转义序列整个包住：

```c
#include <hneoc.h>

int main(void) {
    int rows, cols;

    term_init_fullscreen(&rows, &cols);   /* 进备用屏幕，退出时画面自动恢复 */
    term_set_scroll_region(0, rows - 2);  /* 状态栏那一行不参与滚动 */

    term_set_bold(1);
    term_set_fg(TERM_LIGHT_CYAN);
    term_gotoxy(0, 2);
    term_printf("screen is %dx%d", cols, rows);

    term_begin_frame();                   /* 藏光标，减少闪烁 */
    term_set_reverse(1);
    term_gotoxy(rows - 1, 0);
    term_printf(" %-40s ", "status");
    term_end_frame();                     /* flush + 显示光标 */

    getkey();
    term_leave_fullscreen();
    return 0;
}
```

全程不出现一个 `"\x1b["`。`user/termdemo.c` 是完整示例：
一个带固定状态栏和滚动区域的进度条。

构建脚本会自动把 `user/lib/src/*.c` 编译成目标文件链进每个程序
（顶层 `user/*.c` 才是可执行程序），配合 `-ffunction-sections
--gc-sections` 丢掉没用到的函数——`bg.lxe` 只有 168 字节。

> ⚠️ 两个格式化函数的坑，都真实踩过：
> 不支持 `%.Ns` 精度会让**参数整体错位**，打出来是一堆垃圾指针值；
> 不支持 `%s` 的宽度则会让 `%-40s` 里的填充空格**整段消失**，
> 靠长度对齐的界面全歪。格式化函数的解析器漏掉一种写法，
> 后果从来不是"那一处显示不对"。



### 内核内置命令（回退）

| 命令 | 说明 |
|------|------|
| `help` | 命令列表 |
| `mem` | E820 内存表、页框分配器、内核堆、分页状态 |
| `heap` | 内存分配器自检 |
| `diskinfo` | ATA 设备信息，并实测一次 PIO 读 |
| `mouse` | PS/2 鼠标状态和回滚缓冲统计 |
| `screendump [colors]` | 把屏幕内容打出来（含逐格配色），调试验证用 |
| `fsstat` | 文件系统超级块与空间占用 |
| `ls` / `cat <f>` | 目录与文件内容 |
| `exec <p>` | 前台运行程序，阻塞到退出 |
| `spawn <p>` | 后台启动程序 |
| `taskmgr` | 任务列表、CPU、内存、系统调用统计 |
| `kill <pid>` | 结束进程 |
| `uptime` / `ticks` | 开机时长 / 定时器滴答 |
| `color` / `colors` / `clear` | 配色 |
| `history` / `banner` | 历史命令 / 重印横幅 |
| `panic <m>` / `reboot` / `halt` | 崩溃测试 / 重启 / 停机 |

行编辑支持退格和 **↑/↓ 翻历史命令**。

### 往回翻看滚过去的输出

屏幕上沿滚出去的内容会存进一个 300 行的历史缓冲，随时可以翻回去看：

| 操作 | 效果 |
|------|------|
| 鼠标滚轮上/下 | 一次翻 3 行 |
| `PageUp` / `PageDown` | 一次翻一屏（25 行） |
| `Home` | 跳到缓冲里最旧的一行 |
| `End` | 回到实时画面 |
| 其他任意键 | 回到实时画面（这个键会被吃掉，避免误输入命令） |

翻历史的时候光标会移开、右上角显示 `-- n --` 表示往回翻了多少行，
一眼就能看出当前不在实时画面上。

实现要点：实时画面始终维护在内存里的 `screen[25][80]` 副本中，
VGA 显存只是它的一个"投影"。翻页时整屏重绘成历史内容，
所以翻页不会破坏实时画面。新输出到来时视角会跟着一起前移，
眼睛看到的那几行不会跳。

> 翻页只改显存、不经过 `vga_putchar`，所以串口镜像里看不到翻页结果。
> 为了能自动化验证，每次视角变化会往串口写一行
> `[scroll] offset=25/122  top: ...`，带上显示区第一行的内容。

---

## 系统调用

用户程序用 `int 0x80` 陷入内核，调用号在 EAX，参数在 EBX/ECX/EDX，
返回值通过 EAX 带回。门类型是 **interrupt gate + DPL=3**：
DPL=3 让 ring 3 能调用；interrupt gate 会清 IF，避免系统调用返回路径上的
`sched_tick` 被时钟中断嵌套重入。需要阻塞等待的调用自己临时开中断。

| 号 | 名称 | 说明 |
|----|------|------|
| 0 | `exit(code)` | 结束进程 |
| 1 | `write(fd, buf, len)` | 写控制台或文件 |
| 2 | `getchar()` | 阻塞读一个按键 |
| 3 | `getpid()` | 当前 pid |
| 4 | `yield()` | 让出 CPU |
| 5 | `sleep(ms)` | 睡眠 |
| 6 / 7 | `uptime()` / `ticks()` | 时间 |
| 8 / 9 | `set_color` / `clear_screen` | 控制台 |
| 10 | `readfile(name, buf, max)` | 一次性读整个文件 |
| 11 | `listfiles(idx, buf, max)` | 遍历目录 |
| 12 | `open(name, flags)` | 打开 / 创建文件，返回 fd |
| 13 | `close(fd)` | 关闭 |
| 14 | `read(fd, buf, len)` | 从文件或控制台读 |
| 15 | `lseek(fd, off, whence)` | 移动读写位置 |
| 16 | `unlink(name)` | 删除文件 |
| 17 | `fstat(fd)` | 取文件大小 |
| 18 | `fsync(fd)` | 把文件表刷回磁盘 |
| 19 | `getkey()` | 原始按键码，含方向键等 0x101+ |
| 20 | `winsize()` | 屏幕尺寸打包成 `(行 << 8) \| 列` |
| 21 | `sbrk(n)` | 把用户堆顶往上推，返回原来的堆顶 |

fd 0/1/2 固定映射到控制台，其余从 3 开始分配，每个进程一张表（16 个）。
进程退出时这张表自动作废。

用户态运行时在 `user/hneoc.h`，是一个只有头文件的极简库（没有 libc）。

### 控制台支持 ANSI 转义序列

`vga_putchar` / `vga_write` 会解释 VT100 风格的转义序列，
所以全屏程序不需要任何"画界面"的系统调用，写字节流就行：

| 序列 | 作用 |
|------|------|
| `ESC[y;xH` | 光标绝对定位（行列从 1 开始） |
| `ESC[nA/B/C/D` | 光标上下左右移动 |
| `ESC[nJ` / `ESC[nK` | 擦除屏幕 / 擦除行（0=到末尾 1=到开头 2=全部） |
| `ESC[nm` | SGR：颜色、粗体、反显（30-37 / 90-97 / 40-47 / 100-107） |
| `ESC[t;br` | 设置滚动区域，编辑器靠它固定状态行 |
| `ESC[nL` / `ESC[nM` | 插入 / 删除行 |
| `ESC[n@` / `ESC[nP` / `ESC[nX` | 插入 / 删除 / 擦除字符 |
| `ESC[?25h/l` | 显示 / 隐藏光标 |
| `ESC[?1049h/l` | 切换备用屏幕缓冲，退出时原画面自动恢复 |

`user/ansi.c` 是完整的演示程序。验证渲染结果用 `screendump`：

```text
hneoc> screendump colors
   0         1         2         3         4         5         6         7
   0123456789012345678901234567890123456789012345678901234567890123456789012 345
 0|  ANSI escape sequences work                                                |
 1|  ------------------------------------------------------------              |
 3|  normal : red green yellow blue magenta cyan                               |
...
attribute runs (row: cols fg/bg), only for non-default cells:
  row  3: 0-1 15/0   11-14 4/0   15-20 2/0   21-27 6/0   28-32 1/0   33-40 5/0   41-44 3/0
  row  4: 0-1 15/0   11-14 12/0  15-20 10/0  21-27 14/0  28-32 9/0   33-40 13/0  41-44 11/0
  row  5: 11-20 0/7   21-29 15/1
```

### 可写的 HNeoFS

文件在磁盘上仍然是**连续存放**的，所以"分配空间"就是找一段够长的
连续空闲扇区。空闲区不单独维护位图，而是从现有文件的范围推出来：
把所有文件区间按起点排序，扫一遍找第一个放得下的空隙。目录项上限
只有 32，这个代价可以忽略，好处是删文件不用显式回收空间。

写入时如果需要更大的空间，就把内容整体搬到新位置、旧区间自动变成空闲。

```text
hneoc> wtest
[1] create + write
  open = 3
  write #1 = 39
  write #2 = 33
  fstat = 72
[2] read it back
  read = 72
first line written by a ring 3 program
second line, also from user mode
[3] lseek + partial read
  lseek(7) = 7
  read 10 bytes = 10
  got: "ine writte"
[4] append
  lseek pos = 72
  write = 32
  fstat = 104
[5] unlink
  unlink = 0
  open again (should fail) = -1
```

数据是真的落盘的：`note` 每次运行往 `notes.txt` 追加一行，
重启（不重新打包镜像）之后 `cat notes.txt` 能看到之前写的所有行。

> 注意：`build.ps1` 每次都会重新生成 VDI，会清掉磁盘上的改动。
> 只想重启看持久化效果就用 `run.ps1 -NoBuild`。


---

## 写一个用户程序

```c
// user/hello.c
#include "hneoc.h"

static int counter[8];      // 放在 .bss，加载器会清零

int main(void) {
    puts("hello from ring 3, pid ");
    put_int(getpid());
    putchar('\n');

    for (int i = 0; i < 5; i++) {
        sleep_ms(200);
    }
    return 0;
}
```

放进 `user/` 目录，下次 `.\build.ps1` 就会自动编译成 `fsroot/hello.lxe`
并打包进磁盘镜像。敲 `hello` 就能跑，不需要注册任何命令表。

> ⚠️ 入口函数必须叫 `main`，且用户程序里**不要**出现排在代码前面的
> 静态初始化逻辑 —— LXE 的入口偏移固定为 0，链接脚本把 `.entry` 放在
> 映像最前面。

---

## 项目结构

```
hneoc-os/
├── boot/boot.asm          引导扇区：E820 探测、加载内核、进保护模式
├── kernel/
│   ├── arch.asm           内核入口、48 个中断存根、GDT/TSS 刷新、上下文切换点
│   ├── kernel.c           初始化流程、panic
│   ├── gdt.c              GDT + TSS（ring 3 段）
│   ├── idt.c              中断分发，异常诊断
│   ├── e820.c             内存布局解析
│   ├── pmm.c              物理页框分配器（位图）
│   ├── heap.c             内核堆 kmalloc/kfree
│   ├── paging.c           页目录、用户页表
│   ├── hneofs.c         文件系统
│   ├── process.c          进程与抢占式调度器
│   ├── syscall.c          int 0x80 处理
│   └── shell.c            命令行
├── drivers/
│   ├── vga.c  keyboard.c  pic.c  timer.c  serial.c  ata.c
├── lib/string.c
├── include/               头文件
├── user/
│   ├── hneoc.h           用户态运行时（系统调用包装）
│   ├── crt0.asm           入口
│   ├── user.ld            用户程序链接脚本（0x40000000）
│   └── *.c                用户程序
├── fsroot/                被打包进镜像的文件（自动生成 .lxe）
├── tools/
│   ├── nasm.exe  mkfs.ps1  mklxe.ps1  vm-type.ps1  vga-dump.ps1
├── build.ps1  run.ps1
├── linker-pe.ld  linker.ld  Makefile
└── README.md
```

---

## 调试技巧

### 串口日志（强烈推荐）

内核把屏幕上的字符同时写到 COM1。把虚拟机串口重定向到文件，
宿主机就能读到和屏幕完全一致的文本，不用截图：

```powershell
& "D:\VB\VBoxManage.exe" modifyvm "HNeoC" `
    --uart1 0x3F8 4 --uartmode1 file "D:\Projects\hneoc-os\build\serial.log"
```

`run.ps1` 已经自动做了这件事。

### 自动化输入

```powershell
.\tools\vm-type.ps1 -VmName "HNeoC" -Text "taskmgr" -Enter
.\tools\vm-type.ps1 -VmName "HNeoC" -Key Up
```

### 内存转储

```powershell
& "D:\VB\VBoxManage.exe" debugvm "HNeoC" dumpvmcore --filename build\vmcore.elf
```

---

## 踩过的坑

按踩坑顺序排列，每一条都真实卡过一次。

**工具链**

1. **MinGW 的 ld 只有 `i386pe`**，不支持 `elf_i386`，也不能 `--oformat binary`。
   解决：用 PE 格式链接，再 `objcopy -O binary` 提取裸二进制。

2. **PE 链接脚本里不能同时用 `--image-base`**。链接器会在基址之上再叠加
   脚本里的 `. = 0x10000`，节之间出现 64KB 空洞（内核 78.7KB → 15.2KB）。

3. **GCC 16 默认按 C23 编译**，`bool` 成了关键字。加 `-std=gnu11`。

4. **`-nostdlib` 下不能出现 64 位除法**，会生成 `__udivdi3` 调用直接链接失败。
   只用 2 的幂次方除法（编译器优化成移位），否则改回 32 位运算。

5. **MinGW 会在 `main` 里插入对 `__main` 的调用**。注意目标文件里实际是
   `___main`（GCC 内部名 `__main` + PE 的前导下划线），给个空实现即可。

**引导与内核**

6. **引导扇区只有 512 字节**。加 E820 之后放不下 CHS 回退路径，只能砍掉，
   改为只支持 INT 13h 扩展读（VirtualBox 的硬盘必然支持）。

7. **`_start` 必须是映像的第一条指令**。曾经把 `__main` 存根排在 `_start`
   前面，CPU 一上来执行的是 `ret`，去读还没映射的栈顶直接缺页。

**内存与分页**

8. **REP STOS 的段边界**：加载内核时分块，每块落在独立的 64KB 段里，
   避免 ES:offset 跨段。最终用 4 块 × 32KB。

9. **PE 链接出来的 `.bss` 不在二进制里**，必须由入口代码清零。
   链接器脚本导出 `_bss_start` / `_bss_end`，`kernel_entry` 负责清零。

**并发与调度**

10. **任务在构造完之前不能标成 READY**。曾经 `create_from_image` 提前置位，
    时钟中断正好切进一个 `esp = 0` 的半成品任务直接崩掉。
    解决：构造期间关中断（`irq_save` / `irq_restore`）。

11. **堆分配必须关中断**。调度器回收僵尸进程时会 `kfree`，
    如果插在 kmalloc 更新链表指针的中间，堆就坏了。

12. **系统调用门要用 interrupt gate 而不是 trap gate**。
    trap gate 不清 IF，时钟中断会嵌套进系统调用返回路径上的 `sched_tick`，
    两个 `sched_tick` 同时操作 `current`，出现间歇性 iret 崩溃。
    需要阻塞的系统调用自己临时开中断。

**显示与脚本**

13. **VGA 文本模式是 CP437，没有汉字字形**。界面文字必须用 ASCII，
    写中文只会显示成乱码（注释可以随意用中文）。

14. **Windows PowerShell 5.1 把无 BOM 的 `.ps1` 当 GBK 读**，
    UTF-8 中文注释会被解码错乱甚至吞掉换行，直接破坏语法。
    带中文的脚本要么存成 UTF-8 with BOM，要么只写 ASCII。
    `run.ps1` 里加了自动补 BOM 的逻辑。

15. **`VBoxManage keyboardputscancode` 的扫描码要作为独立参数传**，
    拼成一个带空格的字符串会报 `is not a hex byte!`。

16. **每次 `convertfromraw` 生成的 VDI 都是新磁盘，UUID 会变**，
    而 VirtualBox 介质注册表里还记着旧 UUID。`build.ps1 -VmName` 会自动
    卸旧盘、注销旧登记、挂新盘。

17. **虚拟机运行时会锁住 VDI 和串口日志文件**，删除前必须先关机。

18. **特殊键码 0x101+ 不能塞进 `char`**。`char` 只有 8 位，
    `KEY_PAGEUP`(0x105) 会被截断成 0x05，和 `switch` 里的常量永远匹配不上；
    更糟的是 `KEY_END`(0x108) 截断成 0x08，和退格 `'\b'` 撞在一起。
    键盘缓冲区改成 `uint16_t`、`keyboard_getchar()` 返回 `int` 才对。

19. **键盘和鼠标共用 0x60 端口**，处理中断时必须先看状态寄存器 bit5
    （数据来自辅助端口吗），读错了就会把对方的数据吞掉。IRQ1 和 IRQ12
    两个处理器都要判断。另外 IRQ12 在从片上，主片的 IRQ2 级联也必须放开。

20. **滚轮要先做 IntelliMouse 握手**：采样率依次设成 200 / 100 / 80，
    再读设备 ID，返回 3 才说明是带滚轮的型号 —— 之后数据包才是 4 字节。
    不做这一步的话第 4 个字节根本不会来，滚轮完全没反应。

21. **调试屏幕类功能时，串口日志是不够的**。串口镜像只是线性的字符流，
    光标定位、擦除、插入删除的结果在日志里完全看不出来（`ansi` 的输出
    在日志里就是一堆首尾相连的文字）。必须直接读屏幕内存副本，
    这就是 `screendump` 存在的理由。

22. **`screendump` 自己也会破坏要读的东西**：它的输出有 27 行，
    打印过程本身就在滚屏，边打边读的话后面的行读到的已经是滚动后的内容，
    位置全错。必须先整屏快照再打印。另外每行必须正好 80 个字符，
    超一个字符就会折行把输出搞乱。

23. **删除目录项会让所有下标失效**。`hneofs_unlink` 把后面的目录项
    整体前移，所以写入相关的接口一律用下标而不是指针，
    而且调用方在下标可能变化时必须重新查找。

---

## 下一步可以做什么

- [ ] 阻塞式 `wait()`，让父进程真正睡眠而不是忙等
- [ ] 每个进程独立的用户区大小（现在固定 4MB）
- [ ] 文件写入与删除（现在文件系统只读）
- [ ] 管道与重定向
- [ ] 信号机制
- [ ] 更完整的 libc（现在是只有头文件的极简运行时）
- [ ] 用 QEMU + GDB 做源码级调试

---

## 许可证

MIT
