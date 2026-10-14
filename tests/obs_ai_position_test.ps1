# Launch OBS, verify that the drawn box matches the camera's reported coordinates.
$ErrorActionPreference = 'Stop'
$out = $PSScriptRoot
$cam = if ($args.Count -gt 0) { $args[0] } else { '192.168.10.106' }
$env:NODE_NO_WARNINGS = 1

if (Get-Process obs64 -ErrorAction SilentlyContinue) { Write-Host 'OBS already running'; exit 3 }

$coll = "$env:APPDATA\obs-studio\basic\scenes\zc-autotest.json"
if (Test-Path $coll) {
    node -e "const fs=require('fs');const p=process.argv[1];const j=JSON.parse(fs.readFileSync(p,'utf8'));const n=j.sources.length;j.sources=j.sources.filter(s=>s.id!=='zcamera_source');fs.writeFileSync(p,JSON.stringify(j,null,2));console.log('collection sources '+n+' -> '+j.sources.length);" $coll
}

$obsBin = 'C:\Program Files\obs-studio\bin\64bit'
$obs = Start-Process -FilePath (Join-Path $obsBin 'obs64.exe') -WorkingDirectory $obsBin -PassThru -ArgumentList @(
    '--collection','zc-autotest','--profile','zc-autotest','--minimize-to-tray','--disable-shutdown-check','--disable-updater')
for ($i=0;$i -lt 120;$i++) {
    if ($obs.HasExited) { break }
    try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1',4455); $c.Close(); break } catch { Start-Sleep -Milliseconds 500 }
}
$env:NO_PROXY=$null; $env:no_proxy=$null; $env:HTTP_PROXY=$null; $env:http_proxy=$null
$ErrorActionPreference = 'Continue'
node (Join-Path $out 'obs_ai_position_driver.mjs') --ip $cam --out $out 2>&1 | Where-Object { $_ -notmatch 'UNDICI-EHPA|trace-warnings' }
$rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'

if (-not $obs.HasExited) { if (-not $obs.WaitForExit(15000)) { Stop-Process -Id $obs.Id -Force } }
exit $rc
