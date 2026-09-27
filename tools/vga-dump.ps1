# Extract the VGA text-mode buffer from a VirtualBox vmcore dump.
#
# VirtualBox's "debugvm <vm> dumpvmcore" writes a 64-bit ELF core file.
# Each PT_LOAD segment maps a guest physical address range to a file offset.
# The VGA text buffer lives at physical 0xB8000 (80x25 cells of 2 bytes).
#
# Usage: .\vga-dump.ps1 -CoreFile .\build\vmcore.elf

param(
    [Parameter(Mandatory = $true)][string]$CoreFile,
    [int]$At = 0xB8000,
    [int]$Width = 80,
    [int]$Height = 25
)

$ErrorActionPreference = "Stop"

$fs = [System.IO.File]::OpenRead($CoreFile)
$br = New-Object System.IO.BinaryReader($fs)

try {
    $magic = $br.ReadBytes(4)
    if ($magic[0] -ne 0x7F -or $magic[1] -ne 0x45 -or $magic[2] -ne 0x4C -or $magic[3] -ne 0x46) {
        throw "not an ELF file"
    }

    $fs.Seek(0x04, 'Begin') | Out-Null
    $eiClass = $br.ReadByte()
    if ($eiClass -ne 2) { throw "expected a 64-bit ELF core, got class $eiClass" }

    # ELF64 header fields
    $fs.Seek(0x20, 'Begin') | Out-Null
    $phoff = $br.ReadUInt64()
    $fs.Seek(0x36, 'Begin') | Out-Null
    $phentsize = $br.ReadUInt16()
    $phnum = $br.ReadUInt16()

    # Locate the PT_LOAD segment covering the target physical address
    $found = $null
    for ($i = 0; $i -lt $phnum; $i++) {
        $fs.Seek([int64]$phoff + $i * $phentsize, 'Begin') | Out-Null
        $p_type   = $br.ReadUInt32()
        $p_flags  = $br.ReadUInt32()
        $p_offset = $br.ReadUInt64()
        $p_vaddr  = $br.ReadUInt64()
        $p_paddr  = $br.ReadUInt64()
        $p_filesz = $br.ReadUInt64()
        $p_memsz  = $br.ReadUInt64()

        if ($p_type -eq 1 -and $At -ge $p_paddr -and $At -lt ($p_paddr + $p_memsz)) {
            $found = @{ Offset = $p_offset; Paddr = $p_paddr }
            break
        }
    }

    if (-not $found) { throw ("no PT_LOAD segment covers physical address 0x{0:X}" -f $At) }

    $delta = $At - $found.Paddr
    $fs.Seek([int64]$found.Offset + $delta, 'Begin') | Out-Null

    $lines = @()
    for ($y = 0; $y -lt $Height; $y++) {
        $sb = New-Object System.Text.StringBuilder
        for ($x = 0; $x -lt $Width; $x++) {
            $ch = $br.ReadByte()
            [void]$br.ReadByte()          # attribute byte (colours), not needed here
            if ($ch -lt 32 -or $ch -gt 126) { $ch = 32 }
            [void]$sb.Append([char]$ch)
        }
        $lines += $sb.ToString().TrimEnd()
    }

    $last = $lines.Count - 1
    while ($last -ge 0 -and $lines[$last] -eq '') { $last-- }

    Write-Host ("+" + ("-" * $Width) + "+")
    for ($y = 0; $y -le $last; $y++) {
        Write-Host ("|" + $lines[$y].PadRight($Width) + "|")
    }
    Write-Host ("+" + ("-" * $Width) + "+")
    Write-Host ""
    Write-Host ("non-empty lines: {0} / {1}" -f ($last + 1), $Height) -ForegroundColor DarkGray
}
finally {
    $br.Close()
    $fs.Close()
}
