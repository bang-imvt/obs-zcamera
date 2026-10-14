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

#include "zc_ws_notifier.h"
#include "zc_http_transport.h"

#include <QDebug>
#include <QJsonDocument>
#include <QUrl>

namespace zc {

ZcWsNotifier::ZcWsNotifier(QObject *parent)
	: QObject(parent)
{
	connect(&socket_, &ZcWsClient::connected, this, &ZcWsNotifier::onConnected);
	connect(&socket_, &ZcWsClient::disconnected, this,
		&ZcWsNotifier::onDisconnected);
	connect(&socket_, &ZcWsClient::textMessageReceived, this,
		&ZcWsNotifier::onTextMessage);
	connect(&socket_, &ZcWsClient::error, this, &ZcWsNotifier::onSocketError);

	reconnectTimer_.setSingleShot(true);
	connect(&reconnectTimer_, &QTimer::timeout, this,
		&ZcWsNotifier::doConnect);
}

void ZcWsNotifier::setTransport(ZcHttpTransport *transport)
{
	transport_ = transport;
}

void ZcWsNotifier::start(const QString &host, bool secure,
			 const QByteArray &extraHeaders)
{
	host_ = host;
	secure_ = secure;
	extraHeaders_ = extraHeaders;
	retry_ = 0;
	stopped_ = false;
	flipLogged_ = false;
	doConnect();
}

void ZcWsNotifier::stop()
{
	stopped_ = true;
	reconnectTimer_.stop();
	socket_.close();
	live_ = false;
	emit socketStateChanged(false);
}

void ZcWsNotifier::doConnect()
{
	if (host_.isEmpty() || stopped_)
		return;
	QUrl url;
	url.setScheme(secure_ ? "wss" : "ws");
	url.setHost(host_);
	/* The notification server always lives on the camera's own port 81, not on
	   the web server's port: pointing the upgrade at the HTTPS port lands on the
	   web UI and comes back 301 -> /www/index.html. The scheme is a guess that
	   gets probed — measured on e2ptz sw 1.0.3 with HTTPS and identity auth
	   enabled, port 81 answers plain ws (no auth headers needed) and refuses a
	   TLS handshake outright, while the HTTP origin for that same camera is
	   https. Following the origin alone therefore never connects. */
	url.setPort(81);
	url.setPath("/");

	/* One line per reconnect cycle: the OBS log then shows that the plugin did
	   try the notification socket, without listing every backoff attempt.
	   qWarning, not qInfo: OBS's message handler drops a plugin's qInfo
	   output, so a qInfo line here would never reach the log. */
	if (retry_ == 0)
		qWarning().noquote()
			<< "[obs-zcamera] notifications connecting:" << url.toString();

	/* The upgrade carries Digest Authorization, x-auth-token and the session
	   cookie, supplied by the HTTP transport. Rebuild it from the CURRENT auth
	   state on every (re)connect: the cached Authorization goes stale (login()
	   clears it, and nc must advance per nonce), so replaying the snapshot
	   handed to start() would leave a later reconnect unauthenticated. */
	const QByteArray headers =
		transport_ ? transport_->currentUpgradeHeaderBlock() : extraHeaders_;
	dialConnected_ = false;
	socket_.open(url, headers);
}

void ZcWsNotifier::onConnected()
{
	/* A dial that was in flight when stop() ran can still emit connected()
	   once it completes; it must not revive the notifier. Tear the just-opened
	   socket down instead. */
	if (stopped_) {
		socket_.close();
		return;
	}
	live_ = true;
	dialConnected_ = true;
	retry_ = 0;
	failureLogged_ = false;
	flipLogged_ = false;
	qWarning().noquote() << "[obs-zcamera] notifications connected:"
			     << (secure_ ? "wss" : "ws") << host_ << "port 81";
	emit socketStateChanged(true);
}

void ZcWsNotifier::onDisconnected()
{
	/* stop() already reported the notifier as down and cancelled reconnects;
	   nothing here should emit or reschedule. */
	if (stopped_)
		return;
	if (live_) {
		live_ = false;
		qWarning().noquote() << "[obs-zcamera] notifications disconnected:"
				     << host_;
		emit socketStateChanged(false);
	}
	noteDialFailure();
	scheduleReconnect();
}

void ZcWsNotifier::onTextMessage(const QString &msg)
{
	if (stopped_)
		return;
	QJsonParseError err{};
	QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return;

	QJsonObject obj = doc.object();
	QString what = obj.value("what").toString();
	if (what.isEmpty())
		what = obj.value("type").toString();
	if (what.isEmpty())
		return;

	emit eventReceived(what, obj);
}

void ZcWsNotifier::onSocketError(const QString &error)
{
	/* A stop()ped notifier must not log, emit or reschedule — an error can
	   still surface from a dial that was in flight when stop() ran. */
	if (stopped_)
		return;
	/* Backoff and reconnect. Never issue camera commands during recovery. */
	/* Once per failure streak: a camera that stays off would otherwise put a
	   line in the log at every backoff interval. */
	if (!failureLogged_) {
		failureLogged_ = true;
		qWarning().noquote()
			<< "[obs-zcamera] notifications failed:" << error;
	}
	live_ = false;
	emit socketStateChanged(false);
	noteDialFailure();
	scheduleReconnect();
}

void ZcWsNotifier::noteDialFailure()
{
	/* Only a dial that never reached the handshake says something about the
	   scheme. A socket that connected and later dropped is a network event, and
	   flipping the scheme there would just make the next attempt wrong too.
	   Flipping (rather than pinning) keeps working on firmware whose port 81
	   really does speak TLS: that one simply gets tried again on the next
	   failure. */
	if (dialConnected_)
		return;
	secure_ = !secure_;
	if (!flipLogged_) {
		flipLogged_ = true;
		qWarning().noquote()
			<< "[obs-zcamera] notifications: port 81 refused"
			<< (secure_ ? "ws" : "wss") << host_
			<< "- retrying with" << (secure_ ? "wss" : "ws");
	}
}

void ZcWsNotifier::scheduleReconnect()
{
	/* Never schedule a reconnect after stop(): a timer armed before stop()
	   would otherwise fire and re-open the socket. */
	if (host_.isEmpty() || stopped_)
		return;
	/* Escalating backoff, a true capped exponential: 1s, 2s, 4s, 8s, then 10s.
	   A raw `1000 << retry_` kept doubling past the cap (16 s at retry 4) and
	   would overflow once the counter reached the int bit width. */
	const int delayMs = (retry_ >= 4) ? 10000 : (1000 << retry_);
	retry_++;
	reconnectTimer_.start(delayMs);
}

} // namespace zc