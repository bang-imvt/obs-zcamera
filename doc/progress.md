# OBS ZCamera Plugin — Implementation Progress

Living document updated as implementation proceeds. Each completed step should
be committed separately.

## Decisions in effect

- Baseline: `obs-ssp` (SSP camera source) — used as a **reference**, then
  core rewritten.
- Target: Windows + macOS; Linux compile-only.
- Decode: true zero-copy hardware decode; **no** `av_hwframe_transfer_data` in
  the zero-copy hot path. FFmpeg's hwaccel **is** the decode engine, so
  `ENABLE_ZERO_COPY` requires FFmpeg (`avcodec`/`avutil`).
  - Windows: D3D11VA shared texture → `gs_texture_open_shared` (the `Auto`
    default), and CUDA/NVDEC → CUDA-registered planes → shader → shared BGRA →
    `gs_texture_open_shared` (explicit choice).
  - macOS: VideoToolbox → IOSurface → `gs_texture_create_from_iosurface`
    (code path present, not yet validated on hardware).
  - Fallback: Software via FFmpeg, behind `ENABLE_SW_DECODE`.
- Camera control: direct port of the `ZCamGuiOpen` protocol (HTTP Digest,
  origin negotiation, WS port 81, settings catalogs).
- Camera control UI: OBS Qt dock mirroring `ZCamGuiOpen` (rail / settings
  sidebar / PTZ / debug). No embedded preview or REC/photo controls — the
  operator watches OBS's own preview of the source.
- Auto-framing: **the panel is complete and shown** as its own dock tab for a
  camera whose `/info` reports auto framing. It speaks the camera's whole
  `/ctrl/framing` API; like the camera's web page it exposes the switches,
  dropdowns and spinners, though that page's framing-box canvas, FOV
  composition preview and EzFraming click-to-centre/zoom are not reproduced.
  `ZcControlDock` inserts the tab through `kFramingPanelVisible` in
  `src/ui/zc_control_dock.cpp` (currently `true`).
- WebSocket: minimal RFC 6455 client over `QTcpSocket` (bundled OBS Qt has no
  `Qt6WebSockets`).

## Rewrite status (2026-09-16)

Kept from the reference: `ssp_connector` subprocess, `ssp-mdns`, `VFrameQueue`,
`ssp-client-iso`, `lib/ssp`. Replaced:
- `obs-ssp-source.cpp` monolith → `src/zc_source.cpp` (`ZcSspSource` class,
  per-instance reconnect, zero-copy `video_render` + CPU fallback).
- `controller/cameracontroller`, `camera-status-manager`, `ssp-controller` →
  `src/cameractrl/`.
- `ssp-dock`/`ssp-toolbar` (obs-browser/CEF) → `src/ui/zc_control_dock`.
- CMake: native decode primary; FFmpeg behind `ENABLE_SW_DECODE`; hwdecode
  interface uses neutral `zc_codec_id` (no FFmpeg type leakage).

## Hardware decode milestone (2026-09-16)

Implemented real zero-copy hardware decode (commit `5e670a3`). FFmpeg's hwaccel
is the decode engine (any GPU Windows/macOS recognize), and the GPU surface is
imported into OBS with **no** `av_hwframe_transfer_data`:

- `src/hwdecode/hw_decode_ffmpeg.cpp` — auto-detect device, keep AVFrame on GPU,
  `import()` wraps it in a `gs_texture`.
- `src/hwdecode/hw_decode_d3d11_win.cpp` — D3D11VA → SHARED texture on OBS's own
  device (`gs_get_device_obj`) → `gs_texture_open_shared`; frames ctx forces
  `D3D11_RESOURCE_MISC_SHARED`.
- macOS uses `gs_texture_create_from_iosurface`.
- `zc_source.cpp` `video_render` imports + draws + releases on the graphics
  thread; pending-frame ownership is per-instance; CPU fallback unchanged.
- Builds clean on Windows x64 with the FFmpeg-hw path.

## Steps

| Step | Status | Commit | Notes |
| --- | --- | --- | --- |
| 0. Baseline fork of obs-ssp | Completed | — | Sources, build infra, `ssp_connector`, `lib/ssp`, mdns, obs-browser headers copied. |
| 1. UI spec | Completed | — | `doc/ui-spec.md` |
| 2. Design document | Completed | — | `doc/design.md` |
| 3. Implementation plan | Completed | — | `doc/plan.md` |
| 4. Progress document | Completed | — | this file |
| 5. CMake scaffold (renamed to obs-zcamera, Qt::WebSockets, D3D11/VT deps) | Completed | — | buildspec/bundleId updated; mdns + obs-browser copied |
| 6. hwdecode interface + Software backend | Completed | — | `src/hwdecode/hw_decode.h`, `hw_decode.c`, `hw_decode_sw.cpp` |
| 7. Windows CUDA/D3D11 + macOS VT backend stubs | In progress | — | D3D11VA wired (step 17) and CUDA/NVDEC wired (step 18); `hw_decode_vt_mac.mm` still absent — the macOS import lives inline in `hw_decode_ffmpeg.cpp` and is unvalidated on hardware. |
| 8. HTTP transport + WS notifier | Completed | — | `zc_http_transport.*`, `zc_ws_notifier.*` |
| 9. Settings engine + client + schema | Completed | — | `zc_settings_engine.*`, `zc_camera_client.*`, `zc_settings_schema.*` |
| 10. Camera control dock UI | Completed | — | `src/ui/*` (dock, rail, settings, PTZ, debug); the HUD/preview pane and the REC/photo controls were later removed (step 21). |
| 11. Wire dock into plugin entry | Completed | — | Tools menu action in `obs-ssp.cpp`; locale strings |
| 12. hwdecode GPU interop + source `video_render` integration | Completed | `5e670a3`, `3b967c0` | decoder built lazily in `onMeta` (dims known → SHARED frames ctx); `video_render` imports/draws/releases; CPU path converts to RGBA and draws a texture. |
| 13. Review fixes: lifecycle, fallback, Digest, WS, build flags | Completed | — | see the 2026-09-17 change log entry |
| 14. Packaging + CI | Pending | — | |
| 15. Cleanup + build verification | Completed | — | Standalone test binaries build and pass (`tests/build_tests.ps1`); the plugin builds via CMake in `build_x64` (`RelWithDebInfo\obs-zcamera.dll`). |
| 16. Automated tests (standalone + Qt) | Completed | — | `tests/cameractrl_tests.cpp`, `tests/decode_test.cpp`, `tests/pipeline_test.cpp`, `tests/build_tests.ps1` |
| 17. Windows D3D11 zero-copy import (NV12 array slice → shared BGRA blt) | Completed | — | `src/hwdecode/hw_decode_d3d11_win.cpp`; verified end-to-end on a local file by `tests/pipeline_test.cpp`. |
| 18. Windows CUDA/NVDEC zero-copy import (planes → shader → shared BGRA) | Completed | — | `src/hwdecode/hw_decode_cuda_win.cpp`; verified end-to-end by `tests/pipeline_test.cpp --backend cuda`; offered as an explicit backend in the source properties. |
| 19. Serialize hardware decode with the OBS graphics thread | Completed | `b9d31c0` | FFmpeg lock callbacks take the OBS graphics mutex, hard decoding is single-threaded, and the graphics lock is always taken before the decoder mutex; the freeze is gone. |
| 20. 10-bit aware decoders (P010) | Completed | `be0bba5`, `ebc0173` | SPS bit-depth sniffing picks the frames context; the software fallback accepts P010/I010. |
| 21. Dock rework: mDNS-refreshed rail, click-to-add source, PTZ-only tab | Completed | — | The preview/HUD and the REC/Photo buttons are gone; `ZcHud` is now dead code. |
| 22. Settings panel renders the camera's own reply | Completed | — | `zc_settings_engine` caches per-key metadata; the panel builds combo/slider/line-edit rows from it and writes changes back. |
| 23. Rail ↔ source lifecycle, PTZ tab gating, preset list | Completed | — | The rail shows which cameras are already sources and owns the add; the PTZ tab appears only for a pan/tilt head; the preset page grid replaces the old 3 fixed buttons. |
| 24. Rail labels carry the address; PTZ traces; exit crash fixed | Completed | — | Rail entries read `<name>  (<ip>)` so they can be matched against the `ZCamera <ip>` source names; the PTZ pane grew the camera's second slot family (traces, `/ctrl/ptrace`); the dock's double delete at unload — the 0xC0000409 exit crash — is fixed with a `QPointer`. |
| 25. Rail lists source-only cameras, offline ones marked | Completed | — | The rail is built from discovery **plus** the `ZCamera <ip>` sources OBS holds, so a camera whose mDNS record is missing is still selectable; a source-only row that does not answer is drawn muted, labelled `(offline)`, and a click on it leaves no client behind (nothing is operable). |
| 26. PTZ settings group gated; decoder follows a resolution change | Completed | — | The Settings group list is rebuilt from the camera's `/info`, so a camera without a pan/tilt head no longer offers the `ptz` group at all. `onMeta` now also rebuilds the decoder when the coded dimensions change, not only on a codec change: the D3D11VA frames context bakes its size in at create time, so a resolution change on a live connection previously kept the old surfaces while `get_width/height` reported the new size. |
| 27. Stream settings: API pinned down and rendered | Completed | — | The camera's streaming API is not the `/ctrl/get` key set the spec assumed. One document per encoder index, read with `?action=query&index=<index>`; the same request **writes** when it carries value parameters (`action=set` is rejected with `code:-1`), and `bitrate` reads in kbps but writes in bps. The dock's Stream group shows the index read-only and offers `bitrate`/`gop_n` sliders bounded by the camera's own `maxBitrate`/`maxGop`, repainting every write from the camera's reply. Contract recorded in `ui-spec.md` §2. |
| 28. Creating a source raises the dock | Completed | — | libobs's `source_create` for a `zcamera_source` raises the control dock, armed only after `OBS_FRONTEND_EVENT_FINISHED_LOADING` so loading a collection does not steal focus (`ui-spec.md` §1 rule L12). |
| 29. Stream: the firmware's index, and codec/resolution/fps | Completed | — | Which index exists is the firmware's (`app_stream1` on newer firmware, `stream1` otherwise) and is discovered from the reply, not assumed. `app_stream1` carries `videoEncoderList`/`resolutionList`/`fpsList` and their `*Ro` flags, so codec, resolution and frame rate become combos there; the write parameters are `venc`, `width`+`height` and `fps`, none of which match the document's field names. On older firmware the three are read-only rows, i.e. the camera's own settings. |
| 30. Rail rows on two lines, per-row add chip, stream bar | Completed | — | Each rail row is painted by a `QStyledItemDelegate` as two lines (name above address) and carries its own action chip — `＋` while the camera is not a source, a gold check once it is — so the bottom "＋ Add to Source" button is gone (`ui-spec.md` §1 L5/L6). The stream controls left the group list entirely and became a two-row bar pinned at the top of the Settings tab (§2). `bitDepthKnown_`/`streamTenBit_` are now reset in `start()`, so a reconnect — exactly what a codec or resolution write causes — re-sniffs the depth instead of reusing the previous connection's. |

## Change log

- 2026-09-15: Forked the `obs-ssp` codebase into `obs-zcamera` at
  `D:\imvt_git\obs-zcamera`. Copied the plugin sources, `ssp_connector`
  subprocess, `lib/ssp` headers, and the full `cmake/` build infrastructure.
- 2026-09-15: Analyzed the zero-copy bottleneck: the inherited
  `ffmpeg-decode.c` selects a hardware device but immediately does
  `av_hwframe_transfer_data` (GPU→CPU) at `ffmpeg-decode.c:361`, then OBS
  re-uploads CPU→GPU. Documented the fix in `doc/design.md`.
- 2026-09-15: Inventoried the `ZCamGuiOpen` camera control protocol
  (HTTP Digest auth, origin negotiation, `/ctrl/*` settings catalogs, WebSocket
  port 81 events, PTZ endpoints) for the C++ port.
- 2026-09-15/16: Wrote the four planning documents: `doc/ui-spec.md`,
  `doc/design.md`, `doc/plan.md`, `doc/progress.md`.
- 2026-09-16: Scaffolded the CMake build (renamed project to `obs-zcamera`,
  updated `buildspec.json`/bundleId, added Qt::WebSockets + D3D11/VideoToolbox
  deps, copied the mdns + obs-browser dependencies).
- 2026-09-16: Implemented the zero-copy decode interface (`src/hwdecode/`):
  `ZcHwDecoder` backend abstraction, resolve/create factory, Software fallback
  backend, and Windows (CUDA/D3D11VA) + macOS (VideoToolbox) backend skeletons
  that currently fall back to Software.
- 2026-09-16: Implemented the camera control layer (`src/cameractrl/`): HTTP
  transport (Digest auth, origin negotiation, FIFO serialization), WebSocket
  port-81 notifier (event normalization, reconnect/backoff), settings engine
  and schema (all catalog groups + dependency map), and the camera client
  facade (status, record/photo, lens, AF, PTZ, stream).
- 2026-09-16: Implemented the camera-control dock UI (`src/ui/`): three-panel
  dock (camera rail / preview+HUD / settings sidebar), transparent HUD with
  telemetry chips, settings panel rendering from the schema, PTZ pad, and a
  sanitized debug panel.
- 2026-09-16: Wired the control dock into the plugin entry point
  (`obs_module_load` Tools-menu action) and added en-US/zh-CN locale strings.
- 2026-09-16: Wrote `README.md` and copied the GPL-2.0 LICENSE.
- 2026-09-17: Second review pass — fixed the remaining findings:
  - `zc_source.cpp`: decoder lifetime is guarded by `decoderMutex_` and
    connection teardown by `lifecycleMutex_`, so `stop()`, `onMeta()` and
    `import()` can no longer free the decoder or the CPU texture under the
    render/decode thread; `av_frame_free`/`gs_texture_destroy` no longer run
    while holding `frameMutex_` on a decode thread (that could deadlock against
    the graphics context); the reconnect thread no longer self-joins and
    `stop()` wakes it through the condition variable instead of blocking for up
    to 15 s; the connection is torn down again after the reconnect join.
  - Implemented the design §9 mid-stream fallback: a failed surface import
    rebuilds the decoder as Software for that connection.
  - Fixed the CPU fallback: per-plane allocated sizes are tracked (a height
    change at an unchanged stride previously overflowed the plane buffer) and
    the sws context is rebuilt on a resolution change, not only a format change.
  - `hw_decode_ffmpeg.cpp`: the OBS D3D11 device is AddRef'd before being handed
    to FFmpeg (FFmpeg's `device_free` releases it); `av_new_packet` failure is
    handled.
  - `zc_http_transport`: implemented the Digest handshake
    (`WWW-Authenticate-X` parsing, MD5/MD5-sess, qop/nc/cnonce, one transparent
    401 retry, session-cookie + `x-auth-token` capture) and fixed replies being
    processed *before* `finished` (status code and body were always empty, so
    no camera request ever saw its response).
  - `zc_ws_client` / `zc_ws_notifier`: the old socket's signals are detached
    before `abort()`, `Sec-WebSocket-Accept` is verified, the upgrade now
    carries the Digest Authorization / cookie / `x-auth-token`, extra header
    lines no longer emit a stray CRLF before the blank line, and no reconnect is
    scheduled after `stop()`.
  - Build: swscale and the CPU frame path are behind `ENABLE_SW_DECODE`, and
    `hw_decode.c` is always compiled with its FFmpeg calls behind
    `ENABLE_ZERO_COPY`, so the `ENABLE_SW_DECODE=OFF` and no-FFmpeg builds link
    again.
  - Dropped `OBS_SOURCE_AUDIO` (audio is not implemented) and moved the source
    name to `ZCameraPlugin.SourceName` ("ZCamera Source").
- 2026-09-19: Third review pass + automated tests.
  - `zc_http_transport.cpp` `parseChallenge()`: digest state is now reset only
    when the nonce actually changes. A repeated challenge with the **same**
    nonce used to restart `nc_` at 1 and clear the cached `Authorization`
    (RFC 2617 requires `nc` to increase per nonce, and clearing the header
    left the WebSocket upgrade unauthenticated). Found by the new
    repeat-challenge test below; verified by temporarily reverting the fix
    (that test then failed, 41/1) and restoring it (42/0).
  - `zc_http_transport.cpp` `negotiateOrigin()`: origin probes now emit
    `requestLogged`, so a failed negotiation leaves a trace instead of
    silently blocking the caller for `3 x timeoutMs`.
  - `zc_source.cpp`: `start()` no longer leaks the decoder when it is
    re-created (`installDecoder(nullptr)` before the rebuild) and resets
    `forceSoftware_`; `onDisconnected()` joins a still-joinable
    `reconnectThread_` before reassigning it (assigning over a joinable
    `std::thread` calls `std::terminate()`).
  - `hw_decode_ffmpeg.cpp`: the `logged_formats` / `logged_non_hw_frame` /
    `import_unsupported` latches are per-decoder members instead of
    function-local statics (the statics were shared across instances and
    racy).
  - Added `tests/`: `cameractrl_tests.cpp` (settings-schema consistency +
    HTTP Digest end-to-end against a fake camera server over loopback),
    `decode_test.cpp` (D3D11VA decode probe), and `build_tests.ps1`
    (builds + runs both).
- 2026-09-19: Fourth pass — the zero-copy import is now implemented, and a
  local-file pipeline test verifies it before the source is used in OBS.
  - `hw_decode_d3d11_win.cpp`: **rewritten**. The D3D11VA decoder hands back one
    element of an NV12 *texture array* (`avf->data[1]` is the slice),
    `IDXGIResource::GetSharedHandle` fails on that surface, and neither
    workaround was possible: `gs_texture_open_shared` opens a non-array 2D view,
    and libobs-d3d11's `ConvertDXGITextureFormat` has no `DXGI_FORMAT_NV12`
    case (the SRV would be created as `DXGI_FORMAT_UNKNOWN`). So the decoded
    slice is now blitted by an `ID3D11VideoProcessor` (NV12 array slice →
    non-array `B8G8R8A8_UNORM`) into a texture created with
    `D3D11_RESOURCE_MISC_SHARED`, and *that* texture is what
    `gs_texture_open_shared` opens. The blit is entirely on the GPU: no
    `av_hwframe_transfer_data`, no CPU round trip. The processor, enumerator,
    output texture/views and shared handle are cached per decoder (recreated on
    a resolution change) and released by the decoder's `destroy()`.
  - `hw_decode_ffmpeg.h`: declares `struct zc_hw_d3d11_import` and
    `zc_hw_d3d11_import_free()` (opaque; the state is owned by the decoder).
  - `hw_decode_ffmpeg.cpp`: the D3D11 branch calls the slice-aware importer and
    reports `VIDEO_FORMAT_BGRA` (the blit converts); the cache is freed in
    `ffmpeg_destroy()`.
  - `zc_source.cpp`: removed the dead `VFrameQueue` wiring — `queue_` was
    constructed, given a frame callback and started, but `enqueue()` was never
    called (the client callback already calls `onVideoData()` directly), so
    feeding it would have double-decoded every frame. Also removed the
    vestigial `streamIndex_`/`PROP_STREAM_INDEX`: never exposed as a property,
    it only forced a spurious reconnect on every settings update.
  - Added `tests/pipeline_test.cpp`: drives the **real** pipeline
    (`hw_decode.c` + `hw_decode_ffmpeg.cpp` + `hw_decode_d3d11_win.cpp`, with
    only six OBS symbols shimmed — `blog`, `blogva`, `obs_enter_graphics`,
    `obs_leave_graphics`, `gs_get_device_obj`, plus a real
    `gs_texture_open_shared`) against a **local video file**, and asserts the
    import produced a shareable, single-2D BGRA texture at the coded
    dimensions, that a read-back holds non-blank pixels, and that the graphics
    enter/leave calls balance.
  - `tests/cameractrl_tests.cpp`: Part B no longer reports this machine's
    loopback port shortage as a transport failure. When the fake server was
    proven reachable but *no* transport probe ever arrived
    (`requestsFromTransport == 0`), the OS refused the client socket before an
    HTTP exchange could start — that is the environment, so it is `[SKIP]`, not
    a bug. The previous guard degraded only when a bare Qt request *also*
    failed, so a run in which the bare probe happened to get a free port turned
    a machine fault into 14 false `[FAIL]`s.
- 2026-09-20: Fifth pass — CUDA/NVDEC zero-copy import implemented and verified
  (the last backend that only fell back to Software on Windows).
  - Measured first (throwaway probe, since deleted): on this machine (RTX 4090 D)
    CUDA and the OBS D3D11 device share an adapter LUID, so interop is possible,
    but `cuGraphicsD3D11RegisterResource` on a D3D11 **NV12** texture exposes
    only its luma plane — `cuGraphicsSubResourceGetMappedArray(res, 1, 0)`
    returns `CUDA_ERROR_INVALID_VALUE`, the luma-only array rejects
    `dstY >= height` (`CUDA_ERROR_INVALID_VALUE`), and a cross-plane
    `CopySubresourceRegion` produced zero chroma. "Let CUDA write NVDEC's NV12
    straight into a D3D11 NV12 texture" is therefore not possible.
  - Added `src/hwdecode/hw_decode_cuda_win.cpp`: copies NVDEC's two
    device-memory planes (`cuMemcpy2D`, device → CUDA array) into
    CUDA-registered `R8_UNORM` (luma) and `R8G8_UNORM` (chroma) D3D11 textures,
    then converts both to a `D3D11_RESOURCE_MISC_SHARED` `B8G8R8A8_UNORM`
    texture with a runtime-compiled full-screen pixel-shader pass (BT.709/601 +
    range taken from the frame's metadata), and finally
    `gs_texture_open_shared()` — the same call the D3D11VA path ends with, so
    `video_render` has no backend-specific code. The whole path is GPU-only.
  - The driver API is loaded from `nvcuda.dll` with `LoadLibrary` +
    `GetProcAddress` and declared locally, so no CUDA toolkit, headers or import
    library are build dependencies, and a machine without an NVIDIA driver just
    sees the backend resolve to Software. `cuMemcpy2D` (with
    `dstMemoryType = CU_MEMORYTYPE_ARRAY`) is used deliberately: `nvcuda.dll`
    exports no `cuMemcpy2DToArray` — that is a runtime-API name.
  - The importer creates its **own** CUDA context (via `cuCtxCreate`) on the
    adapter whose LUID matches the OBS D3D11 device, instead of borrowing
    FFmpeg's. That keeps `<libavutil/hwcontext_cuda.h>` (and therefore `cuda.h`)
    out of the build, gives the importer a clean lifetime, and stops the
    graphics thread from sharing a context with the decode thread.
  - `hw_decode_ffmpeg.cpp`: selects the CUDA ordinal by LUID and passes it to
    `av_hwdevice_ctx_create`, requires a match (CUDA-D3D11 interop cannot work
    without one), adds the `AV_PIX_FMT_CUDA` import branch, and frees the CUDA
    importer first in `destroy()`. The D3D11VA-only OBS-device binding is now
    explicitly guarded to `AV_HWDEVICE_TYPE_D3D11VA` (it used to run for any
    type, which would have hand-initialised a CUDA hw device context).
  - `hw_decode_ffmpeg.h`: the new CUDA entry points are declared with struct tags
    (`struct AVFrame`, `struct ID3D11Device`) so the header stays valid C —
    `hw_decode.c` includes it.
  - `zc_source.cpp`: "CUDA (NVDEC)" is now offered in the decode-backend list.
  - CMake: `hw_decode_cuda_win.cpp` added and `d3dcompiler` linked (Windows).
  - `tests/pipeline_test.cpp`: new `--backend auto|d3d11va|cuda` flag (auto still
    expects D3D11VA; cuda expects `ZC_HW_CUDA` and skips when unavailable), the
    read-back now also reports the largest per-pixel distance from grey, and a
    new assertion fails the run if the converted frame carries luma only —
    which is exactly what a lost chroma plane would look like.
  - `tests/build_tests.ps1`: compiles the CUDA source, links `d3dcompiler`, and
    runs the pipeline test a second time with `--backend cuda`, folding its exit
    code into the result.
  - Removed the throwaway `tests/cuda_probe.*` used for the measurements above.
- 2026-09-21: Sixth pass — the freeze is fixed, the dock loses its preview, and
  the settings panel finally renders the camera's own reply.
  - **Freeze root cause** (`b9d31c0`): the D3D11 immediate context was being
    driven from two threads at once — OBS's graphics thread (which the source's
    `video_render` runs on) and FFmpeg's decode thread. The driver-level
    deadlock that produced the hang is gone once the decode path is serialized
    against the graphics thread: the FFmpeg `lock`/`unlock` callbacks now take
    `obs_enter_graphics`/`obs_leave_graphics`, hardware decoding is pinned to a
    single thread, and the graphics lock is always acquired before the decoder
    mutex (fixing an ABBA ordering that could deadlock the other way). The
    stream also self-heals: a silent stall is detected and reconnects.
  - **10-bit decoders** (`be0bba5`, `ebc0173`): the decoder sniffs the SPS
    bit depth and builds a matching frames context (`P010`), and the software
    fallback accepts `P010`/`I010` CPU frames instead of rejecting them.
  - **Dock rework**: the embedded preview/HUD pane and the `● REC` / `◉ Photo`
    buttons were removed — the operator watches the OBS preview, not a second
    copy of the stream inside the dock, and record/photo are not wired in v1.
    `ZcHud` is now dead code. `doc/ui-spec.md` and `doc/design.md` were updated
    to match (the REC/Photo section and the HUD chips are gone from the spec).
  - **Settings panel showed no rows**: three separate faults, all fixed in
    `src/ui/zc_control_dock.cpp` and `src/cameractrl/zc_settings_engine.cpp`:
    - the per-row `QHBoxLayout` was added with `QBoxLayout::addItem()` instead
      of `addLayout()`, so the rows were counted by `buildRows()` but never
      entered the container's layout — the pane stayed blank. The rows now use
      `addLayout()`.
    - `ZcSettingsEngine` stored the raw JSON value with `QJsonValue::toString()`,
      which is empty for a JSON **number**. Range settings (type 2) arrive as
      numbers, so their sliders got an empty initial value. A new `jsonScalar()`
      normalises string/number/bool to text for both `readKey` and `applyBatch`.
    - switching groups left the previous group's controls visible, because the
      clear loop deleted the nested layout but not the widgets it owned. A
      recursive `clearRows()` now deletes the widgets too.
    - `ZcControlDock` is resized to 1040x640 on construction: at the default
      width the 170 px label + 200 px control of a row did not fit the settings
      pane and the controls were clipped out of view.
  - The engine now caches per-key metadata (`defs_`) and per-catalog key order
    (`groupKeys_`), exposes `definition()` / `groupKeys()` / `groupRefreshed`,
    and `ZcCameraClient` forwards them, so the panel can build combo/slider/
    line-edit rows without re-reading the camera. Unsupported (`code -1`) and
    read-only keys are hidden.
  - `tests/cameractrl_tests.cpp`: the `getbatch` fixture now returns the range
    value as a JSON **number** (it used to be a string, which is why the bug
    slipped through) and asserts both `value("brightness")` and
    `definition("brightness")`. `tests/build_tests.ps1` mocs and compiles
    `zc_settings_engine.cpp` into the test binary.
  - **A source added from the dock never connected** (fixed after the settings
    work): `zc_source_getdefaults()` wrote its keys with `obs_data_set_*`
    instead of `obs_data_set_default_*`. OBS merges the settings given to
    `obs_source_create()` into the source's data object and only *then* calls
    `get_defaults` (`obs_source_create_internal` → `obs_source_init_context` →
    `get_defaults`), so the defaults overwrote them. The dock's "Add to Source"
    passes `zc_source_ip` at create time, so the source was persisted with an
    empty address (`"zc_source_ip": ""` in the scene collection), `start()`
    returned at once on the empty IP, and OBS showed nothing — while the
    integration test kept passing, because it configured the source with an
    explicit `SetInputSettings` (an update, not a create) and so never
    exercised the create path. `get_defaults` now uses the `_default_` setters,
    and `addCameraToSource()` re-applies the IP with `obs_source_update()` when
    the source already exists, so sources already saved with an empty address
    heal when the camera is clicked again. `obs_ws_driver.mjs` now passes the
    settings to `CreateInput` and asserts they survive `get_defaults` before
    the explicit update can mask a regression.
- 2026-09-21: Seventh pass — the rail now owns the source lifecycle, the PTZ tab
  is gated on the camera's own capability, and the PTZ pane grows a real preset
  list. The lifecycle rules are normative in `doc/ui-spec.md` (§1, L1–L10), so
  this entry only records what changed in code.
  - **Rail ↔ source lifecycle.** The dock used to add a source on every click
    of a rail entry, and nothing ever showed whether a camera was already a
    source, so a second click could stack duplicates and a removed source left
    the rail lying. A rail entry is now a *selection*, not a create:
    - `ZcCameraRail` gained `setAddedHosts()` / `selectedHost()` and a gold
      check badge on the icon of any camera that is already a source
      (`zcCameraIcon(bool added)`). `setAddedHosts()` updates the icons **in
      place** instead of rebuilding the list, so refreshing presence never
      drops the current selection or the open connection.
    - Source identity is the name `ZCamera <ip>`, resolved live through
      `obs_get_source_by_name()` — never cached (`sourceNameForHost()`,
      `isCameraAdded()`). "Remove" stays plain OBS: the dock has no delete
      action at all (`addCameraToSource()` is the only writer).
    - The **Add to Source** button is enabled only while a *not yet added*
      camera is selected, and shows `Already in scene` otherwise
      (`updateAddButton()`); clicking a rail entry no longer creates anything,
      it only connects and refreshes presence.
    - Presence is refreshed from libobs's own global signals —
      `source_create` / `source_destroy` / `source_rename` on
      `obs_get_signal_handler()` — marshalled to the GUI thread with
      `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`
      (`onObsSourceSignal()`), with the existing 2500 ms rail refresh as the
      backstop. `obs-frontend-api` has no source event of its own. The
      connections are dropped in the destructor.
  - **PTZ tab gating.** The tab was always present. It is now added to and
    removed from the `QTabWidget` by `updatePtzTab()` from
    `ZcCameraClient::cameraInfoChanged()`, which fires after `/info` is read on
    connect and after disconnect. The decision itself is a pure function,
    `zc::cameraSupportsPtz(info)` in `zc_settings_schema`: the
    `feature.lens_ctrl_ptz` flag is authoritative, and a model (or
    `cameraName`) containing "ptz" is the fallback for firmware that omits the
    flag — the flag is what makes the user's `P2-R1-18x_010360` / `e2ptz` show
    the tab. `ZcCameraClient` exposes `cameraModel()` / `cameraSupportsPtz()`.
  - **Preset list.** The PTZ pad's three fixed preset buttons are replaced by
    `ZcPresetPanel`: 10 pages x 10 slots (`kPresetPerPage` / `kPresetPages`),
    each slot a thumbnail + name tile, plus Recall / Add-Replace / Delete /
    Rename and a RECALL SPEED row (unit 0 = speed, 1 = time in ms). Slot
    indexes are 0-based on the wire and 1-based in the label (`Preset 001`),
    matching the camera's own web UI, which was read again to pin the contract:
    `Api.preset` (`controller.min.js:13605`), `_setupPresetPageIndex`
    (`r = page*10 + slot`), `_paddedZero`, and
    `/app_data/preset/thm_${padded(n)}.jpg?act=thm`. Renames are clamped to
    18 characters and have `&`/`=` stripped before they go out.
  - **The preset wire shape is now a pure function too**:
    `zc::presetQuery(action, index, extra)` and `zc::presetThumbnailPath(index)`
    live in `zc_settings_schema`, and `zc_camera_client.cpp` calls them, so the
    request shape and the thumbnail URL are unit-tested without a camera. A
    rename must reach the wire percent-encoded (`new_name=A%26B`), and a 3 s
    preset duration must be sent as `preset_time=3000`.
  - `tests/cameractrl_tests.cpp`: new Part A2 asserts the gating against the
    user's real `/info` reply (`lens_ctrl_ptz: true` -> tab shown), the model
    fallback, a fixed `e2-m4` with `lens_ctrl_ptz: false` -> tab hidden, the
    thumbnail padding, and the query encoding above.
    `cameractrl_tests` went from 54 to **70 passed, 0 failed**.
- 2026-09-21 (same pass, two live OBS bugs reported by the operator):
  - **PTZ pane covered the Settings page on first launch.** The pane was
    created as a child of the `QTabWidget` but not inserted as a tab until a
    PTZ-capable camera connected; an un-inserted widget parented to a
    `QTabWidget` is a *plain child* and is painted on top of the current page,
    so it drew Settings under itself until the first selection. The pane is now
    `hide()`n on construction (`zc_control_dock.cpp`); `addTab()` shows it when
    the tab is selected.
  - **Sources from an old profile stayed black.** The dock names its sources
    `ZCamera <ip>` and the name was the only place the address survived if the
    stored `zc_source_ip` was empty (a `get_defaults` artifact fixed earlier).
    `ZcSspSource` now derives the address from its own name in `update()` and
    again in `start()` (`ipFromSourceName()`), so such a source heals on the
    next OBS launch with no dock interaction. `tests/obs_ws_driver.mjs` gained
    a legacy case: a `ZCamera <ip>` source created with `zc_source_ip: ''` must
    come up at the real video size with a decoded picture.
- 2026-09-21: Eighth pass — three operator reports: the rail could not be
  matched against the source list, the PTZ pane only knew presets and not the
  camera's second slot family, and OBS crashed on exit.
  - **The rail now shows the address.** The source that represents a camera is
    named `ZCamera <ip>` (spec §1 L3), so a rail entry that showed only the
    camera's model name could not be matched against the Sources list, and two
    cameras of the same model were indistinguishable. Entries read
    `<name>  (<ip>)` and carry `ZCamera <ip>` as their tooltip, i.e. exactly the
    string the operator sees in OBS's own list (`ZcCameraRail::setCameras()`).
  - **Presets and traces share one index space.** The camera has two
    independent 0..99 slot families: presets (`/ctrl/preset`) and traces
    (`/ctrl/ptrace`). Only the `PresetChange` WebSocket notification adds a
    2000 base offset to a trace index (the reference web UI's
    `tracePresetBaseIndex = 2e3`, `controller.min.js:8687`, undone at `:8794`);
    the HTTP endpoints take the plain 0-based slot. Verified against the live
    camera: `GET /ctrl/ptrace?action=get_info&index=0` ->
    `{"name":"Trace 001","prepared_index":127,...}`,
    `GET /ctrl/ptrace?action=query` -> `{"status":"none","st_code":0}`,
    `/app_data/trace/thm_000.jpg?act=thm` -> 200 while `thm_001.jpg` is 404 for
    the empty slot 1 (and the trace thumbnails live in `/app_data/trace/`, not
    `/app_data/preset/`).
  - **The trace wire contract is a pure function too**
    (`zc_settings_schema`): `ptraceQuery(action, index, extra)`,
    `ptraceThumbnailPath(index)`, `traceStateFromInt()` and `traceActions()`
    for the six `st_code` states (0 none, 1 recording, 2 preparing,
    3 ready-to-play, 4 playing, 5 deleting, 6 ready-to-record).
    `ZcCameraClient` gained `ptraceDelete/SetName/RecordStart/RecordStop/
    PlayStop/PlayPrepare/PlayStart/Query/Info/Thumbnail`. The transport-wide
    actions (`rec_stop`, `play_start`, `play_stop`, `query`) must go out
    *without* an index — the camera ignores one, but it is wrong on the wire,
    so it is asserted.
  - **`ZcPresetPanel` is a two-family panel**: a `Preset` / `Trace`
    checkable-button selector with a mode label, and two action buttons whose
    meaning follows the mode — `Recall` | `Start/Stop Record` and
    `Add/Replace` | `Prepare`/`Play`/`Stop Playback`. The RECALL SPEED row is
    preset-only and is hidden in trace mode. Enablement in trace mode comes
    from `traceActions()`: while recording only Stop Record is live and
    page/slot navigation is locked; while playing only Stop Playback; while
    preparing or deleting everything is locked; otherwise all four are live.
    Replies that arrive after the family was switched are dropped
    (`loadSlot()`/`applySlotInfo()` capture `traceMode_` per request), and
    switching clears the cached names and thumbnails so nothing leaks across.
    Like the preset pane — and unlike the web UI — overwriting or deleting a
    trace asks for no confirmation dialog.
  - **Exit crash (0xC0000409).** The six dumps in
    `%LOCALAPPDATA%\CrashDumps` from earlier the same day (10:32 … 15:01) all
    carry the same signature: a CRT fast-fail in `ucrtbase.dll` during process
    teardown. That is what a heap corruption looks like — the bad free does not
    fault where it happens, the CRT's fast-fail fires at exit — and the
    teardown log of the crashing session matches: it reaches
    `[obs-zcamera] obs-zcamera unloaded` and dies only after that. The
    corrupting free was the dock: `ZcControlDock` is parented to OBS's main
    window, so Qt destroys it together with that window, while
    `obs_module_unload()` deleted it a second time through a raw pointer. It is
    now held in a `QPointer` and detached (`setParent(nullptr)`) before the
    delete, so whichever side runs first wins and the pointer can never dangle.
    `tests/dump_probe.ps1` (untracked) is the minidump reader used for the
    attribution: it prints the exception code, the faulting module and a raw
    scan of the crashing thread's stack.
  - **The rail no longer depends on discovery alone (spec §1, L11).**
    `refreshCameras()` still starts from the mDNS records, then enumerates the
    sources OBS holds (`obs_enum_sources`) and appends every host that a
    `ZCamera <ip>` name yields and discovery did not mention — sorted by
    address, so those rows do not move between scans. The address/name parsing
    is a pair of pure functions (`zc::cameraSourceName()` /
    `zc::hostFromSourceName()`) so a scene or another plugin's source can never
    be mistaken for a camera, and the dock's own `sourceNameForHost()` now
    delegates to the first of them instead of repeating the literal.
    Such a row shows the address only (discovery is what supplies a camera
    name) and is marked offline until it answers: `ZcRailCamera::offline`
    drives a muted glyph and text, an `(offline)` suffix and an
    `offline, control disabled` tooltip, all composed in one place
    (`ZcCameraRail::applyRow()`), which `setHostOffline()` re-runs in place so
    the click that changed the state does not drop the selection.
    `openCamera()` now returns `bool` and, when the camera does not answer,
    detaches all three panels (`setClient(nullptr)`), deletes the client and
    removes the PTZ tab, so a dead camera leaves **no** control behind — the
    status bar says `Offline: <ip> — not reachable, control disabled`. Which
    hosts answered is remembered (`reachableHosts_`), so a source-only row that
    is really there becomes an ordinary row instead of flipping back to offline
    at the next 2.5 s scan, and hosts whose connect failed are remembered too
    (`unreachableHosts_`) so a discovered camera that has gone away stays
    marked rather than clearing at the next scan.
  - **The PTZ settings group is gated on the camera, not always offered.** A
    camera without a pan/tilt head has no PTZ configuration, so offering the
    `ptz` group only produced rows whose keys the camera rejects.
    `zc::catalogRequiresPtz()` marks the one catalog that depends on the
    capability, and `ZcSettingsPanel::populateGroups()` now rebuilds the group
    list from `client_->cameraSupportsPtz()` — on `setClient()` and again on
    `cameraInfoChanged()`, so the group appears as soon as `/info` arrives. The
    rebuild is `QSignalBlocker`ed and repairs the selection itself (keeps the
    current group if it survived, else falls back to the first), so it cannot
    repaint the rows pane behind the caller's back, and it stays safe to call
    from the constructor before the rows pane exists.
  - **A resolution change now rebuilds the decoder.** The rebuild decision in
    `ZcSspSource::onMeta()` only looked at the codec and the software-fallback
    state, so a stream that changed size while the connection stayed up kept a
    frames context built for the old size: `ffmpeg_set_params()` stores the new
    width/height but the D3D11VA shared pool was allocated at create time from
    the old ones, while `get_width()`/`get_height()` already reported the new
    size. The comparison against the previous `metaW_`/`metaH_` (with a log
    line) closes it. A reconnect was already safe: `start()` runs
    `installDecoder(nullptr)`, so the next `onMeta` builds a decoder at the new
    size. For the record, the reconnect path itself is `watchdog()` (fires
    after `ZC_STALL_TIMEOUT_MS` of silence, keyed off received packets, not
    decoded frames) → `onDisconnected()` → `reconnectThread()` (3/6/10/15 s
    backoff, joinable and interruptible) → `reconnectNow()` → `start()`, and
    the last decoded frame stays on screen while disconnected (`drawn_`), so a
    stall freezes the picture rather than going black.
- 2026-09-21 (eleventh pass — the Stream group and the dock raise):
  - **The Stream settings group is rendered, not just specified.** The
    contract pinned down in the tenth pass is now wired end to end.
    `zc_settings_schema` gained the stream helpers — `streamSettingQuery()`
    (always `action=query`, because that is also the write verb, plus `index`
    and any field parameters), `isStreamCatalog()` and
    `kBitrateWriteScale = 1000` for the kbps-read/bps-write asymmetry — and
    `makeCatalogs()` now declares a `stream` group with **no keys**, since it
    is not a getbatch catalog. `ZcCameraClient` gained
    `streamSettingQuery()` / `streamSettingWrite()` over one private
    `sendStreamSetting()` round trip (an empty parameter list is a pure read).
    The panel's `buildRows()` special-cases the group before it consults
    `settingGroupKeys()`, and `buildStreamRows()` draws the `bitrate (kbps)`
    slider (scaled by `kBitrateWriteScale`) and the `gop_n` slider, both
    bounded by the camera's own `maxBitrate`/`maxGop` and disabled by
    `bitrateRo`/`gopRo`, plus rows for the encoder's identity and state.
  - **A stream write repaints from the camera's reply, not from the widget.**
    `writeStreamParams()` sends the fields and feeds the returned document
    straight back into `streamDoc_` + `buildRows()`, so a field the camera
    refuses snaps back to the camera's value instead of lying. `streamIndex_`
    and `streamDoc_` live on the panel and survive a row rebuild, and every
    reply is guarded on the client pointer, the index and
    `zc::isStreamCatalog(...)`, so a slow reply cannot paint rows for a camera
    or an index the operator has already left. `setClient()` drops both so a
    new camera never shows the old camera's numbers.
  - **Creating a `zcamera_source` raises the dock.** `obs-ssp.cpp` connects
    libobs's `source_create` and, for a source whose id is `zcamera_source`,
    raises the control dock — marshalled with a queued invocation because the
    signal can arrive off the Qt thread. The handler is armed only by
    `OBS_FRONTEND_EVENT_FINISHED_LOADING`, so loading a scene collection (which
    creates every stored source at once) does not steal focus on startup.
    Recorded as rule L12 in `ui-spec.md` §1.
- 2026-09-21 (twelfth pass — the stream index is the firmware's, and codec /
  resolution / fps really are changeable):
  - **The eleventh pass's "`encoderType` and `fps` are not writable" was
    wrong**, and the mistake was reading the wrong index. Both probes had been
    made against `stream1`, whose document carries no choices. The camera also
    serves **`app_stream1`**, and that document *does* carry them:
    `videoEncoderList`, `resolutionList`, `fpsList` and the matching
    `encoderRo` / `resolutionRo` / `fpsRo` flags. `app_stream0` answers
    `{"code":0,"streaming":false}` — only the `1` index of that family exists.
  - **Which index to use is discovered, not assumed.** `isStreamDocument()`
    tells a real document apart from the camera's "no such index" answer (a
    document names its own `streamIndex`). `loadStream()` asks for
    `app_stream1` first and falls back to `stream1` when that index is absent,
    so one code path serves both firmwares. The index is now a **read-only
    row**, not the `stream0`..`stream3` combo the eleventh pass drew: it is
    the firmware's choice, so the operator cannot pick it. `streamIndexName()`
    and `kStreamCount` are gone with the combo.
  - **The write parameter names are not the document's field names.** Found by
    probing every plausible spelling, one field at a time, and checking the
    reply: `bitrate` (bps), `gop_n`, `width` **and** `height` together for a
    resolution, **`venc`** for the encoder, and `fps` for the frame rate.
    `encoderType`, `encoder_type`, `video_encoder`, `videoEncoder`,
    `stream_video_encoder`, `codec`, `enc_type` and `encoderName` all leave the
    field untouched; `resolution` as one `WxH` field does too. `action=set` is
    rejected outright with `{"code":-1}` rather than silently ignored.
  - **The document's lists decide what is a control.** Where
    `resolutionList` / `fpsList` / `videoEncoderList` are present they become
    combos (honouring the matching `*Ro` flag, and prepending a current value
    the list does not mention so the box cannot misrepresent the camera);
    where they are absent — the older `stream1` — the value is a read-only row,
    which is the camera's own setting. `bitrate` and `gop_n` are sliders on
    both. `writeStreamParams()` took over from `writeStreamField()` because a
    resolution is two parameters, and `resolutionParams()` is the pure helper
    that turns `1280x720` into that pair.
  - Two behaviours worth recording: a write of `fps` also **rescales `gop_n`**
    (60 fps took `gop_n` from 35 to 60 on 192.168.10.104), which is exactly why
    the dock repaints from the camera's reply rather than from the widget; and
    `app_stream1` and `stream1` are two views of the same encoder, so a write
    through either shows in both.
  - `ui-spec.md` §2 was rewritten around the discovered index, the write-name
    table and the lists.
- 2026-09-21: **Thirteenth pass — the rail's two-line rows, the per-row chip,
  the stream bar, and the codec/resolution reconnect.** Four UI/behaviour
  changes, all recorded in `ui-spec.md`:
  - The rail row is no longer a one-line label with a bottom Add button. A
    `ZcRailDelegate` paints each row as **two lines** (name above address) and
    hit-tests an **action chip** on the right: gold `＋` while the camera is not
    an OBS source, a gold check once it is. `editorEvent()` acts on the chip and
    deliberately lets the press through to the view otherwise, so the chip never
    steals the click that selects the row; a `mutable QString clickedHost_`
    keeps a press/release pair from firing the add twice. A `QStyledItemDelegate`
    was chosen over an item widget because a widget swallows that selection
    click, and over `setWordWrap` because the chip needs a hit-test anyway.
    `updateAddButton()` and `selectedHost()` are gone, and so is `addBtn_`; the
    delegate is signal-free (`std::function`) like `ZcPresetSlotFrame`, so no
    extra `Q_OBJECT`/`.moc` include was needed.
  - The stream controls are **no longer a settings group**. The `stream` entry
    was removed from `makeCatalogs()` and `isStreamCatalog()` deleted, so no
    catalog can claim them; the controls moved into a two-row `#StreamBar` frame
    pinned above the group list — row 1 `STREAM | Codec | Resolution | FPS |
    <index> · <bitwidth> · <status>`, row 2 `Bitrate | GOP`. The controls are
    created once and updated in place under `QSignalBlocker`, so a write's reply
    cannot close an open combo popup and cannot be mistaken for an operator
    change. Sliders write on `sliderReleased` (a drag must not send a request
    per pixel).
  - **A codec or resolution write drops the stream, and the source was checked
    against exactly that.** The path is `onDisconnected()` → `reconnectThread()`
    (3/6/10/15 s) → `reconnectNow()` → `start()`, and `onMeta` rebuilds the
    decoder when the codec or the coded dimensions change. One real gap was
    found and fixed: `bitDepthKnown_`/`streamTenBit_` described the *source*, but
    they are facts about the **connection** — after a reconnect the SPS sniff
    was skipped, so the new decoder was built with the previous stream's depth
    and the `decoderTenBit_ != streamTenBit_` repair could not fire either,
    because it compares against that same stale value. `start()` now clears
    both.
  - The camera's `videoEncoderList` advertises `h265_10b`, but `venc=h265_10b`
    is **silently refused** by this firmware (no error, `code:0`, encoder
    unchanged) while `venc=h265`/`h264` apply — so the codec combo must repaint
    from the reply, which it does. Recorded in `ui-spec.md` §2.

- 2026-09-28: **Fifteenth pass — the auto-framing port lands in code; the tab
  was initially deferred, later enabled.** The camera's own page drives auto
  framing from a separate
  `/www/js/framing.js` module (226 KB), whose non-canvas controls now all exist
  in `ZcFramingPanel`: detect / trace / restart-now, auto target, zoom + tilt
  lock, sensitivity, acceleration, scene, start position, target count, lost
  action + timeout + wait, detection-area enable, registered-face tracking,
  start-on-power-up, green tally, tracking-triggered recording, follow-me
  (waving), view-angle adjustment, and the composition-preset list (add /
  recall / replace / rename / reset / delete). Reads collapse into a single
  `GET /ctrl/framing/all_status` snapshot — the bare `/ctrl/framing` root returns
  an **empty body** on this firmware, which is why the earlier two-control panel
  never left "Reading…" — and writes are the page's own
  `/ctrl/framing/<sub>?…` calls, all of them exercised against `192.168.10.104`
  (`{"code":0}`). `FramingStatusChanged` / `FramingSettingChanged` are wired
  through `ZcCameraClient` so a change made in the web UI repaints the panel.
  The tab is **shown** (the canvas-only parts — the framing-box drag, the FOV
  preview, EzFraming click-to-centre — are not reproduced). `ZcControlDock`
  inserts it for a camera whose `/info` reports auto framing;
  `kFramingPanelVisible` (`true`) in
  `src/ui/zc_control_dock.cpp` is the single switch that hides the tab again.
  Working through this also removed the PTZ page's dead "Range limit" checkbox
  and its four edge buttons (deferred bug 13), and dropped the unused `de-DE` /
  `ru-RU` locale files.

- 2026-09-29: **Sixteenth pass — the PTZ pad's Position / GOTO row is gone.**
  The left column's last two rows (the "Position pan/tilt" read-out with its
  Refresh button, and the absolute/relative GOTO spins with the "rel" checkbox
  and the Goto button) are removed from `ZcPtzPad`; the joystick, the presets
  and the traces already cover the movement this panel needs. `git log -S`
  settles where it came from: the block arrived in `f2786f6` and was never
  deleted, and the row removed earlier was the range-limit one (`79fb6f7`,
  removed in `25e26bc`) — so this is a first removal, not a regression. A
  comment in `zc_control_dock.cpp` now records why the row is absent.
  `ZcCameraClient::ptzGoto()` / `ptzQuery()` are kept as the `/ctrl/pt` protocol
  facade; they simply have no UI caller any more.

## Verification log

- **2026-09-21** (thirteenth pass — the two-line rail rows, the per-row chip, the
  stream bar and the codec/resolution reconnect, all driven in a real OBS):
  - `tests\build_tests.ps1` — **ALL TESTS PASSED (7 cases)**;
    `cameractrl_tests.exe` **116 passed, 0 failed, 0 skipped**; `decode_test` and
    `pipeline_test` **18 passed each**. The stream group's `isStreamCatalog()`
    checks were replaced by their inverse: no catalog in `zc::catalogs()` may
    have the id `stream`.
  - **The rail, through UI Automation in a live OBS.** OBS 31.0.3 on the
    throwaway profile/collection `zc-autotest`/`zc-stream`, dock opened from
    `Tools > ZCamera Control`:
    - Every rail row measured **46 px tall** — two lines of text, not one
      (`ZCAM-E2-F6-CABE01` above `192.168.10.174`, and so on for the five
      discovered cameras). Screenshot: the name sits on the first line, the
      address on the second, and the chip is at the right edge.
    - The **only button left in the dock is `Refresh`** — the bottom
      `＋ Add to Source` button is gone.
    - **The chip really adds the source.** The source was removed through
      obs-websocket (`RemoveInput`), and the row's chip was then clicked at its
      computed position (row right edge − 18 px, vertically centred):
      `GetInputList` came back with `ZCamera 192.168.10.174` again. Since a click
      on the row body only selects and connects, the chip is what created it.
  - **The stream bar, through the same run.** The dock's group list read
    `[exposure, wb, lens, image, record, video, audio, system, maintanence,
    network, security, multicam, assitool, timecode]` — **no `stream` entry** —
    while a `STREAM` label was present above it. The bar's controls read back
    `Codec [h265_10b, h265, h264]`, `Resolution [3840x2160, 1920x1080, 1280x720,
    848x480]`, `FPS [30]` and the two sliders `value=8000 min=0 max=300000`
    (bitrate) and `value=30 min=1 max=120` (GOP) — exactly the camera's own
    lists, and exactly what the twelfth pass found *inside* the group.
  - **The resolution write, and the reconnect it causes.** `848x480` was
    selected in the bar's resolution combo; the next `curl` read
    `"width":848,"height":480` with `status:"streaming"`, and selecting
    `1920x1080` put it back. The plugin's own log shows what the source did with
    each drop, with no crash and no stuck state:
    `18:41:49 disconnected, reconnecting...` → `18:41:52 hw_frames_ctx init=0
    (848x480)` → `d3d11 video processor ready: NV12 slice -> shared BGRA
    848x480`, then `18:42:12 disconnected, reconnecting...` → `18:42:15
    hw_frames_ctx init=0 (1920x1080)`. **The decoder is rebuilt at the new
    dimensions on its own**, ~3 s after the drop (the first escalation step —
    the TCP EOF arrives before the 10 s stall watchdog would fire).
  - **The codec write, and the per-connection bit depth.** The codec combo's
    click did not land in the probe (the popup list is scanned across all
    top-level windows and a stale popup shadowed the target item), so the codec
    leg was driven with the same wire call the combo sends — `venc` — against a
    live source. `h264` → `h265` → `h264`:
    `18:50:08 disconnected, reconnecting...` → `18:50:12 get_format: hw_fmt=d3d11
    profile=1 level=123 coded=1920x1080` (HEVC Main) → `18:50:34 disconnected,
    reconnecting...` → `18:50:37 get_format: ... profile=100 level=42
    coded=1920x1088` (H.264 High). **A codec change is handled: drop, reconnect,
    decoder rebuilt for the new codec.**
  - **Direct evidence for the `bitDepthKnown_` fix.** The log carries
    `stream bit depth: 8-bit (8-bit)` on *every* connection of the resolution
    run — the first one, the 848x480 one and the restored 1920x1080 one. Before
    the fix the flag survived the reconnect and the sniff was skipped, so only
    the first connection could ever log it.
  - **A finding, not a defect of this change:** the camera lists `h265_10b` in
    `videoEncoderList` but refuses it. `venc=h265_10b` left `encoderType`
    unchanged while `venc=h265` and `venc=h264` applied, and `bitwidth=10bit`,
    `venc=h26510b`, `venc=hevc_10b` and an explicit `width`/`height` alongside
    it were all tried and all refused. A 10-bit HEVC source therefore could not
    be produced on this hardware, and the bit-depth re-sniff is exercised by the
    reconnect (above) rather than by a 10-bit stream.
  - Deployed `obs-zcamera.dll` is 443392 bytes, SHA256
    `E983B37C3596B8CE38C26F781CB0D0875A79E7359486CA1139C7303E3893932C` — the
    build output and the file at
    `C:\ProgramData\obs-studio\plugins\obs-zcamera\bin\64bit\` hash identically.
    No new dump in `%LOCALAPPDATA%\CrashDumps`, no `obs64`/`ssp-connector` left
    behind, and the camera was left exactly as found (`app_stream1`
    1920x1080/h264/fps 30/gop 30).
  - **Still UI-only, not asserted automatically**: the rail's two-line painting,
    the chip hit-test, the stream bar and the combo write path are driven through
    UI Automation against a live camera, so `build_tests.ps1` covers only the
    pure helpers. The codec leg's UI click is the one step that had to be
    replaced by the equivalent `curl` write; the resolution leg proves the widget
    write path end to end.

- **2026-09-21** (eleventh pass — the Stream group and the dock raise, verified
  in a real OBS):
  - `tests\build_tests.ps1` — **all 7 cases PASS**; `cameractrl_tests.exe`
    **116 passed, 0 failed** (was 107; the 9 new checks are the stream
    contract: the read query carrying exactly `action`+`index`, the write query
    carrying the field, `kBitrateWriteScale == 1000`, `isStreamCatalog()`
    discriminating the stream group from the others, and the stream group
    declaring no getbatch keys).
  - **The Stream group, driven through UI Automation in a live OBS.** OBS
    31.0.3 was launched on the throwaway profile/collection `zc-autotest`
    (obs-websocket on 127.0.0.1:4455, no auth) and a `zcamera_source` for
    `192.168.10.174` was created through `CreateInput` — i.e. the create path a
    user takes, not a hand-written config. Two behaviours were confirmed:
    - **The dock raised itself.** `CreateInput` brought the `ZCamera Control`
      dock to the front with no Tools-menu action, and a launch on a collection
      that already held sources did **not** raise it — the
      `FINISHED_LOADING` arming works.
    - **The rows matched the camera's own document.** Selecting the `stream`
      group for `192.168.10.174` read back `bitrate (kbps) 8000`, `gop_n 30`,
      `encoderType h264`, `resolution 1920x1080`, `fps 30`, `bitwidth 8bit`,
      `status streaming` — byte-for-byte the document `curl` returns for
      `?action=query&index=stream1`.
    - **The write path works end to end.** Dragging the `gop_n` slider took the
      camera to 47 and the dock repainted 47 *from the camera's reply*;
      dragging again gave 28; the value was restored to 30 with `curl` and the
      camera was left exactly as found (`stream1` bitrate 8000, gop_n 30).
- **2026-09-21** (twelfth pass — the discovered index and the encoder /
  resolution / fps combos, verified in a real OBS):
  - `tests\build_tests.ps1` — **all 7 cases PASS**; `cameractrl_tests.exe`
    **118 passed, 0 failed** (was 116). The new checks are
    `kAppStreamIndex`/`kLegacyStreamIndex`, the read query naming
    `app_stream1`, a write carrying `venc`, `isStreamDocument()` accepting a
    document and rejecting both the `{"code":0,"streaming":false}` answer and
    an empty object, and `resolutionParams()` turning `1280x720` into the
    `width`+`height` pair while rejecting `""`, `"1280"`, `"1280x"` and `"axb"`.
  - **The protocol, probed live with `curl`** (both cameras, every candidate
    spelling one field at a time, each change restored afterwards):
    `app_stream1` exists on 192.168.10.174 *and* 192.168.10.104 while
    `app_stream0` answers `{"code":0,"streaming":false}`; `width`+`height`
    changed the resolution and `venc` the encoder, while `encoderType`,
    `encoder_type`, `video_encoder`, `videoEncoder`, `stream_video_encoder`,
    `codec`, `enc_type`, `encoderName` and a single `resolution=WxH` field were
    all refused; `fps` applied on 192.168.10.104 (`fpsList` `[30,60]`) and
    `action=set` came back `{"code":-1}`. Both cameras were left exactly as
    found — 192.168.10.174 `app_stream1` 1920x1080/h264/fps 30/gop 30, and
    192.168.10.104 1280x720/h264/fps 60/gop 35 after `fps` was seen to rescale
    `gop_n` to 60 and was put back.
  - **The Stream group, driven through UI Automation in a live OBS.** A
    throwaway collection (`zc-stream`) was written by hand with one
    `zcamera_source` for `192.168.10.174`, OBS 31.0.3 was launched on it, the
    dock opened from `Tools > ZCamera Control`, the rail row
    `ZCAM-E2-F6-CABE01  (192.168.10.174)` clicked, and the `stream` group
    selected. The rows read back:
    `index app_stream1` (read-only, as intended), `bitrate (kbps) 8000`,
    `gop_n 30`, `resolution`, `fps`, `encoder`, `bitwidth 8bit`,
    `status streaming`; the two sliders were `value=8000 min=0 max=300000` and
    `value=30 min=1 max=120`, and the three combos carried exactly the camera's
    lists — `3840x2160, 1920x1080, 1280x720, 848x480`,
    `30`, and `h265_10b, h265, h264`. **Selecting `848x480` in the resolution
    combo moved the camera** (`"width":848,"height":480` on the next `curl`),
    and selecting `1920x1080` again put it back — so the
    `resolutionParams()` → `width`+`height` path works through the real
    widget, not only in the unit test.
  - Deployed `obs-zcamera.dll` is 431104 bytes, SHA256
    `ADEA0D79741687C9C9E637CAB4C80FBF5B936687EECE80A427141B65437EDDDD`
    (re-read at the destination and matching the build output). No new dump in
    `%LOCALAPPDATA%\CrashDumps`; OBS was closed cleanly and the throwaway
    collection removed.
  - **Still UI-only, not asserted automatically**: the raise, the stream rows,
    the combos and the combo write path are driven through UI Automation / a
    live camera, so `build_tests.ps1` covers only the pure helpers. The
    `stream1` fallback is covered by `isStreamDocument()` alone — no camera
    without `app_stream1` was available to exercise it.
- **2026-09-21** (tenth pass — PTZ group gating, resolution change, stream API):
  - `tests\build_tests.ps1` — **all 7 cases PASS**; `cameractrl_tests.exe`
    **107 passed, 0 failed** (was 104; the 3 new checks cover
    `catalogRequiresPtz()`: the `ptz` catalog needs a pan/tilt head, the other
    catalogs do not, and the schema really declares a group that the gate
    hides). `decode_test` / `pipeline_test` unchanged (18 passed each).
  - **Camera API probed live** (`curl -sk --digest -u admin:admin`, HTTP only —
    443 is closed on this firmware). `?action=query&index=streamN` returns the
    document for `stream0`..`stream3`; `/ctrl/stream_setting?action=query`
    without an index returns `{"code":0,"streaming":true}`; `stream_setting` is
    **not** a getbatch catalog and `stream_video_encoder` / `stream_bitrate`
    are **not** `/ctrl/get` keys, so the shape the spec originally assumed does
    not exist. Two behaviours worth recording: a `query` carrying a value
    parameter **writes** it (this is how `bitrate` was changed and then
    restored — writing `8000000` bps reads back as `8000` kbps), and
    `action=set` answers `code:0` while ignoring the parameter. `/ctrl/rtmp`
    answers `{"url":"","key":"","bw":0.0,"status":"idle","autoRestart":0}`.
  - **Not yet verified in OBS**: the PTZ group disappearing needs a dock
    restart, and an OBS instance (profile `virtual_zcam`) was running while
    this was built, so it was left alone; the new DLL (413184 bytes, SHA256
    `04038BF2F78359AD612FF0BBE9BF8B1AFC3ADD76F4E0891F384BB4E9EF6A4748`) loads
    on the next start. The resolution-change fix has no automated test either:
    it needs a camera that changes resolution mid-connection.
- **2026-09-21** (ninth pass — the source-only rail row, verified in a real OBS):
  - `powershell -ExecutionPolicy Bypass -File tests\build_tests.ps1` — **all 7
    cases PASS**. `cameractrl_tests.exe`: **104 passed, 0 failed, 0 skipped**
    (was 98; the 6 new checks are the source-name contract:
    `the source for a camera is named 'ZCamera <ip>'`,
    `the address is recovered from the source name`,
    `naming and parsing are inverses`, `a scene name yields no camera address`,
    `the bare prefix without an address is not a camera source`,
    `the prefix is case-sensitive, matching the source name we create`).
    `decode_test.exe` / `pipeline_test.exe` unchanged (18 passed / 0 failed
    each, zero-copy markers still present).
  - **The offline row, driven through UI Automation** (probe script kept
    outside the repo). A throwaway scene collection (`zc-offline`) was written
    by hand with two `zcamera_source` sources — `ZCamera 192.168.10.174` (a
    camera that is really there) and `ZCamera 192.168.10.240` (nothing at that
    address) — so the second one exists in OBS but no mDNS record mentions it.
    OBS 31.0.3 was launched on it, the dock opened from `Tools > ZCamera
    Control`, and the rail read back as:
    `ZCAM-E2-F6-CABE01  (192.168.10.174)` /
    `ZCAM-P2-R1-18x-010360  (192.168.10.120)` /
    `192.168.10.240  (offline)` — i.e. the camera discovery does **not**
    advertise is listed, address only, marked offline. A real mouse click on
    that row left the status bar reading
    `Offline: 192.168.10.240 — not reachable, control disabled`, the row still
    `(offline)` on the next scan, the tab bar down to `Settings` alone (no PTZ
    tab) and the only enabled dock button `Refresh` — the Add button reads
    `Already in scene` and is disabled, since the camera *is* a source. Clicking
    the row for `192.168.10.174` then read back `Connected: 192.168.10.174 ()`.
    The dock was also screenshotted: the third row is drawn muted, with the
    address in place of a name. The probe was run twice — once per build — with
    identical results; the deployed `obs-zcamera.dll` is 412160 bytes, SHA256
    `86C46DBEAF038988C1F8DF98F85FD06971CC443F725BB696B1785D4C15DC43AB`
    (re-read at the destination and matching the build output).
  - No new dump in `%LOCALAPPDATA%\CrashDumps` (newest still 15:01, before the
    `QPointer` fix), so this build still exits cleanly.
  - **Still UI-only, not asserted automatically**: the reachable-source-only
    case (a camera that answers but that discovery does not advertise) could
    not be staged here, because both cameras on this LAN are advertised; it
    follows from the same `reachableHosts_` branch the offline case exercises.
- **2026-09-21** (eighth pass):
  - `powershell -ExecutionPolicy Bypass -File tests\build_tests.ps1` — **all 7
    cases PASS**. `cameractrl_tests.exe`: **98 passed, 0 failed, 0 skipped**
    (was 70; the 28 new checks are the trace wire contract, e.g.
    `ptraceQuery(get_info, 7) is action=get_info&index=7 (0-based slot)`,
    `rec_stop is transport-wide: no index on the wire`,
    `ptraceThumbnailPath(0) is thm_000 in the trace directory, not the preset
    one`, `traceStateFromInt(2) is Preparing`,
    `traceActions(Recording) locks everything but Stop Record`).
    `decode_test.exe` and both `pipeline_test.exe` branches are unchanged
    (18 passed / 0 failed each, zero-copy markers still present).
  - `powershell -ExecutionPolicy Bypass -File tests\obs_integration_test.ps1
    -CameraIp 192.168.10.120 -ForceStopObs` — **12 passed, 0 failed**. The
    legacy-recovery case had failed reproducibly; the cause was the camera and
    not the plugin: three simultaneous raw TCP connections to
    `192.168.10.120:9999` returned data on the first (`avail=67`) and nothing
    on the other two, i.e. **the camera serves one SSP stream per client**, so
    a second source pointed at the same camera stays black. The driver now
    removes the main source before the legacy case, which is about address
    recovery rather than two concurrent streams.
  - **Exit crash, before and after.** Before the `QPointer` fix this machine
    held six dumps from the same day (10:32, 12:59, 13:07, 13:12, 13:16,
    15:01), every one `exception : 0xC0000409` /
    `FAULTING MODULE: C:\Windows\System32\ucrtbase.dll + 0x7286E`. After the
    fix (`obs-zcamera.dll` SHA256
    `DF8036EDF88ECB15F27E02CE15D1D08CAD6CB825427BFAED12DD79D2A5CDB85F`,
    399872 bytes — the deployed binary and the build output match): **three
    clean exits, no new dump** — two plain starts and one with the ZCamera dock
    open (checked through UI Automation that the `ZCamera Control` window
    existed, and that a plain start does *not* create it). The shutdown log now
    runs past the point where the crashing sessions died:
    `[obs-zcamera] mdns query thread stopped.` /
    `[obs-zcamera] obs-zcamera unloaded`.
  - **Not yet verified in OBS**: the Preset/Trace selector, trace
    record/prepare/play and the trace thumbnails have been exercised only by
    the unit tests and the live-camera HTTP probes above; they still need a
    visual check in the dock. The rail's new `<name>  (<ip>)` label is likewise
    UI-only and untested automatically.
- **2026-09-21** (the two live OBS bugs):
  `powershell -ExecutionPolicy Bypass -File tests\obs_integration_test.ps1
  -CameraIp 192.168.10.120 -ForceStopObs` — **12 passed, 0 failed** (was 9; the
  three new checks are the legacy empty-address case). The new evidence:
  `the legacy source really starts with an empty address — zc_source_ip=''`,
  `a source saved with an empty address recovers it from its name — 1280x720
  luma max=239`, `the recovered legacy source is active in OBS —
  {"videoActive":true,"videoShowing":true}`. That source was created with
  `zc_source_ip: ''` and named `ZCamera 192.168.10.120`, which is exactly the
  state of the operator's saved scene collection, so the old build would have
  failed it. The pre-existing checks still pass, including the recording one:
  `OBS recorded a clip while the source was rendering — ... (3679763 bytes)`
  and `recorded clip shows the picture (frame luma max=222)`. The plugin log
  carries the zero-copy marker: `d3d11 video processor ready: NV12 slice ->
  shared BGRA 1280x768 (zero-copy import path)`.
  Deployed SHA256 `5886BA512E4D220872852F5E3B33648F6F8CC5479AA614FB93ECECAC1D737B82`
  (re-read at the destination and matching the build). OBS is left closed.
  The PTZ-pane fix is UI-only and has **not** been verified by an automated
  test — it needs a visual check on the next launch.
- **2026-09-21** (seventh pass): `powershell -ExecutionPolicy Bypass -File
  tests\build_tests.ps1` — **all 7 cases PASS**.
  - `cameractrl_tests.exe`: **70 passed, 0 failed, 0 skipped** (was 54; the 16
    new checks are Part A2). The gating checks read the user's real `/info`
    reply: `lens_ctrl_ptz=true -> cameraSupportsPtz is true (PTZ tab shown)`,
    `model "e2ptz" without a feature map -> true`,
    `cameraName fallback is matched case-insensitively`,
    `lens_ctrl_ptz=false + non-PTZ model -> false (PTZ tab hidden)`,
    `empty /info -> false`. The wire checks:
    `presetThumbnailPath(7) pads to thm_007`,
    `presetQuery(recall, 5) is action=recall&index=5 (0-based index)`,
    `the '&' inside a preset name is percent-encoded on the wire`,
    `a 3 s preset duration is sent as 3000 ms`.
  - `decode_test.exe` / `pipeline_test.exe` (raw Annex-B and mp4 container,
    `auto` and `cuda`): unchanged from the previous run — 0 failures and 18
    passed / 0 failed each. The zero-copy markers still appear
    (`d3d11 video processor ready: NV12 slice -> shared BGRA 1280x720`,
    `cuda import ready: NVDEC NV12 -> R8/R8G8 -> shared BGRA 1280x720`).
  - Plugin build: `cmake --build build_x64 --config RelWithDebInfo` links
    `RelWithDebInfo\obs-zcamera.dll` with the new schema helpers, deployed to
    `C:\ProgramData\obs-studio\plugins\obs-zcamera\bin\64bit\obs-zcamera.dll`
    (SHA256 `B8A0C14F0249955A1BCF5E23AA1547A17AC6D047A7D2833C3459AEE36C00B00A`,
    384000 bytes; the copy's hash was re-read at the destination and matches).
  - **Not yet verified in OBS**: the dock changes above still need a live run
    against the camera (rail badge + Add button state, PTZ tab gating, preset
    page grid). The unit tests cover the gating decision and the preset wire
    contract, but not the widgets.
- **2026-09-21**: `powershell -ExecutionPolicy Bypass -File tests\build_tests.ps1`
  — **all 7 cases PASS**:
  - `cameractrl_tests.exe`: **54 passed, 0 failed, 0 skipped** (was 42 before
    the settings-engine metadata assertions were added).
  - `decode_test.exe` (raw Annex-B and mp4 container): **0 failures**. Still
    measures `GetSharedHandle=FAILED` on the D3D11VA NV12 array texture, which
    is why the import blits into its own shared BGRA texture.
  - `pipeline_test.exe` (raw Annex-B and mp4 container, `auto` and `cuda`):
    **18 passed, 0 failed, 0 skipped** each. D3D11VA evidence: `d3d11 video
    processor ready: NV12 slice -> shared BGRA 1280x720`, `[texture] 1280x720
    fmt=87 array=1 miscFlags=0x2`, `[readback] populated=yes min=0 max=255
    maxRGBspread=255 meanRGB=121/126/126`. CUDA evidence: `cuda import ready:
    NVDEC NV12 -> R8/R8G8 -> shared BGRA 1280x720 (zero-copy)`, same texture and
    read-back figures. Both branches assert the frame carries chroma and keeps
    its colour balance.
- **2026-09-21, on hardware**: the settings dock was exercised against the real
  camera at `192.168.10.174:80` in a running OBS. The `exposure` group rendered
  all 10 writable rows (5 read-only/unsupported keys hidden) with `ev`'s slider
  showing its gold value, switching to `wb`/`image`/`lens` left no stale rows,
  and a write was verified end to end: moving the `flicker` combo from `60Hz` to
  `50Hz` made `/ctrl/get?k=flicker` answer `"50Hz"`, and moving it back restored
  `60Hz`. The deployed binary is
  `C:\ProgramData\obs-studio\plugins\obs-zcamera\bin\64bit\obs-zcamera.dll`,
  SHA256 `D8F3CCD8ED13900A27BF622A3AE4FB8408948AE214971371B4280B954520FD0E`.
- **2026-09-21, in OBS** (the `get_defaults` fix, commit `4a2e5e1`):
  `powershell -ExecutionPolicy Bypass -File tests\obs_integration_test.ps1
  -ForceStopObs` — **9 passed, 0 failed**. The new regression assertion fires
  first and is the one that matters: `the settings passed to CreateInput survive
  the plugin defaults — zc_source_ip='192.168.10.174'`. Before the fix this read
  `''`, which is exactly the state the user's scene collection was in. The rest
  of the run then proves the picture actually arrives: `source reports itself
  active in OBS — {"videoActive":true,"videoShowing":true}`, `source renders at
  a real video size — 1280x720`, `every rendered frame shows the picture — 3
  frames, luma max=242/240/241`, `the feed is live, not one frozen frame —
  changed-pixel fractions: 1.0%, 1.2%`, and `OBS recorded a clip while the
  source was rendering — ...13-47-39.mkv (2878906 bytes)` (1920x1080, 3.8 s,
  luma max=223). The plugin log carries the zero-copy marker for the same run:
  `d3d11 video processor ready: NV12 slice -> shared BGRA 1280x720 (zero-copy
  import path)`. The run deploys the rebuilt DLL to
  `C:\ProgramData\obs-studio\plugins\obs-zcamera\bin\64bit\obs-zcamera.dll`,
  now SHA256 `D0427EE233FA1542BC0D4EC6ACDFB52BFCBF68DE8B2612DC879F08A25E498CE2`
  (the `D8F3CCD8…` build above is superseded). Note: `-ForceStopObs` kills any
  OBS already running, and OBS is left closed afterwards.
- **Corrected 2026-09-19**: a toolchain *is* present on this machine — MSVC 14.42
  (VS 2022 Community), Qt 6.5.3 `msvc2019_64` at `D:\Qt6\6.5.3\msvc2019_64`,
  and the FFmpeg deps at `.deps\obs-deps-2024-09-12-x64`. The earlier "no
  toolchain" notes below were wrong. OBS headers + `obs.lib` are installed at
  `.deps\include` / `.deps\lib`, so the plugin and the tests can both link
  against the real libobs.
- **Plugin build**: the full CMake build *has* been run — `build_x64` holds the
  solution and `build_x64\RelWithDebInfo\obs-zcamera.dll`, relinked after this
  pass's source changes. (The note that used to be here claimed the opposite.)
- **Standalone tests** — `powershell -ExecutionPolicy Bypass -File
  tests\build_tests.ps1` builds and runs all three binaries:
  - `cameractrl_tests.exe`: **42 passed, 0 failed, 0 skipped** when loopback
    behaves. Part A checks the settings-schema structure; Part B drives a real
    HTTP Digest round trip against a fake camera server on `127.0.0.1`.
    Loopback is used only for the test. This machine's dynamic port range is
    **1024 + 13977** and is shared with the whole desktop, so Qt's loopback
    sockets fail intermittently (`WSAEADDRINUSE`); Part B now reports `[SKIP]`
    in every such case instead of failing — see the 2026-09-19 change log.
    Observed over six consecutive runs: `42/0/0` twice and `19/0/1` four times,
    exit code 0 every time.
  - `decode_test.exe test_720p.h264 plugin --max-frames 3`: **0 failures**.
    Measured on this GPU: the D3D11VA output is an NV12 *array* texture
    (`ArraySize=20`) and `IDXGIResource::GetSharedHandle` **fails** on it
    (DXGI format `103`) — which is exactly why the import blits into its own
    shared BGRA texture rather than sharing the decoder's surface.
  - `pipeline_test.exe test_720p.h264 --max-frames 3`: **17 passed, 0 failed,
    0 skipped**. The end-to-end check of decode → import on a local file (no
    camera, no OBS), D3D11VA branch. Measured evidence: `d3d11 video processor
    ready: NV12 slice -> shared BGRA 1280x720`, `[texture] 1280x720 fmt=87
    array=1 miscFlags=0x2` (87 = `B8G8R8A8_UNORM`, `0x2` =
    `D3D11_RESOURCE_MISC_SHARED`), `[readback] populated=yes min=0 max=255
    maxRGBspread=255`, and `packets=5 decoded=3 hw(D3D11VA)=3 imported=1`.
  - `pipeline_test.exe test_720p.h264 --backend cuda --max-frames 3` (added
    2026-09-20): **17 passed, 0 failed, 0 skipped**. Same assertions on the
    CUDA branch. Measured evidence: `cuda import ready: NVDEC NV12 -> R8/R8G8 ->
    shared BGRA 1280x720 (zero-copy)`, `[texture] 1280x720 fmt=87 array=1
    miscFlags=0x2`, `[readback] populated=yes min=0 max=255 maxRGBspread=255`
    (chroma present, i.e. both planes reached the shader), `get_format:
    hw_fmt=cuda offered=[cudadxva2_vld d3d11va_vld d3d11 d3d12 yuv420p]`, and
    `packets=5 decoded=3 hw(CUDA (NVDEC))=3 imported=1`.
  - Plugin build: `cmake --build build_x64 --config RelWithDebInfo` links
    `RelWithDebInfo\obs-zcamera.dll` with the CUDA source and `d3dcompiler`.
- 2026-09-17: Still no toolchain here. The changes above are **untested on
  hardware**. In particular these need validation on a real camera/GPU:
  - the D3D11VA shared-texture import, including whether
    `gs_texture_open_shared` + `gs_draw_sprite` samples an NV12 surface
    correctly (an NV12 shader may be required);
  - the mid-stream hardware→software fallback;
  - Digest auth and the authenticated WebSocket upgrade against a real camera;
  - the `ENABLE_SW_DECODE=OFF` and no-FFmpeg configure/link combinations.
- 2026-09-29: Camera login verified through the plugin's own transport against
  two cameras, using `192.168.10.174` and `192.168.10.173`. What they differ in
  was established by measurement, not assumption.
  - `192.168.10.174` — E2-F6 (`echof6`), sw **1.0.18**: `/ctrl/getbatch` is gated
    (401 unauthenticated). `GET /login/` with a Digest `Authorization` answers
    **200** and returns the session cookie + `x-auth-token`; `/ctrl/getbatch`
    then reads fine (200 + `{"cfgs":[…]}`). This is what `login()` implements and
    it is confirmed working end to end.
  - `192.168.10.173` — E2-M4-MKII (`edgem4`), sw **1.0.16**: its `/ctrl/*`
    endpoints are **not** gated. A client that never logged in gets **200** from
    `/ctrl/getbatch?catalog=exposure` *with real `cfgs`* and from `/ctrl/get?k=ev`,
    so this camera **does** support catalog batching and needs **no** login —
    `ZcCameraClient::connect()` succeeds on the unauthenticated
    `probeAuthRequired()` result alone. An earlier note claiming sw 1.0.16
    lacks `/ctrl/getbatch` was **wrong**; the 404/non-JSON/empty-`cfgs` fallback
    in `readGroup()`/`readGroupByKeys()` remains the right safety net for other
    old builds, not a description of this unit.
  - **Login method is GET `/login/`** with a Digest `Authorization`, matching the
    camera's own `www/js/login.min.js` (`method:'GET'`, `credentials:'same-origin'`,
    `X-Requested-With: XMLHttpRequest`). `POST /login/` is **not** portable
    (edgem4/1.0.16 rejects it), so `login()` issues GET for both the challenge
    and the credential step. echof6/1.0.18 accepts GET and POST.
  - **The 173 login failure was its Digest nonce being bound to the TCP
    connection that issued it** — not a credential problem and not a lockout.
    Measured, in this order:
    - The camera's own `loginUsingWWWAuthDigest` was captured from the browser's
      `login.min.js` and got `200` on `/login/`; recomputing its `response` by
      hand with standard MD5 reproduced the exact value the camera accepted, so
      the algorithm and the credential (`admin` / `Admin_123`) are correct.
    - The same two-step Digest carried over **one** raw TCP connection
      (`Connection: keep-alive`; challenge, then authenticated retry) answers
      `HTTP/1.1 200 OK`. The identical exchange split across **two** connections
      answers `401 {"error":"Invalid username or password"}` even though the
      `Authorization` value is byte-identical.
    - 173's 401 is `Content-Length`-delimited and carries `Connection: keep-alive`
      (`WWW-Authenticate-X: Digest realm=…,qop="auth",nonce=…,opaque=…`), so the
      connection stays usable between the two legs.
    - `curl --digest` was misleading evidence: it 401s there too, and `curl -v`
      shows it sends `qop="auth"` **quoted** (the camera wants the bare token, as
      `login.min.js` sends it) and goes through the local proxy.
    => `login()` now runs both legs on ONE socket. `HttpExchange` gained an
    optional continuation: when leg 1 answers `401` with a parsable Digest
    challenge it computes the `Authorization` and sends leg 2 on the same socket
    (that handshake never uses `Connection: close`) instead of finishing and
    reconnecting. Firmware that does *not* bind the nonce (echof6/1.0.18) is
    unaffected, and a server that closes after the challenge still gets one
    authenticated retry on a fresh connection.
    Verified through the plugin's own transport: 173 `login=1` (cookie +
    `x-auth-token` captured, `/ctrl/getbatch` 200), 174 `login=1` (unchanged);
    a deliberately wrong password on 173 gives `login=0`, confirming the
    challenge is genuinely verified rather than short-circuited.
  - **Do not hammer `/login/` with a known-good credential to debug a 401.** The
    401 body is `{"error":"Invalid username or password"}` both for a wrong
    password and for a nonce the camera will not accept, so it cannot tell the
    two apart — measure with one clean-session attempt and read the framing
    instead.
