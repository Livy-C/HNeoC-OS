# ============================================================
#  mkhnpkg.ps1 - pack a package directory into a .hnpkg file
#
#  A .hnpkg file is one file: a 256-byte header, a file table of
#  64-byte entries, and the file contents back to back. The layout
#  is defined in user/lib/include/hnpkg.h and must be kept in sync
#  with it - the on-OS side (user/hpm.c) parses exactly this.
#
#    [0           .. 255]   header
#    [256         ..    ]   file table, HNPKG_MAX_FILES entries
#    [data_offset ..    ]   file data, table entries point into it
#
#  Input layout of a package directory:
#
#    packages\<name>\manifest.txt    key: value lines
#    packages\<name>\src\*.c         compiled by build.ps1 into -BinDir
#    packages\<name>\files\...       installed at the same relative path
#
#  Entries are sorted (bin/ first, then the rest, each alphabetically
#  by install path, ordinal comparison) so that packing the same input
#  twice produces byte-identical output.
#
#  NOTE: ASCII-only on purpose - Windows PowerShell 5.1 reads BOM-less
#  .ps1 files as GBK on a Chinese locale and mangles non-ASCII bytes.
#
#  Usage:
#    .\tools\mkhnpkg.ps1 -PackageDir packages\hello `
#        -OutputFile fsroot\var\hpm\repo\hello-1.0.hnpkg `
#        -BinDir build\packages\hello
# ============================================================

param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [Parameter(Mandatory = $true)][string]$OutputFile,
    [string]$BinDir = "",
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

# --- format constants (must match user/lib/include/hnpkg.h) ---
$HNPKG_MAGIC       = [uint32]0x4B504E48     # 'HNPK'
$HNPKG_VERSION     = [uint32]1
$HEADER_SIZE       = 256
$ENTRY_SIZE        = 64
$PATH_MAX          = 48
$NAME_MAX          = 32
$VER_MAX           = 16
$DEPENDS_MAX       = 48
$SUMMARY_MAX       = 96
$MAX_FILES         = 32
$FLAG_ADMIN        = [uint32]0x0001

$MODE_EXEC         = [uint32]0x1ED          # 0755, owner rwx / other rx
$MODE_DATA         = [uint32]0x1A4          # 0644, owner rw  / other r

# Three output levels, matching build.ps1 / mkfs.ps1: a step line, an
# "OK" result line, and the raw detail lines in between. Everything is
# silenced by -Quiet.
function Write-Step($text) {
    if (-not $Quiet) { Write-Host "  $text" -ForegroundColor Cyan }
}
function Write-Ok($text) {
    if (-not $Quiet) { Write-Host "    OK  $text" -ForegroundColor Green }
}
function Write-Detail($text) {
    if (-not $Quiet) { Write-Host "        $text" }
}

# Copy an ASCII string into a fixed-size header field, NUL terminated
# by the zero-filled buffer. Anything that does not fit is an error:
# silently truncating gives a package that installs under a wrong name.
function Copy-AsciiField {
    param(
        [byte[]]$Buffer,
        [int]$Offset,
        [int]$FieldSize,
        [string]$Text,
        [string]$What
    )

    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Text)

    # Every character must be one byte, otherwise the byte count and the
    # on-screen length disagree and the length limits below lie.
    if ($bytes.Length -ne $Text.Length) {
        throw "$What contains a non-ASCII character: '$Text'"
    }
    if ($bytes.Length -ge $FieldSize) {
        throw "$What is too long: max $($FieldSize - 1) characters, got $($bytes.Length): '$Text'"
    }
    if ($bytes.Length -gt 0) {
        [Array]::Copy($bytes, 0, $Buffer, $Offset, $bytes.Length)
    }
}

# Read manifest.txt: "key: value" lines, '#' comments, blank lines ok.
function Read-Manifest {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "manifest not found: $Path"
    }

    $map = @{}
    $lines = [System.IO.File]::ReadAllLines($Path)

    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i].Trim()

        if ($line -eq "" -or $line.StartsWith("#")) { continue }

        $colon = $line.IndexOf(":")
        if ($colon -lt 1) {
            throw "$Path line $($i + 1): expected 'key: value', got '$line'"
        }

        $key = $line.Substring(0, $colon).Trim().ToLower()
        $val = $line.Substring($colon + 1).Trim()
        $map[$key] = $val
    }

    return $map
}

# Insertion sort on the ordinal comparison of "<group>/<path>".
# Sort-Object compares with the current culture, which can order two
# paths differently on two machines; CompareOrdinal cannot.
function Sort-Entries {
    param([object[]]$Items)

    $list = New-Object System.Collections.ArrayList
    foreach ($it in $Items) { [void]$list.Add($it) }

    for ($i = 1; $i -lt $list.Count; $i++) {
        $cur = $list[$i]
        $curKey = "$($cur.Group)/$($cur.Path)"
        $j = $i - 1

        while ($j -ge 0) {
            $prevKey = "$($list[$j].Group)/$($list[$j].Path)"
            if ([string]::CompareOrdinal($prevKey, $curKey) -le 0) { break }
            $list[$j + 1] = $list[$j]
            $j--
        }
        $list[$j + 1] = $cur
    }

    return $list
}

# --- read the manifest ---------------------------------------
if (-not (Test-Path -LiteralPath $PackageDir)) {
    throw "package directory not found: $PackageDir"
}

$manifestPath = Join-Path $PackageDir "manifest.txt"
$manifest = Read-Manifest -Path $manifestPath

$name    = ""
$version = ""
$depends = ""
$summary = ""
$admin   = [uint32]0

if ($manifest.ContainsKey("name"))    { $name    = $manifest["name"] }
if ($manifest.ContainsKey("version")) { $version = $manifest["version"] }
if ($manifest.ContainsKey("depends")) { $depends = $manifest["depends"] }
if ($manifest.ContainsKey("summary")) { $summary = $manifest["summary"] }

if ($manifest.ContainsKey("admin")) {
    $raw = $manifest["admin"].ToLower()
    if ($raw -eq "1" -or $raw -eq "true" -or $raw -eq "yes") {
        $admin = $FLAG_ADMIN
    } elseif ($raw -eq "0" -or $raw -eq "false" -or $raw -eq "no" -or $raw -eq "") {
        $admin = [uint32]0
    } else {
        throw "$manifestPath : admin must be 0 or 1, got '$($manifest['admin'])'"
    }
}

if ($name -eq "")    { throw "$manifestPath : 'name' is empty" }
if ($version -eq "") { throw "$manifestPath : 'version' is empty" }

# Validate the header strings early, so the failure message names the
# field instead of blowing up while the header is being written.
$probe = New-Object byte[] $HEADER_SIZE
Copy-AsciiField -Buffer $probe -Offset 8   -FieldSize $NAME_MAX    -Text $name    -What "package name"
Copy-AsciiField -Buffer $probe -Offset 40  -FieldSize $VER_MAX     -Text $version -What "version"
Copy-AsciiField -Buffer $probe -Offset 56  -FieldSize $DEPENDS_MAX -Text $depends -What "depends"
Copy-AsciiField -Buffer $probe -Offset 104 -FieldSize $SUMMARY_MAX -Text $summary -What "summary"

# --- collect the file list -----------------------------------
# group "0" sorts before group "1": bin/ entries first, then the rest.
$entries = New-Object System.Collections.ArrayList

if ($BinDir -ne "") {
    if (Test-Path -LiteralPath $BinDir) {
        $binFiles = @(Get-ChildItem -LiteralPath $BinDir -Filter *.lxe -File |
                      Sort-Object Name)
        foreach ($f in $binFiles) {
            [void]$entries.Add([pscustomobject]@{
                Path   = "bin/" + $f.BaseName + ".lxe"
                Group  = "0"
                Mode   = $MODE_EXEC
                Source = $f.FullName
            })
        }
        Write-Step "bin/: $($binFiles.Count) executable(s) from $BinDir"
    } else {
        # Not an error: a package with no C sources has no -BinDir output,
        # and build.ps1 may run this before compiling anything.
        Write-Step "bin/: $BinDir does not exist, skipped"
    }
}

$filesRoot = Join-Path $PackageDir "files"
if (Test-Path -LiteralPath $filesRoot) {
    $dataFiles = @(Get-ChildItem -LiteralPath $filesRoot -Recurse -File |
                   Sort-Object FullName)

    foreach ($f in $dataFiles) {
        $rel = $f.FullName.Substring($filesRoot.Length).TrimStart("\", "/")
        $rel = $rel -replace "\\", "/"

        $mode = $MODE_DATA
        if ($rel -like "*.lxe") { $mode = $MODE_EXEC }

        [void]$entries.Add([pscustomobject]@{
            Path   = $rel
            Group  = "1"
            Mode   = $mode
            Source = $f.FullName
        })
    }
    Write-Step "files/: $($dataFiles.Count) file(s)"
}

if ($entries.Count -eq 0) {
    throw "package '$name' has no files: nothing in $(Join-Path $PackageDir 'files') and no .lxe in '$BinDir'"
}
if ($entries.Count -gt $MAX_FILES) {
    throw "package '$name' has $($entries.Count) files, the format allows at most $MAX_FILES"
}

$sorted = @(Sort-Entries -Items @($entries))

# --- validate paths and build the table ----------------------
$seen = @{}
$dataOffset = $HEADER_SIZE + ($ENTRY_SIZE * $sorted.Count)
$table = New-Object byte[] ($ENTRY_SIZE * $sorted.Count)
$dataLen = 0

for ($i = 0; $i -lt $sorted.Count; $i++) {
    $e = $sorted[$i]

    if ($e.Path.Length -ge $PATH_MAX) {
        throw "install path too long (max $($PATH_MAX - 1) characters): '$($e.Path)' from $($e.Source)"
    }
    if ($e.Path.StartsWith("/")) {
        throw "install path must be relative to '/', got '$($e.Path)'"
    }
    if ($seen.ContainsKey($e.Path)) {
        throw "two files would install to the same path: '$($e.Path)'"
    }
    $seen[$e.Path] = $true

    if (-not (Test-Path -LiteralPath $e.Source)) {
        throw "file disappeared while packing: $($e.Source)"
    }
    $size = [uint32](Get-Item -LiteralPath $e.Source).Length

    $base = $i * $ENTRY_SIZE
    Copy-AsciiField -Buffer $table -Offset $base -FieldSize $PATH_MAX -Text $e.Path -What "install path"
    [BitConverter]::GetBytes([uint32]$dataLen).CopyTo($table, $base + 48)
    [BitConverter]::GetBytes($size).CopyTo($table, $base + 52)
    [BitConverter]::GetBytes([uint32]$e.Mode).CopyTo($table, $base + 56)
    [BitConverter]::GetBytes([uint32]0).CopyTo($table, $base + 60)      # reserved

    $e | Add-Member -NotePropertyName Offset -NotePropertyValue $dataLen -Force
    $e | Add-Member -NotePropertyName Size   -NotePropertyValue $size    -Force

    $dataLen += $size
}

$totalSize = $dataOffset + $dataLen

# --- build the header ----------------------------------------
$header = New-Object byte[] $HEADER_SIZE
[BitConverter]::GetBytes([uint32]$HNPKG_MAGIC).CopyTo($header, 0)
[BitConverter]::GetBytes([uint32]$HNPKG_VERSION).CopyTo($header, 4)
Copy-AsciiField -Buffer $header -Offset 8   -FieldSize $NAME_MAX    -Text $name    -What "package name"
Copy-AsciiField -Buffer $header -Offset 40  -FieldSize $VER_MAX     -Text $version -What "version"
Copy-AsciiField -Buffer $header -Offset 56  -FieldSize $DEPENDS_MAX -Text $depends -What "depends"
Copy-AsciiField -Buffer $header -Offset 104 -FieldSize $SUMMARY_MAX -Text $summary -What "summary"
[BitConverter]::GetBytes([uint32]$sorted.Count).CopyTo($header, 200)
[BitConverter]::GetBytes([uint32]$dataOffset).CopyTo($header, 204)
[BitConverter]::GetBytes([uint32]$totalSize).CopyTo($header, 208)
[BitConverter]::GetBytes([uint32]$admin).CopyTo($header, 212)
[BitConverter]::GetBytes([uint32]$HEADER_SIZE).CopyTo($header, 216)     # table_offset
# bytes 220..255 are reserved and stay zero

# --- write header, table, data -------------------------------
$outDir = Split-Path $OutputFile -Parent
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

$stream = [System.IO.File]::Open($OutputFile, [System.IO.FileMode]::Create,
                                           [System.IO.FileAccess]::Write)
try {
    $stream.Write($header, 0, $header.Length)
    $stream.Write($table, 0, $table.Length)

    # Table order is the data order, so this is one sequential pass.
    foreach ($e in $sorted) {
        $bytes = [System.IO.File]::ReadAllBytes($e.Source)
        if ($bytes.Length -ne $e.Size) {
            throw "file changed while packing: $($e.Source)"
        }
        $stream.Write($bytes, 0, $bytes.Length)
    }
}
finally {
    $stream.Close()
}

$written = (Get-Item -LiteralPath $OutputFile).Length
if ($written -ne $totalSize) {
    throw "wrote $written bytes but the header says $totalSize"
}

# --- report ---------------------------------------------------
if (-not $Quiet) {
    Write-Host ("  HNPKG packed into {0}" -f [System.IO.Path]::GetFileName($OutputFile)) -ForegroundColor Green

    Write-Ok ("name      : {0} {1}" -f $name, $version)
    if ($depends -eq "") {
        Write-Ok "depends   : (none)"
    } else {
        Write-Ok ("depends   : {0}" -f $depends)
    }
    Write-Ok ("summary   : {0}" -f $summary)
    if ($admin -eq $FLAG_ADMIN) {
        Write-Ok "flags     : admin only"
    } else {
        Write-Ok "flags     : 0"
    }
    Write-Ok ("files     : {0}" -f $sorted.Count)
    Write-Ok ("data      : offset {0}, {1} bytes" -f $dataOffset, $dataLen)
    Write-Ok ("total     : {0} bytes" -f $totalSize)

    Write-Host ""
    foreach ($e in $sorted) {
        $modeText = "0644"
        if ($e.Mode -eq $MODE_EXEC) { $modeText = "0755" }
        Write-Detail ("{0,-46} {1,7} B  {2}" -f $e.Path, $e.Size, $modeText)
    }
}
