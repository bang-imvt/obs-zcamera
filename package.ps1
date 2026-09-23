# Builds obs-zcamera and packs it into a portable OBS plugin folder (zip) that
# can be handed to another machine. The user unzips the resulting "obs-zcamera"
# folder into OBS's plugins directory:
#
#     plugins\obs-zcamera\bin\64bit\obs-zcamera.dll   (the plugin + runtimes)
#     plugins\obs-zcamera\bin\64bit\ssp-connector.exe (the SSP bridge process)
#     plugins\obs-zcamera\bin\64bit\libssp.dll
#     plugins\obs-zcamera\bin\64bit\avcodec-61.dll, avutil-59.dll,
#             swscale-8.dll, swresample-5.dll         (FFmpeg runtime)
#     plugins\obs-zcamera\data\locale\*.ini           (translations, OBS data dir)
#
# Typical install paths:
#     Per-user:   %APPDATA%\obs-studio\plugins\          (recommended)
#     All-users:  C:\ProgramData\obs-studio\plugins\
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File package.ps1          # build + pack
#   powershell -ExecutionPolicy Bypass -File package.ps1 -NoBuild # pack only

param(
    # The CMake config to (re)build. The plugin normally ships RelWithDebInfo.
    [ValidateSet("Debug", "RelWithDebInfo", "Release")][string]$Config = "RelWithDebInfo",
    # Skip the build; just repackage whatever is in build_x64\$Config.
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build  = Join-Path $Root "build_x64"
$DepsBin = Join-Path $Root ".deps\obs-deps-2024-09-12-x64\bin"
$LibSsp = Join-Path $Build "_deps\libssp-src\lib\win_x64_vs2017\libssp.dll"
$Version = "0.14.0"

function Assert-Path($p, $what) {
    if (-not (Test-Path -LiteralPath $p)) { throw "$what not found: $p" }
}

# ---- 1) build -----------------------------------------------------------
if (-not $NoBuild) {
    Write-Host "=== building obs-zcamera ($Config) ==="
    & cmake --build $Build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed (cmake exit $LASTEXITCODE)" }
} else {
    Write-Host "=== skipping build (-NoBuild) ==="
}

Assert-Path (Join-Path $Build "$Config\obs-zcamera.dll") 'built plugin'
Assert-Path (Join-Path $Build "ssp_connector\$Config\ssp-connector.exe") 'ssp-connector'

# ---- 2) stage a portable OBS plugin folder -----------------------------
$Dist  = Join-Path $Root "dist"
$Stage = Join-Path $Dist "obs-zcamera"
$Bin    = Join-Path $Stage "bin\64bit"
$Locale = Join-Path $Stage "data\locale"
foreach ($d in $Bin, $Locale) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

Write-Host "=== staging plugin into $Stage ==="
Copy-Item -LiteralPath (Join-Path $Build "$Config\obs-zcamera.dll") -Destination $Bin -Force
Copy-Item -LiteralPath (Join-Path $Build "ssp_connector\$Config\ssp-connector.exe") -Destination $Bin -Force
if (Test-Path -LiteralPath $LibSsp) {
    Copy-Item -LiteralPath $LibSsp -Destination $Bin -Force
} else {
    Write-Host "  warning: libssp.dll not found ($LibSsp)"
}
# FFmpeg is linked dynamically by the plugin; its runtime DLLs must travel
# with it (the same ones the OBS integration test deploys).
foreach ($dll in 'avcodec-61.dll', 'avutil-59.dll', 'swscale-8.dll',
                  'swresample-5.dll') {
    $from = Join-Path $DepsBin $dll
    if (Test-Path -LiteralPath $from) {
        Copy-Item -LiteralPath $from -Destination $Bin -Force
    } else {
        Write-Host "  warning: $dll not found in .deps\bin"
    }
}
Copy-Item -Path (Join-Path $Root "data\locale\*.ini") -Destination $Locale -Force

# ---- 3) zip ------------------------------------------------------------
$Zip = Join-Path $Dist "obs-zcamera-$Version-win64.zip"
if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
Write-Host "=== packing $Zip ==="
# Zip the folder so the archive root is "obs-zcamera\..." and the user can
# unzip it straight into OBS's plugins directory.
Compress-Archive -Path $Stage -DestinationPath $Zip -Force

Write-Host ""
Write-Host "Package written: $Zip"
Write-Host "Install: unzip the 'obs-zcamera' folder into OBS's plugins dir, e.g."
Write-Host "   %APPDATA%\obs-studio\plugins\      (per user, recommended)"
Write-Host "   or C:\ProgramData\obs-studio\plugins\   (all users)"
Write-Host "then start/restart OBS Studio."