# OBS ZCamera Plugin (obs-zcamera)

Network A/V in OBS Studio with **Simple Stream Protocol (SSP)** cameras, built
for **Windows** and **macOS**, with **true zero-copy hardware decoding** and an
**embedded camera-control dock** mirroring the standalone `ZCamGuiOpen`
client.

This project is derived from the `obs-ssp` plugin (source-registration, mDNS
discovery, the SSP subprocess pipeline and the Qt controller are reused
unchanged). It adds two things on top:

1. **Zero-copy hardware decode** — decoded frames stay on the GPU and are drawn
   by OBS directly (no `av_hwframe_transfer_data` GPU→CPU copy, no CPU→GPU
   upload):
   - Windows: CUDA (NVDEC) → shared D3D11 texture → OBS `gs_texture`.
   - macOS: VideoToolbox → IOSurface → OBS `gs_texture`.
   - Software CPU fallback when no hardware path is available.
2. **Camera-control dock** — a native OBS panel with a camera rail, a settings
   pane (all catalog groups), a PTZ pad, and a sanitized debug panel. The camera
   protocol (HTTP Digest auth, origin negotiation, `/ctrl/*` settings, WebSocket
   port 81 notifications) is a direct port of `ZCamGuiOpen`. The operator
   watches OBS's own preview of the source; the dock renders no preview of its
   own, and record/photo are not wired in v1.

## Status

Scaffolding and the first implementation slice are in place (see
[`doc/progress.md`](doc/progress.md)). The zero-copy backends currently fall
back to Software; the GPU interop and the source `video_render` integration are
the next implementation milestones (see [`doc/plan.md`](doc/plan.md)).

## Documentation

| Doc | Contents |
| --- | --- |
| [`doc/ui-spec.md`](doc/ui-spec.md) | Customer-facing UI specification for the control dock |
| [`doc/design.md`](doc/design.md) | Architecture: zero-copy decode + camera control |
| [`doc/plan.md`](doc/plan.md) | Phased implementation roadmap |
| [`doc/progress.md`](doc/progress.md) | Living status + change log |

## Features

- **SSP source** — add a ZCAM camera as an OBS input source (`ssp_source`).
- **Automatic camera discovery** via mDNS (`_eagle._tcp`), plus manual IP entry.
- **Low-latency streaming** from SSP cameras (H.264/HEVC + AAC).
- **Zero-copy hardware decode** (in progress): NVDEC/D3D11VA on Windows,
  VideoToolbox on macOS.
- **Camera control dock** (in progress): a per-camera rail whose rows carry their
  own add/remove chip, a settings pane with the stream controls pinned on top, a
  PTZ pad with presets and traces, and a sanitized debug panel.
- **Automatic decode backend**: the source resolves the best hardware path
  (D3D11VA / NVDEC / VideoToolbox) at runtime and falls back to Software when no
  hardware frame can be imported.

## Requirements

- OBS Studio ≥ 31.0.0 (with the obs-browser module for the legacy toolbar).
- CMake 3.16+, a working OBS/obs-deps/Qt6 toolchain for the target platform.
- For CUDA decode on Windows: an NVIDIA GPU with NVDEC and a CUDA-capable
  build. Without one, the plugin falls back to D3D11VA or Software.

## Building

The build follows the standard OBS plugin CMake layout inherited from
`obs-ssp`:

```sh
cmake -S . -B build --preset windows-x64
cmake --build build --config RelWithDebInfo
```

macOS uses the Xcode generator with universal binaries; see `CMakePresets.json`.
FFmpeg (`avcodec`, `avutil`) and Qt6 (Widgets, Network, WebSockets) are required.

## Manual install

#### Windows
1. Extract the ZIP to your OBS Studio installation directory.
2. Files land at:
   - Plugin DLL: `obs-plugins/64bit/obs-zcamera.dll`
   - `ssp-connector.exe` + `libssp.dll`: `obs-plugins/64bit/`
   - Data: `data/obs-plugins/obs-zcamera/*`

#### macOS
1. Extract the package.
2. Copy the `obs-zcamera.plugin` directory to
   `~/Library/Application Support/obs-studio/plugins/`.

## Usage

1. Launch OBS Studio.
2. Add a source: `+` → **SSP Source**, pick an IP (or a discovered device).
3. The source's Properties dialog is deliberately minimal — it only points at
   the dock, which is where a ZCamera source is actually driven. The decode
   backend is resolved automatically.
4. Open **Tools → ZCamera Control** for the embedded camera-control dock.

## License

GPL v2. See [LICENSE](LICENSE).

## Acknowledgments

- OBS Studio team.
- IMVT for the SSP protocol specification and `libssp`.
- The `obs-ssp` project (Yibai Zhang / summershrimp) which this is derived from.
- The `ZCamGuiOpen` client whose camera-control protocol and UI this mirrors.
