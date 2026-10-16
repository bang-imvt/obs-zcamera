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

/* Camera client facade, ported from ZCamGuiOpen camera-client.ts +
   camera-state.ts. Wraps the HTTP transport, WebSocket notifier and settings
   engine into typed operations used by the control dock UI. */

#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <memory>
#include <functional>
#include "zc_http_transport.h"
#include "zc_ws_notifier.h"
#include "zc_settings_engine.h"

namespace zc {

/* Per-host credentials remembered for the running session. Memory-only by
   design: never written to disk and never stored in the scene collection, so
   they are gone when OBS exits. The dock fills them in when the operator logs
   a camera in (bug 4); the source's Properties bitrate push reads them to
   reach a camera that requires a login, because a source has no login UI of
   its own. Accessed from the OBS UI thread only — the dock and source
   updates both run there — so a plain function-local static is enough. */
void rememberCredentials(const QString &host, const ZcCredentials &creds);
ZcCredentials lookupCredentials(const QString &host);

class ZcCameraClient : public QObject {
	Q_OBJECT

public:
	explicit ZcCameraClient(QObject *parent = nullptr);
	~ZcCameraClient() override;

	/* Connect to a camera at host (digest creds optional). Returns true
	   when an origin answered and login (if required) succeeded. */
	bool connect(const QString &host, int port = 80,
		     const ZcCredentials &creds = {});

	/* Disconnect: stop session and WS, keep no state. */
	void disconnect();

	bool isConnected() const { return connected_; }
	QString host() const;
	QString mode() const { return mode_; }
	bool isRecording() const { return recording_; }
	double temperature() const { return temperature_; }
	/* The camera answered a request with a Digest challenge. A connect() that
	   fails while this is true failed for want of credentials, not because the
	   camera is unreachable: the dock asks the operator for them (bug 4). */
	bool authRequired() const { return authRequired_; }

	/* The camera only answers over HTTPS but this Qt build cannot initialise
	   TLS: the operator must switch the camera to HTTP to control it. */
	bool tlsUnavailable() const { return transport_.tlsUnavailable(); }

	/* Camera identity from the /info reply (fetched on connect). */
	QString cameraModel() const { return model_; }
	/* The camera's firmware string ("1.0.3"), from /info `sw`. Empty until
	   the first /info answer arrives. */
	QString firmwareVersion() const { return firmwareVersion_; }
	/* True when the firmware is at least `ver` ("1.0.5"). Drives the
	   framing panel's version gates (bug list #10/#11/#48/#49). An empty
	   version never satisfies a comparison — an unknown firmware keeps
	   everything visible rather than hiding features it may have. */
	bool firmwareAtLeast(const char *ver) const;
	/* Whether the camera has a pan/tilt head: /info feature.lens_ctrl_ptz, or
	   a model name that says "ptz". The PTZ tab is gated on this. */
	bool cameraSupportsPtz() const { return supportsPtz_; }
	/* Whether the camera does auto framing: /info feature.autoFraming. The
	   auto-framing tab is gated on this, independently of the PTZ tab. */
	bool cameraSupportsAutoFraming() const { return supportsAutoFraming_; }

	/* ---- Commands (each returns when the request completes) ---- */
	void startRecording();
	void stopRecording();
	void capturePhoto();
	void setMode(const QString &mode); /* to_rec / to_standby / exit_standby */

	/* Lens / AF. */
	void lensZoomIn(int speed);
	void lensZoomOut(int speed);
	void lensZoomStop();
	void lensFocusNear(int speed);
	void lensFocusFar(int speed);
	void lensFocusStop();
	void afOnePush();

	/* PTZ. Preset indexes are 0-based, matching the camera's own web UI.
	   Movement takes a direction action (up/down/left/right/rightup/...) and a
	   0-1 speed: the camera's /ctrl/pt expects `action=<dir>&fspeed=<0-1>`,
	   not separate pan/tilt numbers. */
	void ptzMoveAction(const QString &action, float fspeed);
	/* Analog move: the centre of the joystick ball. The camera's own web page
	   drives it with `action=pt&pan_speed=<signed>&tilt_speed=<signed>` —
	   per-axis speeds in -1..1, sign = direction — not a direction word plus
	   one fspeed (bug list #28). The sector keys keep ptzMoveAction. */
	void ptzMoveAnalog(float panSpeed, float tiltSpeed);
	void ptzStop();
	void ptzHome();
	/* Full PTZ state: current pan/tilt, flip, range limit (+ detail=y). */
	void ptzQuery(bool detail, std::function<void(const QJsonObject &)> cb);
	/* Range limit: move one corner of the head's travel box. `direct` is
	   "upright" (pan_max/tilt_max) or "downleft" (pan_min/tilt_min); the pair is
	   the whole corner position, exactly as the camera's own web UI sends it. */
	void ptzLimitUpdate(const QString &direct, double pan, double tilt);
	/* Move to an absolute (relative=0) or relative (relative=1) position. */
	void ptzGoto(float pan, float tilt, float speed, bool relative,
		     bool speedWithZoom);
	/* Auto-framing. One snapshot read — GET /ctrl/framing/all_status — carries the
	   whole state (detect, trace, detect_restart, ...), which is also what the
	   camera's own web UI falls back to. The switches are written one at a time,
	   exactly as the web UI does. */
	void setFramingEnabled(bool enabled);
	void setFramingTrace(bool enabled);
	void setFramingRestart(int seconds);
	/* Restart detection right now (the web UI's Restart button). */
	void setFramingRestartNow();
	void framingQuery(std::function<void(const QJsonObject &)> cb);
	/* GET /ctrl/framing/status -> {"status":N}. N indexes the web UI's status
	   words: 0 IDLE, 1 Detecting, 2 Tracking, 3 Target Lost, 4 Disabled. The
	   same index rides on the FramingStatusChanged notification as `value`. */
	void framingStatusQuery(std::function<void(const QJsonObject &)> cb);

	/* The rest of the camera's framing page — the same calls its web UI makes.
	   Every one of these is a GET on a /ctrl/framing/<sub> endpoint. */
	void setFramingScene(int scene);          /* /scene?scene= */
	void setFramingStartPos(int index);       /* /start_position?index= */
	void setFramingTargetNum(int num);        /* /target_num?num= */
	void setFramingAutoTarget(bool enabled);  /* /auto_target?enable= */
	void setFramingAutoStart(bool enabled);   /* /auto_start?enable= */
	void setFramingZoomLock(bool enabled);    /* /zoom_lock?enable= */
	void setFramingTiltLock(bool enabled);    /* /tilt_lock?enable= */
	void setFramingTrackActiveFace(bool enabled); /* /track_active_face?enable= */
	void setFramingTrackingRecord(bool enabled);  /* /tracking_record?enable= */
	void setFramingTally(bool enabled);       /* /tally?enable= */
	void setFramingSensitivity(int value);    /* /speed?sensitivity= */
	void setFramingAcceleration(int value);   /* /speed?acceleration= */
	void setFramingLostAction(int action);    /* /target_lost?action= */
	void setFramingLostTimeout(int seconds);  /* /target_lost?timeout_sec= */
	void setFramingMaskEnable(bool enabled);  /* /mask_area?enable= */
	/* The detection-area rectangle, normalized (0..1). The camera answers
	   GET /mask_area with the current rect plus its w_min/h_min, and refuses
	   a rect smaller than those. Firmware that does not implement the
	   geometry still answers {"code":0} and keeps its old values, so a
	   caller must read back rather than trust the reply. */
	void setFramingMaskRect(bool enabled, float left, float top, float right,
				float bottom); /* /mask_area?enable=&left=&… */
	/* GET /mask_area -> {left,top,right,bottom,w_min,h_min,enable}. */
	void framingMaskQuery(std::function<void(const QJsonObject &)> cb);

	/* Registered-face database (bug list: face registration was missing).
	   The family is /ctrl/facedb/*, NOT the older /ctrl/face (which this
	   firmware answers "Face detection is not supported" for). The exact
	   request shapes below are taken from the camera's own web client
	   (controller.min.js, the static `facedb` object):
	     query   -> GET /ctrl/facedb/query
	                {status, max, max_active, next_priority,
	                 faces:[{uid,name,priority,...}]}
	     status  -> GET /ctrl/facedb/status   {status, enable, prepare_ready}
	     set     -> GET /ctrl/facedb/set?enable=<0|1>
	     prepare -> GET /ctrl/facedb/prepare[?face_id=<n>]
	                captures a face from the live view:
	                {status, detail, uid, thm}
	     add     -> GET /ctrl/facedb/add?which=<uid>&name=&priority=&activate=
	     update  -> GET /ctrl/facedb/update?uid=&name=&priority=&activate=
	     delete  -> GET /ctrl/facedb/delete?uid=
	   `prepare` fails with status 2 "face not found" until somebody is in
	   frame, and `add` answers -1 "prepare not ready" until a prepare has
	   succeeded, which is why adding is a two-step capture-then-name flow. */
	void framingFaceQuery(std::function<void(const QJsonObject &)> cb);
	void framingFaceStatus(std::function<void(const QJsonObject &)> cb);
	/* Turn the face database on/off. The camera's page does this before it
	   captures; harmless where the firmware ignores it. */
	void setFramingFaceEnable(bool enabled); /* /facedb/set?enable= */
	void framingFacePrepare(std::function<void(const QJsonObject &)> cb);
	/* `which` is the uid a successful prepare returned. */
	void framingFaceAdd(int which, const QString &name, int priority,
			    bool activate = true);
	void framingFaceUpdate(int uid, const QString &name, int priority,
			       bool activate = true);
	void framingFaceDelete(int uid);
	void setFramingWaitTime(int seconds);     /* /manual_target?wait_time= */
	void setFramingWaving(bool enabled);      /* /gesture?waving= */
	/* The web UI's "View Angle Adjustment" writes all three gesture FOV
	   switches (fullbody/waist/closeup) to the same value at once. */
	void setFramingGestureFov(bool enabled);
	/* The framing canvas' composition offsets: how far the subject sits from
	   the centre (placement) and how tall the head should be (face_height).
	   Both are the same -12..12 grid the web UI draws as a segment menu, and
	   both are what its four-arrow framing pad nudges one step at a time. */
	void setFramingPlacement(int value);      /* /fov?placement= */
	void setFramingFaceHeight(int value);     /* /fov?face_height= */
	/* The page's own "View Angle" and "Size" pair, which are two parameters
	   of the same /fov endpoint:
	     type      0..3 — Full Body / Waist / Close Up / Remain, i.e. which of
	               the four composition guides is active;
	     body_size 0..4 — the guide's 80% / 90% / 100% / 110% / 120% step.
	   `fov_size` looks like a third and is not one: the camera answers
	   `GET /fov?fov_size=N` with the whole {type,body_size,face_height,
	   placement} record and stores nothing, so writing it does nothing. */
	void setFramingFovType(int type);         /* /fov?type= */
	void setFramingFovBodySize(int size);     /* /fov?body_size= */
	/* The >=1.0.5 firmware's "Size" control: /fov?fov_size= 0..100. On the
	   1.0.3 firmware this parameter answers with the whole /fov record and
	   stores nothing (see the note above), so the panel only offers the Size
	   row when all_status actually carries `fov_size` (bug list #10). */
	void setFramingFovSize(int percent);
	/* GET /fov -> {"type":N,"body_size":N,"face_height":N,"placement":N}. The
	   page re-reads this after each nudge so its copy of the offsets cannot
	   drift from the camera's; so does the dock's framing pad. */
	void framingFovQuery(std::function<void(const QJsonObject &)> cb);
	/* Whether the camera draws its own marker on the subject it has locked
	   onto — the page's "Detected Indicator". /marker?enable= (its older
	   spelling /indicator answers {"code":-1}). */
	void setFramingMarker(bool enabled);      /* /marker?enable= */
	/* GET /export -> the framing profile as JSON, for the page's Export
	   button. A camera without it answers {"code":-1}. NOTE: this reply is
	   only a work order — {"code":0,"filename":"..."} — the profile itself
	   is a file the page then downloads from /app_data/preset/<filename>.
	   framingExportProfile() hands back that raw work-order object;
	   framingExportProfileFile() performs the two-step download and
	   delivers the actual profile bytes. */
	void framingExportProfile(std::function<void(const QJsonObject &)> cb);
	/* Result code for framingExportProfileFile's callback:
	   0  ok, `profile` holds the downloaded profile JSON
	   <0 the camera has no profile export ({"code":-1} work order)
	   1  transport/HTTP error, or the camera answered without a filename */
	void framingExportProfileFile(
	    std::function<void(int result, const QByteArray &profile)> cb);
	/* Multipart POST /ctrl/framing/import (the page's
	   showUploadFramingProfileDialog): upload a previously exported profile
	   file. `result` is true when the camera answers {"code":0}. */
	void framingImportProfile(const QByteArray &profile,
				  const QString &filename,
				  std::function<void(bool ok, const QString &error)> cb);
	/* The page's framing-box control (FramingBoxCtrl): two boxes, the fixed
	   angle box and the tracking box, each a GET with ?enable=. Its
	   getFixAngleBox() probe is how the page learns whether the firmware has
	   the feature at all; that reply is {"code":-1} on a camera that does
	   not. */
	void setFramingFixAngleBoxEnabled(bool enabled); /* /fix_angle_box?enable= */
	void setFramingTrackingBoxEnabled(bool enabled); /* /tracking_box?enable= */
	void framingFixAngleBoxQuery(std::function<void(const QJsonObject &)> cb);
	/* The tracking box's stored ROI: {enable, top, left, width, height,
	   min_width, min_height}, normalized to the frame — the rect the web
	   preview outlines and the overlay draws as the "range" frame (bug 17). */
	void framingTrackingBoxQuery(std::function<void(const QJsonObject &)> cb);

	/* Composition presets (/ctrl/framing/preset): the row of tiles the web UI
	   draws under the framing controls. The reply carries `presets` (each with
	   index/name/…), `default_count`, `max_count` and `max_name_len`. */
	void framingPresetQuery(std::function<void(const QJsonObject &)> cb);
	void framingPresetAdd();
	void framingPresetRecall(int index);
	void framingPresetUpdate(int index, const QString &name);
	void framingPresetRename(int index, const QString &name);
	void framingPresetReset(int index);
	void framingPresetDelete(int index);
	void ptzPresetSet(int index);
	void ptzPresetRecall(int index);
	void ptzPresetDelete(int index);
	/* One preset's stored state: name/speed/unit/time/status. Empty object on
	   a transport or parse failure. */
	void ptzPresetInfo(int index,
			   std::function<void(const QJsonObject &)> cb);
	void ptzPresetSetName(int index, const QString &name);
	/* unit 0 = speed (ptz_common_speed range), 1 = time (ptz_common_time). */
	void ptzPresetSetSpeedUnit(int index, int unit);
	void ptzPresetSetSpeedByIndex(int index, int speed);
	/* duration in seconds; the camera takes seconds on the wire (its web UI sends
	   `preset_time = ms / 1000`). */
	void ptzPresetSetSpeedByDuration(int index, int seconds);
	/* Authenticated GET of a preset thumbnail; empty bytes when unavailable. */
	void ptzPresetThumbnail(int index, std::function<void(const QByteArray &)> cb);
	/* Preset index (0-based) -> /app_data/preset/thm_<nnn>.jpg?act=thm */
	static QString presetThumbnailPath(int index);

	/* Traces ("trajectories"): the camera's second PTZ slot family, served by
	   /ctrl/ptrace over the same 0-based 0..99 index space as presets. The
	   optional `cb`, when given, receives the camera's `code` (0 = accepted;
	   non-zero, e.g. -2 while auto-framing owns the head or the transport is
	   still finalizing a recording) so the panel can say why nothing
	   happened instead of silently falling back to the idle face. */
	void ptraceDelete(int index,
			  std::function<void(int)> cb = {});
	void ptraceSetName(int index, const QString &name,
			   std::function<void(int)> cb = {});
	/* Start recording a trace over slot `index` (overwrites it). */
	void ptraceRecordStart(int index, std::function<void(int)> cb = {});
	/* Stop the recording / the playback; both are transport-wide. */
	void ptraceRecordStop(std::function<void(int)> cb = {});
	void ptracePlayStop(std::function<void(int)> cb = {});
	/* Load slot `index` as the trace to play next. */
	void ptracePlayPrepare(int index, std::function<void(int)> cb = {});
	/* Play the prepared trace. */
	void ptracePlayStart(std::function<void(int)> cb = {});
	/* Current transport state (TraceState); -1 when the camera did not
	   answer. */
	void ptraceQuery(std::function<void(int)> cb);
	/* One trace slot: name + prepared/playing/recording index. Empty object on
	   a transport or parse failure. */
	void ptraceInfo(int index, std::function<void(const QJsonObject &)> cb);
	/* Authenticated GET of a trace thumbnail; empty bytes when unavailable. */
	void ptraceThumbnail(int index, std::function<void(const QByteArray &)> cb);
	/* Trace index (0-based) -> /app_data/trace/thm_<nnn>.jpg?act=thm */
	static QString ptraceThumbnailPath(int index);

	/* Streaming encoders: one /ctrl/stream_setting document per index, named
	   by the firmware (`app_stream1` on newer, `stream1` otherwise). Reading and
	   writing are the same request — the camera's `action=query` applies any
	   field parameters it is handed — so both calls deliver the (updated)
	   document, and an empty object means the camera did not answer. The write
	   parameter names are not the document's field names, and bitrate is written
	   in bps while it reads in kbps (spec §2 Stream); the caller scales it. */
	void streamSettingQuery(const QString &index,
				std::function<void(const QJsonObject &)> cb);
	void streamSettingWrite(const QString &index,
				const QList<QPair<QString, QString>> &params,
				std::function<void(const QJsonObject &)> cb);

	/* Settings. */
	void readGroup(const QString &catalog, std::function<void(bool)> cb);
	void writeSetting(const QString &key, const QString &value);
	/* Read one key's value + metadata (type/min/max/opts). */
	void readSetting(const QString &key,
			 std::function<void(const ZcSettingValue &)> cb);

	/* Cached settings state, for the settings panel: the value of a key, its
	   metadata (type/opts/min/max/ro) and the keys the camera returned for a
	   catalog, in the camera's own order. */
	QString settingValue(const QString &key) const;
	bool settingDefinition(const QString &key, ZcSettingValue *out) const;
	QStringList settingGroupKeys(const QString &catalog) const;

	/* Refresh live HUD keys. */
	void refreshStatus();

	/* Ethernet address. `info` is /ctrl/network?action=info (mode + ipaddr +
	   netmask + gateway + dns); setNetworkStatic switches the interface to the
	   given static address (mode=static). Both deliver the camera's reply, and
	   an empty object means it did not answer. */
	void networkInfo(std::function<void(const QJsonObject &)> cb);
	void setNetworkStatic(const QString &ip, const QString &netmask,
			      const QString &gateway, const QString &dns,
			      std::function<void(const QJsonObject &)> cb);

signals:
	/* Normalized events from the WS notifier. */
	void statusChanged(const QJsonObject &status);
	void modeChanged(const QString &mode);
	void recordingStarted();
	void recordingStopped();
	void recordingDuration(int seconds);
	void recordingRemaining(int seconds);
	void temperatureChanged(double celsius);
	void configChanged(const QString &key, const QString &value);
	void afAreaChanged(double x, double y, double w, double h);
	/* Live PTZ position from a ptzfChange notification: pan/tilt in degrees and
	   the zoom position as a percentage, so the read-out follows the head. */
	void ptzPositionChanged(double pan, double tilt, double zoomPercent);

	/* A catalog finished loading; the settings panel rebuilds its rows. */
	void groupRefreshed(const QString &catalog);

	/* The /info reply arrived (or was cleared on disconnect): model and PTZ
	   support may have changed. */
	void cameraInfoChanged();

	/* Auto-framing notifications (ws_events::kFramingStatus / kFramingSetting).
	   The status payload carries the live switches (detect/trace) so the panel
	   repaints without another round-trip; the setting event only says the
	   stored values changed, so the listener re-reads the snapshot. */
	void framingStatusChanged(const QJsonObject &status);
	void framingSettingsChanged();

	void connectionStateChanged(bool connected);
	/* The camera's notification socket dropped or re-established (power loss,
	   network drop). The dock marks the row offline/online from this so the rail
	   reflects a camera that went away without the operator clicking (bug 49). */
	void notifierStateChanged(bool live);
	void statusMessage(const QString &msg);
	/* Re-emitted from ZcHttpTransport for the debug panel. */
	void requestLogged(const QString &line);

private:
	/* WS event dispatcher. */
	void handleWsEvent(const QString &what, const QJsonObject &obj);

	/* GET /info and refresh model_/supportsPtz_, then emit cameraInfoChanged. */
	void refreshCameraInfo();

	/* One /ctrl/stream_setting round trip; an empty `params` is a pure read. */
	void sendStreamSetting(const QString &index,
			       const QList<QPair<QString, QString>> &params,
			       const std::function<void(const QJsonObject &)> &cb);

	void setRecording(bool rec);

	ZcHttpTransport transport_;
	ZcWsNotifier notifier_;
	ZcSettingsEngine settings_;

	bool connected_ = false;
	/* Set by a connect() that failed on a Digest challenge with no credentials
	   (bug 4). */
	bool authRequired_ = false;
	QString host_;
	QString mode_;
	bool recording_ = false;
	double temperature_ = -1.0;
	QString model_;
	QString firmwareVersion_;
	bool supportsPtz_ = false;
	bool supportsAutoFraming_ = false;

	/* Cached values for HUD display. */
	QJsonObject hudValues_;
};

} // namespace zc