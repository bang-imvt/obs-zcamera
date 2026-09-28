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
	void ptzStop();
	void ptzHome();
	/* Full PTZ state: current pan/tilt, flip, range limit (+ detail=y). */
	void ptzQuery(bool detail, std::function<void(const QJsonObject &)> cb);
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
	void setFramingWaitTime(int seconds);     /* /manual_target?wait_time= */
	void setFramingWaving(bool enabled);      /* /gesture?waving= */
	/* The web UI's "View Angle Adjustment" writes all three gesture FOV
	   switches (fullbody/waist/closeup) to the same value at once. */
	void setFramingGestureFov(bool enabled);

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
	/* duration in seconds; sent on the wire as milliseconds. */
	void ptzPresetSetSpeedByDuration(int index, int seconds);
	/* Authenticated GET of a preset thumbnail; empty bytes when unavailable. */
	void ptzPresetThumbnail(int index, std::function<void(const QByteArray &)> cb);
	/* Preset index (0-based) -> /app_data/preset/thm_<nnn>.jpg?act=thm */
	static QString presetThumbnailPath(int index);

	/* Traces ("trajectories"): the camera's second PTZ slot family, served by
	   /ctrl/ptrace over the same 0-based 0..99 index space as presets. */
	void ptraceDelete(int index);
	void ptraceSetName(int index, const QString &name);
	/* Start recording a trace over slot `index` (overwrites it). */
	void ptraceRecordStart(int index);
	/* Stop the recording / the playback; both are transport-wide. */
	void ptraceRecordStop();
	void ptracePlayStop();
	/* Load slot `index` as the trace to play next. */
	void ptracePlayPrepare(int index);
	/* Play the prepared trace. */
	void ptracePlayStart();
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
	void statusMessage(const QString &msg);

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
	bool supportsPtz_ = false;
	bool supportsAutoFraming_ = false;

	/* Cached values for HUD display. */
	QJsonObject hudValues_;
};

} // namespace zc