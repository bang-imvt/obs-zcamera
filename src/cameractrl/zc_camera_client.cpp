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

/* The rectangle only — the enable flag is its own request (/mask_area?enable=),
   exactly as the camera page writes it: its setMaskArea() sends the four
   coordinates and nothing else. Sending an extra `enable` here made the camera
   answer 200 and keep its old rectangle, which is why an edit never reached it
   (bug 29). Coordinates are 0..1, the same space the page uses. */
void ZcCameraClient::setFramingMaskRect(float left, float top, float right,
					float bottom)
{
	QUrlQuery q;
	q.addQueryItem("left", QString::number(left, 'f', 5));
	q.addQueryItem("top", QString::number(top, 'f', 5));
	q.addQueryItem("right", QString::number(right, 'f', 5));
	q.addQueryItem("bottom", QString::number(bottom, 'f', 5));
	sendQuery(transport_, http::kFraming, "/mask_area", q);
}

void ZcCameraClient::framingMaskQuery(std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/mask_area", QUrlQuery(),
			    jsonObjectCb(cb), 20000);
}

/* The face database lives on a prefix of its own, which moved between
   generations: 1.0.3 and older answer /ctrl/facedb/<sub> (what the camera's
   own registration client drives — the static `facedb` object in
   controller.min.js), while 1.0.5 dropped the /ctrl and answers
   /facedb/<sub> (bug list: "1.0.5 版本人脸注册界面的所有接口都需要去 ctrl").
   Picking the wrong one looks exactly like "this camera has no face
   database", which is why the first /query retries the other prefix once and
   remembers which one answered. `prepare` captures a face from the live view
   and hands back a uid + thumbnail; `add` then stores it under the operator's
   name. The camera rejects `add` with "prepare not ready" until a prepare has
   succeeded, so the two calls are always a pair. */
QString ZcCameraClient::faceDbBase()
{
	if (!faceDbBase_.isEmpty())
		return faceDbBase_;
	/* Unknown /info keeps the older prefix; the probe corrects it. */
	return firmwareAtLeast("1.0.5") ? QStringLiteral("/facedb")
					: QStringLiteral("/ctrl/facedb");
}

void ZcCameraClient::framingFaceQuery(std::function<void(const QJsonObject &)> cb)
{
	const QString base = faceDbBase();
	transport_.getQuery(
	    base + QStringLiteral("/query"), QUrlQuery(),
	    jsonObjectCb([this, cb, base](const QJsonObject &doc) {
		    const bool refused =
			doc.contains(QLatin1String("code")) &&
			doc.value(QLatin1String("code")).toInt() < 0;
		    if (refused && !faceDbProbed_ && base == faceDbBase()) {
			    /* A refusal on the guessed prefix: ask the other
			       generation's one before reporting "not supported". */
			    faceDbBase_ = base == QLatin1String("/facedb")
					      ? QStringLiteral("/ctrl/facedb")
					      : QStringLiteral("/facedb");
			    faceDbProbed_ = true;
			    framingFaceQuery(cb);
			    return;
		    }
		    faceDbProbed_ = true;
		    cb(doc);
	    }),
	    20000);
}

void ZcCameraClient::framingFaceStatus(std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(faceDbBase() + QStringLiteral("/status"),
			    QUrlQuery(), jsonObjectCb(cb), 20000);
}

void ZcCameraClient::setFramingFaceEnable(bool enabled)
{
	/* The camera's page enables the store before capturing. It answers
	   {"code":0} regardless on this firmware and /status keeps reporting the
	   live state, but sending it keeps parity with the page. */
	QUrlQuery q;
	q.addQueryItem("enable", enabled ? "1" : "0");
	transport_.getQuery(faceDbBase() + QStringLiteral("/set"), q, nullptr,
			    20000);
}

void ZcCameraClient::framingFacePrepare(
    int faceId, std::function<void(const QJsonObject &)> cb)
{
	/* Capturing takes a moment and fails while nobody is in frame, so it gets
	   the longer timeout and its own reply. `face_id` is the target slot and
	   is NOT optional — the page always sends it, and with the parameter
	   missing the camera answers "face not found" whatever the picture holds
	   (bug list #3). The uid the reply carries is what add() needs, and it is
	   this same slot. */
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("face_id"), QString::number(faceId));
	transport_.getQuery(faceDbBase() + QStringLiteral("/prepare"), q,
			    jsonObjectCb(cb), 20000);
}

void ZcCameraClient::framingFaceAdd(int which, const QString &name, int priority,
				    bool activate)
{
	QUrlQuery q;
	q.addQueryItem("which", QString::number(which));
	q.addQueryItem("name", name);
	q.addQueryItem("priority", QString::number(priority));
	q.addQueryItem("activate", activate ? "1" : "0");
	transport_.getQuery(faceDbBase() + QStringLiteral("/add"), q, nullptr,
			    20000);
}

void ZcCameraClient::framingFaceUpdate(int uid, const QString &name, int priority,
				       bool activate)
{
	QUrlQuery q;
	q.addQueryItem("uid", QString::number(uid));
	q.addQueryItem("name", name);
	q.addQueryItem("priority", QString::number(priority));
	q.addQueryItem("activate", activate ? "1" : "0");
	transport_.getQuery(faceDbBase() + QStringLiteral("/update"), q, nullptr,
			    20000);
}

void ZcCameraClient::framingFaceDelete(int uid)
{
	QUrlQuery q;
	q.addQueryItem("uid", QString::number(uid));
	transport_.getQuery(faceDbBase() + QStringLiteral("/delete"), q, nullptr,
			    20000);
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

void ZcCameraClient::setFramingFovSize(int percent)
{
	/* The >=1.0.5 firmware's "Size" slider: 0..100 on /fov (the page sends
	   the same; `preset=1` is the double-click "store as preset" variant,
	   which the panel does not offer). */
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("fov_size"), QString::number(percent));
	sendQuery(transport_, http::kFraming, "/fov", q);
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

/* The camera's own page (exportFramingProfile in controller.js) does this in
   two steps: GET /ctrl/framing/export answers only a work order
   {"code":0,"filename":"framing_profile.json"} — writing THAT to a file is
   what made the plugin's exported JSON carry no settings (bug list #3) — and
   then fetches the real profile from /app_data/preset/<filename>. */
void ZcCameraClient::framingExportProfileFile(
    std::function<void(int result, const QByteArray &profile)> cb)
{
	framingExportProfile([this, cb](const QJsonObject &doc) {
		if (doc.contains("code") && doc.value("code").toInt() < 0) {
			cb(-1, QByteArray()); /* firmware without the feature */
			return;
		}
		const QString filename = doc.value("filename").toString();
		if (filename.isEmpty()) {
			cb(1, QByteArray());
			return;
		}
		transport_.get(QStringLiteral("/app_data/preset/") + filename,
			       [cb](const ZcHttpResponse &rsp) {
				       if (rsp.statusCode != 200 ||
					   rsp.body.isEmpty()) {
					       cb(1, QByteArray());
					       return;
				       }
				       cb(0, rsp.body);
			       },
			       20000);
	});
}

/* Multipart upload, exactly the request the page's Upload helper builds:
   one form part named "file" carrying the profile JSON. The camera's own
   refusal text ("bad profile version", "not a framing profile", …) is
   passed back verbatim: "code -1" alone tells nobody which part of the
   file it objected to (bug list #7). */
void ZcCameraClient::framingImportProfile(
    const QByteArray &profile, const QString &filename,
    std::function<void(bool ok, const QString &error)> cb)
{
	const QByteArray boundary = QByteArrayLiteral("----obs-zcamera-form");
	QByteArray body;
	body += "--" + boundary + "\r\n";
	body += "Content-Disposition: form-data; name=\"file\"; filename=\"" +
		filename.toUtf8() + "\"\r\n";
	body += "Content-Type: application/json\r\n\r\n";
	body += profile;
	body += "\r\n--" + boundary + "--\r\n";

	transport_.postBody(
	    QString(http::kFraming) + "/import", body,
	    "multipart/form-data; boundary=" + boundary,
	    [cb](const ZcHttpResponse &rsp) {
		    if (rsp.statusCode != 200) {
			    cb(false, rsp.statusCode == 0
					  ? rsp.errorText
					  : QStringLiteral("HTTP ") +
						QString::number(rsp.statusCode));
			    return;
		    }
		    QJsonParseError err{};
		    const QJsonDocument doc =
			    QJsonDocument::fromJson(rsp.body, &err);
		    const QJsonObject obj =
			err.error == QJsonParseError::NoError && doc.isObject()
			    ? doc.object()
			    : QJsonObject();
		    const int code = obj.value(QLatin1String("code")).toInt(-1);
		    if (code == 0) {
			    cb(true, QString());
			    return;
		    }
		    QString why;
		    for (const char *k : {"detail", "error", "msg", "message"}) {
			    const QString v =
				obj.value(QLatin1String(k)).toString();
			    if (!v.isEmpty()) {
				    why = v;
				    break;
			    }
		    }
		    QString msg = QStringLiteral("code ") +
				  QString::number(code);
		    if (!why.isEmpty())
			    msg += QStringLiteral(": ") + why;
		    cb(false, msg);
	    },
	    20000);
}

/* Enabling a box is TWO writes on the camera's own page (framing.js
   _saveFixAngle / _saveTrackingBox): the current pan/tilt — and for the
   tracking box its ROI — are stored into the box with action=update FIRST,
   and only then is ?enable=1 sent ("enable 时先保存参数再使能"). Sending the
   bare ?enable=1 the plugin used to send left the camera's box switch off no
   matter what the plugin UI showed (bug list "固定角度切换失败"). Disabling
   stays the one ?enable=0 write, exactly like the page. */
void ZcCameraClient::setFramingFixAngleBoxEnabled(bool enabled)
{
	if (!enabled) {
		framingFlag(transport_, "/fix_angle_box", false);
		return;
	}
	/* zoom_factor: the page keeps the live focal ratio in memory and writes
	   it with the update; the plugin has no zoom stream, so it re-sends the
	   ratio the box already stores (a camera that has never saved one gets
	   the page's 1.0 default). */
	framingFixAngleBoxQuery([this](const QJsonObject &box) {
		const double zoom = box.contains("zoom_factor")
					    ? box.value("zoom_factor").toDouble()
					    : 1.0;
		ptzQuery(true, [this, zoom](const QJsonObject &pt) {
			const double pan = pt.contains("pan_pos")
						   ? pt.value("pan_pos").toDouble()
						   : pt.value("pan").toDouble();
			const double tilt = pt.contains("tilt_pos")
						    ? pt.value("tilt_pos").toDouble()
						    : pt.value("tilt").toDouble();
			QUrlQuery q;
			q.addQueryItem("action", "update");
			q.addQueryItem("pan", QString::number(pan));
			q.addQueryItem("tilt", QString::number(tilt));
			q.addQueryItem("zoom_factor", QString::number(zoom));
			sendQuery(transport_, http::kFraming, "/fix_angle_box",
				  q);
			framingFlag(transport_, "/fix_angle_box", true);
		});
	});
}

void ZcCameraClient::setFramingTrackingBoxEnabled(bool enabled)
{
	if (!enabled) {
		framingFlag(transport_, "/tracking_box", false);
		return;
	}
	transport_.getQuery(QString(http::kFraming) + "/tracking_box",
			    QUrlQuery(),
			    jsonObjectCb([this](const QJsonObject &box) {
				    const double zoom =
					    box.contains("zoom_factor")
						    ? box.value("zoom_factor")
							      .toDouble()
						    : 1.0;
				    /* The record's stored ROI; a camera that
				       has never saved one gets the page's
				       full-frame default. */
				    const double top = box.contains("top")
							   ? box.value("top").toDouble()
							   : 0.0;
				    const double left = box.contains("left")
								? box.value("left").toDouble()
								: 0.0;
				    const double width = box.contains("width")
								 ? box.value("width").toDouble()
								 : 1.0;
				    const double height =
					    box.contains("height")
						    ? box.value("height").toDouble()
						    : 1.0;
				    ptzQuery(true,
					     [this, zoom, top, left, width,
					      height](const QJsonObject &pt) {
						     const double pan =
							     pt.contains("pan_pos")
								     ? pt.value("pan_pos").toDouble()
								     : pt.value("pan").toDouble();
						     const double tilt =
							     pt.contains("tilt_pos")
								     ? pt.value("tilt_pos").toDouble()
								     : pt.value("tilt").toDouble();
						     QUrlQuery q;
						     q.addQueryItem("action", "update");
						     q.addQueryItem("pan", QString::number(pan));
						     q.addQueryItem("tilt", QString::number(tilt));
						     q.addQueryItem("zoom_factor", QString::number(zoom));
						     q.addQueryItem("top", QString::number(top));
						     q.addQueryItem("left", QString::number(left));
						     q.addQueryItem("width", QString::number(width));
						     q.addQueryItem("height", QString::number(height));
						     sendQuery(transport_, http::kFraming,
							       "/tracking_box", q);
						     framingFlag(transport_,
								 "/tracking_box",
								 true);
					     });
			    }),
			    20000);
}

void ZcCameraClient::framingFixAngleBoxQuery(
    std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/fix_angle_box", QUrlQuery(),
			    jsonObjectCb(cb), 20000);
}

void ZcCameraClient::framingTrackingBoxQuery(
    std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/tracking_box", QUrlQuery(),
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

void ZcCameraClient::framingConfigQuery(std::function<void(const QJsonObject &)> cb)
{
	transport_.getQuery(QString(http::kFraming) + "/config", QUrlQuery(),
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

/* /ctrl/ptrace with one action + optional index, plus any extra parameters.
   `cb`, when given, receives the camera's `code` field (0 = accepted); a
   transport failure or a non-JSON body reports -1. The camera rejects trace
   actions with code -2 while auto-framing owns the head or while the transport
   is still finalizing the just-stopped recording — ignoring that reply made a
   failed re-record/playback look like a silent no-op (bug list #137). */
static void sendPtrace(ZcHttpTransport &tr, const QString &action, int index,
		       const QList<QPair<QString, QString>> &extra = {},
		       std::function<void(int)> cb = {})
{
	tr.getQuery(http::kPtrace, ptraceQuery(action, index, extra),
		    [cb](const ZcHttpResponse &rsp) {
			    if (!cb)
				    return;
			    QJsonParseError err{};
			    const QJsonDocument doc =
				QJsonDocument::fromJson(rsp.body, &err);
			    cb(err.error == QJsonParseError::NoError &&
				       doc.isObject()
				   ? doc.object().value("code").toInt(-1)
				   : -1);
		    },
		    20000);
}

void ZcCameraClient::ptraceDelete(int index, std::function<void(int)> cb)
{
	sendPtrace(transport_, "del", index, {}, std::move(cb));
}

void ZcCameraClient::ptraceSetName(int index, const QString &name,
				   std::function<void(int)> cb)
{
	sendPtrace(transport_, "set_name", index, {{"new_name", name}},
		   std::move(cb));
}

void ZcCameraClient::ptraceRecordStart(int index, std::function<void(int)> cb)
{
	sendPtrace(transport_, "rec_start", index, {}, std::move(cb));
}

void ZcCameraClient::ptraceRecordStop(std::function<void(int)> cb)
{
	sendPtrace(transport_, "rec_stop", -1, {}, std::move(cb));
}

void ZcCameraClient::ptracePlayPrepare(int index, std::function<void(int)> cb)
{
	sendPtrace(transport_, "play_prepare", index, {}, std::move(cb));
}

void ZcCameraClient::ptracePlayStart(std::function<void(int)> cb)
{
	sendPtrace(transport_, "play_start", -1, {}, std::move(cb));
}

void ZcCameraClient::ptracePlayStop(std::function<void(int)> cb)
{
	sendPtrace(transport_, "play_stop", -1, {}, std::move(cb));
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
		/* The firmware string ("1.0.3") drives the framing panel's
		   version gates (bug list #11/#48: items the >=1.0.5 firmware
		   drops, #10: items it replaces). Empty when absent, which every
		   firmwareAtLeast() comparison then fails. */
		firmwareVersion_ = info.value("sw").toString();

		/* Qualified: the class has a same-named accessor. */
		supportsPtz_ = zc::cameraSupportsPtz(info);
		supportsAutoFraming_ = zc::cameraSupportsAutoFraming(info);
		/* The whole feature map, for the row/catalog gates. */
		features_ = zc::ZcFeatures(info);
		emit cameraInfoChanged();
	});
}

bool ZcCameraClient::firmwareAtLeast(const char *ver) const
{
	/* Numeric triple compare: "1.0.5" <= ours. Dotted-octet style; any
	   missing component counts as 0, and an empty/unparseable ours never
	   satisfies the comparison (an unknown firmware hides nothing). */
	QStringList want =
	    QString::fromLatin1(ver).split(QChar('.'));
	QStringList have = firmwareVersion_.split(QChar('.'));
	if (have.isEmpty() || have.first().isEmpty())
		return false;
	for (int i = 0; i < qMax(want.size(), 3); ++i) {
		const int w = i < want.size() ? want[i].toInt() : 0;
		const int h = i < have.size() ? have[i].toInt() : 0;
		if (h > w)
			return true;
		if (h < w)
			return false;
	}
	return true;
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