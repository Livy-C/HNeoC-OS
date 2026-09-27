# Send keystrokes to a running VirtualBox VM by converting text into
# PS/2 scan-code set 1 make/break codes.
#
# NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1 reads
# BOM-less .ps1 files as GBK on a Chinese locale, which mangles non-ASCII
# bytes and can swallow line breaks.
#
# Usage:
#   .\vm-type.ps1 -VmName HNeoC -Text "help" -Enter
#   .\vm-type.ps1 -VmName HNeoC -Key Up

param(
    [Parameter(Mandatory = $true)][string]$VmName,
    [string]$Text = "",
    [switch]$Enter,
    [string]$Key = "",
    [int]$DelayMs = 250
)

$ErrorActionPreference = "Stop"

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

# PS/2 scan-code set 1: character -> make code
$scan = @{
    'a'=0x1E; 'b'=0x30; 'c'=0x2E; 'd'=0x20; 'e'=0x12; 'f'=0x21; 'g'=0x22
    'h'=0x23; 'i'=0x17; 'j'=0x24; 'k'=0x25; 'l'=0x26; 'm'=0x32; 'n'=0x31
    'o'=0x18; 'p'=0x19; 'q'=0x10; 'r'=0x13; 's'=0x1F; 't'=0x14; 'u'=0x16
    'v'=0x2F; 'w'=0x11; 'x'=0x2D; 'y'=0x15; 'z'=0x2C
    '0'=0x0B; '1'=0x02; '2'=0x03; '3'=0x04; '4'=0x05; '5'=0x06; '6'=0x07
    '7'=0x08; '8'=0x09; '9'=0x0A
    ' '=0x39; '-'=0x0C; '='=0x0D; '['=0x1A; ']'=0x1B; ';'=0x27
    "'"=0x28; '\'=0x2B; ','=0x33; '.'=0x34; '/'=0x35
}

# These characters need Shift held down
$shifted = @{
    '!'='1'; '@'='2'; '#'='3'; '$'='4'; '%'='5'; '^'='6'; '&'='7'
    '*'='8'; '('='9'; ')'='0'; '_'='-'; '+'='='; '{'='['; '}'=']'
    ':'=';'; '"'="'"; '~'='`'; '|'='\'; '<'=','; '>'='.'; '?'='/'
}

$special = @{
    'Enter'     = @(0x1C, 0x9C)
    'Backspace' = @(0x0E, 0x8E)
    'Tab'       = @(0x0F, 0x8F)
    'Space'     = @(0x39, 0xB9)
    'Escape'    = @(0x01, 0x81)
    'Up'        = @(0xE0, 0x48, 0xE0, 0xC8)
    'Down'      = @(0xE0, 0x50, 0xE0, 0xD0)
    'Left'      = @(0xE0, 0x4B, 0xE0, 0xCB)
    'Right'     = @(0xE0, 0x4D, 0xE0, 0xCD)
}

function Send-Codes([int[]]$codes) {
    # VBoxManage wants each scan code as its own argument, e.g.
    #   keyboardputscancode 1C 9C
    # Passing "1C 9C" as a single string fails with "is not a hex byte!".
    $hex = @($codes | ForEach-Object { "{0:X2}" -f $_ })
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $vbox controlvm $VmName keyboardputscancode @hex 2>&1 | Out-Null
    $ErrorActionPreference = $saved
    Start-Sleep -Milliseconds $DelayMs
}

# --- single special key ---
if ($Key -ne "") {
    if (-not $special.ContainsKey($Key)) { throw "unknown key: $Key" }
    Send-Codes $special[$Key]
    exit 0
}

# --- text ---
foreach ($ch in $Text.ToCharArray()) {
    # Hashtable.ContainsKey does no char-to-string coercion, so convert first
    $asString = [string]$ch
    $lower = $asString.ToLowerInvariant()

    if ($scan.ContainsKey($lower)) {
        $code = $scan[$lower]
        $needsShift = ($asString -cne $lower)
    } elseif ($shifted.ContainsKey($asString)) {
        $code = $scan[$shifted[$asString]]
        $needsShift = $true
    } else {
        throw "cannot type character: '$ch'"
    }

    $make = @()
    if ($needsShift) { $make += 0x2A }      # left shift down
    $make += $code
    $make += ($code -bor 0x80)              # key up
    if ($needsShift) { $make += 0xAA }      # left shift up

    Send-Codes $make
}

if ($Enter) {
    Send-Codes $special['Enter']
}
