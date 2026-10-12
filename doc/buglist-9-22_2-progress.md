# obs-zcamera bug list progress — OBS-Zcamra-buglist-9-22_2.csv

Grouped by category. `#` is the row number in the CSV. Status legend:

- `fixed`   — code change landed (verify on hardware where noted)
- `wip`     — in progress
- `planned` — understood, not yet changed
- `hw`      — needs a real camera/GPU to verify; code change may be needed
- `design`  — needs a product decision before changing code

## Installer (1, 3, 4) — fixed in 7adaf11/ae7d97d
- #1 install layout to obs-plugins\64bit + data\obs-plugins — `fixed`
- #3 default install path under Program Files — `fixed`
- #4 wizard shows version — `fixed`

## Auth / Security (2, 17, 18)
- #2 auth/https causes all settings to read empty — digest challenge is now
  handled (prompt + retry); re-verify on a camera with auth/https on. `wip`
- #17 hide Security identity/HTTPS for Avatar & E2_F6_Pro — `catalogSupportedByModel`
  added (hides `security` group). `fixed` (verify on those models)
- #18 restart prompt for HTTPS/network switches — `fixed`

## Camera model compatibility (5)
- #5 Avatar camera settings empty — relies on the `/ctrl/getbatch` → per-key
  fallback (settings engine). Verify on an Avatar unit. `hw`

## Decode / rendering (6, 7, 25, 26)
- #6/#7 green bar with H.265 + non-64-multiple / 720p — likely 10-bit P010
  surface not handled by the import shader/VideoProcessor (crushed/chroma
  edge). Investigate the H.265 10-bit path in hw_decode + importer. `wip`
- #25 stream1 encoder cannot switch to H.265_10bit — schema encoder list /
  stream setting handling. `planned`
- #26 1080p preview tearing — timing/import; needs reproduction. `hw`

## Audio (21, 22, 24)
- #21 camera has no audio source — AAC decode added (7adaf11, OBS_SOURCE_AUDIO);
  verify. `hw`
- #22 2nd ZCamera source has no mixer icon — OBS mixer icon refresh; often an
  OBS-side timing thing; verify after audio lands. `hw`
- #24 audio settings: missing Input Gain control, spurious audio input gain —
  schema keys. `planned`

## Settings schema / groups (19, 20, 23, 24, 28, 29, 32)
- #19 exposure missing shutter / max_shutter; EV duplicates EV Choice — schema
  keys added for shutter/max_shutter (7adaf11); EV↔EV Choice linkage still
  needed. `planned`
- #20 WDR naming (compose_mode) + grey-out video_tl_interval when record
  mode=Normal — schema label + conditional grey-out. `planned`
- #23 system: missing HDMI_Layout / Camera Name; grey 3g_sdi_mode when SDI off;
  sdi_fmt/sdi1_fmt oddity; desqueeze; Lcd_backlight for PTZ — schema +
  per-model gating. `planned`
- #28 grey-out IP/mask/gw/dns when ethmode is DHCP/Direct — dock network pane
  grey-out logic. `planned`
- #29 still shows static IP fields when not static + stale "Apply" button —
  dock network pane state. `planned`
- #32 camera/web-side changes not auto-synced (needs manual Refresh) — subscribe
  to ConfigChanged/ws and refresh groups. `planned`

## Network (27, 30, 31, 34, 36)
- #27 "Apply static IP" ineffective (switch to static disconnects before apply)
  — this is exactly the case the auto-test RULE now forbids; the UI should
  confirm before a network-mode change. `planned`
- #30 static network settings added — `fixed`
- #31 "Restore defaults" clears the source-IP box — confirm intended behaviour
  (restore plugin defaults) or scope it. `design`
- #34 no IP format/reachability validation on the source-IP box — add validation
  in the dock/source properties. `planned`
- #36 double-clicking the source pops the IP-source window — the get_properties
  side-effect opening the dock fires on every properties access; only raise the
  dock on user action, not on internal re-queries. `planned`

## PTZ (8, 9, 10, 11, 12, 13)
- #8 PTZ speed slider P/T ineffective
- #9 speed joystick centre-drag ineffective
- #10 preset recall speed unit=time value ineffective
- #11 trace does not auto-stop at end of playback
- #12 trace controls not greyed when autoframing on
- #13 PTZ pane missing Range Limit Up/Down/Left/Right, Current Position,
  common time, Autoframing tab; common_speed not greyed when unit=time
  — all PTZ schema/UI in zc_control_dock PTZ pane. `planned` (one session)

## Dock selection / rail (14, 15)
- #14 selecting a ZCamera source then opening Settings shows the previous
  camera, not the selected one — the dock does not follow the selected OBS
  source. `planned`
- #15 adding a source does not flip its "+" to "✔"; re-click duplicates — the
  rail "added" state only updates on presence refresh; ensure it reflects a
  source created from the source-IP box. `planned`

## i18n (33, 37)
- #33 settings not translated in Simplified-Chinese OBS — dock strings use ztr
  (obs_module_text) but the camera key labels come from the schema; localize the
  readable labels. `planned`
- #37 Debug button still present — built when `ZC_ENABLE_DEBUG_PANEL`; ensure
  release builds disable it. `fixed` (CMakeLists gated it)

---

## Auto-test rule
`tests/obs_ws_driver.mjs`: the automated test MUST NOT change the camera
network mode (direct/static) — it cannot be auto-recovered and orphans the
camera. See the RULE block in the driver header.

## Progress update 2026-09-24
- **ro=1 grey-out**: settings rows that are read-only are now shown but
  disabled (greyed) instead of hidden — affects #13.5 (ptz_common_speed when
  unit=time), #20.2 (video_tl_interval), #23.2 (3g_sdi_mode), #24, #28. `fixed`
- **#28 network fields grey-out**: in the static-network block, the IP/netmask/
  gateway/DNS fields and the Apply button are greyed out unless ethmode is
  static. `fixed` (self-testable)
- #29 partially: fields greyed in non-static mode (the prefill still shows the
  camera's reported values; that is informational). Remaining: hide the stale
  static values when the interface is not static. `wip`
- The settings schema keys/labels and the ws-notification auto-sync (#32) still
  need alignment to ZCamGuiOpen; the reference schema has per-setting `label`
  and `perKey` that the plugin's flat key list lacks. `planned`

## Progress update 2 (network UI removed)
- Network settings are removed from the UI entirely (streaming product should not
  touch the camera's network mode): the `network` catalog (wifi/wifi_channel) is
  dropped from the schema and the static-Ethernet block (IP/mask/gw/dns/Apply,
  bug 15/27/28/29/30) plus its function/declaration are removed. `fixed`

## Progress update 3
- #15 rail "added" now matches the camera address (name OR zc_source_ip), so a
  source added via the source-IP box flips '+' to a check and re-click reuses it
  instead of duplicating. `fixed`
- System settings section (System/Security/Multicam) removed from the UI
  (streaming product). WDR (compose_mode) removed from Video. Exposure keeps
  ev_choice only (dropped the duplicate 'ev'). `fixed`
- Readable + localized labels for exposure and audio keys (bugs 19/24). `fixed`
- ro=1 keys are now greyed (shown but disabled) instead of hidden. `fixed`
- NOT done (recorded): #14 selected-source follow, #13/#8-12 PTZ pane,
  #6/#7/#25/#26 decode, #21/#22 audio verify, #31 restore-defaults,
  #32 ws auto-sync, #34 IP validation, #33 full i18n.

## Progress update 4 (PTZ)
- #8 PTZ speed + #9 joystick: fixed. The plugin wrongly sent /ctrl/pt?action=pt
  &pan_speed/tilt_speed; the camera wants action=<dir>&fspeed=<0-1>. ptzMoveAction
  now maps pan/tilt to the 8 direction actions, sends fspeed normalized 0-1
  (slider 1-100, default 0.5), and the analog joystick uses drag distance as
  fspeed and drag direction as direction. `fixed` (verify on a PTZ camera)
- #13 Range Limit found: /ctrl/pt?action=query -> limit{enable,pan_max,pan_min,
  tilt_max,tilt_min}. The write action, current-position source and autoframing
  entry still unresolved (web needs auth; ZCamGuiOpen snapshot has no PTZ).

## PTZ API resolved (from the camera's own web, /www/js/controller.js + /ctrl probes)
- Move: action=<dir>&fspeed=<0-1> (fixed in #8); also action=pt&pan_speed/tilt_speed exists.
- Query (current pos + range limit): /ctrl/pt?action=query&detail=y -> {pan_pos, tilt_pos,
  limit{enable,pan_max/min,tilt_max/min}, unlimit}. #13.2 Current Position = pan_pos/tilt_pos.
- Range Limit enable: /ctrl/set?ptz_limit=On. Range limit set: /ctrl/pt?action=limit&direct=&pan_pos=&tilt_pos=.
- GOTO (absolute/relative positioning): /ctrl/pt?action=goto&pan_pos=&tilt_pos=&pan_speed=&tilt_speed=&relative=0|1&speed_with_zoom=.
- Autoframing: gated by info.feature.autoFraming; enable /ctrl/framing/detect?enable=; restart
  /ctrl/framing/detect_restart?time=; state from ws (FramingSettingChanged {detect,trace});
  when framing.detect is on, preset/trace actions disabled (#12).
- Trace play-end (#11): poll /ctrl/ptrace?action=query (st_code) to flip playing->ready.
- Preset time (#10): write param name to confirm from web (preset_time vs time).
