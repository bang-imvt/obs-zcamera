# OBS ZCamera Plugin — Design Document

This document describes the architecture of **obs-zcamera**, an OBS Studio
plugin derived from `obs-ssp` that adds ZCAM (SSP) camera sources with **true
zero-copy hardware decoding** and an embedded camera-control dock.

## 1. Goals and non-goals

### Goals

- Same feature set as `obs-ssp`: mDNS discovery, SSP camera sources, OBS async
  audio/video, reconnect.
- **Zero-copy hardware decode** on Windows (CUDA/NVDEC or D3D11VA) and macOS
  (VideoToolbox/IOSurface): the GPU-decoded surface is handed to OBS's renderer
  **without a GPU→CPU memcpy and without a CPU→GPU upload**.
- A full camera-control dock that reproduces the `ZCamGuiOpen` UX inside OBS:
  camera rail, settings sidebar, PTZ, debug.
- Target Windows and macOS (the two platforms `obs-ssp` already ships).

### Rewrite stance

This project is a **rewrite**, not a fork. Only the genuinely good parts of the
reference `obs-ssp` are kept; the tangled monolith is replaced:

| Kept | Replaced |
| --- | --- |
| `ssp_connector` subprocess (isolates libssp) | `obs-ssp-source.cpp` monolith → `zc_source.cpp` |
| `ssp-mdns` discovery | `controller/cameracontroller` dead code → `cameractrl/` |
| `VFrameQueue` | `camera-status-manager` → inlined state |
| `ssp-client-iso` subprocess bridge | `ssp-dock`/`ssp-toolbar` (obs-browser/CEF) → `ui/zc_control_dock` |

The reference source used a **global static** reconnect guard that serialized
reconnects across all sources; the rewrite uses per-instance state
(`ZcSspSource`) so each source reconnects independently.

### Non-goals (v1)

- Linux (kept compiling where trivial, not supported).
- Firmware upgrade, user admin, network config, certificate management.
- Direct RTSP playback; the source stays a raw SSP client.

## 2. Background: how `obs-ssp` works today

Data flow in the source project (`obs-ssp`):

```
camera --TCP:9999 (SSP)--> ssp-connector subprocess
   --> stdout pipe --> SSPClientIso::ReceiveThread
   --> imf::SspH264Data --> VFrameQueue (copies ES packet)
   --> ssp_on_video_data
   --> ffmpeg_decode_video
        [ optional HW device, then av_hwframe_transfer_data GPU->CPU ]   <-- bottleneck
   --> obs_source_frame2 (CPU, NV12)
   --> obs_source_output_video2
   --> OBS async converter: CPU->GPU upload -> gs_texture -> render
```

The existing plugin already selects an FFmpeg hardware device
(`hw_priority[] = { QSV, CUDA, DXVA2, D3D11VA, VIDEOTOOLBOX, NONE }`,
`src/ffmpeg-decode.c:25-28`) but **immediately discards the GPU surface** with
`av_hwframe_transfer_data(decode->frame, out_frame, 0)` at `ffmpeg-decode.c:361`.
Every frame is copied GPU→CPU, and OBS later uploads it CPU→GPU. That is two
extra full-frame copies and the CPU is the staging buffer — high latency, high
CPU, and it defeats the hardware decoder.

### The `obs_source_frame2` limitation

`obs_source_frame2` is a **CPU pixel buffer** struct. The standard
`obs_source_output_video2` path always means "upload this CPU buffer to a GPU
texture." There is no way to get zero-copy through that API. Real zero-copy
requires the plugin to **render itself** from a GPU texture that aliases the
decoder's surface.

## 3. Zero-copy architecture

### 3.0 Is FFmpeg strictly required? (corrected decision)

**For the zero-copy path, FFmpeg is the decode engine — and that is the right
call, not a compromise.** Hardware-accelerated decode that works on **any GPU
Windows or macOS can recognize** is, in practice, FFmpeg's hwaccel: it
auto-detects the best device (Windows: CUDA/NVDEC, D3D11VA, QSV, DXVA2; macOS:
VideoToolbox) and covers NVIDIA, AMD, Intel and Apple Silicon. Writing a native
per-vendor decoder would only cover one GPU and is a much larger effort.

The mistake in the reference plugin is **not** that it uses FFmpeg — it's that
it immediately does `av_hwframe_transfer_data` (GPU→CPU) at
`ffmpeg-decode.c:361`, then OBS uploads CPU→GPU. Our fix keeps FFmpeg as the
hardware decoder **but never leaves the GPU**:

```
FFmpeg hwaccel decode -> AVFrame on GPU -> import() wraps surface in gs_texture
                                         (no transfer_data, no CPU->GPU upload)
   Windows: D3D11VA -> SHARED D3D11 texture (on OBS's device) -> gs_texture_open_shared()
   macOS:   VideoToolbox -> IOSurface -> gs_texture_create_from_iosurface()
```

So FFmpeg is required (it is the any-GPU decoder), and zero-copy is achieved by
importing its GPU surface directly into an OBS texture. `ENABLE_SW_DECODE`
controls the pure-CPU fallback; both hardware decode and the fallback use
FFmpeg.

### 3.1 Decode backend abstraction

A new module `src/hwdecode/` introduces a backend-agnostic interface that
decodes an H.264/HEVC access unit to a GPU texture and hands it to OBS.

```cpp
// src/hwdecode/hw_decode.h
enum class ZcHwBackend { Auto, Cuda, D3d11, VideoToolbox, Software };

struct ZcDecodedFrame {
  gs_texture_t *texture;   // OBS GPU texture aliasing the decoder surface
  bool owned;              // texture ownership (we created it)
  uint32_t width, height;  // coded size
  enum video_format format;// NV12 / P010 etc (matches gs texture format)
  enum video_colorspace cs;
  enum video_range_type range;
  uint64_t timestamp_ns;   // wall-clock or PTS-based
  void *priv;              // backend-private ref (CUDA graph resource / IOSurface)
};

class ZcHwDecoder {
 public:
  // Returns the strongest backend actually available (or Software).
  static ZcHwBackend Detect(const char *codec_name, obs_source_t *source);
  virtual bool Decode(const uint8_t *data, size_t size, bool is_keyframe,
                      ZcDecodedFrame *out) = 0;
  virtual void Release(ZcDecodedFrame *frame) = 0;
  virtual ~ZcHwDecoder() = default;
};
```

Each backend is a subclass. The source picks one at init based on the
`ssp_recv_hw_accel` property (`Auto/CUDA/D3D11VA/VideoToolbox/Software`),
falling back to Software when the requested backend is unavailable. The old
`ffmpeg_decode_video` CPU path is retained as the `Software` backend and as a
fallback when no zero-copy device is usable.

### 3.2 Source render path

The source changes from a pure async-frame source to one that can render its
own GPU texture. Two complementary mechanisms:

1. **Video render callback** — the source registers a `video_render` handler
   and drops `OBS_SOURCE_ASYNC_VIDEO` from its output flags in zero-copy mode.
   Each GPU frame is stored in the connection, then drawn with
   `obs_source_draw(...)` inside `video_render`.
2. **Timestamp-ordered queue** — because decode and render run on different
   threads, decoded `ZcDecodedFrame`s are pushed into a small ring buffer keyed
   by timestamp. `video_render` pops the frame whose timestamp is newest but
   `<= now` and draws it, then releases the older ones. This keeps A/V sync and
   avoids queueing.

The **Audio** path is unchanged (`obs_source_output_audio`), so audio stays
async while video becomes render-driven. This is the same split used by other
OBS hardware-decoding plugins (e.g. the `decklink` and `nvfbc` plugins).

**Fallback (as built):** the source never registers `OBS_SOURCE_ASYNC_VIDEO` —
video is always drawn by `video_render`. When no zero-copy backend is
available (or `import()` fails, see §9), the decoder is the **Software**
backend: it produces CPU frames which the source converts to RGBA with swscale
and uploads into a texture it draws. A connection therefore uses exactly one
mode at a time, and it can switch from hardware to software mid-stream but
never back (until the next reconnect).

### 3.3 Windows backend (CUDA / D3D11 interop)

The Windows path decodes with FFmpeg's CUDA (NVDEC) hwaccel. NVDEC does **not**
produce a D3D11 texture: the `AVFrame` is `AV_PIX_FMT_CUDA`, with `data[0]` and
`data[1]` being `CUdeviceptr` to the luma and chroma planes of an NV12 image in
CUDA device memory, and `linesize[]` their pitch-aligned strides. Nothing there
is sampleable by OBS, and only a CUDA context can touch it.

Zero-copy steps (as implemented in `hw_decode_cuda_win.cpp`):

1. Create the NVDEC decoder on the CUDA device whose adapter LUID matches the
   D3D11 device OBS renders with (`cuDeviceGetLuid` vs
   `DXGI_ADAPTER_DESC.AdapterLuid`). A mismatch means interop cannot work, so
   the backend is not selected at all.
2. The importer creates its **own** CUDA context on that device — not FFmpeg's —
   which keeps `cuda.h` out of the build and the graphics thread out of the
   decode thread's context.
3. Allocate two CUDA-registered D3D11 plane textures (`R8_UNORM` for luma,
   `R8G8_UNORM` for chroma) plus a **shared** BGRA render-target texture
   (`D3D11_RESOURCE_MISC_SHARED`).
4. Per frame: `cuGraphicsMapResources` → `cuMemcpy2D` from the NVDEC plane
   pointers into the two plane textures' CUDA arrays.
5. Convert both planes to the shared BGRA texture with a full-screen
   pixel-shader pass (BT.709/BT.601 + range from the stream's metadata).
6. Open the shared handle in OBS with `gs_texture_open_shared` and draw it in
   `video_render` — the same call the D3D11VA path ends with, so the render path
   has no backend-specific code.

This has **no CPU copy**: the only movement is within-GPU, device memory →
CUDA array → (shader) → shared texture.

The obvious shortcut — register a D3D11 **NV12** texture with CUDA and let
NVDEC's plane copies fill both of its planes — was measured and does not work:
`cuGraphicsD3D11RegisterResource` accepts the texture but exposes only its luma
plane (`cuGraphicsSubResourceGetMappedArray` returns
`CUDA_ERROR_INVALID_VALUE` for subresource 1, and the luma-only array rejects
`dstY >= height`), and a cross-plane `CopySubresourceRegion` never delivers
chroma. Hence the two-plane + shader design above.

**D3D11VA alternative:** where CUDA is unavailable, D3D11VA decode surfaces are
`ID3D11Texture2D` array elements already on the D3D11 device; they are blitted
to a shared BGRA texture with an `ID3D11VideoProcessor` (§3.6). This path has no
CUDA dependency and is the default for `Auto`.

### 3.4 macOS backend (VideoToolbox / IOSurface)

macOS decode uses FFmpeg's `AV_HWDEVICE_TYPE_VIDEOTOOLBOX`. VideoToolbox
produces `CVPixelBuffer`s that are **IOSurfaces** (`IOSurfaceGetID`).

Zero-copy steps:

1. Retrieve the decoded `CVImageBufferRef` from the `AVFrame`
   (`CVPixelBufferGetIOSurface` / IOSurface port).
2. Wrap the IOSurface into an OBS `gs_texture_t` using OBS's
   `gs_texture_create_from_iosurface` (OBS Metal/OpenGL import an IOSurface by
   its `IOSurfaceID`).
3. Draw that texture in `video_render`.

The IOSurface is GPU memory; OBS's Metal/OpenGL backend imports it directly, so
there is no CPU copy. Color space/range come from the VT pixel buffer's
CVImageBuffer attachments (BT.709/BT.2020/HLG/PQ), passed to OBS via the frame
or rendered with the proper matrix.

### 3.5 Color handling

- Zero-copy frames carry `video_colorspace` and `video_range_type` derived from
  the decoder (HDR-aware, matching the HDR work already ported into
  `ffmpeg-decode.c`).
- The render path sets these on the `obs_source_frame`-equivalent metadata or
  applies them in the draw call so OBS's color pipeline converts correctly.
- The CPU fallback continues to use `VIDEO_CS_DEFAULT` /
  `VIDEO_RANGE_PARTIAL` as today.

### 3.6 Implementation status and known gaps

Implemented:

- FFmpeg-hwaccel decode with a **D3D11VA** frames context on Windows; the
  decoder really does produce `AV_PIX_FMT_D3D11` hardware frames (verified
  standalone by `tests/decode_test.cpp`, mode `plugin`).
- **Windows zero-copy import for D3D11VA** (`hw_decode_d3d11_win.cpp`).
  D3D11VA hands back one element of an NV12 texture *array*
  (`AVFrame::data[1]` is the slice index) that OBS cannot open: the array
  element is not a 2D texture, `MiscFlags` is 0 (the
  `AVD3D11VAFramesContext.MiscFlags = D3D11_RESOURCE_MISC_SHARED` request is
  not applied to the decoder's own pool, so `GetSharedHandle()` fails), and
  libobs-d3d11's `ConvertDXGITextureFormat()` has no `DXGI_FORMAT_NV12` case.
  The importer therefore blits the NV12 slice into a **shared BGRA** texture
  with an `ID3D11VideoProcessor` and opens *that* with
  `gs_texture_open_shared` — the mechanism FFmpeg's own `d3d11va_transfer_data`
  and obs-browser use. Verified by `tests/pipeline_test.exe` (default
  backend), including a staging read-back that proves the blit ran.
- **Windows zero-copy import for CUDA/NVDEC** (`hw_decode_cuda_win.cpp`).
  Verified on hardware (RTX 4090 D) that CUDA's D3D11 interop cannot be used
  the obvious way: `cuGraphicsD3D11RegisterResource` accepts a D3D11 NV12
  texture but exposes **only its luma plane** (`cuGraphicsSubResourceGetMappedArray`
  fails for subresource 1), and a cross-plane `CopySubresourceRegion` does not
  deliver chroma. The importer instead copies NVDEC's two device-memory planes
  (`cuMemcpy2D`) into CUDA-registered `R8_UNORM`/`R8G8_UNORM` D3D11 textures and
  converts them to the same shared BGRA texture with a full-screen
  pixel-shader pass — still GPU-only, still no CPU copy. The CUDA context is
  created by the importer on the adapter whose LUID matches the OBS D3D11
  device, so `cuda.h` stays out of the build (`nvcuda.dll` is loaded
  dynamically) and the graphics thread never shares a context with the decode
  thread. Verified by `tests/pipeline_test.exe --backend cuda`.
- macOS VideoToolbox import through `gs_texture_create_from_iosurface` (code
  path only; not validated on hardware).
- Render-driven output (`video_render` + `gs_draw_sprite`); the CPU fallback
  converts to RGBA and draws a texture instead of using
  `obs_source_output_video2`.
- Mid-stream fallback (§9): a failed `import()` drops the GPU path for that
  connection and rebuilds the decoder as **Software**.
- Source `id "zcamera_source"`; no `OBS_SOURCE_AUDIO` because the SSP audio
  path is not implemented (declaring it would only add a silent track).

Known gaps (open before 1.0):

- **10-bit/HDR**: the D3D11VA frames context is built with
  `sw_format = AV_PIX_FMT_NV12`, and the CUDA importer assumes 8-bit NV12
  planes, so P010/10-bit streams are unsupported.
- **Windows CUDA import falls back to Software** when no CUDA device matches
  the OBS D3D11 adapter's LUID (e.g. an iGPU drives OBS and the NVIDIA GPU is
  not the render device): CUDA-D3D11 interop is then impossible by
  construction. CUDA is therefore an explicit choice, not part of `Auto`.
- **QSV/DXVA2** have no import path and are never selected.
- **Audio**: `obs_source_output_audio` is not wired.
- **Camera-control dock**: the dock covers the camera list, the settings pane,
  the PTZ pad and the status/debug log. It does not render its own preview — the
  operator watches OBS's preview of the `zcamera_source` — and record/photo are
  not wired to any control in v1.
- **WebSocket**: text frames only (no fragmentation); binary frames ignored.

## 4. Camera control architecture

### 4.1 Overview

A new module `src/cameractrl/` ports the `ZCamGuiOpen` camera control protocol
into C++. It talks to the camera's HTTP API (port 80, with 443/HTTPS origin
negotiation) and subscribes to the WebSocket notification socket (port 81).

```
QThread (network)                     UI thread (Qt dock)
+----------------+   Qt signals   +-----------------------+
| ZcHttpTransport|───────────────>| ZcCameraClient        |
|  (Digest auth, |                |  (state store)        |
|   origin nego, |                +-----------+-----------+
|   FIFO queue)  |                             | emits
+----------------+                             v
+----------------+                      ZcCameraControlDock (QWidget)
| ZcWsNotifier   |----> events ---->   - rail / settings / PTZ
+----------------+
```

### 4.2 HTTP transport (`zc_http_transport`)

Direct port of `ZCamGuiOpen` `http-transport.ts`:

- **Origin negotiation**: try `http:80` → `https:443` → `https:80`; lock the
  first that answers; follow redirects; remember final origin.
- **Digest auth**: parse the non-standard `WWW-Authenticate-X` challenge,
  compute `MD5`/`MD5-sess` with `qop`, `nc`, `cnonce`; cache the `Authorization`
  header; transparent single retry on a fresh `401 (stale=true)`. Preserve the
  session cookie `account` and `x-auth-token` for WS upgrade.
- **Serialization FIFO**: the camera's HTTP server is single-threaded and drops
  concurrent requests; one request in flight per camera.
- Timeouts: 20 s default, 10 s for WS handshake, 1 s for reboot/shutdown.
- Uses Qt `QNetworkAccessManager` (already a dependency of `obs-ssp`).
- Origin negotiation is synchronous (nested `QEventLoop`, like the reference
  client) with a short per-candidate timeout, so it blocks its caller for up to
  ~4.5 s when the camera is unreachable. The request path itself is fully
  asynchronous (the FIFO slot is only released from the `finished` handler).
- Errors normalized: timeout, HTTP 401/403/other, parse, network.

### 4.3 WebSocket notifications (`zc_ws_notifier`)

Port of `ZCamGuiOpen` `notifications.ts` + `camera-state.ts`:

- Connect to `ws(s)://<address>:81/` with Digest + cookie + `x-auth-token`.
- Reconnect with backoff; on drop, mark state stale and fall back to HTTP
  refresh (never send commands during recovery).
- Dispatch on the `what` field. Normalized events:

| Raw `what` | Emitted signal |
| --- | --- |
| `camera_status` | `statusChanged` |
| `RecStarted` / `RecStoped` | `recordingStarted` / `recordingStopped` |
| `RecUpdateDur` / `RecUpdateRemain` | `recordingDuration` / `recordingRemaining` |
| `RecordingFile` | `recordingFile` |
| `ModeChanged` | `modeChanged` |
| `TempUpdate` | `temperature` |
| `UpdateStreamSetting` | `streamSettingsChanged` |
| `ConfigChanged` | `configChanged(key, value)` |
| `UpdateAfArea` | `afAreaChanged(x,y,w,h)` |
| `basicInfo` | `basicInfo(snapshot)` |
| `Shutdown` / `hb` | `shutdown` / `heartbeat` |

Uses Qt `QWebSocket` (adds `Qt::WebSockets` to the build).

### 4.4 Settings engine (`zc_settings_engine`)

Port of `ZCamGuiOpen` `settings-engine.ts` + `settings-schema.ts`:

- Group catalog loaded via `/ctrl/getbatch?catalog=<name>`.
- Per-key reads via `/ctrl/get?k=<key>`.
- Writes via `/ctrl/set?<key>=<value>` (GET with query params).
- Firmware response shape: `{ code, desc, key, type, ro, value, opts[], min,
  max, step }`; `type 1` choice, `type 2` range, `type 3` text; `ro` read-only;
  `code === -1` unsupported (row hidden).
- Post-write dependency map (`DEPENDENT_GROUPS` from `ZCamGuiOpen`) re-reads
  affected groups (e.g. `compose_mode` → `record`,`video`; `iso` →
  `exposure`; `lut` → `exposure`).
- The engine exposes a catalog-driven `QAbstractItemModel` so the Qt dock can
  render rows generically.

### 4.5 Camera client (`zc_camera_client`)

Port of `ZCamGuiOpen` `camera-client.ts` + `camera-state.ts`:

- `connect` (probe `/info`, login, session), `disconnect`, `sessionPing`.
- `/ctrl/mode` transitions (`to_rec`, `to_standby`, `exit_standby`).
- Record start/stop/query/remain, card present, photo capture
  (`/ctrl/rec?action=...`, `/ctrl/card?action=present`,
  `/ctrl/still?action=cap`).
- Lens: zoom/focus move/stop (`/ctrl/lens?action=...`), one-push AF
  (`/ctrl/af`).
- PTZ: move/stop/home/reset/limit (`/ctrl/pt?action=...`), presets
  (`/ctrl/preset?action=...`).
- Stream settings (`/ctrl/stream_setting?action=query|...&index=stream1`).
- State store holding current values of the telemetry keys, mode, temperature,
  recording state, card/remain. The record/photo/lens/stream commands above are
  part of the client API but no longer have a control in the dock.

### 4.6 Telemetry keys (not wired)

`zc_settings_schema.cpp` still exposes `hudKeys()` (a port of `ZCamGuiOpen`'s
`HUD_KEYS`), and the client keeps the telemetry in its state store, but the dock
no longer renders a HUD: the embedded preview and the REC/photo controls were
removed, so nothing consumes these values in v1. The `refreshHud()` logic is
therefore not implemented.

## 5. Module layout

```
src/
  obs-ssp.cpp / obs-ssp.h         plugin entry, registration, Tools-menu action
  zc_source.cpp                   source: backend selection, video_render + CPU path
  ffmpeg-decode.c/.h              CPU decode (Software backend), ENABLE_SW_DECODE
  VFrameQueue.*                   (unchanged)
  ssp-mdns.*                      discovery (unchanged)
  ssp-client-iso.*                subprocess bridge (unchanged)
  hwdecode/
    hw_decode.h                   backend interface (new)
    hw_decode.c                   detect/resolve/create + destroy (always built)
    hw_decode_ffmpeg.h/.cpp       FFmpeg-hwaccel decode + surface import
                                  (ENABLE_ZERO_COPY)
    hw_decode_d3d11_win.cpp       D3D11VA video-processor import (Windows)
    hw_decode_cuda_win.cpp        CUDA/NVDEC plane + shader import (Windows)
    hw_decode_sw.h/.cpp           software backend = old CPU path
                                  (ENABLE_SW_DECODE)
  cameractrl/
    zc_http_transport.h/.cpp      Digest + origin nego + FIFO (new)
    zc_ws_client.h/.cpp           minimal RFC 6455 client over QTcpSocket (new)
    zc_ws_notifier.h/.cpp         WS notifications (new)
    zc_settings_engine.h/.cpp     settings model (new)
    zc_camera_client.h/.cpp       client + state store (new)
    zc_settings_schema.h/.cpp     catalog/key table (new)
  ui/
    zc_control_dock.h/.cpp        dock widget (camera rail + Settings/PTZ tabs
                                  + status bar; rail/settings/PTZ classes live
                                  here, ZcHud is unused)
    zc_debug_panel.h/.cpp         debug log
ssp_connector/                    (unchanged subprocess)
lib/ssp                           (unchanged libssp headers)
cmake/, CMakePresets.json         (extended: Qt Network, D3D11 on win,
                                   VideoToolbox/IOSurface on mac)
```

Not yet present (planned in earlier revisions of this document, still open):
`hw_decode_vt_mac.mm` (the macOS import currently lives inline in
`hw_decode_ffmpeg.cpp`). The rail, settings pane and PTZ pad are implemented as
`ZcCameraRail` / `ZcSettingsPanel` / `ZcPtzPad` inside `zc_control_dock.cpp`;
`ZcHud` is declared but no longer instantiated. There is no `src/controller/`:
`controller/cameracontroller`, `camera-status-manager` and `ssp-controller`
were deleted with the rewrite.

## 6. Build system

Reuses the `obs-ssp` CMake skeleton. Additions:

- **Windows**: link `d3d11.lib`, `dxgi.lib`, and `d3dcompiler.lib` (the CUDA
  importer compiles its conversion shader at runtime). No CUDA toolkit is
  required: `nvcuda.dll` is loaded with `LoadLibrary`/`GetProcAddress` and only
  the handful of driver entry points used are declared locally, so CUDA builds
  work on machines without the toolkit and simply falls back where it is
  absent. FFmpeg deps already from
  `obs-deps` (add `avcodec`, `avutil` — retained). Add `Qt6::WebSockets`.
- **macOS**: link `VideoToolbox.framework`, `CoreVideo.framework`,
  `IOSurface.framework`, `Metal.framework`. Add `Qt6::WebSockets`. Keep the
  `ssp-connector` x86_64-only subprocess + `arch -x86_64` shim.
- `ENABLE_ZERO_COPY` option (default ON). `ENABLE_CAMERA_CONTROL_DOCK` (default
  ON; requires Qt + frontend API).
- MSVC/clang flags for the new backend sources; `.mm` compiled with the
  Objective-C++ rules on macOS.

## 7. Registration and versioning

- Keep `obs_module_load` / `obs_module_unload` / `obs_module_name` /
  `obs_module_description`; version gated to OBS ≥ 31 (matches `obs-ssp`).
- The source is registered with `id "zcamera_source"` and the display name from
  `ZCameraPlugin.SourceName` ("ZCamera Source"). The original plan was to keep
  `id "ssp_source"`; that was dropped because `obs-ssp` can be installed in the
  same OBS profile and two plugins cannot own one source id — the unique id
  avoids the collision and keeps the two sources distinguishable in a scene.
  Consequence: a scene saved against an earlier `ssp_source` id of this plugin
  needs its source re-added once.
- A source property picks the decode backend (Auto / D3D11VA on Windows /
  VideoToolbox on macOS / Software).
- Add a Tools-menu action to open the camera-control dock
  (`obs_frontend_add_tools_menu_qaction`), replacing the old browser-toolbar
  entry.

## 8. Concurrency and lifecycle

- Decode thread (VFrameQueue callback) → `ZcHwDecoder::Decode`, push `ZcDecodedFrame`
  into the render ring buffer with a mutex.
- Graphics thread (`video_render`) → pop + `obs_source_draw` + release.
- Camera control runs on its own `QThread`; all signals cross via
  `Qt::QueuedConnection` to the UI thread.
- On source stop: drain ring buffer, release all GPU frames, destroy decoder
  inside `obs_enter_graphics()`/`obs_leave_graphics()` so texture destruction
  happens on the GPU context thread.
- Reconnect retains the existing detached-thread logic but the decoder is torn
  down and recreated per connection so the backend re-initializes on the right
  device.

## 9. Error handling and fallback

- If the requested zero-copy backend cannot initialize (no GPU / no compatible
  codec / interop failure), fall back to **Software** and log a warning.
- If zero-copy rendering fails mid-stream (`import()` cannot open the decoded
  surface), drop the GPU path and rebuild the decoder as Software for the same
  connection.
- Camera control errors are surfaced in the dock status bar and debug panel;
  mutating commands never silently retry (matches `ZCamGuiOpen`).

Status: both fallbacks are implemented in `zc_source.cpp`
(`zc_hw_decoder_create` falls back at creation, and the import failure is
handled in `video_render`). The transport retries a request once on a fresh
`401` challenge after re-computing the Digest response.

## 10. Security

- Digest credentials kept in memory only; never written to disk or logs; never
  put in URLs.
- Debug panel sanitizes authorization headers, cookies, and credentials.
- All camera traffic to one camera is serialized (the camera's embedded server
  drops concurrent requests).
