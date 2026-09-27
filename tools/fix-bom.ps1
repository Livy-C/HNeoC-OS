param(
    [switch]$Quiet
)

# ============================================================
#  fix-bom.ps1 - make sure the .ps1 files that contain Chinese
#  are stored as UTF-8 **with** BOM.
#
#  Why this exists: Windows PowerShell 5.1 reads a BOM-less .ps1
#  as GBK on a Chinese locale, so every Chinese comment/string turns
#  into mojibake and the script usually fails to parse at all.
#  Editing tools that write UTF-8 without a BOM (many editors, and
#  some agent workflows) silently reintroduce the problem, so this
#  check is worth running after touching any script.
#
#  Usage:
#    .\tools\fix-bom.ps1            # fix every .ps1 in the tree
#    .\tools\fix-bom.ps1 -Quiet     # only report what changed
#
#  ASCII-only on purpose (see README trap 14 - this file itself
#  must not depend on the BOM it is fixing).
# ============================================================

$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$utf8Bom   = New-Object System.Text.UTF8Encoding($true)

$fixed   = 0
$checked = 0

foreach ($file in (Get-ChildItem -Path $root -Filter *.ps1 -Recurse -File)) {
    # build/ holds generated copies; nothing there is a source file
    if ($file.FullName -like "*\build\*") { continue }
    $checked++

    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
               $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)

    # Does the file actually contain non-ASCII bytes? Pure ASCII files do not
    # need a BOM (and mklxe.ps1 / mkhnpkg.ps1 are deliberately ASCII-only).
    $nonAscii = $false
    foreach ($b in $bytes) {
        if ($b -gt 0x7F) { $nonAscii = $true; break }
    }

    if ($hasBom) {
        if (-not $Quiet) { Write-Host ("  ok      " + $file.Name) -ForegroundColor DarkGray }
        continue
    }
    if (-not $nonAscii) {
        if (-not $Quiet) { Write-Host ("  ascii   " + $file.Name) -ForegroundColor DarkGray }
        continue
    }

    $text = $utf8NoBom.GetString($bytes)
    [System.IO.File]::WriteAllText($file.FullName, $text, $utf8Bom)
    Write-Host ("  fixed   " + $file.Name) -ForegroundColor Yellow
    $fixed++
}

Write-Host ""
if ($fixed -gt 0) {
    Write-Host "  $fixed file(s) got a UTF-8 BOM back (out of $checked checked)" -ForegroundColor Green
} else {
    Write-Host "  all $checked script(s) are fine" -ForegroundColor Green
}
