# Scrollback test: generate lots of output, then scroll back.
param(
    [string]$VmName = "HNeoC"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot | Split-Path
$log  = Join-Path $root "build\serial.log"
$type = Join-Path $root "tools\vm-type.ps1"

& (Join-Path $root "build.ps1") -VmName $VmName | Out-Null

# drop the old serial log
$vbox = $null
foreach ($c in @($env:VBOX_MSI_INSTALL_PATH, "D:\VB",
                 "C:\Program Files\Oracle\VirtualBox")) {
    if ($c -and (Test-Path (Join-Path $c "VBoxManage.exe"))) {
        $vbox = Join-Path $c "VBoxManage.exe"; break
    }
}
$saved = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $vbox controlvm $VmName poweroff 2>&1 | Out-Null
Start-Sleep -Seconds 2
if (Test-Path $log) { Remove-Item $log -Force }
& $vbox modifyvm $VmName --uart1 0x3F8 4 --uartmode1 file $log 2>&1 | Out-Null
& $vbox startvm $VmName --type headless 2>&1 | Out-Null
$ErrorActionPreference = $saved

Start-Sleep -Seconds 7

# generate enough output to push lines into the scrollback
foreach ($cmd in @("mem", "mem", "help")) {
    & $type -VmName $VmName -Text $cmd -Enter
    Start-Sleep -Milliseconds 900
}

Write-Host ""
Write-Host "=== tail of the serial log before scrolling ===" -ForegroundColor Cyan
Get-Content $log -Tail 4

# PageUp three times
Write-Host ""
Write-Host "=== PageUp x3 ===" -ForegroundColor Cyan
for ($i = 0; $i -lt 3; $i++) {
    & $vbox controlvm $VmName keyboardputscancode E0 49 E0 C9 2>&1 | Out-Null
    Start-Sleep -Milliseconds 400
}

# Home jumps to the oldest line
Write-Host "=== Home (oldest) ===" -ForegroundColor Cyan
& $vbox controlvm $VmName keyboardputscancode E0 47 E0 C7 2>&1 | Out-Null
Start-Sleep -Milliseconds 400

# End returns to the live screen
Write-Host "=== End (back to live) ===" -ForegroundColor Cyan
& $vbox controlvm $VmName keyboardputscancode E0 4F E0 CF 2>&1 | Out-Null
Start-Sleep -Milliseconds 500

# PageDown one screen
Write-Host "=== PageDown ===" -ForegroundColor Cyan
& $vbox controlvm $VmName keyboardputscancode E0 51 E0 D1 2>&1 | Out-Null
Start-Sleep -Milliseconds 500

Write-Host ""
Write-Host "=== scroll trace ===" -ForegroundColor Green
Get-Content $log | Select-String -Pattern "\[scroll\]"
