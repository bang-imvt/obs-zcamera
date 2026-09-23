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

/* Camera HTTP transport, ported from ZCamGuiOpen http-transport.ts.

   - Origin negotiation: try http:<port> -> https:443 -> https:80, lock the
     first that answers, follow redirects, remember the final origin.
   - Digest auth: the ZCAM firmware advertises the challenge via the
     non-standard WWW-Authenticate-X header (the standard WWW-Authenticate is
     hidden from WebKit JS). We parse it directly, compute MD5/MD5-sess with
     qop/nc/cnonce, cache the Authorization header, and retry the same request
     once on a fresh 401 (stale=true).
   - Serialization: the camera's embedded HTTP server is single-threaded and
     drops concurrent requests, so all traffic to one camera runs one request
     at a time (a per-camera FIFO queue).

   The Qt HTTP stack does not let us see the -X challenge header, so we do the
   Digest dance with our own parser. Credentials are kept in memory only. */

#pragma once

#include <QObject>
#include <QUrl>
#include <QUrlQuery>
#include <QString>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <functional>
#include <queue>

namespace zc {

struct ZcHttpResponse {
	int statusCode = 0;       /* HTTP status (0 if transport error)    */
	QByteArray body;
	QString errorText;        /* transport error description          */
	QString finalUrl;
	bool timedOut = false;
};

using ZcHttpCallback = std::function<void(const ZcHttpResponse &)>;

struct ZcCredentials {
	QString username;
	QString password;
};

class ZcHttpTransport : public QObject {
	Q_OBJECT

public:
	explicit ZcHttpTransport(QObject *parent = nullptr);
	~ZcHttpTransport() override;

	/* Connect to a camera at host:port (port 80 default). Probes and locks
	   the working origin (http/https). Returns true if any origin answered.

	   NOTE: this runs a nested event loop (the reference client does the same)
	   and therefore blocks its caller for up to 3 x timeoutMs when the camera
	   is unreachable. The timeout is deliberately short: the camera is on the
	   LAN, so a reachable origin answers well inside 1.5 s. */
	bool negotiateOrigin(const QString &host, int port = 80,
			     int timeoutMs = 1500);

	/* Set credentials for Digest auth (session only). */
	void setCredentials(const ZcCredentials &cred);

	/* Whether we believe authentication is required (got a 401 challenge). */
	bool authRequired() const { return auth_required_; }

	/* Base origin of the locked camera (e.g. http://192.168.1.2:80). */
	QString baseUrl() const { return base_url_; }
	QString host() const { return host_; }

	/* Session/auth state handed to the WebSocket upgrade (design §4.3): the
	   cached Digest Authorization header, the session cookie and
	   x-auth-token. Memory only. */
	QByteArray authHeader() const { return auth_header_; }
	QByteArray cookieHeader() const { return cookie_; }
	QByteArray xAuthToken() const { return x_auth_token_; }
	/* Those three as ready-to-send CRLF-separated upgrade header lines. */
	QByteArray upgradeHeaderBlock() const;

	/* One-shot GET/POST. Enqueued behind any in-flight request. */
	void get(const QString &path, ZcHttpCallback cb,
		 int timeoutMs = 20000);
	void getQuery(const QString &path, const QUrlQuery &query,
		      ZcHttpCallback cb, int timeoutMs = 20000);
	void post(const QString &path, const QByteArray &body,
		  ZcHttpCallback cb, int timeoutMs = 20000);

signals:
	void requestLogged(const QString &line); /* for the debug panel */

private:
	void enqueue(std::function<void()> run);
	void pump();
	QNetworkReply *send(const QUrl &url, const QByteArray &body,
			    int timeoutMs);
	/* Issue a request and process it when it actually finishes (a reply
	   handled before `finished` has no status code and no body). */
	void doRequest(const QUrl &url, const QByteArray &body, ZcHttpCallback cb,
		       int timeoutMs, int retries);
	void onFinished(QNetworkReply *reply, ZcHttpCallback cb, int retries,
			const QUrl &url, const QByteArray &body, int timeoutMs);
	/* Parse a Digest challenge (WWW-Authenticate-X preferred) and cache the
	   request-independent parts. False when no usable challenge is present. */
	bool parseChallenge(const QNetworkReply *reply);
	/* Build + cache the Authorization header for one request (nc counts up). */
	void buildAuthorization(const QByteArray &method, const QByteArray &uri);
	/* Remember Set-Cookie / x-auth-token for the WS upgrade. */
	void captureSession(const QNetworkReply *reply);

	QString host_;
	QString base_url_;
	bool auth_required_ = false;
	ZcCredentials creds_;
	bool busy_ = false;
	std::queue<std::function<void()>> queue_;
	QNetworkAccessManager mgr_; /* per-transport, owned by this instance */

	/* Digest state (memory only; never logged, never written to disk). */
	QByteArray realm_;
	QByteArray nonce_;
	QByteArray opaque_;
	QByteArray qop_;         /* offered qop list                  */
	QByteArray auth_qop_;    /* qop actually used (auth)          */
	QByteArray auth_header_; /* cached Authorization header value */
	QByteArray cnonce_;
	int nc_ = 0;
	bool md5_sess_ = false;

	/* Session state reused by the WebSocket upgrade. */
	QByteArray cookie_;
	QByteArray x_auth_token_;
};

} // namespace zc
