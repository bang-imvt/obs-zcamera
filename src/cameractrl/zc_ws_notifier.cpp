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

void ZcWsNotifier::start(const QString &host, bool httpsOrigin,
			 const QByteArray &extraHeaders)
{
	host_ = host;
	httpsOrigin_ = httpsOrigin;
	extraHeaders_ = extraHeaders;
	retry_ = 0;
	stopped_ = false;
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
	url.setScheme(httpsOrigin_ ? "wss" : "ws");
	url.setHost(host_);
	url.setPort(81);
	url.setPath("/");

	/* The upgrade carries Digest Authorization, x-auth-token and the
	   session cookie, supplied by the HTTP transport. */
	socket_.open(url, extraHeaders_);
}

void ZcWsNotifier::onConnected()
{
	live_ = true;
	retry_ = 0;
	emit socketStateChanged(true);
}

void ZcWsNotifier::onDisconnected()
{
	if (live_) {
		live_ = false;
		emit socketStateChanged(false);
	}
	scheduleReconnect();
}

void ZcWsNotifier::onTextMessage(const QString &msg)
{
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
	/* Backoff and reconnect. Never issue camera commands during recovery. */
	live_ = false;
	emit socketStateChanged(false);
	scheduleReconnect();
}

void ZcWsNotifier::scheduleReconnect()
{
	/* Never schedule a reconnect after stop(): a timer armed before stop()
	   would otherwise fire and re-open the socket. */
	if (host_.isEmpty() || stopped_)
		return;
	/* Escalating backoff: 1s, 2s, 4s, ... capped at 10s. */
	int delayMs = (retry_ < 5) ? (1000 << retry_) : 10000;
	retry_++;
	reconnectTimer_.start(delayMs);
}

} // namespace zc