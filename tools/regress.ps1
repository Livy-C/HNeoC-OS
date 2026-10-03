# ============================================================
#  regress.ps1 - boot the VM once, drive it from the host, and
#  assert on the serial console mirror.
#
#  Why this exists: every feature in this project was verified by
#  hand - build, boot, type 20 commands with sleeps, then grep the
#  serial log. That works, but it is slow to repeat and easy to get
#  wrong (a typo in a delay and the next command lands in the wrong
#  program). This script is that same sequence, made repeatable and
#  with assertions, so "did I break anything?" is one command.
#
#  How it works:
#    - build (unless -SkipBuild), boot headless, wait for the banner
#    - for each check: type its commands, then POLL the serial log
#      until every expected regex shows up (no fixed sleeps), with a
#      per-check timeout
#    - a check only looks at the part of the log written AFTER the
#      previous check, so an old matching line cannot make it pass
#    - if the VM dies (gurumeditation / aborted) the run stops there
#      and says which check was running
#
#  Usage:
#    .\tools\regress.ps1                 # build + full run
#    .\tools\regress.ps1 -SkipBuild      # reuse the current image
#    .\tools\regress.ps1 -Only hpm       # only checks whose name matches
#    .\tools\regress.ps1 -KeepRunning    # leave the VM up at the end
#    .\tools\regress.ps1 -List           # just list the checks
#
#  NOTE: ASCII-only on purpose. Windows PowerShell 5.1 reads a
#  BOM-less script as GBK on a Chinese locale, which mangles any
#  non-ASCII byte (README trap 14). Keep it that way.
# ============================================================

param(
    [switch]$SkipBuild,
    [switch]$KeepRunning,
    [switch]$List,
    [string]$Only = "",
    [int]$BootWait = 34,
    # How fast to type into the guest. 250ms is the conservative default:
    # VBoxManage keyboardputscancode is a separate process per keystroke, and
    # on a loaded machine faster values have been seen to drop or duplicate
    # scan codes ("systest" arriving as "sstes"). If the input looks scrambled
    # in the log, first make sure nobody else is typing into the same VM -
    # two input sources at once mangle the stream in exactly the same way.
    # Raise this with -TypeDelayMs if a run still looks flaky.
    [int]$TypeDelayMs = 250,
    [int]$StepTimeout = 30
)

$ErrorActionPreference = "Stop"

$root     = Split-Path $PSScriptRoot -Parent
$vmName   = "HNeoC"
$logPath  = Join-Path $root "build\serial.log"
$typeTool = Join-Path $PSScriptRoot "vm-type.ps1"

# --- find VBoxManage the same way the other scripts do -------------
$vbox = $null
foreach ($candidate in @($env:VBOX_MSI_INSTALL_PATH, "D:\VB",
                         "C:\Program Files\Oracle\VirtualBox",
                         "C:\Program Files (x86)\Oracle\VirtualBox")) {
    if ($candidate -and (Test-Path (Join-Path $candidate "VBoxManage.exe"))) {
        $vbox = Join-Path $candidate "VBoxManage.exe"
        break
    }
}
if (-not $vbox) { throw "VBoxManage.exe not found" }

function Write-Ok($text)   { Write-Host ("  [ OK ] " + $text) -ForegroundColor Green }
function Write-Bad($text)  { Write-Host ("  [FAIL] " + $text) -ForegroundColor Red }
function Write-Step($text) { Write-Host $text -ForegroundColor Cyan }

# --- serial log ----------------------------------------------------
# VirtualBox keeps the file open, so read it with sharing enabled.
function Get-SerialText {
    if (-not (Test-Path $logPath)) { return "" }
    $fs = New-Object System.IO.FileStream($logPath,
            [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::ReadWrite)
    $sr = New-Object System.IO.StreamReader($fs)
    $t  = $sr.ReadToEnd()
    $sr.Close(); $fs.Close()
    return $t
}

function Get-VmState {
    # $ErrorActionPreference = "Stop" turns ANY stderr from a native command
    # into a terminating error, and VBoxManage writes diagnostics there
    # ("Machine is not currently running" and friends). Relax it around the
    # call - same trick build.ps1 uses in Invoke-Tool.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $vbox showvminfo $vmName --machinereadable 2>$null
    $ErrorActionPreference = $saved
    $line = $out | Select-String '^VMState='
    if (-not $line) { return "unknown" }
    return ($line.Line -replace '^VMState="', '' -replace '"$', '')
}

function Stop-Vm {
    $state = Get-VmState
    if ($state -ne "running" -and $state -ne "paused" -and
        $state -ne "gurumeditation") {
        return
    }
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $vbox controlvm $vmName poweroff 2>$null | Out-Null
    $ErrorActionPreference = $saved
    Start-Sleep -Seconds 3
}

function Send-Text($text) {
    & $typeTool -VmName $vmName -Text $text -Enter -DelayMs $TypeDelayMs | Out-Null
}

function Send-Key($key) {
    & $typeTool -VmName $vmName -Key $key -DelayMs $TypeDelayMs | Out-Null
}

# --- the checks ----------------------------------------------------
#
# Send : strings to type, each finished with Enter
# Key  : optional named key sent first (Escape, Enter, Up, ...)
# Expect : every one of these regexes must appear after the previous
#          check's log offset
#
$checks = @(
    @{ Name = "boot banner";      Send = @();
       Expect = @("HNeoC OS v0\.3\.0 boot complete",
                  "Reserved the boot stack at 0x90000",
                  "Kernel heap \(kmalloc / kfree\)") },

    @{ Name = "login as root";    Send = @("root", "root");
       Expect = @("welcome, root \(admin\)") },

    @{ Name = "shell + ver";      Send = @("ver");
       Expect = @("kernel        : 0\.3\.0") },

    @{ Name = "mem: heap range";  Send = @("mem");
       Expect = @("range   : 0x00100000 - 0x00500000") },

    @{ Name = "bigbss 512KB bss"; Send = @("bigbss");
       Expect = @("zero filled at start : yes",
                  "the 512KB array works") },

    # KNOWN-ISSUES #1: an image past 64KB used to triple-fault the VM while
    # the loader read it into a kmalloc'd buffer. The loader now reads the
    # image in page-sized chunks straight into the process's page frames, so
    # there is no staging buffer at all.
    @{ Name = "a 65KB image loads and runs"; Send = @("toobig");
       Expect = @("an oversized program image",
                  "toobig: if you can read this") },

    # The original extreme reproducer (a 512KB *initialised* array, so the
    # .lxe is 526KB) now loads and runs too. The checksum pins down that the
    # whole body really arrived: all 512 sampled bytes are 0 except buf[0].
    @{ Name = "a 526KB image loads and runs"; Send = @("bigdata");
       Expect = @("bigdata: a 512KB \*initialised\* array",
                  "the first few bytes are initialised",
                  "checksum : 116",
                  "the big .data image works") },

    # ... and an image past PROCESS_MAX_LOAD_BYTES (1MB) is still refused
    # with a plain error instead of a crash.
    @{ Name = "image past the load limit is refused";
       Send = @("waytoobig");
       Expect = @("program image is too large to load") },

    @{ Name = "bigio long I/O";   Send = @("bigio");
       Expect = @("size intact", "ticks during I/O") },

    @{ Name = "systest guards";   Send = @("systest");
       Expect = @("the guards held") },

    @{ Name = "fault kills only the process"; Send = @("fault");
       Expect = @("killed: Page Fault") },

    @{ Name = "vi opens a file";  Send = @("vi /share/hncc/hello.c");
       Expect = @("NORMAL") },

    @{ Name = "vi quits";         Key = "Escape"; Send = @(":q!");
       # single quotes: in a double-quoted string PowerShell would expand
       # $root and the regex would become "hneoc\D:\Projects\..." (it uses
       # the backtick as its escape character, not the backslash)
       Expect = @('hneoc\$root') },

    @{ Name = "hpm avail";        Send = @("hpm avail");
       Expect = @("available packages", "examples") },

    @{ Name = "hpm install pulls deps"; Send = @("hpm install examples");
       Expect = @("installed hello 1\.0", "installed tools 1\.0",
                  "installed examples 1\.0") },

    @{ Name = "installed program runs"; Send = @("hi");
       Expect = @("installed with hpm") },

    @{ Name = "hpm files";        Send = @("hpm files examples");
       Expect = @("/share/examples/hello\.c") },

    @{ Name = "hpm verify";       Send = @("hpm verify");
       Expect = @("0 missing") },

    @{ Name = "hpm remove";       Send = @("hpm remove examples");
       Expect = @("removed examples") },

    @{ Name = "hncc compiles t1.c"; Send = @("hncc /share/hncc/t1.c");
       Expect = @("compiled /share/hncc/t1\.c",
                  "output : /share/hncc/t1\.lxe") },

    @{ Name = "hncc output is correct"; Send = @("/share/hncc/t1.lxe");
       Expect = @("1 65 66", "2 LITERAL-OK", "3 10", "4 7",
                  "5 67", "6 87 90", "7 16", "8 1") },

    # 只发射用得到的运行时函数之后，hello.c 的代码区从一万多字节掉到
    # 一千五百左右。这里的 `\d{1,4}` 必须紧跟 `bytes`，所以五位数字
    # （旧的 10382）匹配不上 —— 死代码消除要是被改回去，这条会红。
    @{ Name = "hncc compiles hello.c"; Send = @("hncc /share/hncc/hello.c");
       Expect = @("compiled /share/hncc/hello\.c",
                  "code\s+:\s+\d{1,4}\s+bytes") },

    @{ Name = "hncc hello runs";  Send = @("/share/hncc/hello.lxe");
       Expect = @("compiled on HNeoC", "counter after the loop") },

    # Structs: local struct, pointer to struct, '->' chains, struct arrays,
    # sizeof, and passing a struct pointer into a function.
    @{ Name = "hncc compiles t2.c (structs)"; Send = @("hncc /share/hncc/t2.c");
       Expect = @("compiled /share/hncc/t2\.c") },

    @{ Name = "hncc struct output is correct"; Send = @("/share/hncc/t2.lxe");
       Expect = @("1 34", "2 34", "3 42", "4 8", "5 8",
                  "6 40", "7 56", "8 24", "9 43") },

    # Initialiser lists and the qualifier keywords (static/const/unsigned/
    # long): `int a[] = {1,2,3}` with an inferred size, global and local,
    # char arrays from a string, and sizeof of an inferred array.
    @{ Name = "hncc compiles t3.c (initialisers)"; Send = @("hncc /share/hncc/t3.c");
       Expect = @("compiled /share/hncc/t3\.c") },

    @{ Name = "hncc initialiser output is correct"; Send = @("/share/hncc/t3.lxe");
       Expect = @("1 1234", "2 hncc", "3 12", "4 30",
                  "5 ac", "6 5008", "7 xyz", "8 109") },

    # Variadic functions: printf lives in hncc's own runtime and pulls its
    # arguments off the stack, so this also tests user-defined varargs.
    @{ Name = "hncc compiles t4.c (printf)"; Send = @("hncc /share/hncc/t4.c");
       Expect = @("compiled /share/hncc/t4\.c") },

    @{ Name = "hncc printf output is correct"; Send = @("/share/hncc/t4.lxe");
       Expect = @("1 42", "2 hncc has 4", "3 xyz",
                  "4 ff -5 12", "5 1 2 3 4 5", "6 n=7%", "7 107") },

    # enum (implicit and explicit values, negatives) and switch (fall-through,
    # 'case 2: case 3:' label lists, default, break vs continue inside a loop),
    # plus a multi-declarator local declaration followed by more statements.
    @{ Name = "hncc compiles t5.c (enum/switch)"; Send = @("hncc /share/hncc/t5.c");
       Expect = @("compiled /share/hncc/t5\.c") },

    @{ Name = "hncc enum/switch output is correct"; Send = @("/share/hncc/t5.lxe");
       Expect = @("1 0 1 2 3", "2 0 7 -3", "3 100 200 300 999",
                  "4 14", "5 110", "6 27", "7 10000", "8 42",
                  "9 -3", "10 2", "11 2 8 3") },

    # typedef (struct/int/pointer), a struct forward declaration so two structs
    # can point at each other, the runtime string functions, and NULL.
    @{ Name = "hncc compiles t6.c (typedef/strings)"; Send = @("hncc /share/hncc/t6.c");
       Expect = @("compiled /share/hncc/t6\.c") },

    @{ Name = "hncc typedef/strings output is correct"; Send = @("/share/hncc/t6.lxe");
       Expect = @("1 42", "2 7", "3 42 8", "4 5",
                  "5 abcdef 6", "6 1 0", "7 cdef 0", "8 0", "9 1") },

    # Anonymous struct typedefs, declarator lists with pointers (`int a, *b;`
    # / `int* c, d;`), typedef lists with pointers, a typedef inside a function
    # body, and print_hex with a negative value.
    @{ Name = "hncc compiles t7.c (typedef forms/declarators)";
       Send = @("hncc /share/hncc/t7.c");
       Expect = @("compiled /share/hncc/t7\.c") },

    @{ Name = "hncc typedef/declarator output is correct";
       Send = @("/share/hncc/t7.lxe");
       Expect = @("1 11 22 8", "2 33 44", "3 7 6", "4 9",
                  "5 8", "6 6", "7 ffffffff") },

    # Built-in type aliases (uint32_t / size_t / bool ...). hncc ignores
    # #include, so without them its own source does not even get past line 79.
    @{ Name = "hncc compiles t8.c (stdint names)"; Send = @("hncc /share/hncc/t8.c");
       Expect = @("compiled /share/hncc/t8\.c") },

    @{ Name = "hncc stdint-name output is correct"; Send = @("/share/hncc/t8.lxe");
       Expect = @("1 1 4 4", "2 42 42", "3 7", "4 1 0", "5 4 1", "6 12345678") },

    # ... and re-typedef'ing one of them is tolerated, but a *conflicting*
    # typedef is still an error (that one would silently change semantics).
    @{ Name = "hncc rejects a conflicting typedef";
       Send = @("hncc /share/hncc/badtypedef.c");
       Expect = @("already a typedef, and with a different type") },

    # #define now evaluates a constant expression (arithmetic, hex, char
    # literals, references to earlier constants, parentheses, bit ops)
    # instead of only accepting a plain decimal number.
    @{ Name = "hncc compiles t9.c (#define expressions)";
       Send = @("hncc /share/hncc/t9.c");
       Expect = @("compiled /share/hncc/t9\.c") },

    @{ Name = "hncc #define output is correct"; Send = @("/share/hncc/t9.lxe");
       Expect = @("1 98304", "2 31", "3 8", "4 7", "5 12", "6 3", "7 4",
                  "8 65 10", "9 1024", "10 9 abcdefgh") },

    @{ Name = "login as guest";   Send = @("login", "guest", "guest");
       Expect = @("welcome, guest") },

    @{ Name = "guest whoami";     Send = @("whoami");
       Expect = @("uid 1001") },

    @{ Name = "guest cannot write /"; Send = @("mkdir /nope");
       Expect = @("mkdir: permission denied") },

    # 期望里带上 `hneoc$guest` 这个提示符：只看 "mine" 的话，登录失败时
    # 命令行会把 "mkdir mine" 回显出来，"mine" 照样能匹配上 —— 那是一条
    # 假通过（真出过一次）。
    @{ Name = "guest can write its own dir"; Send = @("mkdir mine", "ls");
       Expect = @('hneoc\$guest', "mine") },

    @{ Name = "guest cannot install packages"; Send = @("hpm install docs");
       Expect = @("only an administrator can install packages") }
)

if ($List) {
    Write-Host ""
    Write-Host "  checks defined: $($checks.Count)" -ForegroundColor Cyan
    foreach ($c in $checks) { Write-Host ("    " + $c.Name) }
    Write-Host ""
    exit 0
}

$selected = @()
foreach ($c in $checks) {
    if ($Only -eq "" -or $c.Name -match $Only) { $selected += $c }
}

if ($Only -ne "") {
    # The checks are stateful: they assume the guest is already logged in and
    # the shell is at a prompt. With -Only we therefore still run the
    # prerequisites first, otherwise the command under test gets typed into
    # the login prompt (which is exactly what happened the first time this
    # filter was used).
    $needsGuest = $false
    foreach ($c in $selected) {
        if ($c.Name -like "guest*") { $needsGuest = $true }
    }

    $prereq = @()
    foreach ($c in $checks) {
        if ($c.Name -eq "boot banner" -or $c.Name -eq "login as root") {
            $prereq += $c
        }
    }
    if ($needsGuest) {
        foreach ($c in $checks) {
            if ($c.Name -eq "login as guest") { $prereq += $c }
        }
    }

    $merged = @()
    foreach ($c in $prereq)  { $merged += $c }
    foreach ($c in $selected) {
        $already = $false
        foreach ($m in $merged) { if ($m -eq $c) { $already = $true } }
        if (-not $already) { $merged += $c }
    }
    $selected = $merged
}

if ($selected.Count -eq 0) {
    throw "no check matches '$Only' (use -List to see the names)"
}

# --- build and boot ------------------------------------------------
Write-Host ""
Write-Step "== HNeoC regression run ($($selected.Count) checks) =="
Write-Host ""

Stop-Vm

if (-not $SkipBuild) {
    Write-Host "  building ..." -ForegroundColor DarkGray

    # Judge the build by the image's timestamp, not by grepping the log for
    # Chinese text: PowerShell's *> redirection writes the file in the ANSI
    # codepage while [System.IO.File]::ReadAllText reads it as UTF-8, so
    # "构建成功" comes back as mojibake and the check fails even though the
    # build worked. (That is exactly what happened the first time.)
    $imgPath   = Join-Path $root "build\hneoc-os.img"
    $imgBefore = [datetime]::MinValue
    if (Test-Path $imgPath) { $imgBefore = (Get-Item $imgPath).LastWriteTime }

    & (Join-Path $root "build.ps1") *> (Join-Path $root "build\regress-build.txt")

    if (-not (Test-Path $imgPath) -or
        (Get-Item $imgPath).LastWriteTime -le $imgBefore) {
        Write-Bad "the build did not produce a fresh image - see build\regress-build.txt"
        # Get-Content uses the same ANSI codepage the redirect wrote, so the
        # Chinese error lines are readable here.
        $tail = (Get-Content (Join-Path $root "build\regress-build.txt") |
                 Select-Object -Last 12) -join "`n"
        Write-Host $tail -ForegroundColor DarkGray
        exit 1
    }
    Write-Ok "build"
}

Write-Host "  booting ..." -ForegroundColor DarkGray
& (Join-Path $root "run.ps1") *> (Join-Path $root "build\regress-run.txt")
Start-Sleep -Seconds $BootWait

$state = Get-VmState
if ($state -ne "running") {
    Write-Bad "the VM did not come up (state: $state)"
    # Two states people actually hit, both needing a manual nudge:
    #   aborted-saved : a leftover saved state blocks the boot (usually after
    #                   a run was killed mid-flight)
    #   aborted       : a stale VM entry; discardstate or a fresh start fixes it
    if ($state -match "saved") {
        Write-Host "  hint: discard the saved state first:" -ForegroundColor Yellow
        Write-Host "        & `"`$VBoxManage`" discardstate $vmName" -ForegroundColor Yellow
    }
    if ($state -eq "unknown") {
        Write-Host "  hint: VBoxManage could not be reached - stray VirtualBox" -ForegroundColor Yellow
        Write-Host "        processes from an interrupted run can wedge it:" -ForegroundColor Yellow
        Write-Host "        taskkill /F /IM VBoxHeadless.exe ; taskkill /F /IM VBoxSVC.exe" -ForegroundColor Yellow
    }
    exit 1
}
Write-Ok "boot ($vmName is running)"
Write-Host ""

# --- run the checks ------------------------------------------------
$passed = 0
$failed = @()

# The boot banner is written while we wait for boot, so the very first check
# has to look at the whole log; every later check only looks at what was
# written after the previous check (so an old matching line cannot pass it).
$offset = 0

foreach ($check in $selected) {
    if ($check.ContainsKey("Key")) { Send-Key $check.Key }
    foreach ($s in $check.Send) { Send-Text $s }

    $deadline = (Get-Date).AddSeconds($StepTimeout)
    $missing  = @($check.Expect)
    $region   = ""
    $died     = $false

    while ((Get-Date) -lt $deadline) {
        $text = Get-SerialText
        if ($text.Length -gt $offset) { $region = $text.Substring($offset) }
        $missing = @()
        foreach ($p in $check.Expect) {
            if ($region -notmatch $p) { $missing += $p }
        }
        if ($missing.Count -eq 0) { break }

        $state = Get-VmState
        if ($state -ne "running") { $died = $true; break }
        Start-Sleep -Milliseconds 400
    }

    if ($missing.Count -eq 0) {
        $passed++
        Write-Ok $check.Name
    } else {
        # PowerShell 5.1 has no "assignment from an if expression", so build
        # the message with a plain if/else.
        $why = "missing: " + ($missing -join " | ")
        if ($died) { $why = "the VM died (state: $state)" }
        Write-Bad ($check.Name + "  --  " + $why)
        $failed += @{ Name = $check.Name; Why = $why; Region = $region }
        if ($died) { break }
    }

    # Everything up to here belongs to this check; the next one only looks
    # at what is written afterwards.
    $offset = (Get-SerialText).Length
}

# --- report --------------------------------------------------------
Write-Host ""
Write-Step "== $passed passed, $($failed.Count) failed =="

if ($failed.Count -gt 0) {
    Write-Host ""
    Write-Host "  console excerpt for each failure:" -ForegroundColor Yellow
    foreach ($f in $failed) {
        Write-Host ""
        Write-Host ("  --- " + $f.Name + " (" + $f.Why + ")") -ForegroundColor Yellow
        $lines = ($f.Region -split "`r?`n")
        $from  = [Math]::Max(0, $lines.Count - 22)
        for ($i = $from; $i -lt $lines.Count; $i++) {
            $l = $lines[$i]
            if ($l.Length -gt 120) { $l = $l.Substring(0, 120) + " ..." }
            Write-Host ("      " + $l) -ForegroundColor DarkGray
        }
    }
}

$finalState = Get-VmState
Write-Host ""
Write-Host "  VM state at the end: $finalState" -ForegroundColor DarkGray

if (-not $KeepRunning) {
    Stop-Vm
    Write-Host "  VM powered off (use -KeepRunning to keep it up)" -ForegroundColor DarkGray
}

Write-Host ""
if ($failed.Count -eq 0) {
    Write-Host "  REGRESSION PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "  REGRESSION FAILED" -ForegroundColor Red
    exit 1
}
