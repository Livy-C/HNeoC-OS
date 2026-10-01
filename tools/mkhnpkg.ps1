# ============================================================
#  mkhnpkg.ps1 - pack a package directory into a .hnpkg file
#
#  A .hnpkg file is one file: a 256-byte header, a file table of
#  64-byte entries, and the file contents back to back. The layout
#  is defined in user/lib/include/hnpkg.h and must be kept in sync
#  with it - the on-OS side (user/hpm.c) parses exactly this.
#
#    [0           .. 255]   header
#    [256         ..    ]   file table, file_count entries (at most
#                           HNPKG_MAX_FILES), 64 bytes each
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
$RESERVED_SIZE     = 36                     # bytes 220..255, stay zero
$FLAG_ADMIN        = [uint32]0x0001

# HNeoFS caps a single directory-entry name at HNEOFS_NAME_MAX - 1 = 31
# characters (include/hneofs.h:34, enforced by tools/mkfs.ps1). The package
# header allows name(31) + version(15), so the file name build.ps1 derives
# from them needs its own check - see the check below the manifest.
$HNEOFS_NAME_MAX   = 32

$MODE_EXEC         = [uint32]0x1ED          # 0755, owner rwx / other rx
$MODE_DATA         = [uint32]0x1A4          # 0644, owner rw  / other r

# --- byte offsets inside the header, derived from the sizes above -------
#
# Never write these as literals: they are arithmetic on $NAME_MAX/$VER_MAX/...
# so that changing a limit moves every offset after it.
#
#   0 magic(4) | 4 version(4) | 8 name | +NAME_MAX version | +VER_MAX depends
#   | +DEPENDS_MAX summary | +SUMMARY_MAX file_count(4) data_offset(4)
#   total_size(4) flags(4) table_offset(4) | reserved to 256
#
# NOTE: build.ps1 reads a packed header back with its **own** copy of these
# offsets ([BitConverter]::ToUInt32($hdr, 200) and friends, right after it
# runs this script). That check is self-consistent by construction: it catches
# a truncated or mis-written file, but it cannot catch a disagreement between
# the offsets here and the layout in user/lib/include/hnpkg.h, because both
# sides of it are build.ps1. What actually pins this file to hnpkg.h is the
# set of constants below plus the _Static_assert offsets in user/hpm.c.
$OFF_MAGIC         = 0
$OFF_VERSION       = $OFF_MAGIC + 4
$OFF_NAME          = $OFF_VERSION + 4
$OFF_VER           = $OFF_NAME + $NAME_MAX
$OFF_DEPENDS       = $OFF_VER + $VER_MAX
$OFF_SUMMARY       = $OFF_DEPENDS + $DEPENDS_MAX
$OFF_FILE_COUNT    = $OFF_SUMMARY + $SUMMARY_MAX
$OFF_DATA_OFFSET   = $OFF_FILE_COUNT + 4
$OFF_TOTAL_SIZE    = $OFF_DATA_OFFSET + 4
$OFF_FLAGS         = $OFF_TOTAL_SIZE + 4
$OFF_TABLE_OFFSET  = $OFF_FLAGS + 4
$OFF_RESERVED      = $OFF_TABLE_OFFSET + 4

# --- byte offsets inside a 64-byte file table entry --------------------
$EOFF_PATH         = 0
$EOFF_OFFSET       = $EOFF_PATH + $PATH_MAX
$EOFF_SIZE         = $EOFF_OFFSET + 4
$EOFF_MODE         = $EOFF_SIZE + 4
$EOFF_RESERVED     = $EOFF_MODE + 4

# The arithmetic above has to add up to what HEADER_SIZE / ENTRY_SIZE claim,
# otherwise everything written below lands at the wrong byte. This assertion
# is self-consistent (both sides are computed in this file), so it catches a
# typo in the constants - it is *not* evidence that the layout matches
# hnpkg.h. That is what user/hpm.c's _Static_assert offset checks are for.
if ($OFF_RESERVED + $RESERVED_SIZE -ne $HEADER_SIZE) {
    throw "internal error: header layout adds up to $($OFF_RESERVED + $RESERVED_SIZE) bytes, but HEADER_SIZE is $HEADER_SIZE"
}
if ($OFF_TABLE_OFFSET + 4 -gt $OFF_RESERVED) {
    throw "internal error: the table_offset field (at $OFF_TABLE_OFFSET) runs into the reserved area at $OFF_RESERVED"
}
if ($EOFF_RESERVED + 4 -ne $ENTRY_SIZE) {
    throw "internal error: table entry layout adds up to $($EOFF_RESERVED + 4) bytes, but ENTRY_SIZE is $ENTRY_SIZE"
}

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

    # ASCII-only, checked character by character.
    #
    # Do NOT test this as
    #   [Text.Encoding]::ASCII.GetBytes($Text).Length -ne $Text.Length
    # The ASCII encoder does not throw on a non-ASCII character: it silently
    # substitutes '?' (0x3F) for it, one byte per character, so the byte count
    # always equals the character count and that condition can never be true.
    # It used to be the guard here, which is how a CJK "summary:" line used to
    # pack as six 0x3F bytes and still exit 0 - a corrupted field, no error.
    for ($i = 0; $i -lt $Text.Length; $i++) {
        $code = [int]$Text[$i]
        if ($code -gt 127) {
            throw ("$What contains a non-ASCII character (U+{0:X4} at position {1}): '$Text'" -f $code, $i)
        }
    }

    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Text)

    # Character count == byte count now that the loop above passed, so the
    # limit can be stated in characters.
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

# build.ps1 names the repo file "<name>-<version>.hnpkg", and HNeoFS caps one
# name component at HNEOFS_NAME_MAX - 1 = 31 characters (tools/mkfs.ps1 checks
# exactly this). The header itself allows name up to 31 and version up to 15,
# so a manifest that is perfectly legal here can produce a 53-character file
# name - and the build then dies two steps later inside mkfs with "name too
# long", which points at the file instead of at the manifest. Say it here,
# while there is still a manifest line to fix.
$pkgFileName = "$name-$version.hnpkg"
if ($pkgFileName.Length -gt ($HNEOFS_NAME_MAX - 1)) {
    throw ("package file name '$pkgFileName' is $($pkgFileName.Length) characters, " +
           "but HNeoFS allows at most $($HNEOFS_NAME_MAX - 1) in one name component: " +
           "shorten 'name' or 'version' in $manifestPath")
}

# Validate the header strings early, so the failure message names the
# field instead of blowing up while the header is being written.
$probe = New-Object byte[] $HEADER_SIZE
Copy-AsciiField -Buffer $probe -Offset $OFF_NAME    -FieldSize $NAME_MAX    -Text $name    -What "package name"
Copy-AsciiField -Buffer $probe -Offset $OFF_VER     -FieldSize $VER_MAX     -Text $version -What "version"
Copy-AsciiField -Buffer $probe -Offset $OFF_DEPENDS -FieldSize $DEPENDS_MAX -Text $depends -What "depends"
Copy-AsciiField -Buffer $probe -Offset $OFF_SUMMARY -FieldSize $SUMMARY_MAX -Text $summary -What "summary"

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
    # Resolve files\ to its canonical full path BEFORE enumerating and
    # trimming.
    #
    # The same directory has several spellings on Windows: a relative path
    # (-PackageDir is documented with a relative one right at the top of this
    # file), an 8.3 short name (C:\Users\RUNNER~1\...), a trailing separator.
    # Get-ChildItem always reports the *long, absolute* form in FullName, so
    # $f.FullName.Substring($filesRoot.Length) is betting that the two
    # spellings are character-for-character identical. When the bet loses the
    # result is not an error but a wrong install path - typically one extra
    # 'files/' component (the package then installs to /files/share/...),
    # occasionally a name sliced out of the middle of the absolute path - and
    # the exit code stays 0. Get-Item normalises, so trimming against its
    # result is arithmetic instead of a coincidence.
    $filesRoot = (Get-Item -LiteralPath $filesRoot).FullName.TrimEnd([char[]]@("\", "/"))

    $dataFiles = @(Get-ChildItem -LiteralPath $filesRoot -Recurse -File |
                   Sort-Object FullName)

    foreach ($f in $dataFiles) {
        # Trimming is only meaningful if the enumerated path really is below
        # the root we resolved; otherwise the "relative" path is garbage.
        if (-not $f.FullName.StartsWith($filesRoot + "\", [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "internal error: '$($f.FullName)' is not under the resolved files root '$filesRoot'"
        }

        $rel = $f.FullName.Substring($filesRoot.Length).TrimStart("\", "/")
        $rel = $rel -replace "\\", "/"

        # Belt and braces: if the trim still left the 'files\' level in place,
        # the prefix length was wrong - refuse to write a package that would
        # install to /files/... instead of failing loudly. In the fixed code
        # the check above already makes this unreachable, so reaching it means
        # either the trimming went wrong again or the package really does
        # contain files\files\..., which would install to /files/...
        if ($rel -eq "files" -or $rel.StartsWith("files/")) {
            throw ("internal error: install path '$rel' still starts with 'files/' - " +
                   "the files root '$filesRoot' was not trimmed correctly, or the package " +
                   "genuinely nests a 'files' directory inside files\")
        }

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
    Copy-AsciiField -Buffer $table -Offset ($base + $EOFF_PATH) -FieldSize $PATH_MAX -Text $e.Path -What "install path"
    [BitConverter]::GetBytes([uint32]$dataLen).CopyTo($table, $base + $EOFF_OFFSET)
    [BitConverter]::GetBytes($size).CopyTo($table, $base + $EOFF_SIZE)
    [BitConverter]::GetBytes([uint32]$e.Mode).CopyTo($table, $base + $EOFF_MODE)
    [BitConverter]::GetBytes([uint32]0).CopyTo($table, $base + $EOFF_RESERVED)   # reserved

    $e | Add-Member -NotePropertyName Offset -NotePropertyValue $dataLen -Force
    $e | Add-Member -NotePropertyName Size   -NotePropertyValue $size    -Force

    $dataLen += $size
}

$totalSize = $dataOffset + $dataLen

# --- build the header ----------------------------------------
$header = New-Object byte[] $HEADER_SIZE
[BitConverter]::GetBytes([uint32]$HNPKG_MAGIC).CopyTo($header, $OFF_MAGIC)
[BitConverter]::GetBytes([uint32]$HNPKG_VERSION).CopyTo($header, $OFF_VERSION)
Copy-AsciiField -Buffer $header -Offset $OFF_NAME    -FieldSize $NAME_MAX    -Text $name    -What "package name"
Copy-AsciiField -Buffer $header -Offset $OFF_VER     -FieldSize $VER_MAX     -Text $version -What "version"
Copy-AsciiField -Buffer $header -Offset $OFF_DEPENDS -FieldSize $DEPENDS_MAX -Text $depends -What "depends"
Copy-AsciiField -Buffer $header -Offset $OFF_SUMMARY -FieldSize $SUMMARY_MAX -Text $summary -What "summary"
[BitConverter]::GetBytes([uint32]$sorted.Count).CopyTo($header, $OFF_FILE_COUNT)
[BitConverter]::GetBytes([uint32]$dataOffset).CopyTo($header, $OFF_DATA_OFFSET)
[BitConverter]::GetBytes([uint32]$totalSize).CopyTo($header, $OFF_TOTAL_SIZE)
[BitConverter]::GetBytes([uint32]$admin).CopyTo($header, $OFF_FLAGS)
# table_offset is a *value*: hpm.c requires it to be exactly HNPKG_HEADER_SIZE,
# i.e. the table starts right after the header. (The offset of the field that
# holds it is $OFF_TABLE_OFFSET - those two numbers are different things.)
[BitConverter]::GetBytes([uint32]$HEADER_SIZE).CopyTo($header, $OFF_TABLE_OFFSET)
# [$OFF_RESERVED .. 255] is the reserved tail and stays zero

# Read the bytes that are about to be written back out of the buffer and
# compare them with the values they must carry, so a wrong offset fails here
# rather than on the OS as "bad package".
$hdrTableOffset = [BitConverter]::ToUInt32($header, $OFF_TABLE_OFFSET)
$hdrCount       = [BitConverter]::ToUInt32($header, $OFF_FILE_COUNT)
$hdrData        = [BitConverter]::ToUInt32($header, $OFF_DATA_OFFSET)
$hdrTotal       = [BitConverter]::ToUInt32($header, $OFF_TOTAL_SIZE)

if ($hdrTableOffset -ne $HEADER_SIZE) {
    throw "internal error: table_offset reads back as $hdrTableOffset, expected $HEADER_SIZE"
}
if ($hdrCount -ne $sorted.Count) {
    throw "internal error: file_count reads back as $hdrCount, expected $($sorted.Count)"
}
if ($hdrData -ne $dataOffset) {
    throw "internal error: data_offset reads back as $hdrData, expected $dataOffset"
}
if ($hdrTotal -ne $totalSize) {
    throw "internal error: total_size reads back as $hdrTotal, expected $totalSize"
}
# data_offset must land exactly after file_count entries: hpm.c rejects a
# package where it does not, and it is the only thing that ties the variable
# length table to where the data starts.
if ($hdrData -ne ($hdrTableOffset + ($ENTRY_SIZE * $hdrCount))) {
    throw "internal error: data_offset $hdrData is not table_offset $hdrTableOffset + $ENTRY_SIZE bytes x $hdrCount entries"
}

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
