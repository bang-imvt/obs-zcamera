# Minimal minidump reader: prints the exception code/address and the module
# that contains it, so an exit crash can be attributed without a debugger.
param([Parameter(Mandatory = $true)][string]$Dump)

$ErrorActionPreference = 'Stop'
$b = [System.IO.File]::ReadAllBytes($Dump)

function U32($o) { [BitConverter]::ToUInt32($b, $o) }
function U64($o) { [BitConverter]::ToUInt64($b, $o) }

if ([Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'MDMP') {
    throw 'not a minidump'
}

$nStreams = U32 8
$dirRva = U32 12
Write-Host ("dump      : {0}" -f $Dump)
Write-Host ("streams   : {0}" -f $nStreams)

$modules = @()
$excCode = 0
$excAddr = 0
$excThread = 0
$threads = @()

for ($i = 0; $i -lt $nStreams; $i++) {
    $e = $dirRva + $i * 12
    $type = U32 $e
    $rva = U32 ($e + 8)

    if ($type -eq 4) {
        # ModuleListStream; MINIDUMP_MODULE is 108 bytes.
        $count = U32 $rva
        for ($m = 0; $m -lt $count; $m++) {
            $o = $rva + 4 + $m * 108
            $nameRva = U32 ($o + 20)
            $len = U32 $nameRva
            $name = [Text.Encoding]::Unicode.GetString($b, $nameRva + 4, $len)
            $modules += [pscustomobject]@{
                Name = $name
                Base = U64 $o
                Size = U32 ($o + 8)
            }
        }
    }
    elseif ($type -eq 3) {
        # ThreadListStream; MINIDUMP_THREAD is 48 bytes.
        $count = U32 $rva
        for ($t = 0; $t -lt $count; $t++) {
            $o = $rva + 4 + $t * 48
            $threads += [pscustomobject]@{
                Id = U32 $o
                StackStart = U64 ($o + 24)
                DataSize = U32 ($o + 32)
                DataRva = U32 ($o + 36)
            }
        }
    }
    elseif ($type -eq 6) {
        # ExceptionStream: ThreadId(4) pad(4) then MINIDUMP_EXCEPTION
        $excThread = U32 $rva
        $excCode = U32 ($rva + 8)
        $excAddr = U64 ($rva + 24)
    }
}

Write-Host ("exception : 0x{0:X8}" -f $excCode)
Write-Host ("address   : 0x{0:X16}" -f $excAddr)
Write-Host ''

$hit = $modules | Where-Object { $excAddr -ge $_.Base -and $excAddr -lt ($_.Base + $_.Size) } |
    Select-Object -First 1
if ($hit) {
    $off = $excAddr - $hit.Base
    Write-Host ("FAULTING MODULE: {0} + 0x{1:X}" -f $hit.Name, $off)
}
else {
    Write-Host 'FAULTING MODULE: not inside any module (bad address)'
}

Write-Host ''
Write-Host 'modules of interest:'
$modules | Where-Object { $_.Name -match 'obs|zcamera|avcodec|Qt6' } |
    ForEach-Object { "  {0,-28} base=0x{1:X12} size=0x{2:X}" -f $_.Name, $_.Base, $_.Size }

# ---- raw stack scan of the crashing thread -------------------------------
# No unwind data is available here, so treat every 8-byte aligned word on the
# stack that lands inside a loaded module as a candidate return address. That
# is enough to attribute the crash to a module and an offset.
Write-Host ''
Write-Host ("threads: {0}" -f $threads.Count)
$threads | Select-Object -First 12 | ForEach-Object {
    Write-Host ("  id={0,-8} stack=0x{1:X12} size={2,-8} rva=0x{3:X}" -f `
            $_.Id, $_.StackStart, $_.DataSize, $_.DataRva)
}
Write-Host ("crashing thread: {0}" -f $excThread)
$th = $threads | Where-Object { $_.Id -eq $excThread } | Select-Object -First 1
if (-not $th) {
    Write-Host 'thread not found in the thread list'
    exit 0
}

$stack = New-Object byte[] $th.DataSize
[Array]::Copy($b, $th.DataRva, $stack, 0, $th.DataSize)
Write-Host ("stack    : 0x{0:X} .. 0x{1:X} ({2} bytes)" -f `
        $th.StackStart, ($th.StackStart + $th.DataSize), $th.DataSize)
Write-Host ''

$seen = @{}
$shown = 0
Write-Host 'raw words:'
0..15 | ForEach-Object {
    Write-Host ("  [{0:X4}] 0x{1:X16}" -f ($_ * 8), [BitConverter]::ToUInt64($stack, $_ * 8))
}
$modRanges = $modules | ForEach-Object {
    [pscustomobject]@{ Leaf = (Split-Path $_.Name -Leaf); Lo = $_.Base; Hi = ($_.Base + $_.Size) }
}
Write-Host ("module ranges: {0}" -f $modRanges.Count)

for ($o = 0; $o + 8 -le $stack.Length; $o += 8) {
    $v = [BitConverter]::ToUInt64($stack, $o)
    if ($v -eq 0) { continue }
    $mod = $modRanges | Where-Object { $v -ge $_.Lo -and $v -lt $_.Hi } |
        Select-Object -First 1
    if (-not $mod) { continue }
    $key = "{0}+{1}" -f $mod.Leaf, ($v - $mod.Lo)
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    Write-Host ("  [{0:X4}] {1,-22} +0x{2:X}" -f ($th.StackStart + $o), $mod.Leaf, ($v - $mod.Lo))
    if (++$shown -ge 45) { break }
}
Write-Host ("frames shown: {0}" -f $shown)
