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

#include "zc_tls_socket.h"

#include <QTcpSocket>
#include <QTimer>
#include <QAbstractSocket>
#include <QNetworkProxy>
#include <QSettings>

#ifdef ZC_HAVE_MBEDTLS
#include "mbedtls/build_info.h"
#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/sha256.h"
#else
#include <QSslSocket>
#include <QSslError>
#include <QSslCertificate>
#include <QCryptographicHash>
#endif

namespace zc {

namespace {
/* Trust-on-first-use pin, shared by the mbedTLS and QSslSocket backends: the
   first certificate fingerprint seen for a host is recorded in QSettings and
   every later connection must match. Deleting "zcamera/tls/<host>" re-trusts
   the host. */
bool pinMatches(const QString &host, const QString &fingerprintHex)
{
	if (host.isEmpty())
		return true;
	QSettings settings;
	const QString key = QStringLiteral("zcamera/tls/%1").arg(host);
	const QString pinned = settings.value(key).toString();
	if (pinned.isEmpty()) {
		settings.setValue(key, fingerprintHex);
		return true;
	}
	return pinned.compare(fingerprintHex, Qt::CaseInsensitive) == 0;
}
} // namespace

#ifdef ZC_HAVE_MBEDTLS
struct ZcTlsSocket::Tls {
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	bool ssl_ready = false;
};
#endif

ZcTlsSocket::ZcTlsSocket(QObject *parent) : QObject(parent)
{
	timer_ = new QTimer(this);
	timer_->setSingleShot(true);
	connect(timer_, &QTimer::timeout, this,
		[this]() { fail(QStringLiteral("connection timed out")); });
}

ZcTlsSocket::~ZcTlsSocket()
{
	abort();
}

void ZcTlsSocket::connectToHost(const QString &host, quint16 port, bool tls,
				int timeoutMs)
{
	/* Tear down anything left over from a previous attempt first. */
	abort();
	is_tls_ = tls;
	host_ = host;
	ready_ = false;
	reported_ = false;
	driving_ = false;
	flushing_ = false;
	rx_.clear();
	tx_pending_.clear();

#ifdef ZC_HAVE_MBEDTLS
	tcp_ = new QTcpSocket(this);
#else
	if (tls) {
		QSslSocket *ssl = new QSslSocket(this);
		/* Cameras ship a self-signed certificate for www.z-cam.com while
		   addressed by IP literal, so the chain can never verify: accept it
		   (VerifyNone) and pin the fingerprint instead, in the encrypted
		   handler below, exactly like the mbedTLS backend. */
		ssl->setPeerVerifyMode(QSslSocket::VerifyNone);
		connect(ssl, &QSslSocket::sslErrors, ssl,
			[ssl]() { ssl->ignoreSslErrors(); });
		connect(ssl, &QSslSocket::encrypted, this, [this, ssl]() {
			if (ready_)
				return;
			/* Trust on first use: a TLS peer must present a certificate and
			   it must match the one recorded for this host (see
			   pinMatches). */
			const QSslCertificate cert = ssl->peerCertificate();
			if (cert.isNull()) {
				fail(QStringLiteral(
				    "certificate error: camera presented no certificate"));
				return;
			}
			const QString fp = QString::fromLatin1(
			    cert.digest(QCryptographicHash::Sha256).toHex());
			if (!pinMatches(host_, fp)) {
				fail(QStringLiteral("certificate error: camera "
						    "certificate changed since first pairing"));
				return;
			}
			ready_ = true;
			stopTimer();
			emit connected();
		});
		tcp_ = ssl;
	} else {
		tcp_ = new QTcpSocket(this);
	}
#endif

	/* A camera is a LAN device: it must never be reached through a proxy. Qt
	   applies QNetworkProxy::applicationProxy() (which an environment or an
	   OBS-wide configuration can set) to QTcpSocket, which at best routes the
	   connection nowhere useful and at worst fails outright with "The proxy
	   type is invalid for this operation". */
	tcp_->setProxy(QNetworkProxy::NoProxy);
	tcp_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
	connect(tcp_, &QTcpSocket::connected, this, &ZcTlsSocket::onTcpConnected);
	connect(tcp_, &QTcpSocket::readyRead, this, &ZcTlsSocket::onTcpReadyRead);
	connect(tcp_, &QTcpSocket::disconnected, this,
		&ZcTlsSocket::onTcpDisconnected);
	connect(tcp_, &QTcpSocket::errorOccurred, this, &ZcTlsSocket::onTcpError);
	/* A pending mbedTLS write may have stopped on WANT_WRITE; retry once the
	   socket has drained. */
	connect(tcp_, &QTcpSocket::bytesWritten, this,
		[this](qint64) { driveTls(); });

	if (timeoutMs > 0)
		timer_->start(timeoutMs);
	tcp_->connectToHost(host, port);
}

bool ZcTlsSocket::isConnected() const
{
	return tcp_ && ready_ &&
	       tcp_->state() == QAbstractSocket::ConnectedState;
}

QHostAddress ZcTlsSocket::peerAddress() const
{
	return tcp_ ? tcp_->peerAddress() : QHostAddress();
}

void ZcTlsSocket::write(const QByteArray &data)
{
	if (data.isEmpty())
		return;
	if (is_tls_) {
#ifdef ZC_HAVE_MBEDTLS
		/* Everything goes through the record layer in order; queued while the
		   handshake or a previous record is still in flight. */
		tx_pending_ += data;
		flushTls();
		return;
#endif
	}
	if (tcp_)
		tcp_->write(data);
}

QByteArray ZcTlsSocket::readAll()
{
	QByteArray out = rx_;
	rx_.clear();
	return out;
}

void ZcTlsSocket::disconnectFromHost()
{
#ifdef ZC_HAVE_MBEDTLS
	if (tls_ && tls_->ssl_ready) {
		/* Best-effort close_notify; a camera that has already gone does not
		   need it, and the socket is closed either way. */
		int ret = mbedtls_ssl_close_notify(&tls_->ssl);
		while (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			ret = mbedtls_ssl_close_notify(&tls_->ssl);
	}
#endif
	if (tcp_)
		tcp_->disconnectFromHost();
}

void ZcTlsSocket::abort()
{
	stopTimer();
	if (tcp_) {
		/* Detach first: abort() would otherwise re-enter onTcpDisconnected and
		   report a drop for a teardown we initiated. */
		tcp_->disconnect(this);
		tcp_->abort();
		tcp_->deleteLater();
		tcp_ = nullptr;
	}
	releaseTls();
	ready_ = false;
	/* fail() can tear the channel down from inside flushTls(); clear the guards
	   so a later connectToHost() on this object is not left wedged. */
	driving_ = false;
	flushing_ = false;
	rx_.clear();
}

void ZcTlsSocket::onTcpConnected()
{
	if (!is_tls_) {
		ready_ = true;
		stopTimer();
		emit connected();
		return;
	}
	startTls();
}

void ZcTlsSocket::onTcpReadyRead()
{
#ifdef ZC_HAVE_MBEDTLS
	if (is_tls_) {
		driveTls();
		return;
	}
#endif
	if (!tcp_)
		return;
	const QByteArray chunk = tcp_->readAll();
	if (!chunk.isEmpty()) {
		rx_ += chunk;
		emit readyRead();
	}
}

void ZcTlsSocket::onTcpDisconnected()
{
	stopTimer();
	ready_ = false;
	emit disconnected();
}

void ZcTlsSocket::onTcpError()
{
	if (!tcp_ || reported_)
		return;
	fail(tcp_->errorString());
}

void ZcTlsSocket::startTls()
{
#ifdef ZC_HAVE_MBEDTLS
	Tls *t = new Tls;
	mbedtls_ssl_init(&t->ssl);
	mbedtls_ssl_config_init(&t->conf);
	mbedtls_entropy_init(&t->entropy);
	mbedtls_ctr_drbg_init(&t->drbg);
	tls_ = t;

	static const char pers[] = "obs-zcamera";
	int ret = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func,
					&t->entropy,
					(const unsigned char *)pers,
					sizeof(pers) - 1);
	if (ret != 0) {
		failTls("TLS random init failed", ret);
		return;
	}

	ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
					  MBEDTLS_SSL_TRANSPORT_STREAM,
					  MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) {
		failTls("TLS config failed", ret);
		return;
	}

	/* The camera serves a self-signed certificate for www.z-cam.com while it is
	   addressed by IP literal, so verification could never succeed. Host trust
	   is enforced by the caller's connected-camera allow-list instead. */
	mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);

	ret = mbedtls_ssl_setup(&t->ssl, &t->conf);
	if (ret != 0) {
		failTls("TLS setup failed", ret);
		return;
	}
	t->ssl_ready = true;

	/* IP literal: no SNI. */
	mbedtls_ssl_set_hostname(&t->ssl, nullptr);
	mbedtls_ssl_set_bio(&t->ssl, this, bioSend, bioRecv, nullptr);
	driveTls();
#else
	if (QSslSocket *ssl = qobject_cast<QSslSocket *>(tcp_))
		ssl->startClientEncryption();
#endif
}

void ZcTlsSocket::driveTls()
{
#ifdef ZC_HAVE_MBEDTLS
	if (driving_ || !tls_ || !tls_->ssl_ready)
		return;
	driving_ = true;

	bool became_ready = false;
	if (!ready_) {
		int ret = mbedtls_ssl_handshake(&tls_->ssl);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			/* Resumed from readyRead()/bytesWritten(). */
			driving_ = false;
			return;
		}
		if (ret != 0) {
			driving_ = false;
			failTls("TLS handshake failed", ret);
			return;
		}
		ready_ = true;
		became_ready = true;
		/* The camera's certificate is self-signed, so a chain check can never
		   succeed; pin its fingerprint instead (trust on first use) so a later
		   MITM on the LAN cannot silently relay the Digest challenge. */
		if (!pinPeerCertificate()) {
			driving_ = false;
			fail(QStringLiteral("certificate error: camera certificate "
					    "changed since first pairing"));
			return;
		}
	}

	for (;;) {
		unsigned char buf[4096];
		int ret = mbedtls_ssl_read(&tls_->ssl, buf, sizeof(buf));
		if (ret > 0) {
			rx_ += QByteArray((const char *)buf, ret);
			continue;
		}
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
		    ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0)
			break;
		driving_ = false;
		failTls("TLS read failed", ret);
		return;
	}

	flushTls();
	driving_ = false;

	/* Emitted with the guard released: a handler may write, reconnect or
	   tear the channel down. */
	if (became_ready) {
		stopTimer();
		emit connected();
	}
	if (!rx_.isEmpty())
		emit readyRead();
#endif
}

void ZcTlsSocket::flushTls()
{
#ifdef ZC_HAVE_MBEDTLS
	if (!tls_ || !tls_->ssl_ready || !ready_ || tx_pending_.isEmpty())
		return;
	/* mbedtls_ssl_write() hands the record to bioSend(), which writes to the
	   QTcpSocket; that can emit bytesWritten synchronously, re-entering
	   driveTls()/flushTls() while the write is still in flight. A second
	   concurrent mbedtls_ssl_write() corrupts the record layer and comes back
	   as MBEDTLS_ERR_SSL_INTERNAL_ERROR, so only one writer at a time. */
	if (flushing_)
		return;
	flushing_ = true;
	while (!tx_pending_.isEmpty()) {
		int ret = mbedtls_ssl_write(
			&tls_->ssl,
			(const unsigned char *)tx_pending_.constData(),
			(size_t)tx_pending_.size());
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			break; /* resumed by readyRead()/bytesWritten() */
		if (ret <= 0) {
			flushing_ = false;
			failTls("TLS write failed", ret);
			return;
		}
		tx_pending_.remove(0, ret);
	}
	flushing_ = false;
#endif
}

void ZcTlsSocket::fail(const QString &message)
{
	if (reported_)
		return;
	reported_ = true;
	abort();
	emit errorOccurred(message);
}

void ZcTlsSocket::failTls(const char *what, int code)
{
#ifdef ZC_HAVE_MBEDTLS
	char buf[128] = {0};
	mbedtls_strerror(code, buf, sizeof(buf));
	fail(QStringLiteral("%1: -0x%2 %3")
		     .arg(QLatin1String(what))
		     .arg(-code, 0, 16)
		     .arg(QLatin1String(buf)));
#else
	Q_UNUSED(code);
	fail(QLatin1String(what));
#endif
}

void ZcTlsSocket::stopTimer()
{
	if (timer_)
		timer_->stop();
}

void ZcTlsSocket::releaseTls()
{
#ifdef ZC_HAVE_MBEDTLS
	if (!tls_)
		return;
	mbedtls_ssl_free(&tls_->ssl);
	mbedtls_ssl_config_free(&tls_->conf);
	mbedtls_ctr_drbg_free(&tls_->drbg);
	mbedtls_entropy_free(&tls_->entropy);
	delete tls_;
	tls_ = nullptr;
#endif
}

#ifdef ZC_HAVE_MBEDTLS
int ZcTlsSocket::bioSend(void *ctx, const unsigned char *buf, size_t len)
{
	ZcTlsSocket *self = static_cast<ZcTlsSocket *>(ctx);
	if (!self->tcp_ || self->tcp_->state() != QAbstractSocket::ConnectedState)
		return MBEDTLS_ERR_NET_CONN_RESET;
	const qint64 n = self->tcp_->write((const char *)buf, (qint64)len);
	if (n < 0)
		return MBEDTLS_ERR_NET_SEND_FAILED;
	/* Do NOT flush() here: Qt emits bytesWritten synchronously from flush(),
	   which would re-enter driveTls()/flushTls() with an mbedtls_ssl_write()
	   still in flight. Data is written as soon as the event loop spins. */
	return (int)n;
}

int ZcTlsSocket::bioRecv(void *ctx, unsigned char *buf, size_t len)
{
	ZcTlsSocket *self = static_cast<ZcTlsSocket *>(ctx);
	if (!self->tcp_)
		return MBEDTLS_ERR_NET_CONN_RESET;
	if (self->tcp_->bytesAvailable() == 0) {
		/* Non-blocking: report "come back later" and let readyRead() resume.
		   This is what keeps the channel off a worker thread. */
		if (self->tcp_->state() != QAbstractSocket::ConnectedState)
			return MBEDTLS_ERR_NET_CONN_RESET;
		return MBEDTLS_ERR_SSL_WANT_READ;
	}
	const qint64 n = self->tcp_->read((char *)buf, (qint64)len);
	if (n < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	if (n == 0)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return (int)n;
}

/* Trust on first use. The camera presents a self-signed certificate for
   www.z-cam.com while addressed by IP literal, so the chain can never verify;
   MBEDTLS_SSL_VERIFY_NONE stays, but the certificate's SHA-256 fingerprint is
   recorded per host on first connection and every later connection must match.
   A changed certificate (a plausible LAN MITM) aborts the channel. Pins live in
   QSettings under "zcamera/tls/<host>"; deleting that key re-trusts the host. */
bool ZcTlsSocket::pinPeerCertificate()
{
	if (!tls_ || host_.isEmpty())
		return true;
	const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&tls_->ssl);
	if (!crt || !crt->raw.p || crt->raw.len == 0)
		return false; /* no peer certificate: a TLS peer must present one */
	unsigned char digest[32];
	if (mbedtls_sha256(crt->raw.p, crt->raw.len, digest, 0) != 0)
		return true;
	const QString fp = QString::fromLatin1(
	    QByteArray(reinterpret_cast<const char *>(digest), sizeof(digest))
		.toHex());
	return pinMatches(host_, fp);
}
#endif /* ZC_HAVE_MBEDTLS */

} // namespace zc