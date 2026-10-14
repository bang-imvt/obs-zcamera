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

   Socket: <scheme>://<address>:81/ — always the camera's notification port. The
   scheme starts out following the HTTP origin, but that is only a guess: at
   least some firmware serves plain ws on 81 even when its web UI is https, so a
   dial that fails before the handshake flips the scheme for the next attempt
   (see onSocketError). Messages are JSON objects dispatched on the `what` field.

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

class ZcHttpTransport;

class ZcWsNotifier : public QObject {
	Q_OBJECT

public:
	explicit ZcWsNotifier(QObject *parent = nullptr);

	/* Start connecting to the camera's notification socket. The socket always
	   lives on the camera's own port 81; `secure` only picks the scheme to try
	   FIRST (secure=false -> ws, secure=true -> wss) — a dial that fails before
	   the handshake flips it, because the notification service does not
	   necessarily follow the web UI's TLS setting. extraHeaders carries the
	   Digest Authorization / Cookie / x-auth-token lines for the upgrade request
	   (from ZcHttpTransport). */
	void start(const QString &host, bool secure,
		   const QByteArray &extraHeaders = QByteArray());
	void stop();

	/* The HTTP transport owns the live auth/session state. When set, the
	   upgrade headers are rebuilt from it on every (re)connect (see
	   ZcHttpTransport::currentUpgradeHeaderBlock), so a header captured once at
	   start() cannot go stale across a reconnect. */
	void setTransport(ZcHttpTransport *transport);

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
	/* A dial that failed before the handshake: try the other scheme next. */
	void noteDialFailure();

	/* Parented on purpose: a caller that moves the notifier to its own thread
	   (moveToThread) must move the socket and its timers with it. As a plain
	   member it would stay on the thread that constructed the notifier, and if
	   that thread has no Qt event loop — which is what OBS's update() caller can
	   be — the socket's connected() signal is never delivered, so the upgrade
	   request is never written: the TCP connection just sits there. */
	ZcWsClient socket_{this};
	QTimer reconnectTimer_{this};
	QString host_;
	QByteArray extraHeaders_; /* upgrade request headers (auth/cookie) */
	ZcHttpTransport *transport_ = nullptr; /* live auth source (may be null) */
	bool secure_ = false;
	/* Whether the dial currently in flight ever reached the handshake. Only a
	   dial that never connected says something about the SCHEME; a socket that
	   connected and later dropped says something about the network. */
	bool dialConnected_ = false;
	/* One log line per failure streak for the scheme flip (failureLogged_ is
	   already set by the time the flip is decided). */
	bool flipLogged_ = false;
	bool live_ = false;
	bool stopped_ = true;
	int retry_ = 0;
	bool failureLogged_ = false; /* one log line per failure streak */
};

} // namespace zc