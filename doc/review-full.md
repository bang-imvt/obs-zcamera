# obs-zcamera — Full Repository Review

Reviewed: full source tree, docs, git history, build/packaging/CI, tests, and the
uncommitted work-in-progress diff. Five subsystem deep-reviews (core source,
hwdecode, cameractrl, dock UI, build/tests) were cross-checked; every Critical
and High finding below was re-verified by reading the cited code.

---

## 1. What this repo is

**obs-zcamera** is an OBS Studio ≥ 31 plugin (GPL-2, Windows + macOS) derived
from `obs-ssp`. It streams from ZCAM (SSP protocol) cameras and adds two things
over the reference:

1. **True zero-copy hardware decode.** The camera's H.264/HEVC stream arrives
   over TCP:9999 into an isolated `ssp-connector.exe` subprocess (libssp
   isolation), is framed over the stdout pipe, and is decoded by FFmpeg hwaccel.
   The GPU surface is imported into an OBS `gs_texture` with **no**
   `av_hwframe_transfer_data` and no CPU→GPU upload:
   - Windows D3D11VA: NV12 array slice → `ID3D11VideoProcessor` blit → shared
     BGRA texture → `gs_texture_open_shared` (the `Auto` default).
   - Windows CUDA/NVDEC: device-memory planes → `cuMemcpy2D` into
     CUDA-registered R8/R8G8 D3D11 textures → YUV→BGRA pixel shader → same
     shared-handle open (explicit choice; currently disabled in the UI because
     it "renders green in OBS").
   - macOS: VideoToolbox → IOSurface → `gs_texture_create_from_iosurface`
     (code path present, never validated on hardware).
   - Software fallback via FFmpeg + swscale→RGBA when no GPU path works.
2. **An embedded camera-control dock** (`Tools → ZCamera Control`): a Qt panel
   with an mDNS-discovered camera rail, a catalog-driven Settings pane (HTTP
   Digest auth with the camera's non-standard `WWW-Authenticate-X`, origin
   negotiation, FIFO serialization against the camera's single-threaded
   httpd), a PTZ pad (joystick, presets, traces, range limits), an auto-framing
   pane, and a debug panel. It is a C++ port of the `ZCamGuiOpen` client,
   including a hand-rolled HTTP/1.1 client, an mbedTLS-over-QTcpSocket TLS
   layer (OBS's bundled Qt has no TLS backend), and a minimal RFC 6455
   WebSocket client (OBS's Qt has no Qt6WebSockets).

### Module map

```
src/obs-ssp.cpp            plugin entry, source + dock registration
src/zc_source.cpp          ZcSspSource: connection, decode callback, video_render,
                           watchdog reconnect, CPU fallback          (~1640 lines)
src/hwdecode/              backend factory + FFmpeg hwaccel engine +
                           D3D11VA / CUDA importers + Software backend
src/cameractrl/            HTTP transport, TLS socket, WS client/notifier,
                           settings engine/schema, camera client
src/ui/zc_control_dock.cpp six Qt classes in one 4,192-line TU
src/ssp-client-iso.*       subprocess bridge (pipe framing, receive thread)
src/ssp-mdns.*             discovery over vendored thirdpty/mdns
ssp_connector/             libssp-isolating subprocess (FetchContent-pinned SHA)
lib/ssp/include/imf/       vendored libssp headers (plugin side)
tests/                     standalone + pipeline + OBS integration suites
doc/                       design / plan / progress / ui-spec / buglists (gitignored!)
```

## 2. Status snapshot

- 27 commits on `main` (squashed "Initial commit" + buglist work through
  2026-09-30). **`main` and `10bit-video` have unrelated histories** — the
  granular development history (zero-copy milestone, 10-bit, tests) lives on the
  unmerged `10bit-video` branch, and `doc/progress.md` cites commit hashes
  (`5e670a3`, `b9d31c0`, `be0bba5`…) that exist **only there**.
- 8 files modified **uncommitted** (PTZ range limits, login-error reporting,
  framing tab enabled, IP validation, source-name sync, preset-time unit fix,
  live PTZ position via `ptzfChange`) — reviewed in §7; quality is high.
- Zero-copy D3D11VA path is implemented, tested end-to-end, and verified inside
  real OBS (integration test with pixel/motion assertions). CUDA is verified
  standalone but **not** inside OBS's render loop, and is now hidden from the
  UI. macOS is compile-only.
- Buglist state: installer/auth/PTZ-move/settings-labels fixed; decode bugs
  #6/#7 (H.265 green bar — 10-bit import gaps), #21/#22 audio verify, #14
  selected-source follow, #32 ws auto-sync still open per `doc/buglist-9-22_2-progress.md`.

## 3. Verified Critical / High findings (fix first)

1. **[Critical] `SSPClientIso::Stop()` can hang OBS forever.**
   `src/ssp-client-iso.cpp:277-283` — `worker.join()` runs **before**
   `os_process_pipe_destroy(pipe)`. The worker is blocked in `ReadFile` on that
   pipe; a connector that is alive but silent (exactly the stall case the
   watchdog exists for) never sends a message, so `join()` never returns. The
   hang happens inside `teardownConnection()` while `lifecycleMutex_` is held
   (`src/zc_source.cpp:686-689`), so the reconnect thread wedges and any
   `update()`/`destroy()` from the OBS UI thread freezes OBS.
   *Fix:* terminate the child / destroy the pipe before joining (note
   `os_process_pipe_destroy` itself waits `INFINITE` — add a timeout +
   `TerminateProcess`).

2. **[High] ABBA deadlock between the OBS graphics mutex and `decoderMutex_`.**
   `videoRender()` holds `decoderMutex_` across `decoder_->import()`
   (`src/zc_source.cpp:1338-1340`), and `ffmpeg_import()` calls
   `obs_enter_graphics()` inside (`src/hwdecode/hw_decode_ffmpeg.cpp:465,492`).
   The receive thread takes the opposite order: `obs_enter_graphics()`
   (`src/zc_source.cpp:948`) → `decoderMutex_` (954). Verified by reading both
   paths. The comment at `zc_source.cpp:938-946` claims the order is
   "graphics → frameMutex_ → decoderMutex_" — videoRender actually acquires
   graphics **last**, inside import. Windows-only; freezes the source.
   *Fix:* `obs_enter_graphics()` at the top of `videoRender()` before
   `frameMutex_`, or don't hold `decoderMutex_` across `import()`.

3. **[High] `requestSync`/`loginSync` nested `QEventLoop` has no escape hatch.**
   `src/cameractrl/zc_http_transport.cpp:725-750` — the only `loop.quit()` is
   the exchange's done-callback; exchanges are children of the transport. If
   the client is destroyed mid-probe (OBS shutdown / dock teardown processed
   inside the nested loop), the exchange and its timeout timer die, `quit()`
   never fires, and the UI thread hangs in `exec()`. The dock's `opening_`
   guard covers re-entrant clicks, not destruction.
   *Fix:* watchdog `QTimer` that quits the loop, or block destruction during
   sync ops.

4. **[High] Detached stderr thread → use-after-free.**
   `src/ssp-client-iso.cpp:207,281` — `std::thread(dump_stderr, pipe).detach()`
   loops on `os_process_pipe_read_err`; `Stop()` then `CloseHandle`s the error
   handle and `bfree`s the pipe struct while that thread may be blocked in
   `ReadFile` / dereferencing `pp`. Handle-reuse UB + UAF.
   *Fix:* joinable stderr thread stopped before pipe destroy.

5. **[High] `reconnectThread_` double-join race.**
   `src/zc_source.cpp:589-592` vs `822-832` — `stop()` (OBS thread) joins the
   reconnect thread while the watchdog path can run `onDisconnected()` on the
   **graphics** thread and also `join()` + reassign it. `running_` is not
   atomic with teardown. Concurrent double-join / assign-mid-join is UB.
   *Fix:* dedicated mutex around `reconnectThread_` on both paths.

6. **[High] CUDA importer leaks its CUDA context on every teardown.**
   `src/hwdecode/hw_decode_cuda_win.cpp:450,363-377` — `cuCtxCreate` implicitly
   makes the context current; the initial push is never popped, so
   `cuda_clear()`'s push/pop nets to depth 1 and `cuCtxDestroy` returns
   `CUDA_ERROR_CURRENT_CONTEXT` — the context is never destroyed. Reached on
   every decoder destroy (reconnect/fallback) and resolution change (834).
   Latent today because CUDA is hidden from the UI, but the code is still
   reachable via saved settings. *Fix:* `cuCtxPopCurrent` right after create.

7. **[High] `cuda_render()` mutates OBS's immediate-context state without
   save/restore.** `src/hwdecode/hw_decode_cuda_win.cpp:693-713` — binds its
   own RTV/viewport/shaders on the shared immediate context and ends by
   unbinding everything, bypassing libobs-d3d11's state cache; inside OBS's
   render pass, later draws run with no RT / wrong viewport. Only ever verified
   in the standalone pipeline test, not inside OBS — consistent with the "CUDA
   renders green in OBS" note that removed it from the UI. *Fix:* save/restore
   touched state or use a separate context — or delete the path.

8. **[High] CI ships a broken Windows artifact.**
   `.github/workflows/build.yml:141` — artifact path
   `build/RelWithDebInfo/obs-zcamera` has no wildcard and never matches
   `obs-zcamera.dll`; `if-no-files-found: warn` hides it. The Windows artifact
   contains only the connector. CI also runs no tests and has dead Linux steps.

9. **[High] The documented build command fails at configure.**
   `CMakePresets.json:53` pins `CMAKE_SYSTEM_VERSION 10.0.18363.657`, but
   `cmake/windows/compilerconfig.cmake:14` FATAL_ERRORs below SDK 10.0.20348 —
   so README's `cmake -S . -B build --preset windows-x64` cannot configure on a
   clean machine.

10. **[High] Feature flags are not actually settable.**
    `CMakeLists.txt:12-21` uses `set()` (normal variables), not `option()`, so
    `-D`/preset cache entries are silently shadowed — `build_noffmpeg`'s cache
    shows `ENABLE_SW_DECODE:UNINITIALIZED=OFF` yet FFmpeg was still linked.
    Only `ZC_ENABLE_DEBUG_PANEL` (a real `option()`) toggles. The presets'
    `ENABLE_FRONTEND_API/ENABLE_QT=false` are no-ops that would break the build
    if honored (no `#ifdef` guards exist for them).

## 4. Medium findings by subsystem

**Core source / subprocess / discovery**
- `msg_recv` trusts the subprocess-controlled `msg->length`
  (`src/ssp-client-iso.cpp:71-73`): unchecked `bmalloc` → NULL memcpy on a
  corrupt/huge length; `%d` prints `size_t`.
- **Bitrate setting is dead**: `bufferSize` is stored but `doStart()` passes
  only `--host/--port`; the connector hardcodes its own buffer
  (`ssp_connector/main.cpp:215`). The user's `zc_bitrate` never reaches the
  camera.
- `SspMDnsIterator` locks the global `ssp_records_lock` in its ctor and unlocks
  in its dtor (`src/ssp-mdns.cpp:539-546`) — held across arbitrary caller code;
  an early return leaks the lock and kills discovery; two iterators deadlock.
- mDNS PTR parse underflow: `length - strlen(domain) - 2` on a short name
  (`src/ssp-mdns.cpp:153-154`) → huge length → OOB read.
- `mdns_args::running` is a plain `bool` across threads (race; `pthread_join`
  can hang).
- Tools-menu `QAction` is never removed/disconnected in
  `obs_module_unload` (`src/obs-ssp.cpp:168-172,183-198`) — clicking after
  unload jumps into unmapped code.
- macOS: connector stderr is never drained (Windows-only `dump_stderr`) → the
  child can block on a full stderr pipe; and the camera IP is concatenated
  unquoted into a `popen` command line (`src/ssp-client-iso.cpp:171-180`) —
  a crafted `zc_source_ip` in a shared scene collection injects shell.

**hwdecode**
- D3D11VA VideoProcessor never sets color spaces
  (`src/hwdecode/hw_decode_d3d11_win.cpp:153-159,347-349`): content desc is
  zero-initialized, `VideoProcessorSetStreamColorSpaces` never called → driver
  heuristics; full-range/BT.601 streams get wrong contrast, while the CUDA path
  does honor range/matrix. Plausibly related to open bug #26 (tearing/contrast).
- Coded-size padding is drawn: the D3D11VA pool aligns to 16 (1080→1088) and
  `avf->crop_*` is ignored (`hw_decode_d3d11_win.cpp:300-318`); garbage
  macroblock rows reach OBS and `get_width/height` report coded dims.
- CUDA importer assumes 8-bit NV12 with no `sw_format` guard
  (`hw_decode_cuda_win.cpp:630-651`) → 10-bit on the explicit CUDA backend
  yields a corrupted picture instead of falling back.
- HEVC SPS sub-layer parsing deviates from the spec when
  `max_sub_layers_minus1 > 0` (`src/hwdecode/hw_decode.c:242-259`) → wrong
  bit-depth decision (recoverable via fallback).
- `av_frame_ref` return unchecked (`hw_decode_ffmpeg.cpp:389`); factory
  silently returns Software while `backend_` records hardware
  (`hw_decode.c:75-92` + `zc_source.cpp:750-755`) → spurious rebuilds.

**cameractrl**
- **WS upgrade auth is captured once and goes stale**
  (`zc_http_transport.cpp:497,585-597` + `zc_ws_notifier.cpp:48,87`): `login()`
  clears `auth_header_`; on firmware where `/login/` is a no-op success the
  header is never rebuilt, and the notifier replays its stored headers on every
  reconnect → permanent 401 loop.
- WS frame length truncated with `(int)len` (`zc_ws_client.cpp:226-244`): no
  size cap → mis-sliced frames / unbounded buffer; no WS handshake timeout
  (design promises 10 s — not implemented) and no read-idle watchdog
  (half-open TCP keeps `live_=true` forever).
- `stop()` can't stop an in-flight connect (`zc_ws_notifier.cpp:54-61`): the
  connect later completes and emits `socketStateChanged(true)` **after** stop —
  reviving a stopped notifier.
- **TLS is `VERIFY_NONE` with no TOFU/pinning** (`zc_tls_socket.cpp:265`): a
  LAN MITM can relay Digest challenges (offline-crackable responses) and inject
  fake `ConfigChanged`/status events. Cheap fix: pin the self-signed cert
  fingerprint on first pairing.
- `login()` clears `auth_header_` but not `nonce_` (`zc_http_transport.cpp:539`)
  → Digest is recomputed and re-sent on every later request, contradicting the
  session model.
- `requestSync` bypasses the FIFO and its nested loop lets timers enqueue async
  requests concurrently → two connections to the single-threaded httpd →
  dropped requests.
- `readGroupByKeys` enqueues one 20 s-timeout request per key
  (`zc_settings_engine.cpp:106-142`) → one read against a dead camera jams the
  FIFO for minutes.
- WS backoff `1000 << retry_` violates its own "capped at 10 s" comment
  (`zc_ws_notifier.cpp:150`).

**Dock UI**
- `presets_->setEnabled(!framingActive(status))`
  (`src/ui/zc_control_dock.cpp:4020`) is the only writer of the preset panel's
  enabled state and is **never reset on camera switch** — switching from a
  framing-active camera to one without framing leaves the whole preset/trace
  panel permanently greyed. (Independently found in the WIP review.)
- Range-limit sliders call `pushLimit()` from `valueChanged`
  (`zc_control_dock.cpp:1616-1637`) — one HTTP write **per notch while
  dragging**, unlike every other slider which writes on `sliderReleased`; this
  floods the camera's single-threaded queue and can store intermediate limits.
  (Independently found in the WIP review.)
- `buildRows()` deletes/recreates every control on each group refresh
  (`:1027-1033,1049`) — a refresh landing mid-drag or mid-typing destroys the
  widget under the user and loses the edit. The stream bar already uses the
  build-once/update-in-place pattern; rows should too.
- Rail rebuild (`:503-533`) drops the connected camera's selection on any scan
  that touches a row.
- **Debug panel has no producer**: `ZcHttpTransport::requestLogged` is emitted
  but connected nowhere — even in `ZC_ENABLE_DEBUG_PANEL` builds the log is
  always empty.
- i18n gaps (bug #33 partial): hardcoded "Refresh", four row hints, PTZ "Speed"
  and "Up/Right/Down/Left" (keys exist but unused), "Debug"/"Clear", and the
  Settings subtabs show **raw catalog ids** (`exposure`, `wb`, `ptz`…) in
  zh-CN. All 200 keys the code *does* use exist in both locales; ~69 defined
  keys are unused; 13 en-only keys are unreferenced.

**Build / packaging**
- CPack scaffolding is broken-but-unused: `CPACK_INSTALL_SCRIPT` and
  `clean_package` reference a `cmake_install_post.cmake` nothing generates
  (`CMakeLists.txt:288,293-300`). Real packaging is `installer.nsi` +
  `package.ps1`.
- Vendored `cmake/Modules/FindFFmpeg.cmake` matches components case-sensitively
  while the project passes `AVCODEC/...` — it is dead code today only because
  Qt6's own case-insensitive module wins the search order; any Qt-less
  configure hard-fails.
- `ssp_connector/CMakeLists.txt:9-35` sets `LIBSSP_LIBRARY` only inside
  `if(NOT libssp_POPULATED)` with `PARENT_SCOPE` — on CMake ≤ 3.27 (presets
  allow 3.22) a reconfigure skips it → empty install list → configure error.
- The "macOS Universal" preset sets no `CMAKE_OSX_ARCHITECTURES` — a local
  `--preset macos` builds single-arch despite the label.
- The root `set_target_properties_plugin` override disables the macOS bundle
  properties (no `Info.plist` shipped; verified OBS still loads the bundle).
- `installer.nsi`: uninstaller is never registered in Add/Remove Programs;
  FFmpeg DLLs are left behind on uninstall.
- Vendored `lib/ssp/include/imf/` headers differ in layout from the pinned
  libssp repo the connector builds against — the two copies can drift silently.

## 5. Low / nit highlights

- `get_properties` raises the dock as a side effect (`zc_source.cpp:1497-1506`)
  — OBS API misuse; it fires on internal re-queries (known bug #36).
- Invalid IP in `update()` tears down a healthy connection with only a blog
  warning (no UI feedback) — WIP adds validation but keeps the teardown.
- `VFrameQueue` is dead code with a latent `stop()` deadlock and leaks —
  delete it.
- `ffmpeg-decode.c` has unchecked allocs (OOM null-derefs); AAC decode-init
  failure retries per audio packet (log spam).
- `video_tick` (watchdog) is registered only under `__APPLE__` — on Windows a
  hidden, unrendered source's stall is never noticed.
- `onLoginSubmitted` reports "login rejected" on *any* connect failure,
  including timeouts (`zc_control_dock.cpp:4119-1127`).
- Preset rename validates 1..18 UTF-16 units while the framing path clamps by
  UTF-8 bytes — an 18-char Chinese name can exceed what the camera accepts.
- Debug sanitizer's `\S+` partially redacts quoted values.
- `tests/build_tests.ps1` hardcodes this machine's Qt/MSVC paths and compiles
  against Qt 6.5.3 while the plugin ships against OBS's Qt 6.6.3.
- `scanTimer_` keeps scanning + `obs_enum_sources` every 2.5 s while the dock
  is hidden.

## 6. Tests — genuinely good, but never run in CI

| Test | Verifies | Automated? |
| --- | --- | --- |
| `cameractrl_tests` | schema structure; real Digest-auth + session + WS-upgrade round trip vs a fake QTcpServer camera | fully automated (Qt needed) |
| `decode_test` | D3D11VA hw-decode config matrix on a local H.264 fixture | automated, any D3D11 GPU |
| `pipeline_test` | the **real** `src/hwdecode` sources end-to-end: decode → import → real `OpenSharedResource` → BGRA pixel read-back (auto + cuda) | automated on a GPU Windows box |
| `obs_integration_test.ps1` + `obs_ws_driver.mjs` | inside real OBS: source registered, settings survive defaults, non-blank frames + >1% motion, recording + ffprobe, legacy-IP recovery | needs camera at 192.168.10.174 + installed OBS |

Not covered anywhere: ssp_connector protocol framing, ssp-mdns, `zc_ws_client`
RFC 6455 framing, watchdog/reconnect logic, any macOS test. **CI runs none of
them.** The driver's "never change the camera network mode" RULE is documented
and respected.

## 7. Uncommitted WIP diff (8 files, +980/−53) — assessment

High quality: every hunk is tied to a buglist item, comments record the
measured protocol facts (preset time in seconds, `ptzfChange` live position,
single-socket Digest). Specifics:

- **Good:** `isValidHostAddress` (dotted-quad or hostname, rejects pasted
  URLs); `syncSourceNameToIp` renames only the bare default `ZCamera` (no
  churn); the `opening_`/`pendingClick_` re-entrancy guard around
  `openCamera()` with the crash post-mortem comment; restart backoff
  20/40/90 s; login-rejection notice; framing tab enabled deliberately
  (capability-gated) — the header comment and `progress.md` are now **stale**,
  not the code.
- **Flagged:** range-limit sliders write per-notch on `valueChanged` (§4 UI);
  `presets_->setEnabled` stuck-disabled across camera switches (§4 UI);
  `setMode("to_rec")` on every `connect()` is an intentional but real side
  effect (a camera left in playback by its own web UI is forced to record
  standby) and adds a round trip on the slow single-threaded server — worth a
  settings toggle or at least a doc note; `tryReconnect` treats an
  `opening_`-refused `openCamera()` as offline failure (minor).
- Minor: the quad parser accepts leading-zero octets and would accept a
  5-octet string as a "hostname" (harmless).

## 8. Repo structure & hygiene

- **Clean git**: 119 tracked files, **zero tracked binaries/artifacts**;
  `.deps/` (3 GB), `build_x64/`, `build_noffmpeg/`, `dist/`, `tests/obs_out/`
  are all correctly gitignored. `.git` is 1.8 MB, never gc'd.
- **`/doc/` is gitignored** — README links to `doc/design.md`, `doc/plan.md`,
  `doc/progress.md`, `doc/ui-spec.md`, but a fresh clone contains none of them.
  Either track the docs or drop the links.
- **Two unrelated histories**: `main` (squashed snapshot + 27 commits) vs
  `10bit-video` (28 granular commits, unmerged, behind main). Docs cite commits
  that only exist on the unmerged branch. Decide: rebase/merge the history or
  delete the branch and stop citing orphan hashes.
- `.trae/` (project rules: build → installer → deploy to
  `%ProgramData%\obs-studio\plugins\` after every change; commit only on
  request) is untracked and not in `.gitignore`.
- `build_x64`'s cache is a copied tree pointing at
  `D:\github\streaming\obs-ssp` — stale absolute paths; a reconfigure landmine
  and the reason several configure bugs (§4 build) haven't bitten yet.
- Working tree carries ~3.7 GB of untracked junk (tests 322 MB, dist 107 MB,
  builds 225 MB, doc 23 MB).

## 9. Documentation drift

- `design.md` §3.6 "Known gaps" still says 10-bit unsupported — the code now
  implements P010 for D3D11VA (`hw_decode_ffmpeg.cpp:799-801`).
- `design.md` §4.1's "QThread (network)" diagram and
  `QNetworkAccessManager`/`QWebSocket` references are stale: everything runs on
  the Qt main thread over the custom mbedTLS + RFC 6455 stack.
- `design.md` §8 still describes a "ring buffer"; the code uses a single
  `pending_`/`drawn_` slot pair.
- `progress.md`/`zc_control_dock.h` still say the framing panel is hidden; the
  WIP enables it.
- README: claims Qt WebSockets is required (deliberately avoided); Status says
  zero-copy "falls back to Software" (it doesn't — it's verified working);
  documents the broken `windows-x64` preset; manual-install instructions
  (extract into OBS dir) contradict `package.ps1`'s plugins-dir zip layout.

## 10. Prioritized fix list

1. `SSPClientIso::Stop()` join-before-destroy hang (freezes OBS) — §3.1.
2. videoRender/import ABBA deadlock — §3.2.
3. Detached stderr thread UAF + `reconnectThread_` double-join — §3.4/3.5.
4. `requestSync` nested-loop escape hatch (UI hang on destroy) — §3.3.
5. CI artifact glob + run the existing tests in CI — §3.8, §6.
6. Fix the `windows-x64` preset SDK pin and convert `ENABLE_*` to `option()` —
   §3.9/3.10.
7. Preset-panel enabled reset on camera switch; range-limit sliders →
   `sliderReleased` (both already in the WIP area) — §4 UI.
8. WS upgrade-auth refresh on reconnect + frame-size cap + handshake timeout —
   §4 cameractrl.
9. TLS TOFU pinning (first-seen fingerprint) — §4 cameractrl.
10. D3D11VA color-space/range + crop handling (likely feeds open decode bugs
    #6/#7/#26) — §4 hwdecode.
11. Bitrate control: wire it through or remove it — §4 core.
12. Docs: un-gitignore `doc/` (or fix README links), reconcile the two git
    histories, refresh README status — §8/§9.

---

# Round 2 — verification of the fix pass (same day)

The author then modified 32 files (+1833/−323, uncommitted). Each prior
Critical/High/Medium was re-verified against the current working tree (three
verification agents + independent spot-checks of the sharpest claims).

## Fixed and verified correct

- **Core/subprocess:** `Stop()` hang (connector `TerminateProcess`'d first,
  bounded wait, both reader threads joined before pipe destroy — the mirrored
  `os_process_pipe` struct matches OBS 31's `pipe-windows.c` exactly); stderr
  thread now a joinable member; `reconnectMutex_` + atomic `stopping_` kill the
  reconnect double-join race; **ABBA deadlock** fixed by a `GraphicsScope` RAII
  at the top of `videoRender` (restores graphics → frameMutex_ → decoderMutex_
  order); `msg_recv` 64 MB cap + null-check; macOS popen host quoting;
  Tools-menu QAction disconnected on unload; `video_tick` registered on
  Windows (watchdog covers hidden sources).
- **hwdecode:** CUDA context push/pop now balanced on every path (pop right
  after `cuCtxCreate`); `cuda_render` saves/restores the full D3D11 state set
  it mutates (Get-calls AddRef, Releases correct, no early returns between
  save and restore); D3D11VA color spaces now set per frame
  (`VideoProcessorSetStreamColorSpace1` + D3D11.0 fallback); CUDA importer
  guards `sw_format == NV12` → Software fallback on 10-bit.
- **cameractrl:** WS upgrade headers re-queried per dial
  (`currentUpgradeHeaderBlock`); frame length is `quint64` with a 4 MiB cap
  applied before slicing; 10 s WS handshake watchdog + 60 s read-idle
  watchdog; backoff capped 1/2/4/8/10 s.
- **Dock:** preset panel re-enabled on every camera switch; range-limit
  sliders write on `sliderReleased` (clamp stays in `valueChanged`); rail
  selection restored across rebuilds; debug panel producer wired
  (`requestLogged` → client → `append`, under `ZC_ENABLE_DEBUG_PANEL`); i18n —
  all 223 code-referenced keys now exist in both locales (catalog-label map
  added; only a cosmetic hardcoded "PTZ" label remains).
- **mdns:** iterator snapshots records under a scoped lock; PTR-length
  underflow guard; `running` is `std::atomic`.
- **Build/CI/docs:** `ENABLE_ZERO_COPY/CAMERA_CONTROL/SW_DECODE` are real
  `option()`s (frontend-api/Qt deliberately stay mandatory `set()` with a
  rationale); preset SDK pin raised to 10.0.20348.0 and dead `ENABLE_*` cache
  vars removed; CI artifact list fixed (`obs-zcamera.dll`, `libssp.dll`,
  `if-no-files-found: error`) and dead Linux steps removed; broken CPack block
  deleted (root + linux defaults); `.gitignore` now tracks `doc/*.md`; README
  status/install/Qt-WebSockets corrected.

## Partially fixed — what remains

- **D3D11VA crop (was Medium) — sized but not cropped.** The display size is
  computed and the output texture sized to it, but
  `VideoProcessorSetStreamSourceRect` is never called, so the whole coded
  slice (1920×1088) is *scaled* into the 1920×1080 output (padding smeared in,
  ~0.7% squash), and the enumerator is created at display size while the input
  view is the coded slice — a driver-dependent mismatch that can fail input-
  view creation → import failure → **permanent Software fallback on the Auto
  path**. Fix: size the enumerator to coded dims + set the source rect.
- **Sync-loop watchdog (was High) — added, but the `loginSync` guard timing is
  wrong (new High, see below).**
- **WS stop() race:** `onConnected`/handlers honor `stopped_`, but `close()`
  still no-ops unless `isConnected()` — an in-flight dial completes the full
  handshake after stop.
- **TLS TOFU:** per-host SHA-256 pin in QSettings, hard fail on cert change —
  but fails open when no peer cert / SHA-256 fails, the QSslSocket fallback is
  entirely unpinned, and the cert-change error is misclassified as
  `tls_unavailable` (UI says "switch camera to HTTP" instead of re-pair).
- **mdns spin:** `mdns_loop` still busy-spins when `send_mdns_query` fails
  immediately (no backoff).
- **Framing docs:** header comment (`zc_control_dock.h:351-356`),
  `updatePtzTab` comment, and `doc/progress.md` still say the tab is hidden.
- **Login error reporting:** still claims "password rejected" on timeouts.

## Not fixed (known, deprioritized)

`login()` leaves `nonce_` set (Digest re-sent per request; now also makes WS
reconnects carry a fresh `nc++` Digest while the first upgrade is cookie-only);
`requestSync` bypasses the FIFO; `readGroupByKeys` 20 s-per-key jam;
`disconnect()`/destructor state; `av_frame_ref` return; factory/caller backend
label mismatch; `buildRows` full rebuild (worse — see NEW-1); framing spin
reset to minimum; UTF-16 rename validation; `\S+` sanitizer; scan timer while
hidden; sticky PresetBusy + `updateSpeedControls` trace guard; dead code
(`ZcHud`, `recent_`, `loginHost_`); `get_properties` dock side effect; dead
bitrate control.

## New issues introduced by the fix pass

1. **[High] `loginSync` watchdog can fire while its exchange is alive.**
   Guard = `timeoutMs + 2000` (10 s for login's 8 s), but the two-leg exchange
   *restarts* its own timeout for leg 2 (`zc_http_transport.cpp:312-315`), so
   the exchange can legitimately run to 2×timeoutMs (16 s). If leg 1 takes
   >2 s, the guard quits the loop, `loginSync` returns, and the exchange's
   `[&]`-captured Done callback later writes to dead stack locals (`done`) and
   calls `loop.quit()` on a destroyed `QEventLoop` — stack corruption, or it
   prematurely quits the *next* nested loop (the fresh-connection retry at
   `:515`). Fix: cancel/keep the `HttpExchange*` when the guard fires, size
   the guard as 2×timeoutMs+margin, or capture state via `shared_ptr`.
2. **[Medium] `~ZcControlDock` deletes `client_` while `opening_`** (nested
   loop pumping during OBS shutdown) — the watchdog ends the hang but
   `negotiateOrigin` then resumes on the freed transport (UAF).
3. **[Medium] CUDA size mismatch:** display-size reporting now applies to CUDA
   too, but the CUDA importer still sizes/copies at coded size — texture
   1920×1088 vs reported 1920×1080; `gs_draw_sprite(tex,0,0,0)` draws padding
   beyond the reported bounds.
4. **[Medium] `cuda_render` doesn't save/set/restore the scissor rect**
   (`RSSetState(nullptr)` doesn't reset it) — the conversion draw runs under
   OBS's last scissor → clipped copy, stale regions.
5. **[Medium] PTZ writes now re-read the whole `ptz` catalog**
   (`writeKey` → `groupRefreshed` → `buildRows` full rebuild) — releasing a
   PTZ slider in the Settings pane destroys/recreates the pane moments later;
   the camera's own `configChanged` can trigger the same mid-drag. Amplifies
   the unfixed rebuild-destroys-widgets issue.
6. **[Medium] Deferred camera click skips `restartTimer_->stop()`** — click B
   while a restart probe for A is in flight: B opens, then the next
   `tryReconnect` opens A and tears B's fresh connection down.
7. **[Low] Range-limit cache:** `pushLimit`/`setEdgeToCurrent` never refresh
   the cached limit box, so toggling Range Limit off/on repaints from stale
   boundaries; `sliderReleased` also pushes while the checkbox is unchecked.
8. **[Low] Presets grey-out only via ws `framingStatusChanged`** — the
   open-time `framingQuery` snapshot never gates it, so a framing-active
   camera shows an enabled preset pane until the first ws event.
9. **[Low] `updatePtzCommonRows` reads the combo's display text** ("1"/"time")
   — breaks on localized choice labels; prefer item data / cached value.

## Round-2 verdict

The fix pass is high quality: every prior Critical and most High/Medium
findings are genuinely fixed with correct, well-commented implementations,
and the build/CI/docs debt was cleaned up in the same pass. Remaining risk is
concentrated in: (1) the new `loginSync` guard-timing bug (stack corruption —
fix before shipping), (2) the D3D11VA crop half-fix (visual squash + a
driver-dependent path to permanent Software fallback on the default Auto
backend), and (3) the mid-interaction pane rebuild, which the new PTZ
catalog re-read made more likely to bite. None of the fixes regressed the
verified working paths (single-socket Digest continuation intact; no new
locks or graphics nesting).
