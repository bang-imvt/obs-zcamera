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

#include "zc_settings_schema.h"

namespace zc {

ZcCameraClient::ZcCameraClient(QObject *parent) : QObject(parent)
{
	QObject::connect(&notifier_, &ZcWsNotifier::eventReceived, this,
			 &ZcCameraClient::handleWsEvent);
	settings_.setTransport(&transport_);
	QObject::connect(&settings_, &ZcSettingsEngine::groupRefreshed, this,
			 &ZcCameraClient::groupRefreshed);
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

	/* The camera requires Digest auth; without credentials nothing else we
	   do can succeed, so fail fast instead of looping on 401s. The Digest
	   handshake itself runs in the transport on the first real request. */
	if (transport_.authRequired() && creds.username.isEmpty()) {
		/* Remembered so the dock can tell "needs a login" from "unreachable"
		   and ask the operator for credentials (bug 4). */
		authRequired_ = true;
		return false;
	}

	/* Start the WebSocket notifications (wss when origin is https), carrying
	   the Digest Authorization / cookie / x-auth-token from the transport. */
	bool https = transport_.baseUrl().startsWith("https://");
	notifier_.start(host, https, transport_.upgradeHeaderBlock());

	/* Read initial state. */
	refreshStatus();
	refreshCameraInfo();

	connected_ = true;
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
		if (!key.isEmpty())
			emit configChanged(key, value);
	} else if (what == ws_events::kAfArea) {
		emit afAreaChanged(obj.value("roi_x").toDouble(),
				   obj.value("roi_y").toDouble(),
				   obj.value("roi_w").toDouble(),
				   obj.value("roi_h").toDouble());
	}
}

void ZcCameraClient::setRecording(bool rec)
{
	if (recording_ != rec)
		recording_ = rec;
}

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

void ZcCameraClient::ptzMove(int panSpeed, int tiltSpeed)
{
	QUrlQuery q;
	q.addQueryItem("action", "pt");
	q.addQueryItem("pan_speed", QString::number(panSpeed));
	q.addQueryItem("tilt_speed", QString::number(tiltSpeed));
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
	/* The camera stores the duration in milliseconds. */
	sendPreset(transport_, "preset_speed", index,
		   {{"preset_time", QString::number(seconds * 1000)}});
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