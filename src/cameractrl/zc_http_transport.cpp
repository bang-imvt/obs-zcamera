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

#include "zc_tls_socket.h"

#include <QEventLoop>
#include <QTimer>
#include <QUrl>
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

/* Case-insensitive lookup of a header value in a raw response header block. */
QByteArray headerValue(const QByteArray &block, const char *name)
{
	const QByteArray wanted(name);
	int pos = 0;
	while (pos < block.size()) {
		int eol = block.indexOf("\r\n", pos);
		if (eol < 0)
			eol = block.size();
		const QByteArray line = block.mid(pos, eol - pos);
		const int colon = line.indexOf(':');
		if (colon > 0 &&
		    line.left(colon).trimmed().compare(wanted,
						       Qt::CaseInsensitive) == 0)
			return line.mid(colon + 1).trimmed();
		pos = eol + 2;
	}
	return QByteArray();
}

/* "HTTP/1.1 200 Ok" -> 200 */
int statusOf(const QByteArray &statusLine)
{
	const int sp = statusLine.indexOf(' ');
	if (sp < 0)
		return 0;
	return statusLine.mid(sp + 1, 3).toInt();
}

/* Decode a chunked body. Returns false on a malformed stream, sets complete
   once the terminating 0-length chunk has been seen. */
bool decodeChunked(QByteArray &in, QByteArray &out, bool &complete)
{
	complete = false;
	for (;;) {
		const int eol = in.indexOf("\r\n");
		if (eol < 0)
			return true; /* chunk header still incomplete */
		bool ok = false;
		const int size = in.left(eol).trimmed().toInt(&ok, 16);
		if (!ok)
			return false;
		if (size == 0) {
			complete = true;
			return true;
		}
		if (in.size() < eol + 2 + size + 2)
			return true; /* chunk data still incomplete */
		out += in.mid(eol + 2, size);
		in.remove(0, eol + 2 + size + 2);
	}
}

/* One HTTP request on its own connection. Asynchronous: the Qt event loop
   drives the socket, so nothing here blocks the UI thread. Deletes itself
   after reporting. */
class HttpExchange : public QObject {
public:
	using Done = std::function<void(int, const QByteArray &,
					const QByteArray &, const QString &, bool)>;

	HttpExchange(const QString &host, quint16 port, bool tls,
		     const QByteArray &request, int timeoutMs, Done done,
		     QObject *parent)
		: QObject(parent), request_(request), done_(std::move(done))
	{
		sock_ = new ZcTlsSocket(this);
		connect(sock_, &ZcTlsSocket::connected, this,
			[this]() { sock_->write(request_); });
		connect(sock_, &ZcTlsSocket::readyRead, this, [this]() {
			raw_ += sock_->readAll();
			evaluate();
		});
		connect(sock_, &ZcTlsSocket::disconnected, this, [this]() {
			if (finished_)
				return;
			evaluate();
			if (finished_)
				return;
			if (!headersDone_)
				finish(0, QByteArray(), QByteArray(),
				       QStringLiteral(
					       "connection closed before a response "
					       "was received"),
				       false);
			else if (closeDelimited_)
				/* Connection: close — EOF delimits the body. */
				finish(status_, headers_, bodyRaw_, QString(),
				       false);
		});
		connect(sock_, &ZcTlsSocket::errorOccurred, this,
			[this](const QString &e) {
				finish(0, QByteArray(), QByteArray(), e, false);
			});

		if (timeoutMs > 0) {
			tt_ = new QTimer(this);
			tt_->setSingleShot(true);
			connect(tt_, &QTimer::timeout, this, [this]() {
				finish(0, QByteArray(), QByteArray(),
				       QStringLiteral("request timed out"),
				       true);
			});
			tt_->start(timeoutMs);
		}
		sock_->connectToHost(host, port, tls, timeoutMs);
	}

private:
	void evaluate()
	{
		if (finished_)
			return;

		if (!headersDone_) {
			const int idx = raw_.indexOf("\r\n\r\n");
			if (idx < 0) {
				if (raw_.size() > 65536)
					finish(0, QByteArray(), QByteArray(),
					       QStringLiteral(
						       "response headers too "
						       "large"),
					       false);
				return;
			}
			headers_ = raw_.left(idx);
			bodyRaw_ = raw_.mid(idx + 4);
			headersDone_ = true;
			status_ = statusOf(headers_);

			const QByteArray te =
				headerValue(headers_, "Transfer-Encoding");
			if (te.contains("chunked")) {
				chunked_ = true;
			} else {
				const QByteArray cl =
					headerValue(headers_, "Content-Length");
				if (!cl.isEmpty()) {
					contentLength_ = cl.toInt();
					if (contentLength_ < 0)
						contentLength_ = 0;
				} else if (status_ / 100 == 1 || status_ == 204 ||
					   status_ == 304) {
					contentLength_ = 0; /* no body by spec */
				} else {
					closeDelimited_ = true;
				}
			}
		}

		if (chunked_) {
			QByteArray in = bodyRaw_;
			QByteArray out;
			bool complete = false;
			if (!decodeChunked(in, out, complete)) {
				finish(status_, headers_, QByteArray(),
				       QStringLiteral("malformed chunked body"),
				       false);
			} else if (complete) {
				finish(status_, headers_, out, QString(), false);
			}
			return;
		}
		if (closeDelimited_)
			return; /* completed on EOF */
		if (bodyRaw_.size() >= contentLength_)
			finish(status_, headers_, bodyRaw_.left(contentLength_),
			       QString(), false);
	}

	void finish(int status, const QByteArray &headers, const QByteArray &body,
		    const QString &error, bool timedOut)
	{
		if (finished_)
			return;
		finished_ = true;
		if (tt_)
			tt_->stop();
		if (sock_)
			sock_->disconnectFromHost();
		Done d = std::move(done_);
		/* Post the delete before handing control to the caller: the callback
		   may tear the transport (and therefore this object) down. */
		deleteLater();
		if (d)
			d(status, headers, body, error, timedOut);
	}

	ZcTlsSocket *sock_ = nullptr;
	QTimer *tt_ = nullptr;
	QByteArray request_;
	QByteArray raw_;
	QByteArray headers_;
	QByteArray bodyRaw_;
	Done done_;
	int status_ = 0;
	int contentLength_ = 0;
	bool headersDone_ = false;
	bool chunked_ = false;
	bool closeDelimited_ = false;
	bool finished_ = false;
};
} // namespace

ZcHttpTransport::ZcHttpTransport(QObject *parent) : QObject(parent) {}

ZcHttpTransport::~ZcHttpTransport() = default;

bool ZcHttpTransport::negotiateOrigin(const QString &host, int port,
				      int timeoutMs)
{
	host_ = host;
	tls_unavailable_ = false;

	/* Order matches ZCamGuiOpen HTTPS_PORTS = [443, 80]: http:80,
	   https:443, then https:80 (a non-default port replaces the first). */
	struct Candidate {
		bool tls;
		int port;
	};
	QVector<Candidate> candidates;
	candidates.push_back({false, port > 0 ? port : 80});
	candidates.push_back({true, 443});
	candidates.push_back({true, 80});

	for (const auto &cand : candidates) {
		bool curTls = cand.tls;
		int curPort = cand.port;
		QString path = QStringLiteral("/info");

		/* Cameras with HTTPS enabled commonly answer the initial HTTP probe
		   with 307 -> https:443. Follow it so the 307 is not mistaken for a
		   valid origin and the locked origin is the HTTPS one. */
		for (int hop = 0; hop < 3; ++hop) {
			tls_ = curTls;
			port_ = curPort;

			RawHttp rsp;
			const bool answered = requestSync("GET", path, QByteArray(),
							  rsp, timeoutMs);
			emit requestLogged(
				QStringLiteral("%1://%2:%3%4  %5  %6")
					.arg(curTls ? "https" : "http", host_)
					.arg(curPort)
					.arg(path, QString::number(rsp.status),
					     rsp.error));
			qWarning().noquote()
				<< "[obs-zcamera] origin probe"
				<< (curTls ? "https" : "http") << "://" << host_
				<< ":" << curPort << "code" << rsp.status << "err"
				<< rsp.error << "path" << path;

			if (!answered) {
				/* Transport failure. A TLS handshake that cannot even
				   start means the host has no usable TLS backend. */
				if (curTls && rsp.error.contains("TLS"))
					tls_unavailable_ = true;
				break; /* next candidate */
			}

			const bool challenged = (rsp.status == 401 ||
						 rsp.status == 403);
			if (challenged) {
				/* Capture the challenge while it is available so the
				   first real request is authenticated without an extra
				   round trip. */
				auth_required_ = true;
				parseChallenge(rsp.headers);
			}
			captureSession(rsp.headers);

			/* Any answer proves this is the camera's origin, and a 401 is
			   one too: a camera with authentication enabled challenges
			   /info rather than serving it. Treating that 401 as "not this
			   origin, try the next candidate" left base_url_ empty, so
			   connect() failed outright and such a camera could not be
			   controlled at all — the credential prompt is only reached
			   once an origin is locked. */
			if ((rsp.status >= 200 && rsp.status < 300) || challenged) {
				/* Lock this origin. Keep the scheme/port that actually
				   answered, so an HTTP probe that was redirected to HTTPS
				   keeps HTTPS and the notifier chooses wss. */
				base_url_ = (curTls ? QStringLiteral("https://")
						    : QStringLiteral("http://")) +
					    host_ + ':' + QString::number(curPort);
				qInfo().noquote()
					<< "[obs-zcamera] origin chosen:" << base_url_
					<< "(code" << rsp.status << ")";
				return true;
			}

			if (rsp.status >= 300 && rsp.status < 400) {
				const QByteArray loc = headerValue(rsp.headers,
								  "Location");
				QUrl url(QString::fromLatin1(loc));
				if (!url.isValid() || url.host().isEmpty() ||
				    url.host() != host_)
					break; /* redirect off-camera: not our origin */
				curTls = url.scheme() != QLatin1String("http");
				curPort = url.port(curTls ? 443 : 80);
				path = url.path();
				if (!url.query().isEmpty())
					path += '?' + url.query();
				continue; /* follow within the camera */
			}
			break; /* other status: try the next candidate */
		}
	}
	return false;
}

bool ZcHttpTransport::probeAuthRequired(int timeoutMs)
{
	if (base_url_.isEmpty())
		return auth_required_;

	/* A control endpoint (not /info, which is public on many builds).
	   /ctrl/getbatch answers 401/403 when the camera needs a login. */
	RawHttp rsp;
	requestSync("GET", QStringLiteral("/ctrl/getbatch"), QByteArray(), rsp,
		    timeoutMs);
	qWarning().noquote() << "[obs-zcamera] auth probe" << base_url_
			     << "/ctrl/getbatch code" << rsp.status;
	if (rsp.status == 401 || rsp.status == 403) {
		auth_required_ = true;
		parseChallenge(rsp.headers);
	}
	return auth_required_;
}

bool ZcHttpTransport::login(int timeoutMs)
{
	if (base_url_.isEmpty() || creds_.username.isEmpty() ||
	    creds_.password.isEmpty())
		return false;

	/* Step 1: ask /login/ for the Digest challenge. The P2/R1 firmware answers
	   401 with a WWW-Authenticate-X Digest challenge here even though /ctrl/*
	   never offers one (the session must exist first). */
	{
		RawHttp ch;
		requestSync("GET", QStringLiteral("/login/"), QByteArray(), ch,
			    timeoutMs);
		/* No challenge: this build does not gate /login/ behind auth — either
		   the camera needs no login (already authed or public) or it answers
		   404 (not a web-UI camera). Treat "no Digest challenge" as success and
		   let the per-request path decide. */
		const bool challenged =
			(ch.status == 401 && parseChallenge(ch.headers));
		captureSession(ch.headers);
		qWarning().noquote() << "[obs-zcamera] login challenge"
				     << base_url_ + "/login/"
				     << "code" << ch.status << "challenged"
				     << challenged << "realm"
				     << QString::fromLatin1(realm_);
		if (!challenged)
			return true;
	}

	/* Step 2: POST /login/ with the computed Digest Authorization. */
	buildAuthorization("POST", "/login/");
	RawHttp lp;
	requestSync("POST", QStringLiteral("/login/"), QByteArray(), lp,
		    timeoutMs);
	captureSession(lp.headers);
	qWarning().noquote() << "[obs-zcamera] login post"
			     << base_url_ + "/login/" << "code" << lp.status
			     << "setcookie" << !cookie_.isEmpty() << "token"
			     << !x_auth_token_.isEmpty();

	/* 2xx (or any redirect) opens the session; the session cookie/token is now
	   on file for every /ctrl/* request. An UNAUTHORIZED with no cookie means a
	   wrong password — the dock keeps the form up. */
	const bool ok = (lp.status >= 200 && lp.status < 300);
	if (ok) {
		auth_required_ = false;
		auth_header_.clear(); /* session replaces per-request Digest */
	}
	return ok;
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
		doRequest("GET", path, QByteArray(), cb, timeoutMs, 1);
	});
}

void ZcHttpTransport::getQuery(const QString &path, const QUrlQuery &query,
			       ZcHttpCallback cb, int timeoutMs)
{
	enqueue([this, path, query, cb, timeoutMs]() {
		QString full = path;
		/* Encode exactly like the reference client so the Digest uri= value
		   matches what is actually requested. */
		const QString qs = query.query(QUrl::FullyEncoded);
		if (!qs.isEmpty())
			full += '?' + qs;
		doRequest("GET", full, QByteArray(), cb, timeoutMs, 1);
	});
}

void ZcHttpTransport::post(const QString &path, const QByteArray &body,
			   ZcHttpCallback cb, int timeoutMs)
{
	enqueue([this, path, body, cb, timeoutMs]() {
		doRequest("POST", path, body, cb, timeoutMs, 1);
	});
}

QString ZcHttpTransport::hostHeader() const
{
	const int def = tls_ ? 443 : 80;
	if (port_ == def)
		return host_;
	return host_ + ':' + QString::number(port_);
}

QByteArray ZcHttpTransport::buildRequest(const char *method,
					 const QString &pathWithQuery,
					 const QByteArray &body) const
{
	QByteArray req;
	req += method;
	req += ' ';
	req += pathWithQuery.toUtf8();
	req += " HTTP/1.1\r\n";
	req += "Host: " + hostHeader().toUtf8() + "\r\n";
	req += "User-Agent: obs-zcamera\r\n";
	req += "X-Requested-With: XMLHttpRequest\r\n";
	req += "Accept: */*\r\n";
	if (!auth_header_.isEmpty())
		req += "Authorization: " + auth_header_ + "\r\n";
	if (!cookie_.isEmpty())
		req += "Cookie: " + cookie_ + "\r\n";
	if (!x_auth_token_.isEmpty())
		req += "x-auth-token: " + x_auth_token_ + "\r\n";
	/*
	 * A POST always carries a Content-Length (0 when empty), matching what the
	 * previous Qt client sent. */
	if (qstrcmp(method, "POST") == 0) {
		req += "Content-Type: application/x-www-form-urlencoded\r\n";
		req += "Content-Length: " + QByteArray::number(body.size()) +
		       "\r\n";
	}
	/* One request per connection: the camera's httpd is single-threaded, so
	   there is nothing to gain from keeping it alive. */
	req += "Connection: close\r\n\r\n";
	if (!body.isEmpty())
		req += body;
	return req;
}

void ZcHttpTransport::dispatch(const char *method,
			       const QString &pathWithQuery,
			       const QByteArray &body, int timeoutMs,
			       ExchangeDone done)
{
	new HttpExchange(host_, (quint16)port_, tls_,
			 buildRequest(method, pathWithQuery, body), timeoutMs,
			 std::move(done), this);
}

bool ZcHttpTransport::requestSync(const char *method,
				  const QString &pathWithQuery,
				  const QByteArray &body, RawHttp &out,
				  int timeoutMs)
{
	QEventLoop loop;
	bool done = false;
	dispatch(method, pathWithQuery, body, timeoutMs,
		 [&](int status, const QByteArray &headers,
		     const QByteArray &rspBody, const QString &error,
		     bool timedOut) {
			 out.status = status;
			 out.headers = headers;
			 out.body = rspBody;
			 out.error = error;
			 out.timedOut = timedOut;
			 done = true;
			 loop.quit();
		 });
	/* A failure can be reported synchronously from dispatch() — an immediate
	   socket error is emitted right out of connectToHost(). quit() before
	   exec() is a no-op, so entering the loop then would block forever. */
	if (!done)
		loop.exec();
	return out.error.isEmpty();
}

void ZcHttpTransport::doRequest(const char *method,
				const QString &pathWithQuery,
				const QByteArray &body, ZcHttpCallback cb,
				int timeoutMs, int retries)
{
	const QByteArray methodBytes(method);

	/* Compute a fresh Digest response for this specific method/URI. */
	if (!nonce_.isEmpty() && !creds_.username.isEmpty())
		buildAuthorization(methodBytes, pathWithQuery.toUtf8());

	dispatch(
		method, pathWithQuery, body, timeoutMs,
		[this, methodBytes, pathWithQuery, body, cb, timeoutMs,
		 retries](int status, const QByteArray &headers,
			  const QByteArray &payload, const QString &error,
			  bool timedOut) {
			emit requestLogged(QStringLiteral("%1 %2  %3")
						   .arg(base_url_ + pathWithQuery,
							QString::number(status),
							error));
			qWarning().noquote()
				<< "[obs-zcamera] req" << base_url_ + pathWithQuery
				<< "code" << status << "err" << error;

			captureSession(headers);

			/* Fresh 401 challenge: transparently re-auth and retry the
			   SAME request once (the reference client does exactly one
			   transparent retry on a stale=true challenge). */
			if (status == 401 && retries > 0 &&
			    !creds_.username.isEmpty() &&
			    parseChallenge(headers)) {
				doRequest(methodBytes.constData(), pathWithQuery,
					  body, cb, timeoutMs, retries - 1);
				return;
			}

			ZcHttpResponse rsp;
			rsp.statusCode = status;
			rsp.body = payload;
			rsp.timedOut = timedOut;
			rsp.errorText = error;
			rsp.finalUrl = base_url_ + pathWithQuery;

			busy_ = false;
			if (cb)
				cb(rsp);
			pump();
		});
}

bool ZcHttpTransport::parseChallenge(const QByteArray &headers)
{
	/* The firmware advertises Digest via the non-standard
	   WWW-Authenticate-X header; accept the standard one as well. */
	QByteArray challenge = headerValue(headers, "WWW-Authenticate-X");
	if (challenge.isEmpty())
		challenge = headerValue(headers, "WWW-Authenticate");
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

void ZcHttpTransport::captureSession(const QByteArray &headers)
{
	/* The session cookie and x-auth-token are needed for the WS upgrade.
	   Values stay in memory only. */
	const QByteArray setCookie = headerValue(headers, "Set-Cookie");
	if (!setCookie.isEmpty()) {
		const int semi = setCookie.indexOf(';');
		QByteArray first = (semi < 0) ? setCookie : setCookie.left(semi);
		first = first.trimmed();
		if (!first.isEmpty())
			cookie_ = first;
	}
	const QByteArray token = headerValue(headers, "x-auth-token");
	if (!token.isEmpty())
		x_auth_token_ = token;
}

} // namespace zc