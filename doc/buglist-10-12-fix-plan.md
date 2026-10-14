# obs-zcamera fix plan — `doc/OBS-Zcamra-buglist-10-12.xlsx`

Source: 104 rows (Excel rows 2–105). `#N` below = the Excel **sheet row**
number so it maps 1:1 to the tester's file.

**Overriding requirement (user):** for every item the plugin's behaviour must
match the camera's **web page**. The authoritative references are:

- `d:\imvt_git\ZCamGuiOpen\app\shared\settings-schema.js` — group/key/label,
  `DEPENDENT_GROUPS`, `BOOLEAN_PAIRS`, `HUD_KEYS`.
- `d:\imvt_git\ZCamGuiOpen\js\api.js` — wire endpoints (PTZ, preset, ptrace,
  framing, gamma, system).
- The camera's own web UI (`controller.min.js`) for conditions the snapshot
  schema does not carry (autoframing enable rules, range-limit greying).

Where a row's `RD feedback` column says "确认网页逻辑，按网页的来" /
"网页就是这样的", that row is a **parity fix**, not a new feature.

## Status tally (from columns E / F)

| Bucket | Rows | Action |
| --- | --- | --- |
| `New` | 3–30, 39, 41, 42, 44–46 | fix |
| `仍存在` (still-open) | 47, 49, 51, 68, 70, 76, 85, 87, 94, 96–100, 104, 105 | fix |
| `pending` (Avatar, "暂时保持") | 31–38 | **won't fix** (product said keep) |
| `not issue` | 50, 59, 71, 93, 102 | no change |
| `fixed` | 2, 40, 43, 48, 52–58, 60–67, 69, 72–75, 77–84, 86, 88–92, 95, 101, 103 | re-verify only |

**Actionable total: 58 rows.** Severity: mostly 中 (medium), a few 低.

---

## 0. Cross-cutting infrastructure (do first — many rows depend on it)

These three mechanisms do not exist yet and are prerequisites; without them
the per-row fixes become one-off hacks.

### 0.1 Firmware-version gating
Today only **model** gating exists:
[zc_settings_schema.cpp](file:///d:/imvt_git/obs-zcamera/src/cameractrl/zc_settings_schema.cpp#L265-L278)
(`catalogSupportedByModel`) and PTZ-capability gating
([zc_settings_schema.cpp](file:///d:/imvt_git/obs-zcamera/src/cameractrl/zc_settings_schema.cpp#L258-L263)).
There is **no firmware-version map**, yet rows #12, #14, #17, #18, #19 are
all "1.0.3 lacks X → hide it".

Plan:
- Parse the running firmware from the `/info` reply in
  [zc_camera_client.cpp](file:///d:/imvt_git/obs-zcamera/src/cameractrl/zc_camera_client.cpp)
  (add a `firmwareVersion()` accessor next to `cameraModel()`,
  [zc_camera_client.h](file:///d:/imvt_git/obs-zcamera/src/cameractrl/zc_camera_client.h#L78-L84));
  store as a comparable tuple `{major,minor,patch}` built from the main
  sw version string (fall back to empty = "unknown → keep everything").
- Add a schema-side availability table
  `bool keyAvailableForFirmware(key, version)` and
  `bool catalogAvailableForFirmware(catalog, version)` in
  `zc_settings_schema.*`, mirroring the model gating already there.
- The dock filters groups/keys through it wherever `catalogSupportedByModel`
  is already applied
  ([zc_control_dock.cpp](file:///d:/imvt_git/obs-zcamera/src/ui/zc_control_dock.cpp#L996-L1038)).

Rules table to encode (from the bug list):

| Feature | Hidden on |
| --- | --- |
| Composition Presets (#12.1) | < 1.0.5 (>1.0.3) |
| Detected Indicator (#14) | < 1.0.5 |
| Sensitivity, Acceleration (#17) | < 1.0.5 |
| Lock Zoom (#18) | < 1.0.5 |
| Recording triggered by tracking (#19) | < 1.0.5 |

### 0.2 Per-key conditional enable ("grey-out") engine
The firmware already returns a per-key `ro` (read-only) which the panel greys
([zc_control_dock.cpp](file:///d:/imvt_git/obs-zcamera/src/ui/zc_control_dock.cpp),
progress-2026-09-24 "ro=1 grey-out fix"). But the bug list needs
**value-driven** greying that the camera does not flag (web computes it
client-side):

| Target control | Grey when | Rows |
| --- | --- | --- |
| `lost_action_wait` / wait-time | auto target **On** | #15 |
| `restart_delay` | auto target **Off** | #16 |
| Range-Limit Up/Right/Down/Left sliders | limit **off** | #22 |
| preset-page recall speed unit + speed | recall mode = **Common** | #6 |
| preset-page 调用速度值 | unit = **time**… (see #105.5) | #105 |
| `video_tl_interval` | record mode = **Normal** | #81 (fixed) |
| `3g_sdi_mode` | SDI **off** | #84 (fixed) |

Plan: add a small declarative rule list to the schema
(`struct EnableRule { QString key; QString dependsOn; QStringList values; bool negate; }`)
returned by `enableRules()`, and have `ZcSettingsPanel` re-evaluate it on every
value change / websocket `ConfigChanged`, exactly like `DEPENDENT_GROUPS`
already re-reads catalogs.

### 0.3 Framing panel: immediate-apply (drop the Apply button)
Web calls one endpoint per control and there is no Apply
([api.js](file:///d:/imvt_git/ZCamGuiOpen/js/api.js#L729-L775)):
`/ctrl/framing/detect?enable=`, `/trace?enable=`, `/auto_target?enable=`,
`/restart`, `/detect_restart?time=`, `/manual_target?wait_time=`,
`/target_lost?action=|timeout_sec=`, `/tilt_lock?enable=`, `/track_active_face`,
`/target_num`, `/mask_area`.
The plugin instead batches everything in `ZcFramingPanel::apply()`
([zc_control_dock.cpp](file:///d:/imvt_git/obs-zcamera/src/ui/zc_control_dock.cpp#L2478-L2507)).
Rework each widget to write its own endpoint on change → fixes #7 and #11 and
removes a whole class of "clicked Apply, nothing happened" reports.

---

## A. Auto-framing web parity (rows 7–21, +68)

| Row | Issue | Fix | Files |
| --- | --- | --- | --- |
| #7 | detect/trace/restart need Apply | immediate-apply (§0.3) | dock framing |
| #11 | *all* framing settings need Apply | immediate-apply (§0.3) | dock framing |
| #8 | tracking target active while detect off | enable `traceChk_` only when `detectChk_` on; auto-disable trace when detect off | dock framing |
| #9 | restart button not greyed when framing disabled | grey `restartNowBtn_` while detect off | dock framing |
| #15 | lost-action-wait not greyed when auto target On | enable-rule (§0.2) | schema + dock |
| #16 | restart-delay not greyed when auto target Off | enable-rule (§0.2) | schema + dock |
| #14 | 1.0.3 missing Detected Indicator | firmware gate (§0.1) | schema |
| #17 | 1.0.3 missing Sensitivity/Acceleration | firmware gate (§0.1) | schema |
| #18 | 1.0.3 missing Lock Zoom | firmware gate (§0.1) | schema |
| #19 | 1.0.3 missing "recording triggered by tracking" | firmware gate (§0.1) | schema |
| #21 | enabling framing wrongly greys the **Preset** page | grey only the **trace/trajectory** controls while `detect` on (web greys trace only); leave preset usable | `ZcPresetPanel` + dock |
| #20 | no REC indicator when track-triggered recording runs | drive a REC badge from the existing framing status + `RecStarted/RecStoped` ws events | dock framing/status |
| #68 | camera/web changes not auto-synced | subscribe `ConfigChanged` and re-read the framing + settings groups | engine + dock |

**Larger items (need a small design pass, list separately):**

- **#12 Composition Presets** — four sub-issues:
  1. hide entirely on 1.0.3 (§0.1);
  2. add **size + position** to the add/edit flow on 1.0.6 (today every new
     preset is identical because those fields are never sent);
  3. **delete deletes the highest index, not the selected one** — a
     selection/index bug in `ZcFramingPanel::rebuildPresets`/delete handler;
     fix to delete `presetList_->currentRow()`;
  4. the 5 **default** presets must be **non-deletable** (disable delete for
     `index < 5` / read the flag from the preset doc).
- **#13 Detection Area** — the switch only toggles the camera flag; the dock
  never shows a draggable box. Implement `/ctrl/framing/mask_area` reporting +
  a movable rect (in the source overlay, see #10) — or, if the box must live in
  the dock, add a small preview canvas. Needs a decision (dock vs source
  overlay).
- **#10 tracked-face marker on preview** — the dock has no preview; OBS's own
  preview shows the source. To match web, the **source** must draw the tracked
  face box, i.e. the SspSource render path needs an overlay rect fed from
  `FramingStatusChanged`/`UpdateAfArea`. Feature-sized; scope with product.

---

## B. PTZ pane web parity (rows 4, 22–26, 28, 51, 105)

| Row | Issue | Fix | Files |
| --- | --- | --- | --- |
| #4 | Pan/Tilt read-out under the pad is static (1.0.6 fw) | verify the `ptzfChange` subscription + `ZcPtzPad::setPosition()`; 1.0.6 may emit a different field/name — add a `ptzfChange`/poll fallback | ws notifier + dock |
| #22 | Range-Limit Up/Right/Down/Left not greyed when limit off | enable-rule (§0.2) + `applyLimitBox()` should also disable the sliders | dock PTZ |
| #23 | clicking a "current position" edge button wrongly toggles the Limit checkbox | a `limitCurrent_[]` button click is bubbling to `limitCheck_`; disconnect the auto-toggle / guard the checkbox handler | dock PTZ |
| #24 | "current position" value differs from web | `setEdgeToCurrent()` must read the same field the web uses (`ptzQuery(detail=y)` → `pan_pos/tilt_pos`) and display truncated degrees | dock PTZ |
| #25 | "current position" buttons have no text label | add text ("取当前位" / "Set current") like web | dock PTZ |
| #26 | Preset/Trace toggle can leave both unselected | make them an exclusive, non-deselectable group (`QButtonGroup` with `setExclusive`, ignore re-click) | `ZcPresetPanel` |
| #28 | acceleration-ball sends `action=<dir>&fspeed=` but web sends `action=pt&pan_speed=&tilt_speed=` | the centre ball must emit the analog `pt` action with directional speeds; keep sector keys on the `fspeed` path | dock PTZ + client |
| #51 | `ptz_flip_auto, pt_rise_curve, pt_fall_curve, pt_s_curve` writes have no effect | confirm the real param names/endpoint on 1.0.6 (these keys are not in the snapshot schema); add correct keys or dedicated endpoint | schema + client |
| #105 | PTZ settings gaps | (1) add Range-Limit Up/Down/Left/Right fields; (2) add Current Position; (3) add the whole Autoframing section **into the PTZ tab** for PTZ cameras; (4) `ptz_common_speed_unit` change must re-read `ptz_common_speed/time` without a manual refresh (dependency rule); (5) `ptz_common_time` unit is **s** on web, plugin shows **ms** — convert + show unit | schema + dock PTZ |

---

## C. Setting names, labels, i18n (rows 39, 44–46, 99; verify 52, 56)

| Row | Issue | Fix |
| --- | --- | --- |
| #39 | `Shutter Precision` → **Shutter Operation**; `extended_iso` → **Extended Manual ISO**; `Ev Choice` → **EV** | relabel schema keys to camera-side names |
| #44 | `可变帧率（MOV）`→`可变帧率`; `预录`→`预录影`; `Recording FPS`→`Playback Framerate` | relabel |
| #45 | Chinese OBS: tab names, `exposure_window_idx`(测光窗口), `extended_iso`(扩展ISO), `mwb_*_offset`(红/绿/蓝偏移), `preroll_duration`, `rec_frame_indicator`, `xlr_input_level`, all option values, `refresh` | add every missing string to `data/locale/*.ini`; localize option labels (not only keys) |
| #46 | hovering shows the **English** key name in Chinese UI | tooltips must use the localized label, not the raw key |
| #99 | whole control page untranslated in zh-CN | same i18n pass as #45 (tab titles + group names) |

Note: the readable labels the plugin already ships
(progress "exposure + audio keys readable") should be the base; extend to
Video/WB/Lens/Image/Record and options.

---

## D. Settings value sync / dependency graying (rows 27, 29, 47, 68)

| Row | Issue | Fix |
| --- | --- | --- |
| #47 | missing `shutter` + `max_shutter`; duplicate `ev` & `ev_choice` not linked | shutter/max_shutter were added (progress) — verify read path; **remove the duplicate `ev`** and keep a single **EV** control that also updates the HUD `ev` value |
| #27 | changing main resolution/fps does not refresh **stream1** resolution/fps | after `resolution`/`project_fps` write, re-read the `app_stream1` document and refresh the top-bar stream row (extend `applyDependencies` + stream doc refresh) |
| #29 | E2_F6_Pro bitrate/GOP sliders not greyed **initially** (only after sliding) | compute the greyed state at build time from the stream doc `bitrateRo`/`gopRo` flags, not only in the slider callback |
| #68 | no auto-sync after camera/web change | subscribe `ConfigChanged` (+ existing `UpdateStreamSetting`) and re-read affected groups; the reference app has **no Refresh button** because ws drives everything |

Related already-fixed and to keep regression-tested: #69, #81, #86, #90.

---

## E. Stream / encode (rows 70, 87, 97; 71 = not issue)

| Row | Issue | Fix |
| --- | --- | --- |
| #70 | H.265 with any resolution → wide **green bar** at frame bottom (848x480 also right side) | decode/import bug: likely the P010/10-bit surface or a stride/crop mismatch when the macroblock size isn't a multiple of the coded size. Investigate `hw_decode_ffmpeg.cpp` crop/plane setup + `hw_decode_d3d11_win.cpp` import. Ties to the earlier #6/#7 findings |
| #87 | stream encoder cannot select **H.265 10-bit**; "先屏蔽掉" | hide the `H265_10bit` entry from `videoEncoderList` in the stream control (the option must not be offered until the decode path supports it) |
| #97 | stream-1 bitrate slider cannot reach the far-left (snaps back) | slider `minimum()`/write-scale bug: the leftmost value maps to the doc's min (or 0) which the writer rejects — clamp to the doc's `bitrate` range and set `setMinimum` from the doc |

---

## F. Reconnect / status / stability (rows 49, 94, 96, 98)

| Row | Issue | Fix |
| --- | --- | --- |
| #49 | E2_F6_Pro Crop-Sensor change reboots the camera; stream never recovers | RD says: increase reconnect backoff **20s → 40s → 90s**; also mark the source offline during the reboot and re-init the SSP stream on recovery. Add the backoff ladder to the source reconnect loop |
| #94 | playback mode → preview/audio drop, silent, no hint | on connect force the camera to **record mode** (RD note), and when `ModeChanged`=play show a "camera in playback — settings inactive" notice + status badge |
| #96 | after OBS restart, opening a source shows an empty control page until you switch sources | the dock does not load settings on first open of an already-restored source; trigger a group read when the dock becomes visible for a source |
| #98 | camera unplugged but status stays "online" | add a heartbeat/`hb`-timeout (or poll `/camera_status`) to flip the rail row to offline without a manual Refresh |

---

## G. Source management / rail (rows 41, 76, 85, 100)

| Row | Issue | Fix |
| --- | --- | --- |
| #41 | after adding via the source-IP box the source is not renamed to `ZCamera <ip>`; rail shows **two** ✔ (one wrongly offline) | rename on successful connect (`cameraSourceName()`), then rebuild the rail from OBS sources — the "wrongly offline" row is a duplicate discovery entry that must be merged by IP |
| #76 | selecting a source then Settings opens the **previously** closed camera | dock must follow the currently-selected OBS source (RD: "参数传过去"); wire the source-selection signal into the rail/panel selection |
| #85 | adding an existing source makes a second, same-named source with no separate audio control | the rail should reuse the existing source instead of creating a duplicate; if OBS forces a new source, give it a unique name so it gets its own mixer entry |
| #100 | source-IP box accepts illegal/unreachable IPs | validate IPv4 format before add + a reachability probe; show an error and don't add on failure |

---

## H. Login / auth (rows 3, 30)

| Row | Issue | Fix |
| --- | --- | --- |
| #3 | after 5 wrong passwords the login dialog vanishes and the main UI loads **empty** | on auth failure keep the dialog open and show the error; never fall through to the settings UI with no credentials. Cap retries and show "账号已锁定/密码错误" |
| #30 | on a wrong password the password box clears; no show/hide toggle | keep the typed password on error; add a reveal (eye) toggle. (Aligns with #58 which is already fixed) |

---

## I. Model / compatibility gating (rows 42, +verify 38/50)

| Row | Issue | Fix |
| --- | --- | --- |
| #42 | Lens `MF assist (standby)` / `MF assist (recording)` have no camera-side setting | hide `mf_mag`/`mf_recording` when `/info` does not report them (extend the model/feature gate, not just `security`) |
| #50 | E2_F6_Pro has no Lens section | **not issue** — model gating must depend on the lens `/info`, which it already does; keep |
| #38 | Avatar Lens keeps PTZ-only items | **pending/won't fix** (Avatar limitation) |

General: extend §0.1 so **any key/group** can be hidden per model (not only
`security`) — this is what #42, #34, #38 all point at.

---

## Deferred / will-not-fix (do not code)

- **#31–#38 (Avatar)** — RD feedback: "暂时保持（avartar 限制）". No change.
- **#50, #59, #71, #93, #102** — marked `not issue`.
- **#5** — RD: "正常，需要时间加载"; optionally add a loading spinner while the
  first group read is in flight (nice-to-have, not a defect).

## Re-verify only (already `fixed`)

Rows 2, 40, 43, 48, 52–58, 60–67, 69, 72–75, 77–84, 86, 88–92, 95, 101, 103.
Spot-check the ones with a high regression risk: #52 (shutter-mode label vs
#39/#47), #56 (label unification vs #39/#44), #69/#81/#86 (dependency graying
vs the new §0.2 engine), #64/#78 (auth/https).

---

## Suggested execution order

1. **Infrastructure** (§0.1 firmware gate, §0.2 enable rules, §0.3
   immediate-apply) — unblocks ~20 rows.
2. **Autoframing** (A) — biggest single cluster; needs one design decision
   (detection-area box + face marker: dock canvas vs source overlay).
3. **PTZ** (B) including #105 additions and #28 ball semantics.
4. **Labels + i18n** (C) — pure string work, low risk.
5. **Settings sync/graying** (D) — depends on §0.2.
6. **Login, rail, reconnect, stream** (E–H).
7. **Re-verify** the fixed rows and run the OBS integration test
   (`tests/obs_integration_test.ps1`) against the real camera.

## Verification / delivery (per repo rules)

Every fix cycle must end with, in order:
`cmake --build build_x64 --config RelWithDebInfo --target obs-zcamera` →
`makensis installer.nsi` (→ `dist\obs-zcamera-<ver>-setup.exe`) → deploy to
`%ProgramData%\obs-studio\plugins\obs-zcamera\{bin\64bit,data\locale}`. Report
installer path, deploy path, and build status. Do not commit unless asked.