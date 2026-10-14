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

#include "zc_camera_client.h"

#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QHash>

#include "zc_settings_schema.h"

namespace zc {

/* The shared per-host credential store (see the header). A function-local
   static: initialised on first use and alive until the process exits, so
   neither the dock nor a source can free it out from under the other. */
static QHash<QString, ZcCredentials> &credentialStore()
{
	static QHash<QString, ZcCredentials> store;
	return store;
}

void rememberCredentials(const QString &host, const ZcCredentials &creds)
{
	credentialStore()[host] = creds;
}

ZcCredentials lookupCredentials(const QString &host)
{
	return credentialStore().value(host);
}

ZcCameraClient::ZcCameraClient(QObject *parent) : QObject(parent)
{
	QObject::connect(&notifier_, &ZcWsNotifier::eventReceived, this,
			 &ZcCameraClient::handleWsEvent);
	/* The notification socket dropping is the camera going away on the wire
	   (power loss / network drop), independent of any click; re-emit it so the
	   rail can mark the row offline (bug 49). */
	QObject::connect(&notifier_, &ZcWsNotifier::socketStateChanged, this,
			 &ZcCameraClient::notifierStateChanged);
	settings_.setTransport(&transport_);
	QObject::connect(&settings_, &ZcSettingsEngine::groupRefreshed, this,
			 &ZcCameraClient::groupRefreshed);
	/* Forward the transport's request log so the debug panel (built only with
	   ZC_ENABLE_DEBUG_PANEL) actually has a producer; harmless when unconnected. */
	QObject::connect(&transport_, &ZcHttpTransport::requestLogged, this,
			 &ZcCameraClient::requestLogged);
}

ZcCameraClient::~ZcCameraClient() = default;

bool ZcCameraClient::connect(const QString &host, int port,
			     const ZcCredentials &creds)
{
	host_ = host;
	authRequired_ = false;
	transport_.setCredentials(creds);

	/* Probe + origin negotiation. */
	if (!transport_.negotiateOrigin(host, port))
		return false;

	/* /info is public on some firmware, so the origin probe alone cannot tell
	   whether a login is needed (bug: the prompt never appeared for such a
	   camera). Probe a control endpoint; if it 401s and we have no credentials,
	   fail fast so the dock asks for them instead of looping on 401s. */
	transport_.probeAuthRequired();

	/* The camera requires a login; without credentials nothing else we do can
	   succeed, so fail fast so the dock asks the operator for them (bug 4). */
	if (transport_.authRequired() && creds.username.isEmpty()) {
		authRequired_ = true;
		return false;
	}

	/* Credentials were supplied for a camera that needs a login: perform the
	   web-UI session login now (GET /login/ + captured cookie). Firmware that
	   accepts per-request Digest instead is a no-op here. A 401 here means the
	   password was wrong — fail connect so the dock keeps the form up rather
	   than showing empty settings behind an open 401 loop. */
	if (!creds.username.isEmpty()) {
		if (!transport_.login()) {
			/* Only reject when the login actually challenged us. A camera that
			   never needed a login returns "no challenge" as a no-op success,
			   so reaching false here means credentials were wrong. */
			if (transport_.authRequired()) {
				authRequired_ = true;
				return false;
			}
		}
	}

	/* Start the WebSocket notifications (wss when origin is https), carrying
	   the Digest Authorization / cookie / x-auth-token from the transport. The
	   notifier rebuilds that header from the live transport on every reconnect
	   (a /login/ no-op success leaves the stored snapshot empty otherwise). */
	bool https = transport_.baseUrl().startsWith("https://");
	notifier_.setTransport(&transport_);
	notifier_.start(host, https, transport_.upgradeHeaderBlock());

	/* Read initial state. */
	refreshStatus();
	refreshCameraInfo();

	connected_ = true;

	/* A camera left in playback mode (from its own web UI) shows no live stream
	   and ignores every setting write, with no error — it looks like the plugin
	   is broken (bug list "camera in playback mode"). Put it back in record
	   standby as part of connecting (RD: set rec mode, not play). Best effort:
	   a camera that does not have the endpoint just ignores it. */
	setMode(QStringLiteral("to_rec"));

	emit connectionStateChanged(true);
	return true;
}

void ZcCameraClient::disconnect()
{
	notifier_.stop();
	if (connected_) {
		transport_.get(QString(http::kSession) + "?action=quit", nullptr,
			       1000);
	}
	connected_ = false;
	mode_.clear();
	recording_ = false;
	model_.clear();
	supportsPtz_ = false;
	/* The framing gate belongs to the connected camera too: leaving it set
	   kept the framing tab (and its preset grey-out) alive on a camera that
	   does not do framing. */
	supportsAutoFraming_ = false;
	temperature_ = 0.0;
	authRequired_ = false;
	emit cameraInfoChanged();
	emit connectionStateChanged(false);
}

QString ZcCameraClient::host() const
{
	return host_;
}

void ZcCameraClient::handleWsEvent(const QString &what,
				   const QJsonObject &obj)
{
	if (what == ws_events::kCameraStatus) {
		emit statusChanged(obj);
		/* Also refresh temperature/mode from the status payload. */
		if (obj.contains("temperature"))
			emit temperatureChanged(obj.value("temperature").toDouble());
		if (obj.contains("mode"))
			emit modeChanged(obj.value("mode").toString());
	} else if (what == ws_events::kRecStarted) {
		setRecording(true);
		emit recordingStarted();
	} else if (what == ws_events::kRecStopped) {
		setRecording(false);
		emit recordingStopped();
	} else if (what == ws_events::kRecDur) {
		int idx = obj.value("index").toInt();
		int seconds = obj.value("value").toInt();
		if (idx == 1 && seconds > 0)
			emit recordingDuration(seconds);
	} else if (what == ws_events::kRecRemain) {
		bool ok = false;
		int seconds = obj.value("value").toString().toInt(&ok);
		if (!ok)
			seconds = obj.value("msg").toString().toInt(&ok);
		if (ok)
			emit recordingRemaining(seconds);
	} else if (what == ws_events::kModeChanged) {
		mode_ = obj.value("value").toString();
		emit modeChanged(mode_);
	} else if (what == ws_events::kTemp) {
		emit temperatureChanged(obj.value("value").toDouble());
	} else if (what == ws_events::kConfigChanged) {
		QString key = obj.value("key").toString();
		if (key.isEmpty())
			key = obj.value("config_key").toString();
		if (key.isEmpty())
			key = obj.value("k").toString();
		QString value = obj.value("value").toString();
		if (value.isEmpty())
			value = obj.value("v").toString();
		if (!key.isEmpty()) {
			emit configChanged(key, value);
			/* A change made outside the plugin (the camera's own web UI, a
			   second controller) arrives only here. Nothing in the UI listens
			   to configChanged, and the settings panel repaints only from
			   groupRefreshed, which an HTTP read produces — so without this
			   the dock kept showing the stale value for both transports.
			   Re-read the catalog the key belongs to; the panel's existing
			   handler repaints it when it is the catalog on screen. */
			const QString catalog = settings_.catalogFor(key);
			if (!catalog.isEmpty())
				settings_.readGroup(catalog, nullptr);
		}
	} else if (what == ws_events::kAfArea) {
		emit afAreaChanged(obj.value("roi_x").toDouble(),
				   obj.value("roi_y").toDouble(),
				   obj.value("roi_w").toDouble(),
				   obj.value("roi_h").toDouble());
	} else if (what == ws_events::kPtzfChange) {
		/* The head reports its live pan/tilt (and zoom position) here as it
		   moves, so the panel's position read-out follows the joystick without
		   polling the camera's single-threaded HTTP server. */
		emit ptzPositionChanged(obj.value("pan_pos").toDouble(),
					obj.value("tilt_pos").toDouble(),
					obj.value("fPosPercent").toDouble());
	} else if (what == ws_events::kFramingStatus) {
		/* The live switches ride on this message (detect/trace as 1/0), so the
		   panel repaints straight from it — no extra request to the camera's
		   single-threaded HTTP server. */
		emit framingStatusChanged(obj);
	} else if (what == ws_events::kFramingSetting) {
		/* Stored configuration changed elsewhere: let listeners re-read. */
		emit framingSettingsChanged();
	}
}

void ZcCameraClient::setRecording(bool rec)
{
	if (recording_ != rec)
		recording_ = rec;
}

/* Forward decl: defined later, but the PTZ/framing methods use it. */
static std::function<void(const ZcHttpResponse &)>
jsonObjectCb(std::function<void(const QJsonObject &)> cb);

/* Convenience query helper. */
static void sendQuery(ZcHttpTransport &tr, const char *path, const QString &pathSuffix,
		      const QUrlQuery &query, int timeout = 20000)
{
	tr.getQuery(QString(path) + pathSuffix, query, nullptr, timeout);
}

void ZcCameraClient::startRecording()
{
	QUrlQuery q;
	q.addQueryItem("action", "start");
	sendQuery(transport_, http::kRec, "", q);
	setRecording(true);
}

void ZcCameraClient::stopRecording()
{
	QUrlQuery q;
	q.addQueryItem("action", "stop");
	sendQuery(transport_, http::kRec, "", q);
	setRecording(false);
}

void ZcCameraClient::capturePhoto()
{
	QUrlQuery q;
	q.addQueryItem("action", "cap");
	sendQuery(transport_, http::kStill, "", q);
}

void ZcCameraClient::setMode(const QString &mode)
{
	QUrlQuery q;
	q.addQueryItem("action", mode);
	sendQuery(transport_, http::kMode, "", q);
}

void ZcCameraClient::lensZoomIn(int speed)
{
	QUrlQuery q;
	q.addQueryItem("action", "zoomin");
	q.addQueryItem("fspeed", QString::number(speed));
	sendQuery(transport_, http::kLens, "", q);
}

void ZcCameraClient::lensZoomOut(int speed)
{
	QUrlQuery q;
	q.addQueryItem("action", "zoomout");
	q.addQueryItem("fspeed", QString::number(speed));
	sendQuery(transport_, http::kLens, "", q);
}

void ZcCameraClient::lensZoomStop()
{
	QUrlQuery q;
	q.addQueryItem("action", "zoomstop");
	sendQuery(transport_, http::kLens, "", q);
}

void ZcCameraClient::lensFocusNear(int speed)
{
	QUrlQuery q;
	q.addQueryItem("action", "focusnear");
	q.addQueryItem("fspeed", QString::number(speed));
	sendQuery(transport_, http::kLens, "", q);
}

void ZcCameraClient::lensFocusFar(int speed)
{
	QUrlQuery q;
	q.addQueryItem("action", "focusfar");
	q.addQueryItem("fspeed", QString::number(speed));
	sendQuery(transport_, http::kLens, "", q);
}

void ZcCameraClient::lensFocusStop()
{
	QUrlQuery q;
	q.addQueryItem("action", "focusstop");
	sendQuery(transport_, http::kLens, "", q);
}

void ZcCameraClient::afOnePush()
{
	sendQuery(transport_, http::kAf, "", QUrlQuery());
}

void ZcCameraClient::ptzMoveAction(const QString &action, float fspeed)
{
	QUrlQuery q;
	q.addQueryItem("action", action);
	/* fspeed is 0-1: omit it for stop, and round so the wire value is short. */
	if (fspeed > 0.0f)
		q.addQueryItem("fspeed", QString::number(fspeed, 'f', 2));
	sendQuery(transport_, http::kPt, "", q);
}

/* The web page's ptMove: `/ctrl/pt?action=pt&pan_speed=&tilt_speed=` with a
   signed per-axis speed in -1..1 (controller.js Api.pt.ptMove). The camera
   reads the direction from the sign, so a diagonal is one request with two
   non-zero speeds. */
void ZcCameraClient::ptzMoveAnalog(float panSpeed, float tiltSpeed)
{
	QUrlQuery q;
	q.addQueryItem("action", "pt");
	q.addQueryItem("pan_speed", QString::number(panSpeed, 'f', 2));
	q.addQueryItem("tilt_speed", QString::number(tiltSpeed, 'f', 2));
	sendQuery(transport_, http::kPt, "", q);
}

void ZcCameraClient::ptzStop()
{
	QUrlQuery q;
	q.addQueryItem("action", "stop");
	sendQuery(transport_, http::kPt, "", q);
}

void ZcCameraClient::ptzHome()
{
	QUrlQuery q;
	q.addQueryItem("action", "home");
	sendQuery(transport_, http::kPt, "", q);
}

/* Full PTZ state; detail=y adds the current pan/tilt position and the range
   limit, matching the camera's own web UI. */
void ZcCameraClient::ptzQuery(bool detail,
			      std::function<void(const QJsonObject &)> cb)
{
	QUrlQuery q;
	q.addQueryItem("action", "query");
	if (detail)
		q.addQueryItem("detail", "y");
	transport_.getQuery(http::kPt, q, jsonObjectCb(cb), 20000);
}

/* Range limit: move one corner of the head's travel box. `direct` selects the
   corner the camera moves — "upright" (pan_max/tilt_max) or "downleft"
   (pan_min/tilt_min) — and the camera's own web UI always sends the whole pair
   for that corner, so both numbers are passed through. */
void ZcCameraClient::ptzLimitUpdate(const QString &direct, double pan, double tilt)
{
	QUrlQuery q;
	q.addQueryItem("action", "limit");
	q.addQueryItem("direct", direct);
	q.addQueryItem("pan_pos", QString::number(qRound(pan)));
	q.addQueryItem("tilt_pos", QString::number(qRound(tilt)));
	sendQuery(transport_, http::kPt, "", q);
}

void ZcCameraClient::ptzGoto(float pan, float tilt, float speed,
			     bool relative, bool speedWithZoom)
{
	QUrlQuery q;
	q.addQueryItem("action", "goto");
	q.addQueryItem("pan_pos", QString::number(pan, 'f', 2));
	q.addQueryItem("tilt_pos", QString::number(tilt, 'f', 2));
	q.addQueryItem("pan_speed", QString::number(speed, 'f', 2));
	q.addQueryItem("tilt_speed", QString::number(speed, 'f', 2));
	q.addQueryItem("relative", relative ? "1" : "0");
	q.addQueryItem("speed_with_zoom", speedWithZoom ? "1" : "0");
	sendQuery(transport_, http::kPt, "", q);
}

/* Auto-framing: the web's /ctrl/framing/detect?enable=<0|1> + a restart delay. */
void ZcCameraClient::setFramingEnabled(bool enabled)
{
	QUrlQuery q;
	q.addQueryItem("enable", enabled ? "1" : "0");
	sendQuery(transport_, http::kFraming, "/detect", q);
}

void ZcCameraClient::setFramingTrace(bool enabled)
{
	QUrlQuery q;
	q.addQueryItem("enable", enabled ? "1" : "0");
	sendQuery(transport_, http::kFraming, "/trace", q);
}

void ZcCameraClient::setFramingRestart(int seconds)
{
	QUrlQuery q;
	q.addQueryItem("time", QString::number(seconds));
	sendQuery(transport_, http::kFraming, "/detect_restart", q);
}

void ZcCameraClient::setFramingRestartNow()
{
	sendQuery(transport_, http::kFraming, "/restart", QUrlQuery());
}

/* The framing page's switches/dropdowns. The web UI writes these one at a time
   on change; each helper is the same `?<param>=<value>` shape. */
static void framingFlag(ZcHttpTransport &tr, const char *sub, bool enabled)
{
	QUrlQuery q;
	q.addQueryItem("enable", enabled ? "1" : "0");
	sendQuery(tr, http::kFraming, sub, q);
}

static void framingIndex(ZcHttpTransport &tr, const char *sub, const char *param,
			 int value)
{
	QUrlQuery q;
	q.addQueryItem(param, QString::number(value));
	sendQuery(tr, http::kFraming, sub, q);
}

void ZcCameraClient::setFramingScene(int scene)
{
	framingIndex(transport_, "/scene", "scene", scene);
}

void ZcCameraClient::setFramingStartPos(int index)
{
	framingIndex(transport_, "/start_position", "index", index);
}

void ZcCameraClient::setFramingTargetNum(int num)
{
	framingIndex(transport_, "/target_num", "num", num);
}

void ZcCameraClient::setFramingAutoTarget(bool enabled)
{
	framingFlag(transport_, "/auto_target", enabled);
}

void ZcCameraClient::setFramingAutoStart(bool enabled)
{
	framingFlag(transport_, "/auto_start", enabled);
}

void ZcCameraClient::setFramingZoomLock(bool enabled)
{
	framingFlag(transport_, "/zoom_lock", enabled);
}

void ZcCameraClient::setFramingTiltLock(bool enabled)
{
	framingFlag(transport_, "/tilt_lock", enabled);
}

void ZcCameraClient::setFramingTrackActiveFace(bool enabled)
{
	framingFlag(transport_, "/track_active_face", enabled);
}

void ZcCameraClient::setFramingTrackingRecord(bool enabled)
{
	framingFlag(transport_, "/tracking_record", enabled);
}

void ZcCameraClient::setFramingTally(bool enabled)
{
	framingFlag(transport_, "/tally", enabled);
}

void ZcCameraClient::setFramingSensitivity(int value)
{
	framingIndex(transport_, "/speed", "sensitivity", value);
}

void ZcCameraClient::setFramingAcceleration(int value)
{
	framingIndex(transport_, "/speed", "acceleration", value);
}

void ZcCameraClient::setFramingLostAction(int action)
{
	framingIndex(transport_, "/target_lost", "action", action);
}

void ZcCameraClient::setFramingLostTimeout(int seconds)
{
	framingIndex(transport_, "/target_lost", "timeout_sec", seconds);
}

void ZcCameraClient::setFramingMaskEnable(bool enabled)
{
	framingFlag(transport_, "/mask_area", enabled);
}

void ZcCameraClient::setFramingWaitTime(int seconds)
{
	framingIndex(transport_, "/manual_target", "wait_time", seconds);
}

void ZcCameraClient::setFramingWaving(bool enabled)
{
	QUrlQuery q;
	q.addQueryItem("waving", enabled ? "1" : "0");
	sendQuery(transport_, http::kFraming, "/gesture", q);
}

void ZcCameraClient::setFramingGestureFov(bool enabled)
{
	framingIndex(transport_, "/gesture", "fullbody", enabled ? 1 : 0);
	framingIndex(transport_, "/gesture", "waist", enabled ? 1 : 0);
	framingIndex(transport_, "/gesture", "closeup", enabled ? 1 : 0);
}

/* Both composition offsets live on the same /fov endpoint; the web UI's
   setFovPlacement / setFovFaceHeight are one query parameter each. */
void ZcCameraClient::setFramingPlacement(int value)
{
	framingIndex(transport_, "/fov", "placement", value);
}

void ZcCameraClient::setFramingFaceHeight(int value)
{
	framingIndex(transport_, "/fov", "face_height", value);
}

/* The page's "View Angle" (a 4-entry dropdown) and "Size" (a 5-step
   stepper) are two parameters of the same /fov endpoint. `fov_size` is not a
   third: the camera answers that query with the whole record and stores
   nothing, so there is no setter for it. */
void ZcCameraClient::setFramingFovType(int type)
{
	framingIndex(transport_, "/fov", "type", type);
}

void ZcCameraClient::setFramingFovBodySize(int size)
{
	framingIndex(transport_, "/fov", "body_size", size);
}

void ZcCameraClient::setFramingMarker(bool enabled)
{
	framingFlag(transport_, "/marker", enabled);
}

void ZcCameraClient::framingFovQuery(std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/fov", QUrlQuery(),
			    jsonObjectCb(cb), 20000);
}

void ZcCameraClient::framingExportProfile(
    std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/export", QUrlQuery(),
			    jsonObjectCb(cb), 20000);
}

/* The framing-box control's two boxes. Both are flags, and the page's
   getFixAngleBox() probe is the same GET without ?enable=. */
void ZcCameraClient::setFramingFixAngleBoxEnabled(bool enabled)
{
	framingFlag(transport_, "/fix_angle_box", enabled);
}

void ZcCameraClient::setFramingTrackingBoxEnabled(bool enabled)
{
	framingFlag(transport_, "/tracking_box", enabled);
}

void ZcCameraClient::framingFixAngleBoxQuery(
    std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/fix_angle_box", QUrlQuery(),
			    jsonObjectCb(cb), 20000);
}

void ZcCameraClient::framingPresetQuery(
    std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/preset", QUrlQuery(),
			    jsonObjectCb(cb), 20000);
}

void ZcCameraClient::framingPresetAdd()
{
	QUrlQuery q;
	q.addQueryItem("action", "add");
	sendQuery(transport_, http::kFraming, "/preset", q);
}

void ZcCameraClient::framingPresetRecall(int index)
{
	QUrlQuery q;
	q.addQueryItem("action", "recall");
	q.addQueryItem("index", QString::number(index));
	sendQuery(transport_, http::kFraming, "/preset", q);
}

void ZcCameraClient::framingPresetUpdate(int index, const QString &name)
{
	QUrlQuery q;
	q.addQueryItem("action", "update");
	q.addQueryItem("index", QString::number(index));
	q.addQueryItem("name", name);
	sendQuery(transport_, http::kFraming, "/preset", q);
}

void ZcCameraClient::framingPresetRename(int index, const QString &name)
{
	QUrlQuery q;
	q.addQueryItem("action", "rename");
	q.addQueryItem("index", QString::number(index));
	q.addQueryItem("name", name);
	sendQuery(transport_, http::kFraming, "/preset", q);
}

void ZcCameraClient::framingPresetReset(int index)
{
	QUrlQuery q;
	q.addQueryItem("action", "reset");
	q.addQueryItem("index", QString::number(index));
	sendQuery(transport_, http::kFraming, "/preset", q);
}

void ZcCameraClient::framingPresetDelete(int index)
{
	QUrlQuery q;
	q.addQueryItem("action", "del");
	q.addQueryItem("index", QString::number(index));
	sendQuery(transport_, http::kFraming, "/preset", q);
}

/* One snapshot call: /ctrl/framing/all_status returns every framing field
   (detect, trace, detect_restart, ...) in a single object, and is what the
   camera's web UI reads. There is no GET on /ctrl/framing itself — it answers
   an empty body, which is why the panel could never paint before. */
void ZcCameraClient::framingQuery(std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/all_status", QUrlQuery(),
			    jsonObjectCb(cb), 20000);
}

/* The one-word framing state the camera's page prints next to the three
   buttons (its `#aframe_tips`). Read separately because all_status carries the
   stored configuration but not the live state word. */
void ZcCameraClient::framingStatusQuery(std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/status", QUrlQuery(),
			    jsonObjectCb(cb), 20000);
}

void ZcCameraClient::ptzPresetSet(int index)
{
	sendQuery(transport_, http::kPreset, "", presetQuery("set", index));
}

void ZcCameraClient::ptzPresetRecall(int index)
{
	sendQuery(transport_, http::kPreset, "", presetQuery("recall", index));
}

void ZcCameraClient::ptzPresetDelete(int index)
{
	sendQuery(transport_, http::kPreset, "", presetQuery("del", index));
}

/* /ctrl/preset with one action + index, plus any extra action parameters. */
static void sendPreset(ZcHttpTransport &tr, const QString &action, int index,
		       const QList<QPair<QString, QString>> &extra = {})
{
	tr.getQuery(http::kPreset, presetQuery(action, index, extra), nullptr,
		    20000);
}

void ZcCameraClient::ptzPresetInfo(int index,
				   std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(
	    http::kPreset, presetQuery("get_info", index),
	    [cb](const ZcHttpResponse &rsp) {
		    QJsonParseError err{};
		    QJsonDocument doc = QJsonDocument::fromJson(rsp.body, &err);
		    cb(err.error == QJsonParseError::NoError && doc.isObject()
			   ? doc.object()
			   : QJsonObject());
	    },
	    20000);
}

void ZcCameraClient::ptzPresetSetName(int index, const QString &name)
{
	sendPreset(transport_, "set_name", index, {{"new_name", name}});
}

void ZcCameraClient::ptzPresetSetSpeedUnit(int index, int unit)
{
	sendPreset(transport_, "preset_speed", index,
		   {{"preset_speed_unit", QString::number(unit)}});
}

void ZcCameraClient::ptzPresetSetSpeedByIndex(int index, int speed)
{
	sendPreset(transport_, "preset_speed", index,
		   {{"preset_speed", QString::number(speed)}});
}

void ZcCameraClient::ptzPresetSetSpeedByDuration(int index, int seconds)
{
	/* The wire unit is seconds: the camera's own web UI sends
	   `preset_time = s.time / 1e3`, i.e. its millisecond value converted to
	   seconds. Sending the raw millisecond value instead made every recall time
	   collapse back to the camera's default (bug list "recall speed is
	   ineffective when the unit is Time"). */
	sendPreset(transport_, "preset_speed", index,
		   {{"preset_time", QString::number(seconds)}});
}

QString ZcCameraClient::presetThumbnailPath(int index)
{
	return zc::presetThumbnailPath(index);
}

void ZcCameraClient::ptzPresetThumbnail(
    int index, std::function<void(const QByteArray &)> cb)
{
	transport_.get(presetThumbnailPath(index),
		       [cb](const ZcHttpResponse &rsp) {
			       cb(rsp.statusCode == 200 ? rsp.body : QByteArray());
		       },
		       20000);
}

/* /ctrl/ptrace with one action + optional index, plus any extra parameters. */
static void sendPtrace(ZcHttpTransport &tr, const QString &action, int index,
		       const QList<QPair<QString, QString>> &extra = {})
{
	tr.getQuery(http::kPtrace, ptraceQuery(action, index, extra), nullptr,
		    20000);
}

void ZcCameraClient::ptraceDelete(int index)
{
	sendPtrace(transport_, "del", index);
}

void ZcCameraClient::ptraceSetName(int index, const QString &name)
{
	sendPtrace(transport_, "set_name", index, {{"new_name", name}});
}

void ZcCameraClient::ptraceRecordStart(int index)
{
	sendPtrace(transport_, "rec_start", index);
}

void ZcCameraClient::ptraceRecordStop()
{
	sendPtrace(transport_, "rec_stop", -1);
}

void ZcCameraClient::ptracePlayPrepare(int index)
{
	sendPtrace(transport_, "play_prepare", index);
}

void ZcCameraClient::ptracePlayStart()
{
	sendPtrace(transport_, "play_start", -1);
}

void ZcCameraClient::ptracePlayStop()
{
	sendPtrace(transport_, "play_stop", -1);
}

void ZcCameraClient::ptraceQuery(std::function<void(int)> cb)
{
	/* Qualified: the member name hides the zc::ptraceQuery() helper. */
	transport_.getQuery(
	    http::kPtrace, zc::ptraceQuery("query"),
	    [cb](const ZcHttpResponse &rsp) {
		    QJsonParseError err{};
		    const QJsonDocument doc =
			QJsonDocument::fromJson(rsp.body, &err);
		    cb(err.error == QJsonParseError::NoError && doc.isObject()
			   ? doc.object().value("st_code").toInt(-1)
			   : -1);
	    },
	    20000);
}

void ZcCameraClient::ptraceInfo(int index,
				std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(
	    http::kPtrace, zc::ptraceQuery("get_info", index),
	    [cb](const ZcHttpResponse &rsp) {
		    QJsonParseError err{};
		    QJsonDocument doc = QJsonDocument::fromJson(rsp.body, &err);
		    cb(err.error == QJsonParseError::NoError && doc.isObject()
			   ? doc.object()
			   : QJsonObject());
	    },
	    20000);
}

QString ZcCameraClient::ptraceThumbnailPath(int index)
{
	return zc::ptraceThumbnailPath(index);
}

void ZcCameraClient::ptraceThumbnail(
    int index, std::function<void(const QByteArray &)> cb)
{
	transport_.get(ptraceThumbnailPath(index),
		       [cb](const ZcHttpResponse &rsp) {
			       cb(rsp.statusCode == 200 ? rsp.body : QByteArray());
		       },
		       20000);
}

void ZcCameraClient::readGroup(const QString &catalog,
			       std::function<void(bool)> cb)
{
	settings_.readGroup(catalog, cb);
}

void ZcCameraClient::streamSettingQuery(
    const QString &index, std::function<void(const QJsonObject &)> cb)
{
	sendStreamSetting(index, {}, cb);
}

void ZcCameraClient::streamSettingWrite(
    const QString &index, const QList<QPair<QString, QString>> &params,
    std::function<void(const QJsonObject &)> cb)
{
	sendStreamSetting(index, params, cb);
}

void ZcCameraClient::sendStreamSetting(
    const QString &index, const QList<QPair<QString, QString>> &params,
    const std::function<void(const QJsonObject &)> &cb)
{
	transport_.getQuery(
	    http::kStream, zc::streamSettingQuery(index, params),
	    [cb](const ZcHttpResponse &rsp) {
		    QJsonParseError err{};
		    const QJsonDocument doc =
			QJsonDocument::fromJson(rsp.body, &err);
		    cb(err.error == QJsonParseError::NoError && doc.isObject()
			   ? doc.object()
			   : QJsonObject());
	    },
	    20000);
}

/* Both network calls deliver the camera's JSON reply; an empty object means it
   did not answer (or answered non-JSON). */
static std::function<void(const ZcHttpResponse &)>
jsonObjectCb(std::function<void(const QJsonObject &)> cb)
{
	return [cb](const ZcHttpResponse &rsp) {
		QJsonParseError err{};
		const QJsonDocument doc = QJsonDocument::fromJson(rsp.body, &err);
		cb(err.error == QJsonParseError::NoError && doc.isObject()
		       ? doc.object()
		       : QJsonObject());
	};
}

void ZcCameraClient::networkInfo(std::function<void(const QJsonObject &)> cb)
{
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("action"), QStringLiteral("info"));
	transport_.getQuery(http::kNetwork, q, jsonObjectCb(cb), 20000);
}

void ZcCameraClient::setNetworkStatic(
    const QString &ip, const QString &netmask, const QString &gateway,
    const QString &dns, std::function<void(const QJsonObject &)> cb)
{
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("action"), QStringLiteral("set"));
	q.addQueryItem(QStringLiteral("mode"), QStringLiteral("static"));
	/* The camera's own client omits a field it was not handed, so an empty
	   string leaves that field as it is. */
	if (!ip.isEmpty())
		q.addQueryItem(QStringLiteral("ipaddr"), ip);
	if (!netmask.isEmpty())
		q.addQueryItem(QStringLiteral("netmask"), netmask);
	if (!gateway.isEmpty())
		q.addQueryItem(QStringLiteral("gateway"), gateway);
	if (!dns.isEmpty())
		q.addQueryItem(QStringLiteral("dns"), dns);
	transport_.getQuery(http::kNetwork, q, jsonObjectCb(cb), 20000);
}

void ZcCameraClient::readSetting(const QString &key,
				 std::function<void(const ZcSettingValue &)> cb)
{
	settings_.readKey(key, cb);
}

void ZcCameraClient::writeSetting(const QString &key, const QString &value)
{
	settings_.writeKey(key, value);
}

QString ZcCameraClient::settingValue(const QString &key) const
{
	return settings_.value(key);
}

bool ZcCameraClient::settingDefinition(const QString &key,
				       ZcSettingValue *out) const
{
	return settings_.definition(key, out);
}

QStringList ZcCameraClient::settingGroupKeys(const QString &catalog) const
{
	return settings_.groupKeys(catalog);
}

/* /info is the camera's identity document: model name plus the `feature` map
   the PTZ tab is gated on (`feature.lens_ctrl_ptz`). It is fetched separately
   from origin negotiation, which only looks at the status code. */
void ZcCameraClient::refreshCameraInfo()
{
	transport_.get(http::kInfo, [this](const ZcHttpResponse &rsp) {
		QJsonParseError err{};
		QJsonDocument doc = QJsonDocument::fromJson(rsp.body, &err);
		if (err.error != QJsonParseError::NoError || !doc.isObject())
			return;
		const QJsonObject info = doc.object();
		model_ = info.value("model").toString();
		if (model_.isEmpty())
			model_ = info.value("cameraName").toString();

		/* Qualified: the class has a same-named accessor. */
		supportsPtz_ = zc::cameraSupportsPtz(info);
		supportsAutoFraming_ = zc::cameraSupportsAutoFraming(info);
		emit cameraInfoChanged();
	});
}

void ZcCameraClient::refreshStatus()
{
	transport_.get(http::kStatus,
		       [this](const ZcHttpResponse &rsp) {
			       QJsonParseError err{};
			       QJsonDocument doc = QJsonDocument::fromJson(
				       rsp.body, &err);
			       if (err.error == QJsonParseError::NoError &&
				   doc.isObject()) {
				       hudValues_ = doc.object();
				       emit statusChanged(hudValues_);
			       }
		       });
}

} // namespace zc