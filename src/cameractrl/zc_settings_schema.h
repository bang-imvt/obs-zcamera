/*
obs-zcamera
 Copyright (C) 2019-2020 Yibai Zhang
 Copyright (C) 2026 ZCamera OBS plugin contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; If not, see <https://www.gnu.org/licenses/>
*/

/* Camera settings catalog + endpoint contract, ported from the ZCamGuiOpen
   settings-schema.ts / api.js. This is the source of truth for the settings
   sidebar in the control dock. */

#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QUrlQuery>
#include <QList>
#include <QPair>
#include <vector>

namespace zc {

/* Firmware response shape for a single setting: { code, desc, key, type, ro,
   value, opts[], min, max, step }. type 1 = choice, 2 = range, 3 = text. */
enum class SettingType { Choice = 1, Range = 2, Text = 3 };

struct SettingDef {
	QString key;
	QString catalog;      /* the getbatch catalog this key belongs to */
	bool perKey;          /* read individually via /ctrl/get instead of batch */
	bool isBoolean;       /* on/off, enable/disable, ... vocabularies   */
	bool conditional;     /* hidden unless a feature flag/model matches */
};

/* A setting group ("catalog") as shown in the settings sidebar. */
struct CatalogDef {
	QString id;
	QString parent;       /* indented under this group (System children) */
	QStringList keys;     /* keys belonging to this catalog              */
};

/* Post-write dependency map: writing any srcKey in the list re-reads the
   listed catalogs afterward. Mirrors ZCamGuiOpen DEPENDENT_GROUPS. */
struct DependencyRule {
	QStringList srcKeys;
	QStringList targetCatalogs;
};

/* ---- Endpoints ---- */
namespace http {
constexpr const char *kInfo = "/info";
constexpr const char *kMode = "/ctrl/mode";
constexpr const char *kStatus = "/camera_status";
constexpr const char *kGet = "/ctrl/get";
constexpr const char *kSet = "/ctrl/set";
constexpr const char *kGetBatch = "/ctrl/getbatch";
constexpr const char *kSession = "/ctrl/session";
constexpr const char *kRec = "/ctrl/rec";
constexpr const char *kCard = "/ctrl/card";
constexpr const char *kStill = "/ctrl/still";
constexpr const char *kLens = "/ctrl/lens";
constexpr const char *kAf = "/ctrl/af";
constexpr const char *kPt = "/ctrl/pt";
constexpr const char *kPreset = "/ctrl/preset";
constexpr const char *kPtrace = "/ctrl/ptrace";
constexpr const char *kStream = "/ctrl/stream_setting";
constexpr const char *kRtmp = "/ctrl/rtmp";
constexpr const char *kNickName = "/ctrl/nick_name";
constexpr const char *kTemp = "/ctrl/temperature";
} // namespace http

/* WebSocket notification event names (the raw `what` field). */
namespace ws_events {
constexpr const char *kCameraStatus = "camera_status";
constexpr const char *kRecStarted = "RecStarted";
constexpr const char *kRecStopped = "RecStoped"; /* deliberate misspelling */
constexpr const char *kRecDur = "RecUpdateDur";
constexpr const char *kRecRemain = "RecUpdateRemain";
constexpr const char *kRecordingFile = "RecordingFile";
constexpr const char *kModeChanged = "ModeChanged";
constexpr const char *kTemp = "TempUpdate";
constexpr const char *kStreamChanged = "UpdateStreamSetting";
constexpr const char *kConfigChanged = "ConfigChanged";
constexpr const char *kAfArea = "UpdateAfArea";
constexpr const char *kBasicInfo = "basicInfo";
constexpr const char *kShutdown = "Shutdown";
constexpr const char *kHeartbeat = "hb";
} // namespace ws_events

/* ---- /info-derived capability + /ctrl/preset wire contract ----
   Pure helpers so the PTZ tab gating and the preset request shape can be
   unit-tested without a camera on the wire. */

/* Whether an /info reply describes a camera with a pan/tilt head. The
   `feature.lens_ctrl_ptz` flag is authoritative; a model name containing
   "ptz" is the fallback for firmware that omits the flag. The dock shows the
   PTZ tab only when this is true. */
bool cameraSupportsPtz(const QJsonObject &info);

/* Query for one /ctrl/preset action. Preset indexes are 0-based (the camera's
   own web UI uses the same indexing). */
QUrlQuery presetQuery(const QString &action, int index,
		      const QList<QPair<QString, QString>> &extra = {});

/* Thumbnail path for a 0-based preset index:
   /app_data/preset/thm_<nnn>.jpg?act=thm (three-digit zero padded). */
QString presetThumbnailPath(int index);

/* ---- /ctrl/ptrace: the PTZ "trace" (trajectory) slot family ----
   Traces are the camera's second PTZ slot family, listed next to presets in
   its own web UI. They share the preset 0-based 0..99 index space on the
   wire; the camera only adds a 2000 base offset to the index it reports in a
   PresetChange notification, never to the index this API accepts. */

/* Query for one /ctrl/ptrace action. A negative `index` omits the index
   parameter, which is what the transport-wide actions (rec_stop, play_start,
   play_stop, query) take. */
QUrlQuery ptraceQuery(const QString &action, int index = -1,
		      const QList<QPair<QString, QString>> &extra = {});

/* Thumbnail path for a 0-based trace index:
   /app_data/trace/thm_<nnn>.jpg?act=thm (three-digit zero padded). */
QString ptraceThumbnailPath(int index);

/* /ctrl/ptrace transport state, as reported by `action=query` (st_code) and
   by the PtzTraceSt notification (val). */
enum class TraceState {
	None = 0,
	Recording = 1,
	Preparing = 2,
	ReadyForPlay = 3,
	Playing = 4,
	Deleting = 5,
	ReadyForRecord = 6,
};

/* Unknown values fall back to None. */
TraceState traceStateFromInt(int value);

/* Which trace controls a transport state leaves actionable. Mirrors the state
   machine of the camera's own web UI (PTMainController). */
struct TraceActions {
	bool record;   /* Record: start, or stop while recording          */
	bool prepare;  /* Prepare / Play / Stop-playback                  */
	bool remove;   /* Delete                                          */
	bool navigate; /* page and slot switching are allowed             */
};
TraceActions traceActions(TraceState state);

/* ---- /ctrl/stream_setting: the streaming encoders ----
   Not a getbatch catalog. The camera keeps one document per encoder index, and
   reading and writing are the *same* request:
   `GET /ctrl/stream_setting?action=query&index=<index>[&field=value]` applies
   the fields it is given and answers with the updated document. `action=set` is
   rejected outright (`code:-1`), so it must not be used.

   Which index to use is the firmware's choice, not ours, so it is discovered
   rather than assumed — `isStreamDocument()` tells a real document apart from
   the `{"code":0,"streaming":false}` the camera answers for an index it does
   not have:
   - `app_stream1` — newer firmware. It carries the *choices* for the encoder,
     the resolution and the frame rate (`videoEncoderList`, `resolutionList`,
     `fpsList`) and the matching `encoderRo` / `resolutionRo` / `fpsRo` flags,
     so all three can be selected there.
   - `stream1` — the fallback when `app_stream1` is absent. Its document
     reports the values but no lists, so codec, resolution and frame rate are
     not changeable through it; they are whatever the camera's own settings
     say, which is what the dock displays.

   The write parameter names are *not* the document's field names — verified
   against firmware, one field at a time:
   - `bitrate`, in bps on the wire but kbps in the document, hence
     kBitrateWriteScale;
   - `gop_n`;
   - `width` and `height` together, for one `resolutionList` entry;
   - `venc`, taking one `videoEncoderList` entry;
   - `fps`, taking one `fpsList` entry.
   Everything else the document reports — `bitwidth`, `status`, `rotation`,
   `splitDuration`, the `*Ro` flags and the ranges — is read-only. */
constexpr const char *kAppStreamIndex = "app_stream1";
constexpr const char *kLegacyStreamIndex = "stream1";
constexpr int kBitrateWriteScale = 1000;

/* Query for one stream document. `params` are the fields to apply, so an empty
   list is a pure read. */
QUrlQuery streamSettingQuery(
	const QString &index,
	const QList<QPair<QString, QString>> &params = {});

/* Whether a reply is a stream document rather than the camera's "no such
   index" answer. This is how the dock decides between kAppStreamIndex and
   kLegacyStreamIndex. */
bool isStreamDocument(const QJsonObject &doc);

/* The `width`+`height` pair that selects a `resolutionList` entry such as
   `1280x720`; empty when the string is not a `WxH` pair. */
QList<QPair<QString, QString>> resolutionParams(const QString &resolution);

/* ---- OBS source naming ----
   The source that represents a camera is identified by its name alone, and
   that name is `ZCamera <ip>` (spec §1 L3). Pure so the rail can rebuild its
   list from the sources OBS holds without a camera on the wire. */

QString cameraSourceName(const QString &host);

/* The address back out of a `ZCamera <ip>` name; empty for any other source,
   so enumerating OBS sources cannot mistake a scene or an unrelated source for
   a camera. */
QString hostFromSourceName(const QString &sourceName);

/* The full set of settings catalogs presented in the control dock, mirroring
   ZCamGuiOpen. */
std::vector<CatalogDef> &catalogs();

/* Whether a catalog is camera-side pan/tilt configuration, which only a camera
   with a pan/tilt head has. The dock hides those groups for a camera whose
   /info says it has none (spec §2), instead of offering keys it would reject. */
bool catalogRequiresPtz(const QString &catalogId);

/* Keys-with-ranges / per-key reads that need individual /ctrl/get. */
bool isPerKey(const QString &key);
bool isBooleanSetting(const QString &key);

/* Dependency rules for post-write re-reads. */
const std::vector<DependencyRule> &dependencyRules();

/* HUD telemetry keys (mirrors ZCamGuiOpen HUD_KEYS). */
QStringList hudKeys();

} // namespace zc