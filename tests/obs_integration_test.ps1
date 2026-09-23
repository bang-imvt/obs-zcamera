# Runs the obs-zcamera integration test inside a REAL OBS Studio, against a
# real camera on the network, without any UI interaction.
#
# How it drives OBS without clicking:
#   1. the freshly built plugin is deployed into OBS's user plugin folder
#   2. OBS is launched on its own scene collection/profile ("zc-autotest") with
#      --minimize-to-tray --disable-shutdown-check, so the user's own collection
#      and any first-run/crash dialogs are avoided
#   3. obs_ws_driver.mjs connects to the obs-websocket server that OBS 28+
#      ships on 127.0.0.1:4455, creates the zcamera_source input exactly like
#      the UI would, and reads the RENDERED source back with
#      GetSourceScreenshot - so the assertions are made on frames OBS produced
#      through the plugin's own video_render, not on a synthetic harness
#   4. the run also records a short clip so the file can be probed with ffprobe
#   5. the plugin's own log lines are checked for the zero-copy import marker
#
# Shutdown: obs-websocket 5.5.6 has no Quit request (checked against the list it
# reports itself), and --minimize-to-tray turns a window close into "hide", so
# this script stops the OBS process it started instead of asking OBS to exit. It
# waits for the process AND the websocket port to be released before the next
# run, otherwise a dying instance swallows the next launch's requests.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tests\obs_integration_test.ps1 `
#       [-CameraIp 192.168.10.174] [-Backend auto|d3d11va|cuda|all] `
#       [-RecordSeconds 5] [-SkipDeploy] [-ForceStopObs] [-KeepObsRunning]

param(
    [string]$CameraIp = '192.168.10.174',
    # 'all' runs this same script once per real backend and aggregates the result.
    [ValidateSet('auto', 'd3d11va', 'cuda', 'all')][string]$Backend = 'auto',
    [double]$RecordSeconds = 5,
    # Per-stage budget for "the decoder came up" and "3 good frames were
    # captured". Generous because this runs on shared machines where a graphics
    # request can stall for tens of seconds.
    [double]$ReadyTimeout = 60,
    [switch]$SkipDeploy,
    [switch]$ForceStopObs,
    [switch]$KeepObsRunning
)

$ErrorActionPreference = 'Stop'

# The plugin resolves its backend when the input is created, so covering every
# backend means restarting OBS between them: re-run this script once per backend
# and aggregate the exit codes.
if ($Backend -eq 'all') {
    if ($KeepObsRunning) {
        Write-Host 'note: -KeepObsRunning is ignored with -Backend all (each run needs a clean OBS)'
    }
    $summary = @()
    foreach ($b in @('auto', 'd3d11va', 'cuda')) {
        $childArgs = @('-Backend', $b, '-CameraIp', $CameraIp,
                       '-RecordSeconds', "$RecordSeconds", '-ReadyTimeout', "$ReadyTimeout")
        if ($SkipDeploy) { $childArgs += '-SkipDeploy' }
        if ($ForceStopObs) { $childArgs += '-ForceStopObs' }
        Write-Host ''
        Write-Host "########## backend=$b ##########"
        & powershell -ExecutionPolicy Bypass -File $PSCommandPath @childArgs
        $summary += [pscustomobject]@{ Backend = $b; Rc = $LASTEXITCODE }
    }
    Write-Host ''
    Write-Host '=== all backends ==='
    foreach ($row in $summary) {
        $state = if ($row.Rc -eq 0) { 'PASS' } else { "FAIL (rc=$($row.Rc))" }
        Write-Host ("  {0,-9} {1}" -f $row.Backend, $state)
    }
    if (@($summary | Where-Object { $_.Rc -ne 0 }).Count -gt 0) { exit 1 }
    exit 0
}

$Root      = $PSScriptRoot                              # ...\tests
$Proj      = Split-Path -Parent $Root                   # repo root
$Build     = Join-Path $Proj 'build_x64\RelWithDebInfo'
$DepsBin   = Join-Path $Proj '.deps\obs-deps-2024-09-12-x64\bin'
$ObsBin    = 'C:\Program Files\obs-studio\bin\64bit'
$ObsExe    = Join-Path $ObsBin 'obs64.exe'
# OBS also scans a machine-wide plugin dir, and this install loads the plugin
# from there — so deploy where OBS actually reads it.
$PluginBin = Join-Path $env:ProgramData 'obs-studio\plugins\obs-zcamera\bin\64bit'
$UserIni   = Join-Path $env:APPDATA 'obs-studio\user.ini'
$ScenesDir = Join-Path $env:APPDATA 'obs-studio\basic\scenes'
$ProfilesDir = Join-Path $env:APPDATA 'obs-studio\basic\profiles'
$LogsDir   = Join-Path $env:APPDATA 'obs-studio\logs'
$OutDir    = Join-Path $Root 'obs_out'
$LogPath   = ''
$Collection = 'zc-autotest'
$Profile    = 'zc-autotest'
$WsPort     = 4455

function Assert-Path($p, $what) {
    if (-not (Test-Path -LiteralPath $p)) { throw "$what not found: $p" }
}

function Test-TcpPort($hostName, $port, $timeoutMs) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect($hostName, $port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne($timeoutMs)) { return $false }
        $client.EndConnect($iar)
        return $true
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

# A killed OBS keeps its websocket port bound for a moment while it tears down.
# Waiting only for the port to answer is not enough: the driver would then attach
# to that dying instance, and every request through it (GetSourceScreenshot,
# StartRecord) fails with a timeout or "undefined". So wait until no obs64 is
# running AND nothing answers on the websocket port before launching our own.
function Wait-ObsShutdown($timeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $left = @(Get-Process obs64 -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0 -and -not (Test-TcpPort '127.0.0.1' $WsPort 500)) {
            return $true
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# OBS writes its log to basic\logs\<date> <time>.txt; take the newest one
# written since the given time.
function Get-NewestObsLog($since) {
    if (-not (Test-Path -LiteralPath $LogsDir)) { return '' }
    $log = Get-ChildItem -LiteralPath $LogsDir -Filter '*.txt' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $since } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($log) { return $log.FullName }
    return ''
}

# ---- preflight -----------------------------------------------------------
Assert-Path $ObsExe 'OBS Studio'
Assert-Path (Join-Path $Build 'obs-zcamera.dll') 'built plugin (run cmake --build build_x64 first)'
Assert-Path (Join-Path $Root 'obs_ws_driver.mjs') 'obs_ws_driver.mjs'
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    throw 'node is required (the obs-websocket driver runs on it)'
}

Write-Host "=== obs-zcamera OBS integration test (backend=$Backend camera=$CameraIp) ==="

# The camera must be reachable: port 9999 is the SSP stream the source pulls.
if (-not (Test-TcpPort $CameraIp 9999 3000)) {
    Write-Host "SKIP: camera $CameraIp:9999 is not reachable; nothing to test."
    exit 0
}
Write-Host "camera $CameraIp is reachable on SSP port 9999"

$obsProcs = @(Get-Process obs64 -ErrorAction SilentlyContinue)
if ($obsProcs.Count -gt 0) {
    if (-not $ForceStopObs) {
        Write-Host 'OBS is already running. Close it (or pass -ForceStopObs) and rerun.'
        exit 1
    }
    Write-Host 'stopping the running OBS (-ForceStopObs)'
    $obsProcs | Stop-Process -Force
}
if (-not (Wait-ObsShutdown)) {
    throw 'a previous OBS still owns the websocket port; refusing to launch on top of it'
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---- prepare the dedicated scene collection and profile -------------------
# --collection/--profile only *switch to* an existing collection/profile, so
# both have to exist on disk beforehand. A collection is any *.json under
# basic\scenes whose "name" field is the collection name; a profile is a
# directory under basic\profiles holding a basic.ini whose [General] Name is
# the profile name.
Write-Host "=== preparing the '$Collection' collection and '$Profile' profile ==="
$collectionFile = Join-Path $ScenesDir "$Collection.json"
New-Item -ItemType Directory -Force -Path $ScenesDir | Out-Null
@"
{
    "current_scene": "",
    "current_program_scene": "",
    "scene_order": [],
    "name": "$Collection",
    "sources": [],
    "groups": [],
    "quick_transitions": [],
    "transitions": [],
    "saved_projectors": [],
    "current_transition": "Fade",
    "transition_duration": 300,
    "preview_locked": false,
    "scaling_enabled": false,
    "scaling_level": 0,
    "scaling_off_x": 0.0,
    "scaling_off_y": 0.0,
    "virtual-camera": {
        "type2": 3
    },
    "modules": {},
    "resolution": {
        "x": 1920,
        "y": 1080
    },
    "version": 2
}
"@ | Set-Content -LiteralPath $collectionFile -Encoding ascii

# x264 rather than NVENC: the decoder is already using the GPU, and a software
# encoder cannot fail for lack of an encoder session.
$recordPath = $OutDir -replace '\\', '\\'
$profileDir = Join-Path $ProfilesDir $Profile
New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
@"
[General]
Name=$Profile

[Output]
Mode=Simple
FilenameFormatting=%CCYY-%MM-%DD %hh-%mm-%ss

[SimpleOutput]
FilePath=$recordPath
RecFormat2=mkv
RecQuality=Stream
VBitrate=6000
ABitrate=160
Preset=veryfast
RecEncoder=obs_x264
RecAudioEncoder=aac
RecTracks=1

[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=30
ScaleType=bicubic
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial

[Audio]
SampleRate=48000
ChannelSetup=Stereo
"@ | Set-Content -LiteralPath (Join-Path $profileDir 'basic.ini') -Encoding ascii

# Restore the user's last-used collection/profile afterwards: launching with
# --collection/--profile makes OBS persist them, which would otherwise change
# what their next manual OBS session opens.
$userIniBackup = "$UserIni.zctest.bak"
if (Test-Path -LiteralPath $UserIni) {
    Copy-Item -LiteralPath $UserIni -Destination $userIniBackup -Force
}

$obsProc = $null
$driverRc = 1
$logRc = 1
$probeRc = 1
$recorded = ''

try {
    # ---- deploy ----------------------------------------------------------
    if (-not $SkipDeploy) {
        Write-Host '=== deploying the plugin into OBS ==='
        New-Item -ItemType Directory -Force -Path $PluginBin | Out-Null
        Copy-Item -LiteralPath (Join-Path $Build 'obs-zcamera.dll') `
            -Destination $PluginBin -Force

        # FFmpeg is linked by the plugin (avcodec/avutil + swscale for the CPU
        # fallback), and avcodec itself needs swresample. Copy the whole set
        # from one tree so the imports and the DLLs always match.
        foreach ($dll in 'avcodec-61.dll', 'avutil-59.dll', 'swscale-8.dll',
                         'swresample-5.dll') {
            $from = Join-Path $DepsBin $dll
            if (Test-Path -LiteralPath $from) {
                Copy-Item -LiteralPath $from -Destination $PluginBin -Force
            } else {
                Write-Host "  warning: $dll not found in .deps\\bin"
            }
        }

        # SSPClientIso resolves the connector next to obs-zcamera.dll.
        $connector = Join-Path $PluginBin 'ssp-connector.exe'
        if (-not (Test-Path -LiteralPath $connector)) {
            $fromConn = Join-Path $Proj 'build_x64\ssp_connector\RelWithDebInfo\ssp-connector.exe'
            Assert-Path $fromConn 'ssp-connector.exe'
            Copy-Item -LiteralPath $fromConn -Destination $PluginBin -Force
        }
        if (-not (Test-Path -LiteralPath (Join-Path $PluginBin 'libssp.dll'))) {
            $fromSsp = Join-Path $Proj 'build_x64\_deps\libssp-src\lib\win_x64_vs2017\libssp.dll'
            if (Test-Path -LiteralPath $fromSsp) {
                Copy-Item -LiteralPath $fromSsp -Destination $PluginBin -Force
            } else {
                Write-Host '  warning: libssp.dll not deployed and not found in the build tree'
            }
        }
        $deployed = Get-Item -LiteralPath (Join-Path $PluginBin 'obs-zcamera.dll')
        Write-Host ("deployed obs-zcamera.dll {0} bytes, {1}" -f `
            $deployed.Length, $deployed.LastWriteTime)
    } else {
        Write-Host '=== deploy skipped (-SkipDeploy) ==='
    }

    # ---- launch OBS ------------------------------------------------------
    # OBS has no --logfile switch; it always writes basic\logs\<date>.txt, so
    # remember the time and pick the newest log afterwards.
    Write-Host '=== launching OBS (own collection/profile, minimised) ==='
    $launchTime = Get-Date
    $obsArgs = @(
        '--collection', $Collection,
        '--profile', $Profile,
        '--minimize-to-tray',
        '--disable-shutdown-check',
        '--disable-updater',
        '--verbose'
    )
    # FFmpeg writes through its default av_log callback, i.e. to stderr — not to
    # the OBS log. Redirect it to a file so decoder-side failures ("Failed setup
    # for format d3d11: hwaccel initialisation returned error") stay readable.
    $obsErrFile = Join-Path $OutDir 'obs_stderr.txt'
    $obsOutFile = Join-Path $OutDir 'obs_stdout.txt'
    $obsProc = Start-Process -FilePath $ObsExe -ArgumentList $obsArgs `
        -WorkingDirectory $ObsBin -PassThru `
        -RedirectStandardError $obsErrFile -RedirectStandardOutput $obsOutFile

    $wsDeadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $wsDeadline) {
        if (Test-TcpPort '127.0.0.1' $WsPort 1000) { break }
        if ($obsProc.HasExited) { break }
        Start-Sleep -Milliseconds 500
    }
    if (-not (Test-TcpPort '127.0.0.1' $WsPort 1000)) {
        Write-Host "obs-websocket never came up on port $WsPort; tail of the OBS log:"
        $LogPath = Get-NewestObsLog $launchTime
        if ($LogPath) {
            Get-Content -LiteralPath $LogPath -Tail 25 | ForEach-Object { "  $_" }
        }
        throw "obs-websocket unreachable (OBS exited: $($obsProc.HasExited))"
    }
    Write-Host "obs-websocket is up on 127.0.0.1:$WsPort"

    # ---- drive OBS through obs-websocket ---------------------------------
    Write-Host '=== injecting the source through obs-websocket ==='
    $backendId = switch ($Backend) { 'cuda' { 1 } 'd3d11va' { 2 } default { 0 } }
    Push-Location $Root
    try {
        & node (Join-Path $Root 'obs_ws_driver.mjs') `
            '--ip' $CameraIp '--backend' "$backendId" '--out' $OutDir `
            '--ready-timeout' "$ReadyTimeout" '--record-seconds' "$RecordSeconds"
        $driverRc = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    Write-Host "obs_ws_driver.mjs exit code: $driverRc"

    if (-not $KeepObsRunning -and $obsProc -and -not $obsProc.HasExited) {
        $quitWatch = [Diagnostics.Stopwatch]::StartNew()
        if ($obsProc.WaitForExit(15000)) {
            Write-Host ('OBS exited on its own in {0:N1}s' -f $quitWatch.Elapsed.TotalSeconds)
        } else {
            Write-Host 'OBS did not quit within 15s; stopping it'
            Stop-Process -Id $obsProc.Id -Force
        }
    }

    # ---- assert on the plugin's own log lines ----------------------------
    Write-Host '=== checking the OBS log for the zero-copy import ==='
    $logRc = 0
    $LogPath = Get-NewestObsLog $launchTime
    if (-not $LogPath) {
        Write-Host '  [FAIL] OBS wrote no log file'
        $logRc = 1
    } else {
        Write-Host "  log: $LogPath"
        Copy-Item -LiteralPath $LogPath -Destination (Join-Path $OutDir 'obs.log') -Force
        $log = Get-Content -LiteralPath $LogPath -Raw

        # Show what the plugin itself said; on a failure this is the first
        # thing worth reading.
        $pluginLines = @($log -split "`r?`n" | Where-Object { $_ -match '\[obs-zcamera\]' })
        if ($pluginLines.Count -gt 0) {
            Write-Host "  plugin log ($($pluginLines.Count) lines):"
            $pluginLines | Select-Object -First 20 | ForEach-Object { "    $_" }
        } else {
            Write-Host '  plugin logged nothing'
        }

        # Both importers log their readiness lazily, on the first frame they
        # actually import - so this line only exists if a decoded GPU frame
        # reached OBS.
        $readyMarker = if ($Backend -eq 'cuda') {
            'cuda import ready: NVDEC NV12'
        } else {
            'd3d11 video processor ready: NV12'
        }
        if ($log -match [regex]::Escape($readyMarker)) {
            Write-Host "  [PASS] log shows the zero-copy import became ready ('$readyMarker')"
        } else {
            Write-Host "  [FAIL] log has no '$readyMarker' marker"
            $logRc = 1
        }

        $badMarkers = @(
            'GPU surface import failed',
            'could not be imported into OBS',
            'no decoder available',
            'zero-copy decode unavailable',
            'd3d11 import:',
            'cuda import:'
        )
        # The "import: ..." prefixes also cover the informational lines, so only
        # lines that contain a failure word count.
        $logLines = $log -split "`r?`n"
        $hits = @($logLines | Where-Object {
            $line = $_
            ($badMarkers | Where-Object {
                $line -match [regex]::Escape($_) -and
                $line -match 'fail|error|could not|unavailable|unsupported'
            }).Count -gt 0
        })
        if ($hits.Count -eq 0) {
            Write-Host '  [PASS] no obs-zcamera import failures in the log'
        } else {
            Write-Host '  [FAIL] obs-zcamera reported import failures:'
            $hits | ForEach-Object { "    $_" }
            $logRc = 1
        }
    }
    Write-Host "log check exit code: $logRc"

    # ---- decoder-side noise that never reaches the OBS log ----------------
    if (Test-Path -LiteralPath $obsErrFile) {
        $errLines = @(Get-Content -LiteralPath $obsErrFile -ErrorAction SilentlyContinue |
            Where-Object { $_.Trim() -ne '' })
        if ($errLines.Count -gt 0) {
            Write-Host "  stderr from OBS ($($errLines.Count) lines):"
            $errLines | Select-Object -First 30 | ForEach-Object { "    $_" }
        }
    }

    # ---- probe the recorded clip -----------------------------------------
    $resultPath = Join-Path $OutDir 'zc_obs_result.json'
    if (Test-Path -LiteralPath $resultPath) {
        $recorded = (Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json).recorded
    }
    Write-Host '=== probing the clip OBS recorded ==='
    $probeExe = (Get-Command ffprobe -ErrorAction SilentlyContinue).Source
    if (-not $probeExe) {
        Write-Host '  [SKIP] ffprobe not on PATH; recorded clip not probed'
        $probeRc = 0
    } elseif (-not $recorded -or -not (Test-Path -LiteralPath $recorded)) {
        Write-Host "  [FAIL] no recorded clip to probe (path='$recorded')"
        $probeRc = 1
    } else {
        $probeRc = 0
        $raw = & $probeExe -v error -select_streams v:0 `
            -show_entries stream=codec_name,width,height,nb_frames `
            -show_entries format=duration -of json $recorded | Out-String
        $probe = $raw | ConvertFrom-Json
        $stream = $probe.streams | Select-Object -First 1
        $duration = [double]$probe.format.duration
        Write-Host ("  clip: {0} bytes, codec={1} {2}x{3} frames={4} duration={5:N1}s" -f `
            (Get-Item -LiteralPath $recorded).Length, $stream.codec_name,
            $stream.width, $stream.height, $stream.nb_frames, $duration)
        if ($stream.width -gt 0 -and $stream.height -gt 0) {
            Write-Host '  [PASS] recorded clip carries a real video track'
        } else {
            Write-Host '  [FAIL] recorded clip has no usable video stream'
            $probeRc = 1
        }
        if ($duration -ge ($RecordSeconds - 1.5)) {
            Write-Host '  [PASS] recorded clip is as long as requested'
        } else {
            Write-Host ("  [FAIL] clip is shorter than requested ({0:N1}s)" -f $duration)
            $probeRc = 1
        }

        # The clip must contain the picture, not just a stream: decoding three
        # frames and reading their luma catches a source that renders black
        # (which a codec/duration probe happily accepts).
        $ffmpegExe = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
        if (-not $ffmpegExe) {
            Write-Host '  [SKIP] ffmpeg not on PATH; clip content not checked'
        } else {
            $luma = & $ffmpegExe -v error -ss 1 -i $recorded -frames:v 3 `
                -vf signalstats,metadata=print:file=- -f null - 2>&1 |
                Select-String 'YMAX=(\d+)' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
            $peak = ($luma | Measure-Object -Maximum).Maximum
            if ($peak -ge 32) {
                Write-Host ("  [PASS] recorded clip shows the picture (frame luma max={0})" -f $peak)
            } else {
                Write-Host ("  [FAIL] recorded clip is blank (frame luma max={0})" -f $peak)
                $probeRc = 1
            }
        }
    }
    Write-Host "ffprobe check exit code: $probeRc"
} finally {
    if (-not $KeepObsRunning -and $obsProc -and -not $obsProc.HasExited) {
        Stop-Process -Id $obsProc.Id -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $userIniBackup) {
        Copy-Item -LiteralPath $userIniBackup -Destination $UserIni -Force
        Remove-Item -LiteralPath $userIniBackup -Force
    }
}

Write-Host ''
if ($driverRc -ne 0 -or $logRc -ne 0 -or $probeRc -ne 0) {
    Write-Host "=== OBS INTEGRATION TEST FAILED (driver=$driverRc log=$logRc probe=$probeRc) ==="
    exit 1
}
Write-Host '=== OBS INTEGRATION TEST PASSED ==='
exit 0