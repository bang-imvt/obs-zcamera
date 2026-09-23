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

/* Minimal RFC 6455 WebSocket client.

   The Qt6 bundled with OBS does not ship Qt6WebSockets, so we implement the
   small subset needed by the camera notification socket (text frames,
   close frames, mask outbound payloads) on top of QTcpSocket / QtNetwork,
   which the bundled Qt does provide. Supports ws:// and wss:// (TLS via
   QSslSocket). This keeps the plugin free of a dependency OBS does not carry. */

#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QAbstractSocket>

class QTcpSocket;
class QSslError;
class QUrl;

namespace zc {

class ZcWsClient : public QObject {
	Q_OBJECT

public:
	explicit ZcWsClient(QObject *parent = nullptr);
	~ZcWsClient() override;

	/* Open a WebSocket connection. ExtraHeaders are appended to the HTTP
	   upgrade request (e.g. Authorization, cookies). */
	void open(const QUrl &url, const QByteArray &extraHeaders = {});
	void close();

	bool isOpen() const;

signals:
	void connected();
	void textMessageReceived(const QString &message);
	void disconnected();
	void error(const QString &errorString);

private slots:
	void onSocketConnected();
	void onSocketReadyRead();
	void onSocketDisconnected();
	void onSslErrors(const QList<QSslError> &errors);

private:
	void writeFrame(int opcode, const QByteArray &payload);
	void handleIncoming(const QByteArray &data);
	void sendHandshake();

	QTcpSocket *socket_ = nullptr;
	bool handshakeDone_ = false;
	QByteArray recvBuffer_;
	QByteArray handshake_;
	QByteArray handshakeKey_; /* Sec-WebSocket-Key, kept for Accept check */
	QByteArray extraHeaders_;
	bool isTls_ = false;
};

} // namespace zc