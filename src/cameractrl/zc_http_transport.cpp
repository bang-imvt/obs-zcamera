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

#include "zc_http_transport.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QList>
#include <QVector>

namespace zc {

namespace {
/* 11-char lowercase-alphanumeric cnonce like the reference client. */
QString makeCnonce()
{
	static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	QString c;
	c.reserve(11);
	for (int i = 0; i < 11; ++i)
		c.append(alphabet[QRandomGenerator::global()->bounded(36)]);
	return c;
}

/* Quote a value per RFC 2617 (strip double quotes we add around it). */
QByteArray qval(const QByteArray &s)
{
	return '"' + s + '"';
}

QByteArray md5hex(const QByteArray &in)
{
	return QCryptographicHash::hash(in, QCryptographicHash::Md5).toHex();
}

QByteArray ha1(const QString &user, const QString &realm,
	       const QString &pass, const QByteArray &nonce, const QByteArray &cnonce,
	       bool md5sess)
{
	QByteArray a1 = (user.toUtf8() + ':' + realm.toUtf8() + ':' +
			 pass.toUtf8());
	QByteArray h = md5hex(a1);
	if (md5sess)
		h = md5hex(h + ':' + nonce + ':' + cnonce);
	return h;
}

QByteArray ha2(const QByteArray &method, const QByteArray &uri)
{
	return md5hex(method + ':' + uri);
}

/* Pull one parameter out of a WWW-Authenticate Digest challenge. Values may
   be quoted or bare (`realm="CAM", nonce=abc, qop="auth"`). */
QByteArray digestParam(const QByteArray &src, const QByteArray &key)
{
	int pos = 0;
	while (pos < src.size()) {
		int eq = src.indexOf('=', pos);
		if (eq < 0)
			break;
		QByteArray k = src.mid(pos, eq - pos).trimmed();
		if (k.startsWith(','))
			k = k.mid(1).trimmed();
		pos = eq + 1;
		QByteArray v;
		if (pos < src.size() && src[pos] == '"') {
			int end = src.indexOf('"', pos + 1);
			if (end < 0)
				break;
			v = src.mid(pos + 1, end - pos - 1);
			pos = end + 1;
		} else {
			int end = src.indexOf(',', pos);
			if (end < 0)
				end = src.size();
			v = src.mid(pos, end - pos).trimmed();
			pos = end;
		}
		if (k.toLower() == key)
			return v;
	}
	return QByteArray();
}
} // namespace

ZcHttpTransport::ZcHttpTransport(QObject *parent) : QObject(parent) {}

ZcHttpTransport::~ZcHttpTransport() = default;

bool ZcHttpTransport::negotiateOrigin(const QString &host, int port,
				      int timeoutMs)
{
	host_ = host;

	/* Order matches ZCamGuiOpen HTTPS_PORTS = [443, 80]: http:80,
	   https:443, then https:80 (a non-default port replaces the first). */
	struct Candidate {
		QString scheme;
		int port;
	};
	QVector<Candidate> candidates;
	candidates.push_back({QStringLiteral("http"), port > 0 ? port : 80});
	candidates.push_back({QStringLiteral("https"), 443});
	candidates.push_back({QStringLiteral("https"), 80});

	for (const auto &cand : candidates) {
		QUrl url;
		url.setScheme(cand.scheme);
		url.setHost(host);
		url.setPort(cand.port);
		url.setPath(QStringLiteral("/info"));

		QNetworkRequest req(url);
		req.setTransferTimeout(timeoutMs);
		/* Cameras with HTTPS enabled commonly answer the initial HTTP probe with
		   307 -> https:443. Follow that redirect explicitly; otherwise the 307
		   itself can be mistaken for a valid origin. */
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
				 QNetworkRequest::NoLessSafeRedirectPolicy);
		req.setRawHeader("User-Agent", "obs-zcamera");
		req.setRawHeader("X-Requested-With", "XMLHttpRequest");
		req.setRawHeader("Referer",
				 url.toString(QUrl::RemovePath).toUtf8() +
				     "/www/html/login.html");

		QEventLoop loop;
		QNetworkReply *reply = mgr_.get(req);
		/* Cameras expose a web UI over https with a self-signed certificate;
		   accept it (the reference client does) or TLS handshakes to https:443
		   keep failing and the whole control surface reads empty. */
		connect(reply, &QNetworkReply::sslErrors, reply, [reply]() {
			reply->ignoreSslErrors();
		});
		connect(reply, &QNetworkReply::finished, &loop,
			&QEventLoop::quit);
		loop.exec();

		int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
				   .toInt();
		if (code == 401 || code == 403) {
			/* Capture the challenge while it is available so the
			   first real request is authenticated without an extra
			   round trip. */
			auth_required_ = true;
			parseChallenge(reply);
		}
		captureSession(reply);
		QString finalUrl = reply->url().toString();
		/* Surface the probe in the dock's request log: this runs before any
		   base URL exists, so without it a failed origin negotiation leaves
		   no trace of why (connect refused, TLS failure, timeout). */
		emit requestLogged(QString("%1 %2  %3").arg(
		    reply->request().url().toString(), QString::number(code),
		    reply->errorString()));
		reply->deleteLater();

		/* Any HTTP answer (even 401 auth-required) means the origin is
		   the right one. Ignore transport failures / redirects to other
		   hosts; follow redirects within the camera. */
		if (code >= 200 && code < 300) {
			/* Only accept the final non-redirect response. Use its URL after Qt
			   followed the camera's internal redirect, so an HTTP probe that ends
			   at HTTPS keeps the HTTPS origin and the notifier chooses wss. */
			QUrl final(finalUrl);
			base_url_ = final.scheme() + "://" + final.host() +
				    (final.port() > 0
					     ? ":" + QString::number(final.port())
					     : QString());
			return true;
		}
	}
	return false;
}

QByteArray ZcHttpTransport::upgradeHeaderBlock() const
{
	/* CRLF-separated header lines for the WebSocket upgrade (ZcWsClient
	   appends them verbatim and adds the terminating blank line). */
	QByteArray h;
	if (!auth_header_.isEmpty())
		h += "Authorization: " + auth_header_ + "\r\n";
	if (!cookie_.isEmpty())
		h += "Cookie: " + cookie_ + "\r\n";
	if (!x_auth_token_.isEmpty())
		h += "x-auth-token: " + x_auth_token_ + "\r\n";
	return h;
}

void ZcHttpTransport::setCredentials(const ZcCredentials &cred)
{
	creds_ = cred;
	/* A new user invalidates any cached Digest response. */
	auth_header_.clear();
	nonce_.clear();
	nc_ = 0;
}

void ZcHttpTransport::enqueue(std::function<void()> run)
{
	queue_.push(std::move(run));
	pump();
}

void ZcHttpTransport::pump()
{
	if (busy_ || queue_.empty())
		return;
	busy_ = true;
	auto next = std::move(queue_.front());
	queue_.pop();
	next();
}

void ZcHttpTransport::get(const QString &path, ZcHttpCallback cb, int timeoutMs)
{
	enqueue([this, path, cb, timeoutMs]() {
		doRequest(QUrl(base_url_ + path), QByteArray(), cb, timeoutMs, 1);
	});
}

void ZcHttpTransport::getQuery(const QString &path, const QUrlQuery &query,
			       ZcHttpCallback cb, int timeoutMs)
{
	enqueue([this, path, query, cb, timeoutMs]() {
		QUrl url(base_url_ + path);
		url.setQuery(query);
		doRequest(url, QByteArray(), cb, timeoutMs, 1);
	});
}

void ZcHttpTransport::post(const QString &path, const QByteArray &body,
			   ZcHttpCallback cb, int timeoutMs)
{
	enqueue([this, path, body, cb, timeoutMs]() {
		doRequest(QUrl(base_url_ + path), body, cb, timeoutMs, 1);
	});
}

QNetworkReply *ZcHttpTransport::send(const QUrl &url, const QByteArray &body,
				     int timeoutMs)
{
	const bool isPost = !body.isEmpty();

	/* Compute a fresh Digest response for this specific method/URI. */
	if (!nonce_.isEmpty() && !creds_.username.isEmpty()) {
		QByteArray uri = url.path(QUrl::FullyEncoded).toUtf8();
		const QString query = url.query(QUrl::FullyEncoded);
		if (!query.isEmpty())
			uri += '?' + query.toUtf8();
		buildAuthorization(isPost ? "POST" : "GET", uri);
	}

	QNetworkRequest req(url);
	req.setTransferTimeout(timeoutMs);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
			 QNetworkRequest::NoLessSafeRedirectPolicy);
	req.setRawHeader("User-Agent", "obs-zcamera");
	req.setRawHeader("X-Requested-With", "XMLHttpRequest");
	if (!auth_header_.isEmpty())
		req.setRawHeader("Authorization", auth_header_);
	if (!cookie_.isEmpty())
		req.setRawHeader("Cookie", cookie_);
	if (!x_auth_token_.isEmpty())
		req.setRawHeader("x-auth-token", x_auth_token_);

	if (isPost) {
		req.setHeader(QNetworkRequest::ContentTypeHeader,
			      "application/x-www-form-urlencoded");
		QNetworkReply *r = mgr_.post(req, body);
		connect(r, &QNetworkReply::sslErrors, r, [r]() {
			r->ignoreSslErrors();
		});
		return r;
	}
	QNetworkReply *r = mgr_.get(req);
	connect(r, &QNetworkReply::sslErrors, r, [r]() { r->ignoreSslErrors(); });
	return r;
}

void ZcHttpTransport::doRequest(const QUrl &url, const QByteArray &body,
				ZcHttpCallback cb, int timeoutMs, int retries)
{
	QNetworkReply *reply = send(url, body, timeoutMs);
	if (!reply) {
		busy_ = false;
		pump();
		return;
	}
	/* Process only once the reply has actually finished: reading it right
	   after send() would always see an empty body and status 0. */
	connect(reply, &QNetworkReply::finished, this,
		[this, reply, cb, url, body, timeoutMs, retries]() {
			onFinished(reply, cb, retries, url, body, timeoutMs);
		});
}

void ZcHttpTransport::onFinished(QNetworkReply *reply, ZcHttpCallback cb,
				 int retries, const QUrl &url,
				 const QByteArray &body, int timeoutMs)
{
	QByteArray payload = reply->readAll();
	int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
			   .toInt();
	QNetworkReply::NetworkError err = reply->error();
	bool timeout = (err == QNetworkReply::TimeoutError);
	QString finalUrl = reply->url().toString();

	emit requestLogged(QString("%1 %2  %3").arg(
	    reply->request().url().toString(), QString::number(code),
	    reply->errorString()));

	captureSession(reply);

	/* Fresh 401 challenge: transparently re-auth and retry the SAME request
	   once (the reference client does exactly one transparent retry on a
	   stale=true challenge). */
	if (code == 401 && retries > 0 && !creds_.username.isEmpty() &&
	    parseChallenge(reply)) {
		reply->deleteLater();
		doRequest(url, body, cb, timeoutMs, retries - 1);
		return;
	}

	reply->deleteLater();

	ZcHttpResponse rsp;
	rsp.statusCode = code;
	rsp.body = payload;
	rsp.timedOut = timeout;
	if (err != QNetworkReply::NoError && code == 0)
		rsp.errorText = reply->errorString();
	rsp.finalUrl = finalUrl;

	busy_ = false;
	if (cb)
		cb(rsp);
	pump();
}

bool ZcHttpTransport::parseChallenge(const QNetworkReply *reply)
{
	/* The firmware advertises Digest via the non-standard
	   WWW-Authenticate-X header; accept the standard one as well. */
	QByteArray challenge = reply->rawHeader("WWW-Authenticate-X");
	if (challenge.isEmpty())
		challenge = reply->rawHeader("WWW-Authenticate");
	challenge = challenge.trimmed();
	if (challenge.size() < 6 ||
	    challenge.left(6).toLower() != QByteArrayLiteral("digest"))
		return false;
	challenge = challenge.mid(6).trimmed();

	QByteArray realm = digestParam(challenge, "realm");
	QByteArray nonce = digestParam(challenge, "nonce");
	if (nonce.isEmpty())
		return false;

	realm_ = realm;
	opaque_ = digestParam(challenge, "opaque");
	qop_ = digestParam(challenge, "qop");
	md5_sess_ = (digestParam(challenge, "algorithm")
			     .compare(QByteArrayLiteral("MD5-sess"),
				      Qt::CaseInsensitive) == 0);
	/* Only a *new* nonce invalidates the cached digest state. A camera (or a
	   connection retry) can repeat the same challenge: resetting the nonce
	   count there would reuse nc with an unchanged nonce (RFC 2617 requires nc
	   to increase per nonce, so a camera tracking replays can reject it), and
	   clearing the cached Authorization would drop the header the WebSocket
	   upgrade reuses — leaving the upgrade unauthenticated. */
	if (nonce != nonce_) {
		nonce_ = nonce;
		nc_ = 0;
		cnonce_ = makeCnonce().toLatin1();
		auth_header_.clear();
	}
	return true;
}

void ZcHttpTransport::buildAuthorization(const QByteArray &method,
					 const QByteArray &uri)
{
	if (nonce_.isEmpty() || creds_.username.isEmpty())
		return;

	QByteArray qop;
	if (!qop_.isEmpty()) {
		/* The camera offers "auth"; auth-int would need a body hash. */
		const QList<QByteArray> offered = qop_.split(',');
		for (const QByteArray &q : offered) {
			if (q.trimmed().compare(QByteArrayLiteral("auth"),
						Qt::CaseInsensitive) == 0) {
				qop = QByteArrayLiteral("auth");
				break;
			}
		}
		if (qop.isEmpty())
			qop = offered.first().trimmed();
	}
	auth_qop_ = qop;

	nc_++;
	const QByteArray nc = QByteArray::number(nc_).rightJustified(8, '0');
	const QByteArray a1 = ha1(creds_.username,
				  QString::fromLatin1(realm_), creds_.password,
				  nonce_, cnonce_, md5_sess_);
	const QByteArray a2 = ha2(method, uri);

	QByteArray response;
	if (!qop.isEmpty())
		response = md5hex(a1 + ':' + nonce_ + ':' + nc + ':' + cnonce_ +
				  ':' + qop + ':' + a2);
	else
		response = md5hex(a1 + ':' + nonce_ + ':' + a2);

	QByteArray h = "Digest username=" + qval(creds_.username.toUtf8()) +
		       ", realm=" + qval(realm_) + ", nonce=" + qval(nonce_) +
		       ", uri=" + qval(uri) + ", response=" + qval(response);
	if (!qop.isEmpty())
		h += ", qop=" + qop + ", nc=" + nc + ", cnonce=" + qval(cnonce_);
	if (!opaque_.isEmpty())
		h += ", opaque=" + qval(opaque_);
	if (md5_sess_)
		h += ", algorithm=MD5-sess";
	auth_header_ = h;
}

void ZcHttpTransport::captureSession(const QNetworkReply *reply)
{
	/* The session cookie and x-auth-token are needed for the WS upgrade.
	   Values stay in memory only. */
	const QByteArray setCookie = reply->rawHeader("Set-Cookie");
	if (!setCookie.isEmpty()) {
		int semi = setCookie.indexOf(';');
		QByteArray first =
			(semi < 0) ? setCookie : setCookie.left(semi);
		first = first.trimmed();
		if (!first.isEmpty())
			cookie_ = first;
	}
	const QByteArray token = reply->rawHeader("x-auth-token");
	if (!token.isEmpty())
		x_auth_token_ = token;
}

} // namespace zc
