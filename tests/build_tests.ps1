# Builds and runs the standalone test binaries for obs-zcamera. This is the
# whole local verification of the decode pipeline: no OBS, no camera, no UI.
#
#   tests\cameractrl_tests.exe  - pure Qt6 tests for src/cameractrl
#                                 (settings schema + HTTP Digest over loopback)
#   tests\decode_test.exe       - FFmpeg/D3D11VA decode probe
#   tests\pipeline_test.exe     - the REAL decode pipeline over a local video
#                                 file (src/hwdecode/*, OBS symbols shimmed)
#
# The pipeline is driven from tests\test_720p.h264 twice over: as a raw Annex-B
# elementary stream (what the camera sends) and, when ffmpeg is on PATH, wrapped
# in an mp4, where the harness has to run the packets through h264_mp4toannexb
# before the decoder can read them. Both hardware backends are exercised.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tests\build_tests.ps1 [-SkipRun]
# Runs from any cwd; all paths are derived from $PSScriptRoot.

param(
    [switch]$SkipRun
)

$ErrorActionPreference = 'Stop'

$Root = $PSScriptRoot                              # ...\tests
$Proj = Split-Path -Parent $Root                   # repo root
$Src  = Join-Path $Proj 'src\cameractrl'
$Deps = Join-Path $Proj '.deps\obs-deps-2024-09-12-x64'
# Installed libobs headers/libs (used by the pipeline test's OBS shim).
$ObsInc = Join-Path $Proj '.deps\include'
$ObsLib = Join-Path $Proj '.deps\lib'

$Qt        = 'D:\Qt6\6.5.3\msvc2019_64'
$QtBin     = Join-Path $Qt 'bin'
$QtLib     = Join-Path $Qt 'lib'
$QtMoc     = Join-Path $QtBin 'moc.exe'
$VsPath    = 'D:\Program Files\Microsoft Visual Studio\2022\Community'
$VsDevShell = Join-Path $VsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'

function Assert-Path($p, $what) {
    if (-not (Test-Path -LiteralPath $p)) {
        throw "$what not found: $p"
    }
}

Assert-Path $VsDevShell 'VS DevShell module'
Assert-Path $QtMoc 'Qt moc'
Assert-Path (Join-Path $Root 'cameractrl_tests.cpp') 'cameractrl_tests.cpp'
Assert-Path (Join-Path $Root 'decode_test.cpp') 'decode_test.cpp'
Assert-Path (Join-Path $Root 'pipeline_test.cpp') 'pipeline_test.cpp'
Assert-Path $ObsInc 'libobs headers (.deps\include)'
Assert-Path $Deps 'obs-deps folder'

Write-Host '=== entering VS 2022 x64 dev shell ==='
Import-Module $VsDevShell
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# ---- common Qt / include / flag sets -------------------------------------
$qtIncludes = @(
    (Join-Path $Qt 'include'),
    (Join-Path $Qt 'include\QtCore'),
    (Join-Path $Qt 'include\QtGui'),
    (Join-Path $Qt 'include\QtNetwork')
)
$qtIncArgs = $qtIncludes | ForEach-Object { "/I$_" }
$srcIncArg = "/I$Src"

$qtCxxFlags = @(
    '/nologo', '/std:c++17', '/Zc:__cplusplus', '/permissive-',
    '/EHsc', '/MD', '/DUNICODE', '/D_WINDOWS'
)

$decodeFlags = @('/nologo', '/std:c++17', '/EHsc', '/O2', '/MD')
$depsIncArg  = "/I$(Join-Path $Deps 'include')"
$depsLibArg  = "/LIBPATH:$(Join-Path $Deps 'lib')"

# Run everything from the tests dir so cl's default .obj output lands here.
Push-Location $Root
try {
    # ---- generate moc for the Q_OBJECT headers ---------------------------
    $mocOut = Join-Path $Root 'moc_zc_http_transport.cpp'
    Write-Host '=== moc zc_http_transport.h ==='
    & $QtMoc (Join-Path $Src 'zc_http_transport.h') -o $mocOut
    if ($LASTEXITCODE -ne 0) { throw "moc failed ($LASTEXITCODE)" }

    $mocEngine = Join-Path $Root 'moc_zc_settings_engine.cpp'
    Write-Host '=== moc zc_settings_engine.h ==='
    & $QtMoc (Join-Path $Src 'zc_settings_engine.h') -o $mocEngine
    if ($LASTEXITCODE -ne 0) { throw "moc failed ($LASTEXITCODE)" }

    # ---- cameractrl_tests.exe -------------------------------------------
    $ctrlSources = @(
        (Join-Path $Root 'cameractrl_tests.cpp'),
        $mocOut,
        $mocEngine,
        (Join-Path $Src 'zc_http_transport.cpp'),
        (Join-Path $Src 'zc_settings_schema.cpp'),
        (Join-Path $Src 'zc_settings_engine.cpp')
    )
    $ctrlOut = Join-Path $Root 'cameractrl_tests.exe'
    Write-Host '=== compiling cameractrl_tests.exe ==='
    & cl @qtCxxFlags @qtIncArgs $srcIncArg @ctrlSources `
        '/link' "/LIBPATH:$QtLib" 'Qt6Core.lib' 'Qt6Network.lib' `
        "/OUT:$ctrlOut"
    if ($LASTEXITCODE -ne 0) { throw "cameractrl_tests build failed ($LASTEXITCODE)" }

    # ---- decode_test.exe -------------------------------------------------
    Write-Host '=== compiling decode_test.exe ==='
    & cl @decodeFlags $depsIncArg (Join-Path $Root 'decode_test.cpp') `
        '/link' $depsLibArg 'avcodec.lib' 'avformat.lib' 'avutil.lib' `
        'd3d11.lib' 'dxgi.lib' `
        "/OUT:$(Join-Path $Root 'decode_test.exe')"
    if ($LASTEXITCODE -ne 0) { throw "decode_test build failed ($LASTEXITCODE)" }

    # ---- pipeline_test.exe ----------------------------------------------
    # Links the REAL decoder sources; only a handful of OBS runtime symbols
    # (blog, gs_get_device_obj, obs_enter/leave_graphics, gs_texture_open_shared,
    # gs_texture_destroy) are shimmed inside pipeline_test.cpp. Hardware decode
    # only, so ENABLE_SW_DECODE stays off (the software backend needs
    # ffmpeg-decode.c and libobs's obs-avc/obs-hevc keyframe helpers).
    $pipeFlags = @(
        '/nologo', '/std:c++17', '/Zc:__cplusplus', '/permissive-',
        '/EHsc', '/MD', '/DUNICODE', '/D_WINDOWS', '/DENABLE_ZERO_COPY'
    )
    $srcRootInc = "/I$(Join-Path $Proj 'src')"
    $obsIncArg  = "/I$ObsInc"
    $hwSources = @(
        (Join-Path $Root 'pipeline_test.cpp'),
        (Join-Path $Proj 'src\hwdecode\hw_decode.c'),
        (Join-Path $Proj 'src\hwdecode\hw_decode_ffmpeg.cpp'),
        (Join-Path $Proj 'src\hwdecode\hw_decode_d3d11_win.cpp'),
        (Join-Path $Proj 'src\hwdecode\hw_decode_cuda_win.cpp')
    )
    Write-Host '=== compiling pipeline_test.exe ==='
    # d3dcompiler is needed by the CUDA/NVDEC importer (it compiles the NV12 ->
    # BGRA pixel-shader pass at runtime).
    & cl @pipeFlags $obsIncArg $srcRootInc $depsIncArg @hwSources `
        '/link' $depsLibArg 'avcodec.lib' 'avformat.lib' 'avutil.lib' `
        'd3d11.lib' 'dxgi.lib' 'd3dcompiler.lib' `
        "/OUT:$(Join-Path $Root 'pipeline_test.exe')"
    if ($LASTEXITCODE -ne 0) { throw "pipeline_test build failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}

if ($SkipRun) {
    Write-Host '=== build only (-SkipRun): not running tests ==='
    exit 0
}

# decode_test.exe loads the FFmpeg DLLs from its own directory; take them from
# .deps so the script runs from a clean checkout (they are git-ignored).
$depsBin = Join-Path $Deps 'bin'
if (Test-Path -LiteralPath $depsBin) {
    Copy-Item -Path (Join-Path $depsBin '*.dll') -Destination $Root -Force
}

# ---- fixtures ------------------------------------------------------------
$Fixture = Join-Path $Root 'test_720p.h264'
if (-not (Test-Path -LiteralPath $Fixture)) {
    throw "the pipeline fixture is missing: $Fixture"
}

# The harness also takes container input, which is a different path through it:
# the packets come from avformat and must be run through h264_mp4toannexb before
# decode() sees Annex-B. Build that copy from the tracked fixture when ffmpeg is
# available; without ffmpeg those cases are skipped rather than failed.
$Container = Join-Path $env:TEMP 'zc_test_720p.mp4'
$ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
if ($ffmpeg) {
    Write-Host '=== building the mp4 fixture from test_720p.h264 ==='
    & $ffmpeg.Source -v error -y -f h264 -i $Fixture -c copy $Container
    if ($LASTEXITCODE -ne 0) {
        throw "ffmpeg could not build the container fixture ($LASTEXITCODE)"
    }
    Write-Host "  $Container"
} else {
    Write-Host 'ffmpeg is not on PATH: the mp4 (container input) cases will be skipped'
}

# ---- run -----------------------------------------------------------------
$results = @()

function Invoke-Case($label, $exe, [string[]]$caseArgs) {
    Write-Host ''
    Write-Host "=== $label ==="
    Push-Location $Root
    try {
        # The test binaries load the FFmpeg DLLs from the tests directory, and
        # cameractrl_tests needs Qt on PATH.
        $env:Path = "$QtBin;$Root;$env:Path"
        & (Join-Path $Root $exe) @caseArgs
        $rc = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    Write-Host "$label exit code: $rc"
    $script:results += [pscustomobject]@{ Case = $label; Rc = $rc }
}

Invoke-Case 'cameractrl_tests' 'cameractrl_tests.exe' @()
Invoke-Case 'decode_test (raw Annex-B)' 'decode_test.exe' `
    @('test_720p.h264', 'plugin', '--max-frames', '3')
Invoke-Case 'pipeline_test (raw Annex-B, auto)' 'pipeline_test.exe' `
    @('test_720p.h264', '--max-frames', '3')
# Skips (exit 0) when no NVIDIA GPU/CUDA device is present.
Invoke-Case 'pipeline_test (raw Annex-B, cuda)' 'pipeline_test.exe' `
    @('test_720p.h264', '--backend', 'cuda', '--max-frames', '3')

if ($ffmpeg) {
    Invoke-Case 'decode_test (mp4 container)' 'decode_test.exe' `
        @($Container, 'plugin', '--max-frames', '3')
    Invoke-Case 'pipeline_test (mp4 container, auto)' 'pipeline_test.exe' `
        @($Container, '--max-frames', '3')
    Invoke-Case 'pipeline_test (mp4 container, cuda)' 'pipeline_test.exe' `
        @($Container, '--backend', 'cuda', '--max-frames', '3')
}

Write-Host ''
Write-Host '=== summary ==='
foreach ($r in $results) {
    $state = if ($r.Rc -eq 0) { 'PASS' } else { "FAIL (rc=$($r.Rc))" }
    Write-Host ("  {0,-32} {1}" -f $r.Case, $state)
}
$failed = @($results | Where-Object { $_.Rc -ne 0 })
if ($failed.Count -gt 0) {
    Write-Host "=== TESTS FAILED ($($failed.Count) of $($results.Count) cases) ==="
    exit 1
}
Write-Host "=== ALL TESTS PASSED ($($results.Count) cases) ==="
exit 0