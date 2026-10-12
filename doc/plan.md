# OBS ZCamera Plugin — Implementation Plan

Phased roadmap for building **obs-zcamera** (zero-copy HW decode + embedded
camera control). This is a **rewrite** of the `obs-ssp` core: we keep the good
isolation pieces (SSP subprocess, mDNS, frame queue) and replace the tangled
source/controller/UI with clean modules.

## Phase 0 — Baseline + first build (done)

- Forked `obs-ssp` source, build infra, `ssp_connector`, `lib/ssp`.
- Configured and **built** on Windows x64 against obs-deps (libobs 31, Qt6,
  FFmpeg), producing `obs-zcamera.dll` + `ssp-connector.exe`.
- Replaced `QWebSocket` with a minimal RFC 6455 client over `QTcpSocket`
  (bundled OBS Qt ships Network but not WebSockets).
- **Verify**: both FFmpeg and no-FFmpeg builds succeed (this is the flag the
  whole design hinges on).

## Phase 1 — Clean-rewrite core (in progress)

- Rewrite the source as a class (`ZcSspSource`, `zc_source.cpp`): clean
  decode/sync/render, **per-instance reconnect** (no global static), zero-copy
  `video_render` path + CPU fallback.
- Drop dead `controller/cameracontroller`, `camera-status-manager`,
  `ssp-controller`; wire the `cameractrl/` client.
- Drop `ssp-dock`/`ssp-toolbar` (obs-browser/CEF) in favor of `ui/zc_control_dock`.
- Decouple the hwdecode interface from FFmpeg (neutral `zc_codec_id`); FFmpeg
  only behind `ENABLE_SW_DECODE`.
- **Verify**: Windows FFmpeg build and no-FFmpeg build both pass (done).

## Phase 2 — Windows zero-copy (D3D11VA done, CUDA/NVDEC open)

- **Done**: FFmpeg D3D11VA decode with a SHARED frames context
  (`hw_decode_ffmpeg.cpp`) imported via `gs_texture_open_shared`
  (`hw_decode_d3d11_win.cpp`); the decoder is bound to OBS's D3D11 device
  (AddRef'd, since FFmpeg releases it) and created with the coded dimensions
  known, from `onMeta`.
- **Done**: `video_render` draws the imported texture and releases it; a failed
  import rebuilds the decoder as Software (design §9).
- **Open**: `hw_decode_cuda_win.cpp` (NVDEC → shared D3D11 via CUDA interop).
  Until it exists the CUDA option is not offered in the UI.
- **Verify** (not done): CPU stays near-idle during 4K decode; no CPU copy in
  the hot path; compare CPU% and frame latency vs. the Software backend; NV12
  sampling of the shared surface.

## Phase 3 — macOS VideoToolbox/IOSurface zero-copy

- Code path present (inline in `hw_decode_ffmpeg.cpp`): VT → IOSurface →
  `gs_texture_create_from_iosurface`.
- **Open**: color space/range from the CVImageBuffer attachments; a dedicated
  `hw_decode_vt_mac.mm` was not needed because the FFmpeg backend covers it.
- **Verify** (not done): decode + draw on Apple Silicon and Intel; no CPU copy;
  HDR passthrough correct.

## Phase 4 — Camera control transport (done in the rewrite)

- `zc_http_transport`: origin negotiation, Digest (`WWW-Authenticate-X`) auth,
  FIFO serialization.
- `zc_ws_notifier` over the minimal `ZcWsClient` (RFC 6455 / QTcpSocket).
- **Verify**: connect to a real ZCAM camera; login via Digest; HTTP reads/writes
  succeed; WS events update a state store.

## Phase 5 — Settings engine + client (done in the rewrite)

- `zc_settings_schema` (catalog/key table) and `zc_settings_engine`
  (`/ctrl/getbatch`, `/ctrl/get`, `/ctrl/set`, dependency re-reads, item model).
- `zc_camera_client`: mode, record, photo, card/remain, lens, PTZ, stream.
- **Verify**: all settings groups render; writes take effect and dependency
  groups refresh; PTZ moves the head.

## Phase 6 — Camera control dock UI (done in the rewrite)

- `zc_control_dock` (camera rail + Settings/PTZ tabs + status bar), containing
  `ZcCameraRail`, `ZcSettingsPanel`, `ZcPtzPad`; plus `zc_debug_panel`.
- Add Tools-menu entry; "Add to Source" adds the selected camera as an
  `zcamera_source` to the current scene.
- The embedded preview/HUD pane and the REC/photo controls were dropped: the
  operator watches OBS's own preview of the source.
- **Verify**: manual QA against `doc/ui-spec.md`.

## Phase 7 — Packaging and hardening

- Update CMake/CPack for both platforms; sign macOS bundle; NSIS/DragNDrop.
- Add platform CI for Windows and macOS.
- **Verify**: clean install into a stock OBS on both platforms; no missing
  `libssp`/`ssp-connector`/Qt deps.

## Phase 8 — Cleanup and docs

- Remove dead code inherited from `obs-ssp` (done for controller/status/dock;
  sweep for anything else).
- Finalize `README`, `doc/*`, and the license header pass.
- **Verify**: full build from clean checkout; release artifacts.

## Cross-cutting concerns

- **GPU lifecycle**: every `gs_texture_t` create/destroy happens inside
  `obs_enter_graphics()`/`obs_leave_graphics()` on the graphics thread.
- **Backend fallback**: any zero-copy failure drops to Software with a warning.
- **Threading**: decode thread never touches OBS graphics APIs; render thread
  never touches the camera client.
- **Secrets**: Digest credentials are memory-only; debug panel sanitizes them.

## Milestones

| Milestone | Deliverable |
| --- | --- |
| M0 | Baseline fork builds on Win (FFmpeg and no-FFmpeg) |
| M1 | Clean-rewrite core (ZcSspSource, cameractrl, dock, CMake) |
| M2 | Windows CUDA zero-copy |
| M3 | macOS VideoToolbox zero-copy |
| M4 | Camera control transport + settings + client (done) |
| M5 | Dock UI (done) |
| M6 | Packaging + CI |
| M7 | Cleanup + release docs |