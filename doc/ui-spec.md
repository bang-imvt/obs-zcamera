# OBS ZCamera Plugin — UI Specification

This is the customer-facing UI specification for the **obs-zcamera** OBS Studio
plugin. The plugin adds ZCAM (SSP) network cameras as OBS sources with true
zero-copy hardware decoding, and embeds a full camera-control panel directly in
OBS — mirroring the layout, grouping, and behavior of the standalone
`ZCamGuiOpen` client so operators have a familiar surface.

The plugin UI has two distinct surfaces:

1. **Native Qt camera-control dock** (the primary new work) — a floating/
   dockable OBS panel with a camera rail and a Settings/PTZ tab pane. This
   replaces the previous "embed the camera's web page in a browser" toolbar.
2. **OBS source properties** — the per-source connection/stream settings that
   stay in OBS's standard source dialog.

## Design principles

- The first thing an operator sees should let them find a camera and get a
  working preview fast.
- Camera selection and connection state stay visible at all times.
- Show clear camera state and action results without exposing protocol details.
- Separate safe read operations from commands that can stop recording, restart
  a stream, or disconnect a camera.
- The operator watches the camera through OBS's own preview; the dock does not
  render a second copy of the stream.
- Do not reproduce the full settings matrix in the first version; expose the
  same representative groups as `ZCamGuiOpen` (Exposure, WB, Focus, Image,
  Record, Video, Stream, Audio, System, PTZ).

## Color and visual language

Follow the `ZCamGuiOpen` palette so the two products feel consistent.

| Token | Value | Use |
| --- | --- | --- |
| Accent gold | `#d8b878` | Active tab indicator, slider value text, active rail item border |
| Panel bg | `#1f2927` | Settings active subtab background |
| Text light | `#eef2ed` | Primary text on dark panels |
| Muted blue-grey | `#8a9bb2` | Secondary text, inactive tab labels |

## Application frame (camera-control dock)

```text
+------------------------------------------------------------------+
| Camera Rail | Settings / PTZ tabs                               |
| ▤          |                                                     |
| +-------+  |  [ Settings ] [ PTZ ]            [Refresh]          |
| | Cam A |  |  +-----------------------------------------------+  |
| | .10.1 |＋|  | STREAM  Codec[h264▾] Res[1920x1080▾] FPS[30▾] |  |
| +-------+  |  |         Bitrate[====] 8000 kbps  GOP[==] 30   |  |
| +-------+  |  +-----------------------------------------------+  |
| | Cam B |✓ |  Exposure        ▸                                  |
| | .10.2 |  |  WB              ▸                                  |
| +-------+  |  Focus           ▸                                  |
|            |  Image           ▸                                  |
|            |  Record          ▸                                  |
|            |  Video           ▸                                  |
|            |  Audio           ▸                                  |
|            |  System          ▸                                  |
|            |  PTZ             ▸                                  |
+------------------------------------------------------------------+
| Status: Ready | Camera: Z CAM E2 | 192.168.10.178:80 | [Debug]   |
+------------------------------------------------------------------+
```

A top-level toggle switches between **Camera Control** (dock) and the OBS
**Sources** panel. The dock header keeps the selected camera and its
connection state visible on every panel.

## Panels

### 1. Camera rail (left, ~208 px)

A vertical list of cameras, fed by the existing mDNS (`_eagle._tcp`) discovery,
manual IP entry, and the `ZCamera <ip>` sources OBS already holds.

- Each row is drawn as **two lines** — the camera name above its address — so a
  model name (`P2-R1-18xx`) and the address that identifies the OBS source both
  stay readable without widening the rail. A camera known only from its source
  shows the address alone on the first line and an `(offline)` marker on the
  second until it answers (L11).
- The active camera gets a gold left border.
- Camera rows are populated/refreshed by the same `SspMDnsIterator` used today,
  on a refresh timer.
- **Each row carries its own action chip** on the right: a gold-bordered `＋`
  while the camera is not an OBS source, replaced by a gold check once it is
  (L5/L6). There is no bottom Add button — the operator acts on the camera they
  are looking at instead of having to select it first. Clicking the chip does
  **not** change the selection, so the gold border keeps marking the camera the
  dock is controlling.

#### Camera rail ↔ OBS source lifecycle (normative)

The rail owns exactly two concerns: **control** (which camera the dock's
Settings/PTZ tabs talk to) and **presence** (whether that camera exists in OBS
as a source). They are deliberately decoupled — selecting a camera for control
never mutates the scene.

| # | Rule |
| --- | --- |
| L1 | The rail lists every discovered camera regardless of whether it is in OBS. Discovery and presence are independent. |
| L2 | A **single click** on a row selects that camera and connects the control client to it. It **must not** create, update, or add an OBS source. |
| L3 | The source that represents a camera is identified solely by its name, `ZCamera <ip>`. Any source with that exact name counts as "added", whoever created it. |
| L4 | "Added" is evaluated live from OBS (`obs_get_source_by_name`), never from a cached flag. The row's chip is recomputed on: every discovery refresh, every OBS source create/destroy/rename, and immediately after the chip runs. |
| L5 | Each row's **action chip** acts on its own row: a gold `＋` while that camera is not an OBS source (add), a gold **check** once it is (remove). There is no bottom Add button, so the action never depends on the selection. |
| L6 | Clicking a row's **＋** chip creates the source if it does not exist and adds it to the current scene; if the source exists but is not in the current scene it is added to it; if it is already in the current scene the action is a no-op. The chip names its own camera, so nothing has to be selected first, and the click must not change the selection. |
| L7 | Re-adding an existing source re-applies `zc_source_ip` through `obs_source_update()`. This is a self-heal for sources persisted before the plugin's `get_defaults` stopped clobbering create-time settings (they hold an empty IP and would stay black); an unchanged IP is ignored by OBS, so it never restarts a healthy stream. |
| L7a | The same self-heal must also work **without any dock interaction**: a source whose stored `zc_source_ip` is empty derives its address from its own name (`ZCamera <ip>`, L3) when it is loaded or started. A scene collection from before the `get_defaults` fix therefore renders on the next OBS launch instead of staying black until the operator re-adds the source. A source that does not follow the `ZCamera <ip>` naming keeps its empty address and is unaffected. |
| L8 | Clicking an added row's **check** removes that source from OBS (`obs_source_remove`: out of every scene, then destroyed with its last reference), so the rail and OBS's Sources list agree without the operator hunting the row down in OBS. It is a single normal OBS operation, so undo/redo, scene persistence and hotkeys behave exactly as for a removal made in OBS itself; the chip turns back into `＋` on the next presence re-evaluation (L4). OBS's own Sources list remains a valid way to remove the source. |
| L9 | Renaming a source in OBS's Sources list to something other than `ZCamera <ip>` makes the rail treat the camera as not-added, and vice versa. This follows from L3/L4 with no extra code. |
| L10 | Closing the dock, or selecting another camera, must not disturb any source or its stream. |
| L11 | The rail also lists cameras that discovery does **not** advertise but that OBS holds as a source (`ZCamera <ip>`). Such a row shows the address only, plus an `(offline)` marker and a muted glyph, until the camera answers a control connection; it is then indistinguishable from a discovered row. An offline row stays selectable, but the dock must leave **no client** behind on a failed connect — no settings, PTZ, preset or trace control may be operable — and the status bar reads `Offline: <ip> — not reachable, control disabled`. |
| L12 | Creating a `zcamera_source` anywhere in OBS (its own Sources list, a hotkey, or an import) **raises the control dock** so the operator can immediately pick that camera and add it to a scene. The raise is wired to libobs's `source_create` signal and is armed only after `OBS_FRONTEND_EVENT_FINISHED_LOADING`, so restoring a scene collection at startup — which creates every stored source at once — does not steal focus. The signal arrives off the Qt thread, so the raise is marshalled with a queued invocation. Opening the source's **Properties** dialog raises the dock the same way — that dialog is deliberately minimal (a hint plus the address) because the dock is the real surface for a ZCamera source, and `get_properties` is the only hook OBS offers for "the operator opened this source". A side effect of that hook is that reading a source's properties through an API (obs-websocket, a script) raises the dock too. |

```text
discovered camera, not in OBS      →  two lines, chip ＋   |  clickable (adds)
discovered camera, in OBS          →  two lines, chip ✓   |  clickable (removes)
source in OBS, not discovered      →  address + (offline) |  chip ✓, control disabled
source in OBS, not discovered, but answers  →  two lines, chip ✓  |  control enabled
removed in OBS (the chip, or its Sources list)  →  chip back to ＋  |  clickable again
```

### 2. Settings pane (right, "Settings" tab)

The dock does not render its own preview: the operator watches the camera
through OBS's preview of the `zcamera_source`, and the dock only drives the
camera's settings and PTZ.

#### Stream bar (top of the tab, above the group list)

How the camera encodes its network stream is the setting an operator reaches for
most, so it is **not a group**: it is a compact two-row bar pinned above the
group list, visible without navigating anywhere. It is deliberately **absent
from the group list** (no `stream` entry), because a group can be navigated away
from while the bar cannot.

```text
STREAM   Codec[h264▾]  Resolution[1920x1080▾]  FPS[30▾]     app_stream1 · 8bit · streaming
Bitrate [============] 8000 kbps   GOP [====] 30
```

- Row 1: the encoder index's own state (`<index> · <bitwidth> · <status>`)
  read-only on the right, and the three choice controls.
- Row 2: the two numeric controls, each with its live gold value.
- Before a camera is connected — or while its document is being read — the bar
  is **disabled** and the info text reads `no camera` / `reading…`. The controls
  are created once and updated in place (never rebuilt), so a write's reply can
  never close an open combo popup; every control is filled with its signals
  blocked so the repaint is never mistaken for an operator change.
- A field the camera marks read-only (`*Ro`) disables just that control.
- Slider ranges come from `maxBitrate` / `maxGop`, widened to the current value
  when the camera reports a value above its own hint.
- Writes go out on **release** for the sliders (a drag must not send a request
  per pixel) and on change for the combos, then the bar repaints from the
  camera's reply rather than assuming — a `fps` write also rescales `gop_n`.
- **A codec or resolution write drops the camera's stream**, and that is
  expected: the bar is not a preview control, and the operator's picture comes
  back on its own. The source is built for it — 10 s of silence trips the stall
  watchdog, `onDisconnected()` escalates its reconnect delay (3/6/10/15 s), and
  `start()` rebuilds the decoder from the new `onMeta` dimensions. The bit depth
  is re-sniffed **per connection**: a switch to a 10-bit encoder (`h265_10b`)
  must not inherit the previous connection's depth, or the new decoder would be
  opened against the old frames context and stay black.

#### Group list and rows

A vertical list of grouped subtabs (gold right border on the active subtab).
Subgroups under System (maintenance, network, security, multicam, timecode) are
indented with a left border. The **PTZ** subtab appears only when the camera
reports PTZ support (`/info` feature `lens_ctrl_ptz === true` or model contains
`ptz`).

Each setting row uses a fixed three-column grid:

```text
Label (230px)  |  current value text  |  control (260px)
```

- Choice keys (`type 1`, `opts[]`) → `QComboBox`.
- Range keys (`type 2`, `min/max/step`) → `QSlider` with a live gold value
  label.
- Text keys (`type 3`) → `QLineEdit`.

Rows load their current value automatically; writes fire on change
(matching the `ZCamGuiOpen` simplification — no per-row Read/Set buttons).
Unsupported (`code === -1`) and read-only rows are hidden. Group-level
`Refresh` re-reads all values in the group. The settings engine is a direct
port of the ZCamGuiOpen settings engine (`/ctrl/getbatch?catalog=<name>`,
post-write dependency re-reads).

Groups exposed in v1 (the `ZCamGuiOpen` catalogs):

- **Exposure** (`exposure`): `ev_choice`, `flicker`, `meter_mode`, `iris`,
  `iso`, `min_iso`, `max_iso`, `iso_ctrl`, `shutter_angle_ctrl`,
  `sht_operation`, `eND`, `lock_ae_in_rec`, `ae_speed`, `bl_comp`.
- **WB** (`wb`): `wb`, `wb_priority`, `lock_awb_in_rec`, `mwb` (Manual mode),
  `tint` (Manual mode), `mwb_r/g/b` (Expert mode).
- **Focus** (`lens`): `focus`, `af_mode`, `caf`, `caf_sens`, `af_area`,
  `live_caf`, `mf_mag`, `mf_recording`, `af_speed`, `af_adjust_with_ptz`,
  `zoom_mode`, `lens_focus_pos`, `lens_zoom_pos`, `fz_speed`.
- **Image** (`image`): `lut`, `sharpness`, `noise_reduction`, `luma_level`,
  `brightness`, `contrast`, `saturation`, `hue`.
- **Record** (`record`): `resolution`, `project_fps`, `vfr_ctrl`, `movvfr`,
  `record_file_format`, `rotation`, `split_duration`, `rec_fps`, `preroll`,
  `video_tl_interval`, `record_meta`, `camera_id`, `reelname`.
- **Video** (`video`): `video_encoder`, `bitrate_level`, `record_mode`,
  `compose_mode`, `rec_proxy_file`, `crop_sensor`, `low_jello`, `photo_q`,
  `eis_on_off`, `vid_rot`.
- **Stream**: **not a group** — these controls live in the stream bar above the
  group list (see above), so no `stream` entry may appear in the list. The bar
  is fed by one `/ctrl/stream_setting` document per encoder index, read with
  `GET /ctrl/stream_setting?action=query&index=<index>`.
  **Which index exists is the firmware's choice, so it is discovered, not
  assumed** — `app_stream1` on newer firmware, `stream1` otherwise. An index the
  camera does not have answers `{"code":0,"streaming":false}` instead of a
  document, and a document is recognised by its `streamIndex` field. The index
  is therefore **shown read-only** in the dock, never offered as a selector.

  The newer `app_stream1` document adds the *choices* and their read-only
  flags, which is what makes codec, resolution and frame rate selectable at
  all:
  `{ streamIndex, bitwidth, bitrate, bitrateRo, gop_n, rotation,
  splitDuration, status, maxBitrate, gopRo, width, height, resolutionRo,
  resolutionList, sample_unit, maxGop, fps, fpsRo, fpsList, encoderType,
  encoderRo, videoEncoderList }` — e.g.
  `{"streamIndex":"app_stream1","bitwidth":"8bit","bitrate":8000,
  "bitrateRo":false,"gop_n":30,"rotation":0,"splitDuration":300,
  "status":"idle","maxBitrate":300000,"gopRo":false,"width":1920,
  "height":1080,"resolutionRo":false,"resolutionList":["3840x2160",
  "1920x1080","1280x720","848x480"],"sample_unit":1001,"maxGop":120,"fps":30,
  "fpsRo":false,"fpsList":[30],"encoderType":"h264","encoderRo":false,
  "videoEncoderList":["h265_10b","h265","h264"]}`.
  The older `stream1` document is the same minus every `*List` / `*Ro` field,
  so nothing about codec, resolution or fps can be changed through it; the dock
  shows those as the camera's own settings, which is what they are.

  Traps, all verified against firmware:
  - **`action=query` also writes.** The same request with value parameters
    applies them and replies with the updated document.
    `action=set` is rejected outright (`{"code":-1}`), so it must not be used.
  - **The write parameter names are not the document's field names.** Verified
    one field at a time; every other spelling leaves the field untouched:
    | document field | write parameter |
    | --- | --- |
    | `bitrate` | `bitrate`, in **bps** |
    | `gop_n` | `gop_n` |
    | `width` + `height` | `width` **and** `height` together, one `resolutionList` entry |
    | `encoderType` | `venc`, one `videoEncoderList` entry |
    | `fps` | `fps`, one `fpsList` entry |
  - **An advertised encoder can still be refused.** On 192.168.10.174's
    `app_stream1`, `venc=h265` and `venc=h264` apply, but `venc=h265_10b` — a
    value the camera itself lists in `videoEncoderList` — is **silently
    ignored**: no error, `code:0`, and the reply still reports the old encoder.
    (`bitwidth=10bit`, `venc=h26510b`, `venc=hevc_10b` and adding an explicit
    `width`/`height` were all tried; none apply.) A codec combo must therefore
    repaint from the reply rather than from what was selected, or it would show
    an encoder the camera never took.
  - A **resolution** write applies on a live stream; the camera then drops it
    and the source reconnects on its own (see the stream bar section).
  - **Units differ per direction.** `bitrate` reads in kbps and writes in bps:
    writing `8000` reads back as `8`, writing `8000000` reads back as `8000`.
    A kbps slider must therefore be multiplied by 1000 on the wire.
  - `bitrateRo` / `gopRo` / `resolutionRo` / `fpsRo` / `encoderRo` mark a field
    read-only, `maxBitrate` / `maxGop` its range (a hint, not a hard bound —
    `stream3` on 192.168.10.174 reported `bitrate` 60000 against `maxBitrate`
    50000), and `bitwidth` is not writable (`none` on an idle stream, `8bit` /
    `10bit` otherwise). A write of `fps` also rescales `gop_n` (60 fps → 60),
    so the dock repaints from the camera's reply rather than assuming.
  - `app_stream1` and `stream1` are two views of the same encoder: a write
    through either is visible in both.
  RTMP push is a separate document, `GET /ctrl/rtmp?action=query` →
  `{"url":"","key":"","bw":0.0,"status":"idle","autoRestart":0,"code":0}`
  (`status` is `idle`/… , `bw` the measured bandwidth).
- **Audio** (`audio`): `primary_audio`, `audio_channel`,
  `audio_phantom_power`, `audio_level_display`, `ain_gain_type`,
  `audio_input_level`, `audio_input_gain`, `audio_noise_reduction`,
  `audio_in_l_gain`, `audio_in_r_gain`, `audio_output_gain`.
- **System** (`system`): `hdmi_fmt`, `hdmi_osd`, `use_edid`, `osd_layout`,
  `led`, `desqueeze`, `usb_device_role`, `tally_on`, `ir`, `ir_id`,
  `color_bar_enable`, `genlock`, `sdi`, `3g_sdi_mode`, `visca_enable`,
  `visca_id`, `visca_baud_rate`, `lcd_backlight`, `gl_shf_coarse`,
  `gl_shf_fine`.
- **PTZ** (`ptz`, conditional): `pt_speedmode`, `ptz_flip`, `ptz_limit`,
  `ptz_preset_mode`, `ptz_speed_mode`, `freeze_during_preset`, `pt_pwr_pos`,
  `pt_priv_mode`, `preset_adjust_speed_with_zoom`, `pt_speed_with_zoom_pos`,
  `ptz_common_speed_unit`, `ptz_common_speed`, `ptz_common_time`.
  This group is camera-side PTZ configuration, so it is offered only for a
  camera that reports PTZ support — the same gate as the PTZ tab (§3). It is
  added to the group list when the `/info` reply says the camera has a pan/tilt
  head, and is absent for a camera that does not.

### 3. PTZ pad (right, "PTZ" tab)

#### Tab gating (normative)

The **PTZ** tab is created only when the connected camera reports PTZ support.
Support is read from the `/info` reply the client fetches on connect:

```text
supportsPtz = (info.feature.lens_ctrl_ptz === true)
              || model name matches /ptz/i
```

- Before a camera is connected, or when the connected camera reports no PTZ
  support, the tab is removed from the tab bar and the Settings tab is shown.
  The pane must be **hidden** while it is not a tab: a widget parented to the
  `QTabWidget` without being inserted is a plain child, so it would otherwise
  be painted on top of the current page (it covered Settings until the first
  camera was selected).
- When a camera that supports PTZ connects, the tab is inserted (after
  Settings) and the pane is built.
- Switching to a camera without PTZ removes the tab again; the pane is only
  ever shown for a supporting camera. `lens_ctrl_ptz` is absent on
  non-PTZ bodies (e.g. box cameras), which is the normal "not supported" case.

#### Joystick

An 8-way joystick pad, plus a speed slider, `Home` and `Stop`:

- Directional move → `/ctrl/pt?action=pt&pan_speed=<p>&tilt_speed=<t>`
- Center drag → same endpoint with the analog pan/tilt pair
- Release / `Stop` → `/ctrl/pt?action=stop`
- `Home` → `/ctrl/pt?action=home`
- Joystick speed slider → scales the pan/tilt pair sent by the joystick only
  (it is not the camera's stored `ptz_common_speed`).

#### Slot list: presets and traces

Full parity with the camera's own web UI preset page, which lists **two slot
families**: presets (`/ctrl/preset`) and traces, a.k.a. trajectories
(`/ctrl/ptrace`). A `Preset` / `Trace` selector at the top of the pane chooses
which family the grid shows; the pane title reads `PRESETS` / `TRACES`.

Both families use the **same wire index space**: 100 slots, 10 per page,
10 page buttons (page `p`, slot `s` → absolute index `p * 10 + s`), **0-based**
on the wire, displayed 1-based and zero-padded to 3 digits (`Preset 001` /
`Trace 001`). The camera only adds a 2000 base offset to the index it reports
in a `PresetChange` notification for traces — never to the index these
endpoints accept. Switching family clears both the name and thumbnail caches:
the two families share indexes but no entries.

- Each slot shows its thumbnail and name. The name is read per slot with
  `?action=get_info&index=N`; a slot whose `name` is empty is empty (no
  thumbnail is fetched, the label shows the default `Preset NNN` / `Trace NNN`).
- Thumbnail URLs (fetched through the dock's authenticated transport):
  `/app_data/preset/thm_<index padded to 3>.jpg?act=thm` for presets and
  `/app_data/trace/thm_<index padded to 3>.jpg?act=thm` for traces. A failed
  fetch hides the image and shows the name text instead. Thumbnails are loaded
  for the visible page only, and cached per index.
- **Selecting** a slot: click toggles selection; clicking the selected slot
  again clears it (`selected = -1`). No action is available while nothing is
  selected.
- **Rename** makes the slot's name field editable; the new name is committed on
  Enter or focus-out. The name must be 1..18 characters; `&` and `=` are
  stripped as they are typed/pasted (they would break the query string).
  A rejected rename reverts the field to the stored name. Both families share
  the rule and the field.

##### Presets

| Action | Wire call |
| --- | --- |
| `Recall` | `/ctrl/preset?action=recall&index=N` |
| `Add` / `Replace` | `/ctrl/preset?action=set&index=N` |
| `Delete` | `/ctrl/preset?action=del&index=N` |
| `Rename` | `/ctrl/preset?action=set_name&index=N&new_name=<url-encoded>` |

- The second button's label toggles **Add** (slot has no name) vs **Replace**
  (slot has a name). `Add`/`Replace` clear the cached thumbnail and re-fetch it
  after the write.
- **Per-preset speed**: each preset can store its own recall speed. A
  `Speed`/`Time` unit selector chooses the interpretation
  (`/ctrl/preset?action=preset_speed&index=N&preset_speed_unit=U`):
  - unit `0` (speed) → `/ctrl/preset?action=preset_speed&index=N&preset_speed=S`
    with `S` in `1..max(ptz_common_speed)`.
  - unit `1` (time) → `/ctrl/preset?action=preset_speed&index=N&preset_time=T`
    with `T` in milliseconds, offered as `1..max(ptz_common_time)/1000` seconds.
  The maxima are read from `/ctrl/get?k=ptz_common_speed` and
  `k=ptz_common_time` (the `max` field of the reply). The whole `RECALL SPEED`
  row is hidden in trace mode — traces have no stored speed.
- A non-zero `status` in the `get_info` reply (the camera is mid-move) is shown
  as a hint under the grid.

##### Traces

| Action | Wire call |
| --- | --- |
| `Start Record` | `/ctrl/ptrace?action=rec_start&index=N` |
| `Stop Record` | `/ctrl/ptrace?action=rec_stop` (no index) |
| `Prepare` | `/ctrl/ptrace?action=play_prepare&index=N` |
| `Play` | `/ctrl/ptrace?action=play_start` (no index) |
| `Stop Playback` | `/ctrl/ptrace?action=play_stop` (no index) |
| `Delete` | `/ctrl/ptrace?action=del&index=N` |
| `Rename` | `/ctrl/ptrace?action=set_name&index=N&new_name=<url-encoded>` |
| status | `/ctrl/ptrace?action=query` → `st_code` |

The trace transport is a single camera-wide state machine, so the first two
buttons are **relabelled and re-enabled per state** instead of being plain
actions. The state is read with `action=query` (`st_code`) and re-read after
every trace command:

| `st_code` | State | Record button | Play button | Delete | Page/slot switching |
| --- | --- | --- | --- | --- | --- |
| 0 | `TRACE_NONE` | `Start Record` | `Prepare` | yes, if named | yes |
| 6 | `TRACE_READY_FOR_RECORD` | `Start Record` | `Prepare` | yes, if named | yes |
| 1 | `TRACE_RECORDING` | `Stop Record` | disabled | disabled | disabled |
| 2 | `TRACE_PREPARING` | disabled | disabled | disabled | yes |
| 3 | `TRACE_READY_FOR_PLAY` | `Start Record` | `Play` | yes, if named | yes |
| 4 | `TRACE_PLAYING` | disabled | `Stop Playback` | disabled | disabled |
| 5 | `TRACE_DELETING` | `Start Record` | `Prepare`, if named | disabled | yes |

- `Start Record` needs a selected slot (it records into that slot, overwriting
  any trace already stored there — the web UI asks for confirmation first, this
  pane does not). `Stop Record` and the play controls are transport-wide, so
  they do not need a selection.
- `Prepare` needs a stored trace in the selected slot; `Play`/`Stop Playback`
  act on whatever trace the camera has prepared, so they never do.
- While recording or playing, slot and page switching is disabled: the
  selection would otherwise point at a different take than the running one.
- The current state is echoed as a hint under the grid (`Trace is recording —
  press Stop Record to keep it.`, `Trace is ready to play.`, `Trace is
  playing.`, or a busy notice while preparing/deleting).

### 4. Debug panel

A togglable panel (default closed, opened by the `Debug` button in the dock
status bar). Intended for engineers, not the normal operator.

```text
[HTTP] [WebSocket] [Errors]              [Clear] [Pause] [Copy]
10:42:01  HTTP  GET  /camera_status                 200    84 ms
10:42:02  WS    camera_status                       event
10:42:03  HTTP  GET  /ctrl/rec?action=query         200    61 ms
```

- Timestamp, direction, transport, method, path, status, duration.
- Sanitized request/response summaries.
- Parsed WebSocket event names.
- **Must never show** passwords, authorization headers, cookies, or secrets.
- Bounded in memory; `Clear` empties it. High-frequency events (`hb`,
  `RecUpdateDur`, `RecUpdateRemain`, `TempUpdate`, `live_*` telemetry) are
  suppressed or deduplicated.
- Opening/closing the debug panel must not change camera behavior.

### 5. OBS source properties

The standard source dialog keeps the existing connection settings:

- Source IP (manual or selected from discovered list)
- **Hardware acceleration**: `ssp_recv_hw_accel` — now selects the decode
  backend: **Auto / CUDA (NVDEC) / D3D11VA / VideoToolbox / Software**.
- Resolution, frame rate, low-noise, bitrate, stream index, encoder.
- Latency mode, sync mode, wait-for-I-frame, LED-tally.

## Action states

- `Disconnected`: only the camera rail is active. No camera is selected, so the
  Settings and PTZ panes stay disabled; a row's `＋` chip is still operable
  because it names its own camera (L6).
- `Connecting`: show probe progress; prevent a second connect.
- `Connected`: enable the Settings tab, plus the PTZ tab when the camera
  reports PTZ support (see §3).
- `Busy`: disable the same action button and show the exact operation in
  progress.
- `Camera unavailable`: preserve the selected camera and offer reconnect; do
  not discard discovery data.
- `Authentication required`: show a credential form (Digest login) without
  putting credentials into the URL or debug log.

## Status indicator

A small connection indicator for camera state notifications:

- `Live` — WebSocket connected and receiving notifications.
- `Reconnecting` — notification socket unavailable; periodic HTTP refresh
  active.
- `Refresh required` — both notification and HTTP refresh failed.

## Responsive behavior

- Wide dock: two-column (camera rail | Settings/PTZ tab pane) layout.
- Narrow dock: the rail collapses to a narrower list; the tab pane keeps its
  full width.
- The dock is resized to 1040x640 on construction so a settings row's label and
  control are never clipped.

## Non-goals for v1

- Firmware upgrade, certificate management, user administration, network
  configuration, power operations, and direct RTSP playback inside OBS.
- Cloud connectivity or remote-network discovery.
- Full parity with the camera's own web settings matrix.
