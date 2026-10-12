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

/* Asynchronous TLS channel used by the camera HTTP and WebSocket transports.

   Why this exists: the Qt6 bundled with OBS ships QtNetwork but not its tls
   backend plugins, so QSslSocket / QNetworkAccessManager cannot complete a
   handshake ("TLS initialization failed"). OBS never deploys Qt's tls
   directory, so this cannot be fixed by configuration. Cameras with HTTPS
   enabled therefore could not be controlled at all.

   The channel is a plain QTcpSocket (non-blocking, driven by the Qt event
   loop) with the mbedTLS record layer on top: mbedTLS's own BIO is replaced by
   callbacks that read/write the QTcpSocket, so nothing blocks and no thread is
   added. `tcpEnabled=false` opens a plain TCP channel, so callers use one type
   for both ws:// and wss://.

   mbedTLS is statically linked from obs-deps (ZC_HAVE_MBEDTLS). Where obs-deps
   provides no mbedTLS, TLS falls back to Qt's own backend, i.e. the behaviour
   this plugin had before. */

#pragma once

#include <cstddef>

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QHostAddress>

class QTcpSocket;
class QTimer;

namespace zc {

class ZcTlsSocket : public QObject {
	Q_OBJECT

public:
	explicit ZcTlsSocket(QObject *parent = nullptr);
	~ZcTlsSocket() override;

	/* Resolve, connect and (when tls) complete the handshake. connected() is
	   emitted only once the channel is usable, i.e. after the handshake for
	   TLS — callers never see a half-open TLS channel. */
	void connectToHost(const QString &host, quint16 port, bool tls,
			   int timeoutMs = 15000);

	/* Reusable subset of QTcpSocket, so callers do not care which backend is
	   underneath. */
	bool isConnected() const;
	QHostAddress peerAddress() const;
	void write(const QByteArray &data);
	QByteArray readAll();
	void disconnectFromHost();
	void abort();

signals:
	void connected();
	void readyRead();
	void disconnected();
	void errorOccurred(const QString &message);

private:
	void onTcpConnected();
	void onTcpReadyRead();
	void onTcpDisconnected();
	void onTcpError();
	void startTls();
	void driveTls();
	void flushTls();
	void releaseTls();
	void fail(const QString &message);
	void failTls(const char *what, int code);
	void stopTimer();

#ifdef ZC_HAVE_MBEDTLS
	/* Trust-on-first-use pin check; false means the peer certificate changed. */
	bool pinPeerCertificate();
#endif

	/* mbedTLS BIO: mbedTLS writes/reads through the QTcpSocket, so the Qt event
	   loop drives everything and nothing blocks. */
	static int bioSend(void *ctx, const unsigned char *buf, size_t len);
	static int bioRecv(void *ctx, unsigned char *buf, size_t len);

	/* mbedTLS record layer, all state hidden from the header. */
	struct Tls;

	QTcpSocket *tcp_ = nullptr;
	QTimer *timer_ = nullptr;
	Tls *tls_ = nullptr;
	QString host_;          /* peer host, for the TOFU certificate pin */
	bool is_tls_ = false;
	bool ready_ = false;    /* handshake done / plain TCP connected */
	bool reported_ = false; /* an error was already reported */
	bool driving_ = false;  /* re-entrancy guard (BIO callbacks re-enter) */
	bool flushing_ = false; /* re-entrancy guard for the record layer write */
	QByteArray rx_;         /* decrypted application data */
	QByteArray tx_pending_; /* plaintext queued for the TLS layer */
};

} // namespace zc