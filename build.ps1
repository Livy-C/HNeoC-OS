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

# 镜像先构建到这个临时文件，等 [6d] 的断言全部通过之后才改名成 $imgFile。
# 这样构建中途出错、或者被 Ctrl+C 打断时，上次那个完好的镜像不会被半成品覆盖。
$imgTmp   = Join-Path $buildDir "hneoc-os.building.img"

# GCC / NASM 的中间文件默认写到系统 %TEMP%，也就是 C: 盘。系统盘一旦被占满，
# 编译器会以 "No space left on device" 直接失败，而报错出现在某个 .c 文件上，
# 看起来像是源码的问题。把临时目录挪到项目自己所在的盘上，构建就不再受
# 系统盘剩余空间影响（这一条是被真实踩到之后加的：C: 只剩 10MB 时
# wtest.c 报 "error writing to ...ccZzLHHI.s: No space left on device"）。
$tmpDir = Join-Path $buildDir "tmp"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$env:TMP  = $tmpDir
$env:TEMP = $tmpDir

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

# 先写临时文件：校验通过才改名成 hneoc-os.img（见 [6d]）
[System.IO.File]::WriteAllBytes($imgTmp, $image)
Write-Ok "临时镜像 (8 MB, 16384 sectors)"

# --- 6b. 编译用户程序并放进 fsroot/ -------------------------
$userDir = Join-Path $root "user"
$fsRoot  = Join-Path $root "fsroot"
$fsBin   = Join-Path $fsRoot "bin"      # 可执行程序统一放 /bin 下
$nmExe   = Join-Path $mingwBin "nm.exe"

# 软件包源码与打包出来的 .hnpkg 仓库目录（宿主机侧）
$packagesDir = Join-Path $root "packages"
$fsRepo      = Join-Path $fsRoot "var\hpm\repo"

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
        # -ffunction-sections 留着：没被调用的库函数会被 --gc-sections 丢掉。
        #
        # 但 -fdata-sections 必须**关掉**：PE 目标上 GCC 把每个变量放进
        # ".data$变量名"（PE 用 $ 当子段分隔符），而 user.ld 里那个 .bss 段
        # 匹配的是 *(.bss) / *(.bss.*)，于是所有**没有初值**的静态数组
        # 都被实体化进 .data —— .lxe 白白胖出一个数组大小，bss_size 永远是 0。
        # 关掉它，未初始化数据才会落进真正的 .bss（不进文件），由加载器按
        # 头部里的 bss_size 清零。
        # 实测：hncc.c 的 .data 从 233KB 降到 224 字节，.bss 变成 229KB。
        "-ffunction-sections", "-fno-data-sections",
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

    # 编译单元 = 系统程序（进 /bin）+ 软件包里的程序（进 build\packages\<包名>，
    # 稍后由 tools\mkhnpkg.ps1 打进 .hnpkg）。
    #
    # 两条路的编译流程一模一样，所以合成一个列表跑同一段代码：
    # 差别只有输出目录，以及中间文件名要带包名前缀，免得包里的
    # hi.c 和系统里的 hi.c 抢同一个 .o。
    $units = @()
    foreach ($src in $programs) {
        $units += [pscustomobject]@{
            Src = $src; Name = $src.BaseName; Out = $fsBin; Prefix = ""
        }
    }
    $pkgDirs = @()
    if (Test-Path $packagesDir) {
        $pkgDirs = @(Get-ChildItem $packagesDir -Directory | Sort-Object Name)
    }
    $pkgPrograms = 0
    foreach ($pkgDir in $pkgDirs) {
        $pkgSrc = Join-Path $pkgDir.FullName "src"
        if (-not (Test-Path $pkgSrc)) { continue }
        $pkgOut = Join-Path $buildDir ("packages\" + $pkgDir.Name)
        New-Item -ItemType Directory -Force -Path $pkgOut | Out-Null
        Get-ChildItem $pkgOut -Filter *.lxe -ErrorAction SilentlyContinue |
            Remove-Item -Force
        foreach ($src in (Get-ChildItem $pkgSrc -Filter *.c -File | Sort-Object Name)) {
            $units += [pscustomobject]@{
                Src = $src; Name = $src.BaseName; Out = $pkgOut
                Prefix = "$($pkgDir.Name)_"
            }
            $pkgPrograms++
        }
    }

    foreach ($unit in $units) {
        $src  = $unit.Src
        $name = $unit.Name
        $obj  = Join-Path $userBuild "$($unit.Prefix)$name.o"
        $pe   = Join-Path $userBuild "$($unit.Prefix)$name.pe"
        $bin  = Join-Path $userBuild "$($unit.Prefix)$name.bin"
        $lxe  = Join-Path $unit.Out "$name.lxe"

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

        # 从符号表里取 .bss 的起止地址，算出 LXE 头部需要的 bss_size。
        #
        # 这里必须检查 nm 的退出码、以及符号是否真的找到了：nm.exe 缺失或改名时，
        # 下面两个正则一条都匹配不上，$bssSize 会静默变成 0，用户程序的 .bss
        # 就永远不会被清零 —— 构建输出里看不出任何异常，只在运行时变成玄学 bug。
        if (-not (Test-Path $nmExe)) {
            Fail "找不到 nm.exe：$nmExe（MinGW 的 nm，用来读 .bss 的大小）"
        }
        $code = Invoke-Tool -Exe $nmExe -Arguments @($pe)
        if ($code -ne 0) { Fail "$($src.Name) 读符号表失败（nm 退出码 $code）" }

        $bssStart = [uint32]0
        $bssEnd   = [uint32]0
        $bssSeen  = $false
        foreach ($line in $script:ToolOutput) {
            if ($line -match '^([0-9a-fA-F]+)\s+\S\s+_ubss_start') {
                $bssStart = [Convert]::ToUInt32($Matches[1], 16)
                $bssSeen  = $true
            }
            if ($line -match '^([0-9a-fA-F]+)\s+\S\s+_ubss_end') {
                $bssEnd   = [Convert]::ToUInt32($Matches[1], 16)
                $bssSeen  = $true
            }
        }
        if (-not $bssSeen) {
            Fail "$($src.Name) 的符号表里既没有 _ubss_start 也没有 _ubss_end，bss_size 会算错"
        }
        if ($bssEnd -lt $bssStart) {
            Fail "$($src.Name) 的 _ubss_end ($bssEnd) 小于 _ubss_start ($bssStart)"
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
    if ($pkgPrograms -gt 0) {
        Write-Ok "$pkgPrograms 个软件包内的程序（编译到 build\packages\，不进 /bin）"
    }
} else {
    Write-Host "    没有 user/ 目录，跳过用户程序编译" -ForegroundColor DarkGray
}

# --- 6b2. 打包软件包（.hnpkg）与仓库索引 -------------------
#
# packages\<名字>\ 是"源码包"：manifest.txt 描述它，src\*.c 是包里的程序，
# files\ 是要原样装到目标路径的数据。这里把它们打成 fsroot\var\hpm\repo\
# 下的 .hnpkg，再生成一个纯文本索引 —— 相当于在宿主机上维护一个本地源。
# OS 里的 hpm 读的就是这个目录，所以"仓库"不需要网络。
$pkgDirs = @()
if (Test-Path $packagesDir) {
    $pkgDirs = @(Get-ChildItem $packagesDir -Directory | Sort-Object Name)
}

if ($pkgDirs.Count -gt 0) {
    Write-Step "[6b2] 打包软件包（.hnpkg）"

    $packer = Join-Path $root "tools\mkhnpkg.ps1"
    if (-not (Test-Path $packer)) { Fail "找不到打包器 $packer" }

    New-Item -ItemType Directory -Force -Path $fsRepo | Out-Null
    Get-ChildItem $fsRepo -Filter *.hnpkg -ErrorAction SilentlyContinue |
        Remove-Item -Force
    Remove-Item (Join-Path $fsRepo "index") -Force -ErrorAction SilentlyContinue

    $indexLines = @(
        "# HNeoC 软件包索引 —— 由 build.ps1 生成，也可以进系统后跑 hpm update 重建",
        "# name`tversion`tfile`tdepends`tsummary"
    )
    $packed = 0

    foreach ($pkgDir in $pkgDirs) {
        $manifest = Join-Path $pkgDir.FullName "manifest.txt"
        if (-not (Test-Path $manifest)) {
            Fail "$($pkgDir.Name)：没有 manifest.txt"
        }

        # manifest.txt 是 "键: 值" 的纯文本，空行和 # 开头当注释
        $meta = @{}
        foreach ($line in (Get-Content $manifest)) {
            $t = $line.Trim()
            if ($t -eq "" -or $t.StartsWith("#")) { continue }
            $c = $t.IndexOf(":")
            if ($c -lt 1) { continue }
            $meta[$t.Substring(0, $c).Trim().ToLower()] = $t.Substring($c + 1).Trim()
        }
        if (-not $meta["name"] -or -not $meta["version"]) {
            Fail "$($pkgDir.Name)：manifest.txt 里缺 name 或 version"
        }

        $pkgFile = "$($meta['name'])-$($meta['version']).hnpkg"
        $pkgOut  = Join-Path $fsRepo $pkgFile
        $pkgBin  = Join-Path $buildDir ("packages\" + $pkgDir.Name)

        $code = Invoke-Tool -Exe "powershell" -Arguments @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", $packer,
            "-PackageDir", $pkgDir.FullName,
            "-OutputFile", $pkgOut,
            "-BinDir", $pkgBin
        )
        if ($code -ne 0) { Fail "$($pkgDir.Name) 打包失败" }

        # 索引直接从打好的包头里读回来：这样"索引里写的"和"包里写的"
        # 必然是同一份数据，顺带在构建期就验证了包头能不能被解析。
        # 偏移量必须和 user/lib/include/hnpkg.h 对齐。
        $hdr = [System.IO.File]::ReadAllBytes($pkgOut)
        if ($hdr.Length -lt 256) { Fail "$pkgFile 比包头还小（$($hdr.Length) 字节）" }
        if ([BitConverter]::ToUInt32($hdr, 0) -ne 0x4B504E48) {
            Fail "$pkgFile 的魔数不对，不是 'HNPK'"
        }

        $hName = [Text.Encoding]::ASCII.GetString($hdr, 8, 32).TrimEnd([char]0)
        $hVer  = [Text.Encoding]::ASCII.GetString($hdr, 40, 16).TrimEnd([char]0)
        $hDep  = [Text.Encoding]::ASCII.GetString($hdr, 56, 48).TrimEnd([char]0)
        $hSum  = [Text.Encoding]::ASCII.GetString($hdr, 104, 96).TrimEnd([char]0)
        $hCnt  = [BitConverter]::ToUInt32($hdr, 200)
        $hData = [BitConverter]::ToUInt32($hdr, 204)
        $hAll  = [BitConverter]::ToUInt32($hdr, 208)

        if ($hName -ne $meta["name"]) {
            Fail "$pkgFile 包头里的包名是 '$hName'，manifest 里写的是 '$($meta['name'])'"
        }
        if ($hCnt -eq 0) { Fail "$pkgFile 里一个文件都没有" }
        if ($hData -lt 256 -or $hAll -ne $hdr.Length) {
            Fail "$pkgFile 的 data_offset/total_size 不对（$hData / $hAll，文件 $($hdr.Length) 字节）"
        }

        $indexLines += ("{0}`t{1}`t{2}`t{3}`t{4}" -f $hName, $hVer, $pkgFile, $hDep, $hSum)
        $packed++
        Write-Ok ("{0,-10} {1,-6} {2,6} B  {3} 个文件" -f `
            $hName, $hVer, $hdr.Length, $hCnt)
    }

    # 索引必须是无 BOM 的 UTF-8 + LF：hpm 在系统里是按字节解析的，
    # 带 BOM 的话第一行会多出 EF BB BF，字段当场错位。
    $indexText = ($indexLines -join "`n") + "`n"
    [System.IO.File]::WriteAllText((Join-Path $fsRepo "index"), $indexText,
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Ok "$packed 个软件包进了本地源（fsroot\var\hpm\repo）"
}

# --- 6b-3. 把 hncc 自己的源码放进 /share/hncc/ -----------------
# 用来做自举试验：在系统里 `hncc /share/hncc/hncc.c` 编译它自己。
# 这是从 user/hncc.c **拷**过去的（生成的副本，不进版本库），
# 免得仓库里躺两份一模一样的四千行源码。
$selfSrc = Join-Path $fsRoot "share\hncc\hncc.c"
if (Test-Path (Join-Path $root "user\hncc.c")) {
    $shareDir = Split-Path $selfSrc -Parent
    if (-not (Test-Path $shareDir)) { New-Item -ItemType Directory -Path $shareDir -Force | Out-Null }
    Copy-Item (Join-Path $root "user\hncc.c") $selfSrc -Force
    Write-Ok "share/hncc/hncc.c（hncc 自己的源码，给自举试验用）"
}

# --- 6c. 把 fsroot/ 里的文件打包成 HNeoFS -----------------
if (Test-Path $fsRoot) {
    $mkfs = Join-Path $root "tools\mkfs.ps1"
    $code = Invoke-Tool -Exe "powershell" -Arguments @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $mkfs,
        "-Image", $imgTmp, "-SourceDir", $fsRoot
    ) -OnLine {
        param($line)
        if ($line -notmatch '^\s*$') { Write-Host "      $line" -ForegroundColor DarkGray }
    }
    if ($code -ne 0) { Fail "打包文件系统失败" }
} else {
    Write-Host "    没有 fsroot/ 目录，跳过文件系统打包" -ForegroundColor DarkGray
}

# --- 6d. 校验镜像，通过之后才让它上岗 ------------------------
# 宁可在这里停下，也不要产出一个"能启动、但没有文件系统"的镜像。
#
# 两道防线：
#   1) 镜像是先写到 $imgTmp 的，只有下面这些断言全部通过，才改名成 hneoc-os.img。
#      所以构建中途报错、或者被 Ctrl+C / 关窗口打断时，磁盘上上次构建好的那个
#      镜像原封不动，不会被一个半成品覆盖。
#   2) 断言会核对超级块魔数、目录项数量，以及每个刚编译出来的程序是否真的在
#      镜像里。注意 "OK 10 个用户程序" 只说明编译了 10 个，不代表它们进了镜像。
Write-Step "[6d] 校验镜像"

if (-not (Test-Path $imgTmp)) {
    Fail "没有生成 $imgTmp"
}

$imgBytes = [System.IO.File]::ReadAllBytes($imgTmp)
$sbOffset = 2048 * 512          # HNEOFS_START_LBA * 512，见 include/hneofs.h
$HNFS_MAGIC = [uint32]0x53464E48   # 'HNFS'，与 include/hneofs.h 的 HNEOFS_MAGIC 一致（PowerShell 不认 C 的 u 后缀）

if ($imgBytes.Length -lt ($sbOffset + 512)) {
    Fail "镜像只有 $($imgBytes.Length) 字节，连 HNeoFS 超级块的位置都到不了"
}

$sbMagic = [BitConverter]::ToUInt32($imgBytes, $sbOffset)
if ($sbMagic -ne $HNFS_MAGIC) {
    Fail (("LBA 2048 处的魔数是 0x{0:X8}，应该是 0x{1:X8} ('HNFS')。" -f $sbMagic, $HNFS_MAGIC) +
          "`n         文件系统没有被写进镜像，这个镜像能启动但不会加载任何用户程序。" +
          "`n         （mkfs 那一步没跑到：检查上面 [6c] 是否报错、脚本是否被中断。）")
}

$sbCount    = [BitConverter]::ToUInt32($imgBytes, $sbOffset + 16)
$sbTableLba = [BitConverter]::ToUInt32($imgBytes, $sbOffset + 20)

if ($sbCount -lt 1) {
    Fail "HNeoFS 超级块存在，但目录项是 0 个：fsroot/ 是空的，或者源码目录走错了"
}

# 把镜像里真实存在的文件名收集出来，逐个核对刚编译好的程序
$sbNames = @{}
for ($i = 0; $i -lt $sbCount; $i++) {
    $o = ($sbTableLba * 512) + ($i * 64)
    if ($o + 32 -gt $imgBytes.Length) { break }
    $nm = [System.Text.Encoding]::ASCII.GetString($imgBytes, $o, 32).TrimEnd([char]0)
    if ($nm.Length -gt 0) { $sbNames[$nm.ToLower()] = $true }
}

if ($programs -and $programs.Count -gt 0) {
    $missing = @()
    foreach ($prog in $programs) {
        if (-not $sbNames.ContainsKey("$($prog.BaseName).lxe".ToLower())) {
            $missing += "$($prog.BaseName).lxe"
        }
    }
    if ($missing.Count -gt 0) {
        # 括号必须把两段都包进去：Fail 一旦执行就 exit 1，
        # 写成 `Fail (...) + "..."` 的话后半句永远打不出来。
        Fail ((("镜像的 HNeoFS 里找不到这些程序：{0}" -f ($missing -join ", ")) +
               "`n         编译出来的 .lxe 没有进到镜像里（检查 fsroot/bin/ 和 mkfs 的 -SourceDir）。"))
    }
    Write-Ok "HNeoFS 超级块正常，$($programs.Count) 个用户程序都在镜像里（共 $sbCount 个目录项）"
} else {
    Write-Ok "HNeoFS 超级块正常（共 $sbCount 个目录项）"
}

# --- 软件包也必须真的进了镜像 --------------------------------
#
# 这一段是"包管理器能不能用"的第一道闸：.hnpkg 被 packer 生成在 fsroot 里，
# 但如果 mkfs 没把它们打包进镜像（或者目录建漏了），系统里跑 hpm 只会看到
# 一个空仓库，而且症状是"没有这个包"，很容易被当成命令写错了。
if ($pkgDirs.Count -gt 0) {
    $repoPkgs = @(Get-ChildItem $fsRepo -Filter *.hnpkg -ErrorAction SilentlyContinue)
    if ($repoPkgs.Count -eq 0) {
        Fail "packages\ 里有 $($pkgDirs.Count) 个源码包，但 fsroot\var\hpm\repo 里没有 .hnpkg"
    }
    if (-not $sbNames.ContainsKey("repo")) {
        Fail ("镜像里没有 /var/hpm/repo 目录（HNeoFS 里没有 'repo' 这一项）。" +
              "`n         hpm 会找不到仓库；检查 fsroot\var\hpm\repo\ 是否真的存在（空目录不会被打包，放一个 .keep）。")
    }

    $missingPkg = @()
    foreach ($rp in $repoPkgs) {
        if (-not $sbNames.ContainsKey($rp.Name.ToLower())) {
            $missingPkg += $rp.Name
        }
    }
    if (-not $sbNames.ContainsKey("index")) {
        $missingPkg += "index"
    }
    if ($missingPkg.Count -gt 0) {
        # 整条消息要在 Fail 之前拼好：写成
        #   Fail ("..." -f (...)) + "..."
        # 的话，+ 后面那半截是**另一个**表达式，Fail 早就带着半句话 exit 了，
        # 那半截永远不会打印出来。
        Fail (("镜像的 HNeoFS 里找不到这些仓库文件：{0}" -f ($missingPkg -join ", ")) +
              "`n         检查 fsroot\var\hpm\repo\ 和 mkfs 的 -SourceDir。")
    }
    Write-Ok "$($repoPkgs.Count) 个 .hnpkg 和索引都在镜像里（共 $sbCount 个目录项）"
}

# 校验通过，临时镜像正式上岗。到这一行为止，原来那个 hneoc-os.img 一直没被动过。
Move-Item -Force $imgTmp $imgFile
Write-Ok "hneoc-os.img 已更新"

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
