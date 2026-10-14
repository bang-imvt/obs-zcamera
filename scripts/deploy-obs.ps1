# Deploy obs-zcamera for local testing, or into the OBS install directory.
#
# Two layouts, and they are NOT interchangeable:
#
#   -Target dev       (default) machine-wide plugin dir, no admin needed.
#                     OBS 31 auto-scans %ProgramData%\obs-studio\plugins\ and
#                     expects a per-plugin folder with bin\64bit\ + data\:
#                         %ProgramData%\obs-studio\plugins\obs-zcamera\
#                             bin\64bit\    obs-zcamera.dll, ssp-connector.exe,
#                                           libssp.dll
#                             data\locale\  en-US.ini, zh-CN.ini
#                     (A per-user %APPDATA%\obs-studio\plugins\<name>\ install is
#                     not scanned, which is why an earlier dev deploy looked
#                     installed but never loaded.)
#
#   -Target installer  the shipped-installer layout, needs an elevated shell:
#                         <obs-studio>\obs-plugins\64bit\
#                         <obs-studio>\data\obs-plugins\obs-zcamera\locale\
#                     installer.nsi also drops the FFmpeg runtimes there; this
#                     script does not, because OBS's own bin\64bit already has
#                     matching avcodec-61 / avutil-59 / swscale-8 / swresample-5
#                     and shadowing them with the obs-deps copies is how you get
#                     a confusing ABI mismatch. Pass -IncludeFfmpeg to override.
#
# Nothing is ever deleted. -WhatIf shows the plan.
#
#   powershell -ExecutionPolicy Bypass -File scripts\deploy-obs.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\deploy-obs.ps1 -Target installer

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet('dev', 'installer')]
    [string]$Target = 'dev',
    [string]$ObsDir = 'C:\Program Files\obs-studio',
    [ValidateSet('RelWithDebInfo', 'Debug', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [string]$BuildDir = 'build_x64',
    [switch]$IncludeFfmpeg
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $root $BuildDir
$bin = Join-Path $buildRoot $Config

# plugin binaries
$binaries = @(
    (Join-Path $bin 'obs-zcamera.dll'),
    (Join-Path $buildRoot "ssp_connector\$Config\ssp-connector.exe"),
    (Join-Path $buildRoot '_deps\libssp-src\lib\win_x64_vs2017\libssp.dll')
)
$locales = @('en-US.ini', 'zh-CN.ini') | ForEach-Object { Join-Path $root "data\locale\$_" }

if ($Target -eq 'dev') {
    $base = Join-Path $env:ProgramData 'obs-studio\plugins\obs-zcamera'
    $binDst = 'bin\64bit'
    $locDst = 'data\locale'
} else {
    if (-not (Test-Path $ObsDir)) {
        throw "OBS not found at '$ObsDir'. Pass -ObsDir <path>."
    }
    $base = $ObsDir
    $binDst = 'obs-plugins\64bit'
    $locDst = 'data\obs-plugins\obs-zcamera\locale'
}

$plan = @()
foreach ($src in $binaries) {
    $plan += @{ src = $src; dst = (Join-Path $base (Join-Path $binDst (Split-Path -Leaf $src))) }
}
foreach ($src in $locales) {
    $plan += @{ src = $src; dst = (Join-Path $base (Join-Path $locDst (Split-Path -Leaf $src))) }
}
if ($IncludeFfmpeg) {
    foreach ($dll in @('avcodec-61.dll', 'avutil-59.dll', 'swscale-8.dll', 'swresample-5.dll')) {
        $plan += @{ src = (Join-Path $root ".deps\obs-deps-2024-09-12-x64\bin\$dll");
                    dst = (Join-Path $base (Join-Path $binDst $dll)) }
    }
}

Write-Host "target : $Target"
Write-Host "base   : $base"
Write-Host "config : $Config ($BuildDir)"
Write-Host ''

$failed = $false
foreach ($f in $plan) {
    if (-not (Test-Path $f.src)) {
        Write-Warning "missing build output: $($f.src)"
        $failed = $true
        continue
    }
    if ($PSCmdlet.ShouldProcess($f.dst, 'deploy')) {
        $dir = Split-Path -Parent $f.dst
        if (-not (Test-Path $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
        try {
            Copy-Item $f.src $f.dst -Force
            Write-Host ("ok   {0}  ({1:N0} bytes)" -f $f.dst.Substring($base.Length + 1),
                        (Get-Item $f.dst).Length)
        } catch {
            Write-Host ("FAIL {0}: {1}" -f $f.dst, $_.Exception.Message) -ForegroundColor Red
            $failed = $true
        }
    }
}

if ($failed) {
    Write-Host ''
    if ($Target -eq 'installer') {
        Write-Host 'If the failures are "access denied", re-run elevated:' -ForegroundColor Yellow
        Write-Host '  Start-Process powershell -Verb RunAs -ArgumentList ''-File','scripts\deploy-obs.ps1'',''-Target'',''installer'''
    }
    exit 1
}

Write-Host ''
Write-Host "Deployed to $base"
Write-Host 'Restart OBS, add the "ZCamera" source, then open its Properties and enable'
Write-Host 'the AI-box overlay: the boxes are drawn in the Properties preview only.'
