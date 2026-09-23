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

#include "zc_ws_client.h"

#include <QTcpSocket>
#include <QSslSocket>
#include <QSslError>
#include <QUrl>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QList>

namespace zc {

namespace {
/* Case-insensitive lookup of a header value inside a raw HTTP header block. */
QByteArray headerValue(const QByteArray &header, const QByteArray &name)
{
	const QList<QByteArray> lines = header.split('\n');
	for (const QByteArray &line : lines) {
		int colon = line.indexOf(':');
		if (colon <= 0)
			continue;
		if (line.left(colon).trimmed().compare(name,
						      Qt::CaseInsensitive) == 0)
			return line.mid(colon + 1).trimmed();
	}
	return QByteArray();
}
} // namespace

ZcWsClient::ZcWsClient(QObject *parent) : QObject(parent) {}

ZcWsClient::~ZcWsClient()
{
	if (socket_)
		socket_->deleteLater();
}

void ZcWsClient::open(const QUrl &url, const QByteArray &extraHeaders)
{
	extraHeaders_ = extraHeaders;
	handshakeDone_ = false;
	recvBuffer_.clear();
	handshake_.clear();

	/* Reconnect path: dispose of the previous socket before creating a new
	   one so we never leak sockets across opens. Detach its signals FIRST:
	   abort() emits disconnected(), which would otherwise be handled as if
	   the NEW connection had dropped — clobbering handshakeDone_ and
	   scheduling a spurious reconnect. */
	if (socket_) {
		socket_->disconnect(this);
		socket_->abort();
		socket_->deleteLater();
		socket_ = nullptr;
	}
	isTls_ = url.scheme() == "wss";
	if (isTls_) {
		QSslSocket *ssl = new QSslSocket(this);
		socket_ = ssl;
		/* Allow the camera's self-signed certificate (host-scoped trust is
		   enforced by the caller via the connected camera allow-list). */
		ssl->setPeerVerifyMode(QSslSocket::VerifyNone);
		connect(ssl, &QSslSocket::sslErrors, this, &ZcWsClient::onSslErrors);
	} else {
		socket_ = new QTcpSocket(this);
	}

	connect(socket_, &QTcpSocket::connected, this, &ZcWsClient::onSocketConnected);
	connect(socket_, &QTcpSocket::readyRead, this, &ZcWsClient::onSocketReadyRead);
	connect(socket_, &QTcpSocket::disconnected, this,
		&ZcWsClient::onSocketDisconnected);

	socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
	socket_->connectToHost(url.host(), url.port());
}

void ZcWsClient::close()
{
	if (socket_ && socket_->state() != QAbstractSocket::UnconnectedState) {
		/* Send a close frame (0x8) then close. */
		writeFrame(0x8, QByteArray());
		socket_->disconnectFromHost();
	}
}

bool ZcWsClient::isOpen() const
{
	return socket_ && handshakeDone_ &&
	       socket_->state() == QAbstractSocket::ConnectedState;
}

void ZcWsClient::onSocketConnected()
{
	if (isTls_) {
		QSslSocket *ssl = qobject_cast<QSslSocket *>(socket_);
		if (ssl)
			ssl->startClientEncryption();
		return;
	}
	sendHandshake();
}

void ZcWsClient::sendHandshake()
{
	/* Build the RFC 6455 upgrade request. */
	const QByteArray key = QCryptographicHash::hash(
				       QByteArray::number(
					   QRandomGenerator::global()
					       ->generate()),
				       QCryptographicHash::Sha1)
				       .toBase64()
				       .left(16);
	handshakeKey_ = key;

	QByteArray req;
	req += "GET / HTTP/1.1\r\n";
	req += "Host: " + socket_->peerAddress().toString().toUtf8() + "\r\n";
	req += "Upgrade: websocket\r\n";
	req += "Connection: Upgrade\r\n";
	req += "Sec-WebSocket-Key: " + key + "\r\n";
	req += "Sec-WebSocket-Version: 13\r\n";
	if (!extraHeaders_.isEmpty()) {
		/* Accept both a bare header line and a CRLF-terminated block, and
		   never emit the terminating blank line twice (a stray CRLF would
		   be read by the peer as frame data after the upgrade). */
		QByteArray extra = extraHeaders_;
		while (extra.endsWith('\n') || extra.endsWith('\r'))
			extra.chop(1);
		if (!extra.isEmpty())
			req += extra + "\r\n";
	}
	req += "\r\n";
	socket_->write(req);
	socket_->flush();
}

void ZcWsClient::onSocketReadyRead()
{
	QByteArray chunk = socket_->readAll();
	if (!handshakeDone_) {
		handshake_ += chunk;
		int idx = handshake_.indexOf("\r\n\r\n");
		if (idx < 0) {
			/* Guard against a server that never completes headers. */
			if (handshake_.size() > 16384)
				emit error("WebSocket handshake too large");
			return;
		}
		QByteArray header = handshake_.left(idx);
		/* Verify the 101 Switching Protocols response and Sec-WebSocket-Accept. */
		if (!header.startsWith("HTTP/1.1 101") && !header.startsWith("HTTP/1.0 101")) {
			emit error("WebSocket handshake failed: " + header.left(120));
			socket_->close();
			return;
		}
		const QByteArray expected = QCryptographicHash::hash(
			handshakeKey_ +
				QByteArrayLiteral(
					"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
			QCryptographicHash::Sha1).toBase64();
		const QByteArray accept =
			headerValue(header, QByteArrayLiteral("Sec-WebSocket-Accept"));
		if (accept.isEmpty() || accept != expected) {
			emit error("WebSocket handshake failed: bad "
				   "Sec-WebSocket-Accept");
			socket_->close();
			return;
		}
		handshakeDone_ = true;
		/* Only the bytes AFTER the header are frames; anything already read
		   past the header lives in handshake_.mid(idx + 4). */
		QByteArray leftover = handshake_.mid(idx + 4);
		handshake_.clear();
		if (!leftover.isEmpty())
			handleIncoming(leftover);
		emit connected();
		return;
	}
	handleIncoming(chunk);
}

void ZcWsClient::onSocketDisconnected()
{
	handshakeDone_ = false;
	emit disconnected();
}

void ZcWsClient::onSslErrors(const QList<QSslError> &)
{
	/* Self-signed camera cert allowed (host trust enforced by caller). */
	QSslSocket *ssl = qobject_cast<QSslSocket *>(socket_);
	if (ssl)
		ssl->ignoreSslErrors();
}

void ZcWsClient::handleIncoming(const QByteArray &data)
{
	recvBuffer_ += data;
	/* Parse frames. */
	while (recvBuffer_.size() >= 2) {
		unsigned char b0 = (unsigned char)recvBuffer_[0];
		unsigned char b1 = (unsigned char)recvBuffer_[1];
		int opcode = b0 & 0x0F;
		bool fin = (b0 & 0x80) != 0;
		bool masked = (b1 & 0x80) != 0;
		quint64 len = b1 & 0x7F;
		int headerLen = 2;
		if (len == 126) {
			if (recvBuffer_.size() < 4)
				return;
			len = ((unsigned char)recvBuffer_[2] << 8) |
			      (unsigned char)recvBuffer_[3];
			headerLen = 4;
		} else if (len == 127) {
			if (recvBuffer_.size() < 10)
				return;
			len = 0;
			for (int i = 0; i < 8; ++i)
				len = (len << 8) | (unsigned char)recvBuffer_[2 + i];
			headerLen = 10;
		}
		int maskLen = masked ? 4 : 0;
		if (recvBuffer_.size() < headerLen + maskLen + (int)len)
			return;

		QByteArray payload = recvBuffer_.mid(headerLen + maskLen, (int)len);
		if (masked) {
			QByteArray mask = recvBuffer_.mid(headerLen, 4);
			for (int i = 0; i < payload.size(); ++i)
				payload[i] = payload[i] ^ mask[i % 4];
		}
		recvBuffer_.remove(0, headerLen + maskLen + (int)len);

		if (opcode == 0x1) { /* text */
			if (fin)
				emit textMessageReceived(QString::fromUtf8(payload));
			else
				emit error("Fragmented text frames unsupported");
		} else if (opcode == 0x8) { /* close */
			socket_->disconnectFromHost();
			return;
		} else if (opcode == 0x9) { /* ping -> pong */
			writeFrame(0xA, payload);
		}
		/* 0x0 continuation and 0x2 binary are not used by the camera
		   notification socket; ignored. */
	}
}

void ZcWsClient::writeFrame(int opcode, const QByteArray &payload)
{
	if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState)
		return;
	QByteArray frame;
	frame += (char)(0x80 | (opcode & 0x0F)); /* FIN + opcode */
	int len = payload.size();
	if (len < 126) {
		frame += (char)(0x80 | len); /* masked */
	} else if (len <= 0xFFFF) {
		frame += (char)(0x80 | 126);
		frame += (char)((len >> 8) & 0xFF);
		frame += (char)(len & 0xFF);
	} else {
		frame += (char)(0x80 | 127);
		for (int i = 7; i >= 0; --i)
			frame += (char)(((quint64)len >> (8 * i)) & 0xFF);
	}
	/* Mask with a random 4-byte key. */
	QByteArray mask;
	mask.reserve(4);
	for (int i = 0; i < 4; ++i)
		mask += (char)(QRandomGenerator::global()->bounded(256));
	frame += mask;
	for (int i = 0; i < len; ++i)
		frame += (char)(payload[i] ^ mask[i % 4]);
	socket_->write(frame);
	socket_->flush();
}

} // namespace zc