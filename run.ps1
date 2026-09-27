# ============================================================
#  HNeoC OS - build, boot headless, and print the serial console
#
#  The kernel mirrors everything it draws on screen to COM1, so
#  redirecting the VM's serial port to a file gives us the exact
#  screen contents without needing a graphical session.
#
#  NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1
#  reads BOM-less .ps1 files as GBK on a Chinese locale, which mangles
#  non-ASCII bytes and can swallow line breaks.
#
#  Usage:
#    .\run.ps1                 build, boot, show screen, stay running
#    .\run.ps1 -NoBuild        boot what is already built
#    .\run.ps1 -Type "help"    type something after boot and show result
#    .\run.ps1 -Stop           power the VM off
# ============================================================

param(
    [string]$VmName = "HNeoC",
    [switch]$NoBuild,
    [switch]$Stop,
    [string[]]$Type = @(),
    [int]$WaitSeconds = 7
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$log  = Join-Path $root "build\serial.log"

function Find-VBoxManage {
    foreach ($candidate in @($env:VBOX_MSI_INSTALL_PATH, "D:\VB",
                            "C:\Program Files\Oracle\VirtualBox",
                            "C:\Program Files (x86)\Oracle\VirtualBox")) {
        if ($candidate -and (Test-Path (Join-Path $candidate "VBoxManage.exe"))) {
            return Join-Path $candidate "VBoxManage.exe"
        }
    }
    throw "VBoxManage.exe not found"
}

$vbox = Find-VBoxManage

# Windows PowerShell 5.1 reads a BOM-less .ps1 file using the system ANSI
# code page (GBK on a Chinese locale), which turns UTF-8 Chinese comments
# into garbage and can even swallow line breaks. Make sure the scripts we
# are about to call carry a UTF-8 BOM.
function Ensure-Utf8Bom([string]$Path) {
    if (-not (Test-Path $Path)) { return }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
        $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        return
    }
    $text = [System.IO.File]::ReadAllText($Path, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($Path, $text, [System.Text.UTF8Encoding]::new($true))
    Write-Host "  (added UTF-8 BOM to $(Split-Path $Path -Leaf))" -ForegroundColor DarkGray
}

Ensure-Utf8Bom (Join-Path $root "build.ps1")
Ensure-Utf8Bom (Join-Path $root "tools\mkfs.ps1")
Ensure-Utf8Bom (Join-Path $root "tools\vm-type.ps1")

function Invoke-VBox {
    param([string[]]$Arguments)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $vbox @Arguments 2>&1
    $ErrorActionPreference = $saved
    return $out
}

# --- stop ---------------------------------------------------
if ($Stop) {
    $running = Invoke-VBox @("list", "runningvms")
    if ($running -match [regex]::Escape($VmName)) {
        Invoke-VBox @("controlvm", $VmName, "poweroff") | Out-Null
        Start-Sleep -Seconds 2
        Write-Host "VM '$VmName' powered off." -ForegroundColor Green
    } else {
        Write-Host "VM '$VmName' is not running." -ForegroundColor DarkGray
    }
    exit 0
}

# --- build --------------------------------------------------
if (-not $NoBuild) {
    & (Join-Path $root "build.ps1") -VmName $VmName
}

# --- make sure the VM is stopped ----------------------------
# 必须在删除串口日志之前关机：日志文件被虚拟机进程占用时删不掉
$running = Invoke-VBox @("list", "runningvms")
if ($running -match [regex]::Escape($VmName)) {
    Invoke-VBox @("controlvm", $VmName, "poweroff") | Out-Null
    Start-Sleep -Seconds 2
}

# --- serial port -> file ------------------------------------
if (Test-Path $log) { Remove-Item $log -Force }
Invoke-VBox @("modifyvm", $VmName, "--uart1", "0x3F8", "4",
              "--uartmode1", "file", $log) | Out-Null

# --- boot ---------------------------------------------------
Write-Host ""
Write-Host "Booting '$VmName' headless..." -ForegroundColor Cyan
Invoke-VBox @("startvm", $VmName, "--type", "headless") | Out-Null
Start-Sleep -Seconds $WaitSeconds

# --- optional keystrokes ------------------------------------
if ($Type.Count -gt 0) {
    # 刚开机时键盘控制器可能还在初始化（鼠标驱动也会往 8042 发命令），
    # 立刻注入按键会丢掉头一两个字符，所以先稳一下
    Start-Sleep -Milliseconds 900

    foreach ($text in $Type) {
        Write-Host "  typing: $text" -ForegroundColor Cyan
        & (Join-Path $root "tools\vm-type.ps1") -VmName $VmName -Text $text -Enter
        Start-Sleep -Milliseconds 700
    }
}

# --- show the screen ----------------------------------------
Write-Host ""
if (Test-Path $log) {
    Write-Host "==================== screen (via COM1) ====================" -ForegroundColor Green
    Get-Content $log
    Write-Host "===========================================================" -ForegroundColor Green
} else {
    Write-Host "No serial output captured - did the VM boot?" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "The VM is still running in headless mode." -ForegroundColor DarkGray
Write-Host "  open the VirtualBox GUI to interact with it, or run:" -ForegroundColor DarkGray
Write-Host "    .\run.ps1 -Stop" -ForegroundColor DarkGray
