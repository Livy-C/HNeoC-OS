# ============================================================
#  mklxe.ps1 - wrap a flat binary into a HNeoC OS executable
#
#  Adds the 40-byte LXE header described in include/lxe.h and writes
#  <header><code> to the output file.
#
#  NOTE: ASCII-only on purpose - Windows PowerShell 5.1 reads BOM-less
#  .ps1 files as GBK on a Chinese locale and mangles non-ASCII bytes.
#
#  Usage:
#    .\mklxe.ps1 -InputFile build\user\hello.bin -OutputFile fsroot\hello.lxe `
#                -Name hello -BssSize 32
# ============================================================

param(
    [Parameter(Mandatory = $true)][string]$InputFile,
    [Parameter(Mandatory = $true)][string]$OutputFile,
    [string]$Name = "",
    [uint32]$Entry = 0,
    [uint32]$BssSize = 0,
    [uint32]$Flags = 0,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

$LXE_MAGIC   = [uint32]0x0045584C     # 'LXE\0'
$LXE_VERSION = [uint32]1
$HEADER_SIZE = 40
$NAME_MAX    = 16

if (-not (Test-Path $InputFile)) { throw "input not found: $InputFile" }

$code = [System.IO.File]::ReadAllBytes($InputFile)

$hdr = New-Object byte[] $HEADER_SIZE
[BitConverter]::GetBytes($LXE_MAGIC).CopyTo($hdr, 0)
[BitConverter]::GetBytes($LXE_VERSION).CopyTo($hdr, 4)
[BitConverter]::GetBytes([uint32]$Entry).CopyTo($hdr, 8)
[BitConverter]::GetBytes([uint32]$code.Length).CopyTo($hdr, 12)
[BitConverter]::GetBytes([uint32]$BssSize).CopyTo($hdr, 16)
[BitConverter]::GetBytes([uint32]$Flags).CopyTo($hdr, 20)

# name[16], always NUL terminated
$nameBytes = [System.Text.Encoding]::ASCII.GetBytes($Name)
$n = [math]::Min($nameBytes.Length, $NAME_MAX - 1)
if ($n -gt 0) { [Array]::Copy($nameBytes, 0, $hdr, 24, $n) }

$out = New-Object byte[] ($HEADER_SIZE + $code.Length)
[Array]::Copy($hdr, 0, $out, 0, $HEADER_SIZE)
[Array]::Copy($code, 0, $out, $HEADER_SIZE, $code.Length)

$dir = Split-Path $OutputFile -Parent
if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

[System.IO.File]::WriteAllBytes($OutputFile, $out)

if (-not $Quiet) {
    Write-Host ("    LXE  {0,-12} code {1,6} B + bss {2,5} B -> {3} B" -f `
        $Name, $code.Length, $BssSize, $out.Length) -ForegroundColor Green
}
