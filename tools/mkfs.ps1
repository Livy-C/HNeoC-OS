# ============================================================
#  mkfs.ps1 - write an HNeoFS filesystem into a disk image
#
#  Layout (see include/hneofs.h):
#    LBA 2048        superblock (512 bytes)
#    LBA 2049-2056   file table (8 sectors = 64 entries of 64 bytes)
#    LBA 2057-...    file data, files packed back to back
#
#  The source directory is walked recursively and turned into a tree:
#  every subdirectory becomes a directory entry, every file a file entry,
#  and each entry's "parent" field points at its containing directory.
#  The root itself has no entry; its children use parent = HNEOFS_ROOT.
#
#  Files whose extension is .lxe get the EXEC flag.
#
#  NOTE: ASCII-only on purpose - Windows PowerShell 5.1 reads BOM-less
#  .ps1 files as GBK on a Chinese locale and mangles non-ASCII bytes.
#
#  Usage: .\mkfs.ps1 -Image build\hneoc-os.img -SourceDir fsroot
# ============================================================

param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [string]$Label = "HNeoFS",
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

$MAGIC          = [uint32]0x53464E48     # 'HNFS'
$VERSION        = [uint32]2
$BLOCK          = 512
$FS_START_LBA   = 2048
$TABLE_LBA      = $FS_START_LBA + 1
$TABLE_SECTORS  = 8
$DATA_LBA       = $TABLE_LBA + $TABLE_SECTORS
$NAME_MAX       = 32
$ENTRY_SIZE     = 64
$MAX_FILES      = ($TABLE_SECTORS * $BLOCK) / $ENTRY_SIZE

$ROOT      = [uint32]::MaxValue    # 0xFFFFFFFF：根目录没有自己的目录项
$TYPE_FILE = [uint32]0
$TYPE_DIR  = [uint32]1

$FLAG_EXEC = [uint32]0x0001
$FLAG_TEXT = [uint32]0x0002

if (-not (Test-Path $Image))     { throw "image not found: $Image" }
if (-not (Test-Path $SourceDir)) { throw "source directory not found: $SourceDir" }

$imageInfo = Get-Item $Image
$totalSectors = [int]($imageInfo.Length / $BLOCK)

# --- walk the tree ------------------------------------------
# Entry indices are the order in which we add them. A directory's index is
# known before recursing into it, so children can point back at it.
$entries = New-Object System.Collections.ArrayList

function Add-Tree {
    param([string]$Path, [uint32]$Parent)

    # directories first, then files, each alphabetically
    $items = Get-ChildItem -LiteralPath $Path |
             Sort-Object { $_.PSIsContainer -eq $false }, Name

    foreach ($item in $items) {
        if ($item.Name.Length -ge $NAME_MAX) {
            throw "name too long (max $($NAME_MAX - 1)): $($item.Name)"
        }
        if ($item.Name.Contains("/")) {
            throw "name contains a slash: $($item.Name)"
        }
        if ($entries.Count -ge $MAX_FILES) {
            throw "too many entries (limit is $MAX_FILES)"
        }

        if ($item.PSIsContainer) {
            $idx = [uint32]$entries.Count
            [void]$entries.Add([pscustomobject]@{
                Name   = $item.Name
                Type   = $TYPE_DIR
                Parent = $Parent
                Full   = $item.FullName
                Size   = 0
                Data   = $null
            })
            Add-Tree -Path $item.FullName -Parent $idx
        } else {
            [void]$entries.Add([pscustomobject]@{
                Name   = $item.Name
                Type   = $TYPE_FILE
                Parent = $Parent
                Full   = $item.FullName
                Size   = $item.Length
                Data   = [System.IO.File]::ReadAllBytes($item.FullName)
            })
        }
    }
}

Add-Tree -Path $SourceDir -Parent $ROOT

# --- assign disk space to files -----------------------------
$table = New-Object byte[] ($TABLE_SECTORS * $BLOCK)
$nextLba = $DATA_LBA

for ($i = 0; $i -lt $entries.Count; $i++) {
    $e = $entries[$i]
    $base = $i * $ENTRY_SIZE

    $nameBytes = [System.Text.Encoding]::ASCII.GetBytes($e.Name)
    [Array]::Copy($nameBytes, 0, $table, $base, $nameBytes.Length)

    $lba   = [uint32]0
    $flags = [uint32]0
    $size  = [uint32]0

    if ($e.Type -eq $TYPE_FILE) {
        $sectors = [int][math]::Ceiling($e.Data.Length / $BLOCK)
        if ($sectors -eq 0) { $sectors = 1 }   # an empty file still gets a sector

        if (($nextLba + $sectors) -gt $totalSectors) {
            throw "image is full while packing $($e.Name)"
        }
        $lba  = [uint32]$nextLba
        $size = [uint32]$e.Data.Length

        $e | Add-Member -NotePropertyName Lba     -NotePropertyValue $nextLba -Force
        $e | Add-Member -NotePropertyName Sectors -NotePropertyValue $sectors -Force
        $nextLba += $sectors
    }

    if ($e.Full -like "*.lxe") { $flags = $flags -bor $FLAG_EXEC }

    [BitConverter]::GetBytes($lba).CopyTo($table, $base + 32)
    [BitConverter]::GetBytes($size).CopyTo($table, $base + 36)
    [BitConverter]::GetBytes($flags).CopyTo($table, $base + 40)
    [BitConverter]::GetBytes([uint32]0).CopyTo($table, $base + 44)   # entry_offset
    [BitConverter]::GetBytes([uint32]$e.Type).CopyTo($table, $base + 48)
    [BitConverter]::GetBytes([uint32]$e.Parent).CopyTo($table, $base + 52)
}

# --- build the superblock -----------------------------------
$sb = New-Object byte[] $BLOCK
[BitConverter]::GetBytes($MAGIC).CopyTo($sb, 0)
[BitConverter]::GetBytes($VERSION).CopyTo($sb, 4)
[BitConverter]::GetBytes([uint32]$BLOCK).CopyTo($sb, 8)
[BitConverter]::GetBytes([uint32]($totalSectors - $FS_START_LBA)).CopyTo($sb, 12)
[BitConverter]::GetBytes([uint32]$entries.Count).CopyTo($sb, 16)
[BitConverter]::GetBytes([uint32]$TABLE_LBA).CopyTo($sb, 20)
[BitConverter]::GetBytes([uint32]$DATA_LBA).CopyTo($sb, 24)
[BitConverter]::GetBytes([uint32]$nextLba).CopyTo($sb, 28)

$labelBytes = [System.Text.Encoding]::ASCII.GetBytes($Label)
$len = [math]::Min($labelBytes.Length, 31)
[Array]::Copy($labelBytes, 0, $sb, 32, $len)

# --- write everything into the image ------------------------
$stream = [System.IO.File]::Open($Image, [System.IO.FileMode]::Open,
                                        [System.IO.FileAccess]::ReadWrite)
try {
    $stream.Seek([int64]$FS_START_LBA * $BLOCK, 'Begin') | Out-Null
    $stream.Write($sb, 0, $BLOCK)

    $stream.Seek([int64]$TABLE_LBA * $BLOCK, 'Begin') | Out-Null
    $stream.Write($table, 0, $table.Length)

    foreach ($e in $entries) {
        if ($e.Type -ne $TYPE_FILE) { continue }

        $padded = New-Object byte[] ($e.Sectors * $BLOCK)
        [Array]::Copy($e.Data, 0, $padded, 0, $e.Data.Length)

        $stream.Seek([int64]$e.Lba * $BLOCK, 'Begin') | Out-Null
        $stream.Write($padded, 0, $padded.Length)
    }
}
finally {
    $stream.Close()
}

if (-not $Quiet) {
    Write-Host "  HNeoFS packed into $([System.IO.Path]::GetFileName($Image))" -ForegroundColor Green
    Write-Host "    label      : $Label"
    Write-Host "    entries    : $($entries.Count)"
    Write-Host "    data start : LBA $DATA_LBA"
    Write-Host "    free start : LBA $nextLba"

    # print the tree so it is easy to eyeball
    $byParent = @{}
    for ($i = 0; $i -lt $entries.Count; $i++) {
        $p = [uint32]$entries[$i].Parent
        if (-not $byParent.ContainsKey($p)) { $byParent[$p] = New-Object System.Collections.ArrayList }
        [void]$byParent[$p].Add($i)
    }

    function Show-Tree([uint32]$Parent, [string]$Indent) {
        if (-not $byParent.ContainsKey($Parent)) { return }
        foreach ($i in $byParent[$Parent]) {
            $e = $entries[$i]
            if ($e.Type -eq $TYPE_DIR) {
                Write-Host ("      {0}{1}/" -f $Indent, $e.Name)
                Show-Tree -Parent ([uint32]$i) -Indent ($Indent + "  ")
            } else {
                Write-Host ("      {0}{1,-16} {2,7} B" -f $Indent, $e.Name, $e.Size)
            }
        }
    }
    Show-Tree -Parent $ROOT -Indent ""
}
