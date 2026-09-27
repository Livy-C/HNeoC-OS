# ============================================================
#  HNeoC OS 构建脚本（Windows / MinGW i686 工具链）
#
#  依赖：
#    - NASM            : .\tools\nasm.exe
#    - MinGW-w64 i686  : D:\mingw32\bin （必须是 i686，不是 x86_64）
#    - VirtualBox      : 可选，用于把镜像转成 VDI
#
#  用法：  .\build.ps1
#          .\build.ps1 -NoVdi              # 跳过 VDI 转换
#          .\build.ps1 -Clean              # 先清理
#          .\build.ps1 -VmName "HNeoC"    # 生成 VDI 后自动挂到该虚拟机
#          .\build.ps1 -NoAttach           # 不自动挂载
# ============================================================

param(
    [switch]$NoVdi,
    [switch]$Clean,
    [string]$VmName = "",
    [switch]$NoAttach
)

$ErrorActionPreference = "Stop"

$root     = $PSScriptRoot
$buildDir = Join-Path $root "build"
$imgFile  = Join-Path $buildDir "hneoc-os.img"
$vdiFile  = Join-Path $buildDir "hneoc-os.vdi"

function Write-Step($text) {
    Write-Host "  $text" -ForegroundColor Cyan
}
function Write-Ok($text) {
    Write-Host "    OK  $text" -ForegroundColor Green
}
function Fail($text) {
    Write-Host ""
    Write-Host "  [错误] $text" -ForegroundColor Red
    Write-Host ""
    exit 1
}

# 运行一个原生命令并返回退出码。
#
# 为什么要单独包一层：$ErrorActionPreference = "Stop" 会把原生命令写到
# stderr 的**任何**内容当成终止错误，而 gcc 的编译告警、链接器的
# "section below image base"、VBoxManage 的进度信息全都走 stderr。
# 这里临时放开策略、把输出按行回调，再原样恢复。
function Invoke-Tool {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [scriptblock]$OnLine
    )

    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $collected = @()
    & $Exe @Arguments 2>&1 | ForEach-Object {
        $collected += $_
        if ($OnLine) { & $OnLine $_ }
    }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved

    # 调用方有时需要解析输出（例如判断虚拟机是否在运行），
    # 所以除了返回退出码，也把输出留在脚本作用域里
    $script:ToolOutput = $collected
    return $code
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  HNeoC OS 构建" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# --- 清理 ---------------------------------------------------
if ($Clean -and (Test-Path $buildDir)) {
    Write-Step "清理 build 目录"
    Remove-Item (Join-Path $buildDir "*") -Recurse -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
}

# --- 定位工具链 ---------------------------------------------
Write-Step "检查工具链"

$nasm = Join-Path $root "tools\nasm.exe"
if (-not (Test-Path $nasm)) {
    $nasmCmd = Get-Command nasm -ErrorAction SilentlyContinue
    if ($nasmCmd) { $nasm = $nasmCmd.Source } else {
        Fail "找不到 NASM。请把 nasm.exe 放到 .\tools\ 目录，或加入 PATH。"
    }
}

$mingwCandidates = @(
    "D:\mingw32\bin",
    (Join-Path $root "tools\mingw32\bin"),
    "C:\mingw32\bin"
)
$mingwBin = $null
foreach ($candidate in $mingwCandidates) {
    if (Test-Path (Join-Path $candidate "gcc.exe")) { $mingwBin = $candidate; break }
}
if (-not $mingwBin) {
    $gccCmd = Get-Command gcc -ErrorAction SilentlyContinue
    if ($gccCmd) { $mingwBin = Split-Path $gccCmd.Source } else {
        Fail "找不到 MinGW。请把 i686 版 MinGW 解压到 D:\mingw32\ 或加入 PATH。"
    }
}

$gcc     = Join-Path $mingwBin "gcc.exe"
$ld      = Join-Path $mingwBin "ld.exe"
$objcopy = Join-Path $mingwBin "objcopy.exe"

# 校验位数：必须是 i686，x86_64 无法编译 32 位内核
$machine = (& $gcc -dumpmachine).Trim()
if ($machine -notmatch "i686|i386") {
    Fail "当前 GCC 目标是 $machine，需要 i686（32 位）版本。请换用 i686 版 MinGW。"
}

Write-Ok "NASM    : $nasm"
Write-Ok "MinGW   : $mingwBin ($machine)"
Write-Host ""

# --- 1. 引导扇区 --------------------------------------------
Write-Step "[1/6] 汇编引导扇区"
& $nasm -f bin (Join-Path $root "boot\boot.asm") -o (Join-Path $buildDir "boot.bin")
if ($LASTEXITCODE -ne 0) { Fail "boot.asm 汇编失败" }
$bootSize = (Get-Item (Join-Path $buildDir "boot.bin")).Length
if ($bootSize -ne 512) { Fail "引导扇区大小是 $bootSize 字节，必须正好 512 字节" }
Write-Ok "boot.bin (512 字节)"

# --- 2. 汇编内核底层代码 ------------------------------------
Write-Step "[2/6] 汇编内核入口与中断存根"
& $nasm -f win32 (Join-Path $root "kernel\arch.asm") -o (Join-Path $buildDir "arch.o")
if ($LASTEXITCODE -ne 0) { Fail "arch.asm 汇编失败" }
Write-Ok "arch.o"

# --- 3. 编译 C 源码 -----------------------------------------
Write-Step "[3/6] 编译 C 源文件"

$cflags = @(
    "-m32",
    "-std=gnu11",
    "-ffreestanding",
    "-fno-pic",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables",
    "-nostdlib",
    "-nostdinc",
    "-Wall",
    "-Wextra",
    "-c",
    "-I$(Join-Path $root 'include')"
)

$sources = @()
$sources += Get-ChildItem (Join-Path $root "kernel")  -Filter *.c | Sort-Object Name
$sources += Get-ChildItem (Join-Path $root "drivers") -Filter *.c | Sort-Object Name
$sources += Get-ChildItem (Join-Path $root "lib")     -Filter *.c | Sort-Object Name

$objects = @(Join-Path $buildDir "arch.o")

foreach ($src in $sources) {
    $obj = Join-Path $buildDir ($src.BaseName + ".o")
    $code = Invoke-Tool -Exe $gcc -Arguments ($cflags + @($src.FullName, "-o", $obj)) -OnLine {
        param($line)
        # 告警不算失败，但要显示出来
        if ($line -match "warning:") {
            Write-Host "      $line" -ForegroundColor Yellow
        } elseif ($line -notmatch "^\s*$") {
            Write-Host "      $line" -ForegroundColor DarkGray
        }
    }
    if ($code -ne 0) { Fail "$($src.Name) 编译失败" }
    $objects += $obj
    Write-Ok "$($src.Name)"
}

# --- 4. 链接内核 --------------------------------------------
Write-Step "[4/6] 链接内核（PE 格式）"

$kernelPe = Join-Path $buildDir "kernel.pe"

$linkArgs = @(
    "-m32",
    "-nostdlib",
    # 注意：这里不能加 --image-base。PE 链接器会在镜像基址之上再叠加
    # 脚本里的 ". = 0x10000"，导致节之间出现 64KB 空洞。用默认基址时
    # 脚本地址会被直接采用，objcopy 才能得到紧凑的裸二进制。
    "-Wl,-T,$(Join-Path $root 'linker-pe.ld')",
    "-Wl,--entry,_kernel_entry",
    "-Wl,--disable-auto-import",
    "-o", $kernelPe
) + $objects

$code = Invoke-Tool -Exe $gcc -Arguments $linkArgs -OnLine {
    param($line)
    # "section below image base" 是 PE 链接器对低地址镜像的常规抱怨，
    # 对裸机二进制没有影响
    if ($line -notmatch "section below image base" -and $line -notmatch "^\s*$") {
        Write-Host "      $line" -ForegroundColor DarkGray
    }
}
if ($code -ne 0 -or -not (Test-Path $kernelPe)) { Fail "内核链接失败" }
Write-Ok "kernel.pe"

# --- 5. 提取裸二进制 ----------------------------------------
Write-Step "[5/6] 提取裸二进制"
$kernelBin = Join-Path $buildDir "kernel.bin"
$code = Invoke-Tool -Exe $objcopy -Arguments @("-O", "binary", $kernelPe, $kernelBin)
if ($code -ne 0) { Fail "objcopy 转换失败" }

$kernelSize = (Get-Item $kernelBin).Length
# 引导扇区分 4 块加载，每块 32KB，合计 128KB
$maxKernel  = 128KB
if ($kernelSize -gt $maxKernel) {
    Fail "内核大小 $([math]::Round($kernelSize/1KB,1))KB 超过引导程序能加载的 128KB"
}
Write-Ok "kernel.bin ($([math]::Round($kernelSize/1KB,2)) KB)"

# --- 6. 生成可启动镜像 --------------------------------------
Write-Step "[6/6] 生成启动镜像"

$bootBytes   = [System.IO.File]::ReadAllBytes((Join-Path $buildDir "boot.bin"))
$kernelBytes = [System.IO.File]::ReadAllBytes($kernelBin)

# 镜像大小：引导扇区 + 内核（最多 128KB，占 LBA 1-256）
# 之后从 LBA 2048（1MB 处）开始是 HNeoFS 文件系统区域
$imageSectors = 16384                                  # 8MB
$image = New-Object byte[] ($imageSectors * 512)
[Array]::Copy($bootBytes, 0, $image, 0, $bootBytes.Length)
[Array]::Copy($kernelBytes, 0, $image, 512, $kernelBytes.Length)

[System.IO.File]::WriteAllBytes($imgFile, $image)
Write-Ok "hneoc-os.img (8 MB, 16384 sectors)"

# --- 6b. 编译用户程序并放进 fsroot/ -------------------------
$userDir = Join-Path $root "user"
$fsRoot  = Join-Path $root "fsroot"
$fsBin   = Join-Path $fsRoot "bin"      # 可执行程序统一放 /bin 下
$nmExe   = Join-Path $mingwBin "nm.exe"

if (Test-Path $userDir) {
    Write-Step "[6b] 编译用户程序"

    $userBuild = Join-Path $buildDir "user"
    New-Item -ItemType Directory -Force -Path $userBuild | Out-Null
    New-Item -ItemType Directory -Force -Path $fsBin | Out-Null

    # 清掉上一次生成的 .lxe，程序删掉之后不会残留在镜像里
    Get-ChildItem $fsBin -Filter *.lxe -ErrorAction SilentlyContinue |
        Remove-Item -Force

    # 入口 crt0
    $crt0 = Join-Path $userBuild "crt0.o"
    $code = Invoke-Tool -Exe $nasm -Arguments @(
        "-f", "win32", (Join-Path $userDir "crt0.asm"), "-o", $crt0)
    if ($code -ne 0) { Fail "crt0.asm 汇编失败" }

    $uflags = @(
        "-m32", "-std=gnu11", "-ffreestanding", "-fno-pic", "-fno-builtin",
        "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
        # MinGW 在 PE 目标上会给大栈帧插入 __chkstk_ms 调用，裸机链接时没有它
        "-mno-stack-arg-probe",
        "-ffunction-sections", "-fdata-sections",
        "-nostdlib", "-nostdinc", "-Wall", "-Wextra", "-c",
        "-I$(Join-Path $userDir 'lib\include')", "-I$(Join-Path $root 'user')"
    )

    # user/lib/ 下是公共运行时（迷你 libc），编译成目标文件链进每个程序，
    # 不会被当成独立的可执行文件
    $libDir = Join-Path $userDir "lib\src"
    $libObjects = @()
    if (Test-Path $libDir) {
        foreach ($lsrc in (Get-ChildItem $libDir -Filter *.c -File | Sort-Object Name)) {
            $lobj = Join-Path $userBuild ("lib_" + $lsrc.BaseName + ".o")
            $code = Invoke-Tool -Exe $gcc -Arguments ($uflags + @($lsrc.FullName, "-o", $lobj)) -OnLine {
                param($line)
                if ($line -match "warning:") {
                    Write-Host "      $line" -ForegroundColor Yellow
                } elseif ($line -notmatch "^\s*$") {
                    Write-Host "      $line" -ForegroundColor DarkGray
                }
            }
            if ($code -ne 0) { Fail "lib/$($lsrc.Name) 编译失败" }
            $libObjects += $lobj
        }
    }

    # -File 排除子目录：只把顶层的 .c 当成程序
    $programs = @(Get-ChildItem $userDir -Filter *.c -File | Sort-Object Name)

    foreach ($src in $programs) {
        $name = $src.BaseName
        $obj  = Join-Path $userBuild "$name.o"
        $pe   = Join-Path $userBuild "$name.pe"
        $bin  = Join-Path $userBuild "$name.bin"
        $lxe  = Join-Path $fsBin "$name.lxe"

        # 编译
        $code = Invoke-Tool -Exe $gcc -Arguments ($uflags + @($src.FullName, "-o", $obj)) -OnLine {
            param($line)
            if ($line -match "warning:") {
                Write-Host "      $line" -ForegroundColor Yellow
            } elseif ($line -notmatch "^\s*$") {
                Write-Host "      $line" -ForegroundColor DarkGray
            }
        }
        if ($code -ne 0) { Fail "$($src.Name) 编译失败" }

        # 链接到 0x40000000。带上 --gc-sections，
        # 没用到的库函数会被丢掉，.lxe 不会白白变大
        $code = Invoke-Tool -Exe $gcc -Arguments (@(
            "-m32", "-nostdlib",
            "-Wl,-T,$(Join-Path $userDir 'user.ld')",
            "-Wl,--entry,_start",
            "-Wl,--disable-auto-import",
            "-Wl,--gc-sections",
            "-o", $pe, $crt0, $obj
        ) + $libObjects) -OnLine {
            param($line)
            if ($line -notmatch "section below image base" -and $line -notmatch "^\s*$") {
                Write-Host "      $line" -ForegroundColor DarkGray
            }
        }
        if ($code -ne 0) { Fail "$($src.Name) 链接失败" }

        # 从符号表里取 .bss 的起止地址，算出 LXE 头部需要的 bss_size
        Invoke-Tool -Exe $nmExe -Arguments @($pe) | Out-Null
        $bssStart = [uint32]0
        $bssEnd   = [uint32]0
        foreach ($line in $script:ToolOutput) {
            if ($line -match '^([0-9a-fA-F]+)\s+\S\s+_ubss_start') {
                $bssStart = [Convert]::ToUInt32($Matches[1], 16)
            }
            if ($line -match '^([0-9a-fA-F]+)\s+\S\s+_ubss_end') {
                $bssEnd = [Convert]::ToUInt32($Matches[1], 16)
            }
        }
        $bssSize = $bssEnd - $bssStart

        # 裸二进制
        $code = Invoke-Tool -Exe $objcopy -Arguments @("-O", "binary", $pe, $bin)
        if ($code -ne 0) { Fail "$($src.Name) 转换二进制失败" }

        # 套上 LXE 头部
        $code = Invoke-Tool -Exe "powershell" -Arguments @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $root "tools\mklxe.ps1"),
            "-InputFile", $bin, "-OutputFile", $lxe,
            "-Name", $name, "-BssSize", "$bssSize",
            "-Entry", "0", "-Flags", "1"
        )
        if ($code -ne 0) { Fail "$($src.Name) 打包 LXE 失败" }
    }

    Write-Ok "$($programs.Count) 个用户程序"
} else {
    Write-Host "    没有 user/ 目录，跳过用户程序编译" -ForegroundColor DarkGray
}

# --- 6c. 把 fsroot/ 里的文件打包成 HNeoFS -----------------
if (Test-Path $fsRoot) {
    $mkfs = Join-Path $root "tools\mkfs.ps1"
    $code = Invoke-Tool -Exe "powershell" -Arguments @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $mkfs,
        "-Image", $imgFile, "-SourceDir", $fsRoot
    )
    if ($code -ne 0) { Fail "打包文件系统失败" }
} else {
    Write-Host "    没有 fsroot/ 目录，跳过文件系统打包" -ForegroundColor DarkGray
}

# --- 可选：转换为 VirtualBox 磁盘 ---------------------------
$vdiCreated = $false
if (-not $NoVdi) {
    Write-Host ""
    Write-Step "转换为 VirtualBox VDI"

    $vboxManage = $null
    $vboxCandidates = @(
        $env:VBOX_MSI_INSTALL_PATH,
        "D:\VB",
        "C:\Program Files\Oracle\VirtualBox",
        "C:\Program Files (x86)\Oracle\VirtualBox"
    )
    foreach ($candidate in $vboxCandidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate "VBoxManage.exe"))) {
            $vboxManage = Join-Path $candidate "VBoxManage.exe"
            break
        }
    }

    if ($vboxManage) {
        # VDI 正被虚拟机占用时文件删不掉，所以先关机
        if ($VmName) {
            Invoke-Tool -Exe $vboxManage -Arguments @("list", "runningvms") | Out-Null
            if (($script:ToolOutput -join "`n") -match [regex]::Escape($VmName)) {
                Write-Host "    虚拟机 '$VmName' 正在运行，先关机以释放磁盘文件" -ForegroundColor Yellow
                Invoke-Tool -Exe $vboxManage -Arguments @("controlvm", $VmName, "poweroff") | Out-Null
                Start-Sleep -Seconds 2
            }
        }

        if (Test-Path $vdiFile) { Remove-Item $vdiFile -Force }

        $vboxExit = Invoke-Tool -Exe $vboxManage `
            -Arguments @("convertfromraw", $imgFile, $vdiFile, "--format", "VDI") -OnLine { param($l) }

        if ($vboxExit -eq 0 -and (Test-Path $vdiFile)) {
            Write-Ok "hneoc-os.vdi"
            $vdiCreated = $true
        } else {
            Write-Host "    VDI 转换失败，可直接使用 IMG" -ForegroundColor Yellow
        }
    } else {
        Write-Host "    未找到 VBoxManage，跳过（不影响 IMG 使用）" -ForegroundColor DarkGray
    }
}

# --- 把新 VDI 重新挂到虚拟机上 -------------------------------
# 每次 convertfromraw 生成的都是一个新磁盘，UUID 会变，而 VirtualBox
# 的介质注册表里还记着旧 UUID，直接启动会报 "does not match the value
# stored in the media registry"。这里自动卸下旧盘、注销旧登记、挂上新盘。
if ($vdiCreated -and $VmName -and -not $NoAttach) {
    Write-Host ""
    Write-Step "把新磁盘挂到虚拟机 '$VmName'"

    $savedEap3 = $ErrorActionPreference
    $ErrorActionPreference = "Continue"

    $vmRunning = (& $vboxManage list runningvms) -match [regex]::Escape($VmName)
    if ($vmRunning) {
        Write-Host "    虚拟机车运行中，先关机" -ForegroundColor Yellow
        & $vboxManage controlvm $VmName poweroff 2>&1 | Out-Null
        Start-Sleep -Seconds 2
    }

    # 找到 IDE 控制器上正在使用的旧磁盘并卸下
    $info = & $vboxManage showvminfo $VmName --machinereadable 2>&1
    $attached = $info | Select-String -Pattern '^"IDE-0-0"="(.+)"' | Select-Object -First 1
    if ($attached) {
        & $vboxManage storageattach $VmName --storagectl "IDE" --port 0 --device 0 `
            --type hdd --medium none 2>&1 | Out-Null
    }

    # 注销所有指向 hneoc-os.vdi 的失效登记项
    $hdds = & $vboxManage list hdds 2>&1
    $currentUuid = $null
    for ($i = 0; $i -lt $hdds.Count; $i++) {
        if ($hdds[$i] -match '^UUID:\s+(\S+)') { $currentUuid = $Matches[1] }
        if ($hdds[$i] -match [regex]::Escape($vdiFile)) {
            & $vboxManage closemedium $currentUuid 2>&1 | Out-Null
        }
    }

    & $vboxManage storageattach $VmName --storagectl "IDE" --port 0 --device 0 `
        --type hdd --medium $vdiFile 2>&1 | Out-Null
    $attachExit = $LASTEXITCODE

    $ErrorActionPreference = $savedEap3

    if ($attachExit -eq 0) {
        Write-Ok "已挂载到 $VmName"
    } else {
        Write-Host "    自动挂载失败，请在 VirtualBox 界面里手动选择磁盘" -ForegroundColor Yellow
    }
}

# --- 完成 ---------------------------------------------------
Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "  构建成功" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
Write-Host ""
Write-Host "输出文件：" -ForegroundColor Cyan
Write-Host "  $imgFile"
if ($vdiCreated) { Write-Host "  $vdiFile" }
Write-Host ""
Write-Host "在 VirtualBox 中测试：" -ForegroundColor Yellow
Write-Host "  1. 新建虚拟机：类型 Other，版本 Other/Unknown (32-bit)"
Write-Host "  2. 内存 32MB 以上"
Write-Host "  3. 硬盘选择：使用已有的虚拟硬盘文件"
if ($vdiCreated) {
    Write-Host "     选 $vdiFile"
} else {
    Write-Host "     选 $imgFile （文件类型选“所有文件”）"
}
Write-Host "  4. 启动，等待内核日志出现 hneoc> 提示符"
Write-Host ""
