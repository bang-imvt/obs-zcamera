# Self-test: AI overlay in the preview render, and absent from the canvas mix.
#   preview screenshot (overlay on)  vs  recording frame (the mix)
# Launches OBS itself, drives it over obs-websocket, extracts a recording frame
# with ffmpeg, and counts red pixels in each.
$ErrorActionPreference = 'Stop'
$out  = $PSScriptRoot
$cam  = if ($args.Count -gt 0) { $args[0] } else { '192.168.10.106' }

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    Write-Host 'OBS is already running; this test starts its own.'
    exit 3
}

$obsBin = 'C:\Program Files\obs-studio\bin\64bit'
$launchTime = Get-Date

# Clean slate: leftover camera sources in the collection would compete for the
# camera's stream and muddy the pixel counts.
$env:NODE_NO_WARNINGS = 1
$coll = "$env:APPDATA\obs-studio\basic\scenes\zc-autotest.json"
if (Test-Path $coll) {
    node -e "const fs=require('fs');const p=process.argv[1];const j=JSON.parse(fs.readFileSync(p,'utf8'));const n=j.sources.length;j.sources=j.sources.filter(s=>s.id!=='zcamera_source');fs.writeFileSync(p,JSON.stringify(j,null,2));console.log('collection sources '+n+' -> '+j.sources.length);" $coll
}

Write-Host '=== launching OBS (zc-autotest collection/profile) ==='
$obs = Start-Process -FilePath (Join-Path $obsBin 'obs64.exe') -WorkingDirectory $obsBin -PassThru -ArgumentList @(
    '--collection', 'zc-autotest', '--profile', 'zc-autotest',
    '--minimize-to-tray', '--disable-shutdown-check', '--disable-updater')

$up = $false
for ($i = 0; $i -lt 120; $i++) {
    if ($obs.HasExited) { break }
    try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1', 4455); $c.Close(); $up = $true; break }
    catch { Start-Sleep -Milliseconds 500 }
}
if (-not $up) {
    Write-Host 'obs-websocket never came up'
    if (-not $obs.HasExited) { Stop-Process -Id $obs.Id -Force }
    exit 2
}

$env:NO_PROXY=$null; $env:no_proxy=$null; $env:HTTP_PROXY=$null; $env:http_proxy=$null
$env:NODE_NO_WARNINGS = 1
Write-Host '=== overlay test ==='
# node writes warnings to stderr; with ErrorActionPreference=Stop PowerShell
# turns that into a terminating NativeCommandError.
$ErrorActionPreference = 'Continue'
Remove-Item (Join-Path $out 'result.json'), (Join-Path $out 'mix.bmp') -ErrorAction SilentlyContinue
$driverOut = node (Join-Path $out 'obs_ai_overlay_driver.mjs') --ip $cam --out $out 2>&1 |
    Where-Object { $_ -notmatch 'UNDICI-EHPA|trace-warnings' }
$ErrorActionPreference = 'Stop'
$driverOut | ForEach-Object { "$_" }
$rec = ($driverOut | Where-Object { $_ -like 'RECORDING *' } | Select-Object -First 1) -replace '^RECORDING ', ''

Write-Host '=== mix frame from the recording ==='
$mixRed = $null
if (-not $rec) {
    # obs-websocket does not always report the finished file name; the profile
    # says where recordings go, so take the newest one written after launch.
    $ini = "$env:APPDATA\obs-studio\basic\profiles\zc-autotest\basic.ini"
    $recDir = $null
    if (Test-Path $ini) {
        $m = Select-String -Path $ini -Pattern 'RecFilePath=(.+)' | Select-Object -First 1
        if ($m) { $recDir = $m.Matches[0].Groups[1].Value.Trim() }
    }
    $dirs = @($recDir, 'D:\imvt_git\obs-zcamera\tests\obs_out', "$env:USERPROFILE\Videos") |
        Where-Object { $_ -and (Test-Path $_) }
    $recFile = Get-ChildItem $dirs -Include *.mp4,*.mkv -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $launchTime } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($recFile) { $rec = $recFile.FullName }
}
if ($rec -and (Test-Path $rec)) {
    Write-Host "  recording: $rec"
    $mixBmp = Join-Path $out 'mix.bmp'
    Remove-Item $mixBmp -ErrorAction SilentlyContinue
    & ffmpeg -y -loglevel error -i "$rec" -vf "select=eq(n\,60)" -vframes 1 $mixBmp 2>&1 | Out-Null
    if (Test-Path $mixBmp) {
        Push-Location $out
        $ErrorActionPreference = 'Continue'
        $mixLine = node --input-type=module -e "import {countRed} from './obs_bmp_pixels.mjs'; import {readFileSync} from 'node:fs'; const r = countRed(readFileSync('mix.bmp')); console.log(r.count + ' ' + JSON.stringify(r.box) + ' ' + r.width + 'x' + r.height);" 2>&1 |
            Where-Object { $_ -notmatch 'UNDICI-EHPA|trace-warnings' }
        $ErrorActionPreference = 'Stop'
        Pop-Location
        Write-Host "  recording frame red: $mixLine"
        $mixRed = if ($mixLine -match '^\s*(\d+)\s') { [int]$Matches[1] } else { $null }
    } else { Write-Host '  ffmpeg produced no frame' }
} else { Write-Host '  no recording file' }

Write-Host '=== stopping OBS ==='
if (-not $obs.HasExited) { if (-not $obs.WaitForExit(15000)) { Stop-Process -Id $obs.Id -Force } }

Write-Host '=== verdict ==='
$r = Get-Content (Join-Path $out 'result.json') -Raw | ConvertFrom-Json
$onR = $r.onRed.count
$offR = if ($null -ne $r.offRed) { $r.offRed.count } else { $null }
Write-Host ("  preview overlay ON : {0} red px (box {1})" -f $onR, ($r.onRed.box | ConvertTo-Json -Compress))
if ($null -ne $offR)  { Write-Host ("  preview overlay OFF: {0} red px" -f $offR) }
if ($null -ne $mixRed) { Write-Host ("  recording (mix)    : {0} red px" -f $mixRed) }

$fail = 0
$onD = $onR / ($r.onRed.width * $r.onRed.height)
# An empty room correctly draws nothing, so the box assertions only mean
# something when the camera was reporting a person while the shot was taken.
$haveBox = [bool]$r.detected
if ($r.variance -le 8) { Write-Host '  [FAIL] the source never delivered a picture'; $fail++ }
elseif (-not $haveBox) { Write-Host "  [SKIP] the camera reported nobody ($($r.detected) boxes): put someone in frame and re-run" }
elseif ($onR -le 400)  { Write-Host '  [FAIL] no red boxes in the preview render'; $fail++ }
else                   { Write-Host '  [PASS] red boxes are drawn in the preview render' }

if ($haveBox -and $null -ne $offR) {
    $offD = $offR / ($r.offRed.width * $r.offRed.height)
    Write-Host ("  red density: overlay on {0:N2}e-4, overlay off {1:N2}e-4 (per pixel x 1e4)" -f ($onD*1e4), ($offD*1e4))
    if ($onR -lt $offR * 3 + 300) { Write-Host '  [FAIL] turning the overlay off did not remove the boxes'; $fail++ }
    else                          { Write-Host '  [PASS] the box red appears only with the overlay on' }
}

if ($null -ne $mixRed) {
    $mixLineDens = $mixRed / (1920 * 1080)
    Write-Host ("  mix red density: {0:N2}e-4" -f ($mixLineDens*1e4))
    # The camera's own red OSD is in both; what must not appear is the density
    # the drawn boxes add in the preview.
    if ($haveBox -and $mixLineDens -gt $onD * 0.5) { Write-Host '  [FAIL] the recording carries box-red pixels: the overlay leaked into the mix'; $fail++ }
    elseif ($haveBox)                              { Write-Host '  [PASS] the recording (canvas mix) carries no box red' }
    else                                           { Write-Host '  [SKIP] mix check needs a detection in frame' }
}

Write-Host '=== plugin log lines ==='
$log = Get-ChildItem "$env:APPDATA\obs-studio\logs\*.txt" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Select-String -Path $log.FullName -Pattern 'AI detection|notifications|startTimer' |
    Select-Object -Last 12 | ForEach-Object { "  $($_.Line)" }
exit $fail
