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
    [int]$TypeDelayMs = 60,
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

    @{ Name = "hncc compiles hello.c"; Send = @("hncc /share/hncc/hello.c");
       Expect = @("compiled /share/hncc/hello\.c") },

    @{ Name = "hncc hello runs";  Send = @("/share/hncc/hello.lxe");
       Expect = @("compiled on HNeoC", "counter after the loop") },

    @{ Name = "login as guest";   Send = @("login", "guest", "guest");
       Expect = @("welcome, guest") },

    @{ Name = "guest whoami";     Send = @("whoami");
       Expect = @("uid 1001") },

    @{ Name = "guest cannot write /"; Send = @("mkdir /nope");
       Expect = @("mkdir: permission denied") },

    @{ Name = "guest can write its own dir"; Send = @("mkdir mine", "ls");
       Expect = @("mine") },

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
