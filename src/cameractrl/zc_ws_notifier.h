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

/* Camera WebSocket notifications, ported from ZCamGuiOpen notifications.ts
   + camera-state.ts.

   Socket: <scheme>://<address>:81/ — always the camera's notification port, on
   ws:// or wss:// depending on whether the HTTP origin negotiated https.
   Messages are JSON objects dispatched on the `what` field.

   On disconnect we mark state stale, reconnect with backoff, and temporarily
   refresh important state by HTTP — but we never send commands as part of
   recovery (matching the reference client). */

#pragma once

#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <memory>
#include "zc_ws_client.h"

namespace zc {

class ZcWsNotifier : public QObject {
	Q_OBJECT

public:
	explicit ZcWsNotifier(QObject *parent = nullptr);

	/* Start connecting to the camera's notification socket. The socket always
	   lives on the camera's own port 81; only the scheme follows the HTTP origin
	   (secure=false -> ws, secure=true -> wss). extraHeaders carries the Digest
	   Authorization / Cookie / x-auth-token lines for the upgrade request (from
	   ZcHttpTransport). */
	void start(const QString &host, bool secure,
		   const QByteArray &extraHeaders = QByteArray());
	void stop();

	/* Mark the socket as live (receiving notifications). */
	bool isLive() const { return live_; }

signals:
	/* Emitted for each normalized WS event. `what` is the raw event name. */
	void eventReceived(const QString &what, const QJsonObject &payload);
	/* State of the notification socket. */
	void socketStateChanged(bool live);

private slots:
	void onConnected();
	void onDisconnected();
	void onTextMessage(const QString &msg);
	void onSocketError(const QString &error);

private:
	void scheduleReconnect();
	void doConnect();

	ZcWsClient socket_;
	QTimer reconnectTimer_;
	QString host_;
	QByteArray extraHeaders_; /* upgrade request headers (auth/cookie) */
	bool secure_ = false;
	bool live_ = false;
	bool stopped_ = true;
	int retry_ = 0;
	bool failureLogged_ = false; /* one log line per failure streak */
};

} // namespace zc