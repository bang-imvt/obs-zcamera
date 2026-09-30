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

   The HTTP engine is our own (src/cameractrl/zc_tls_socket.*) rather than
   Qt's: Qt's HTTP stack cannot complete a TLS handshake inside OBS (the Qt6
   shipped with OBS has no tls backend plugin), which made every HTTPS-only
   camera uncontrollable, and it also hides the non-standard challenge header.
   Requests stay asynchronous — the socket is a non-blocking QTcpSocket driven
   by the Qt event loop — so the UI thread is never blocked while a request is
   in flight. Credentials are kept in memory only. */

#pragma once

#include <QObject>
#include <QUrlQuery>
#include <QString>
#include <QByteArray>
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

	/* Some firmware (the P2/R1 family) does not accept Digest on each /ctrl/*
	   request: it answers 401 with no WWW-Authenticate at all until a session
	   has been opened. login() performs the web-UI login — GET /login/ for the
	   Digest challenge, then GET /login/ with the computed Authorization (the
	   camera's own login.min.js uses method:'GET'; older edgem4 sw 1.0.16
	   firmware rejects a POST here, so GET is the safe common denominator) —
	   and captures the session cookie / x-auth-token so subsequent requests are
	   authenticated. Both legs run on ONE connection, because some firmware
	   binds the nonce to the connection that issued the challenge, so a retry on
	   a fresh connection is answered 401 no matter how correct the digest is.
	   Returns true when a session is established. A camera that does not require
	   a login (no /login challenge) is a no-op success. */
	bool login(int timeoutMs = 8000);

	/* Probe one control endpoint to find out whether the camera demands a login.
	   /info is public on many firmware versions, so the origin probe cannot tell
	   (bug: the login prompt never appeared for such a camera). Returns true and
	   sets auth_required_ when the probe got a 401/403 challenge. */
	bool probeAuthRequired(int timeoutMs = 1500);

	/* Whether we believe authentication is required (got a 401 challenge). */
	bool authRequired() const { return auth_required_; }

	/* Base origin of the locked camera (e.g. http://192.168.1.2:80). */
	QString baseUrl() const { return base_url_; }
	QString host() const { return host_; }
	/* Port the locked origin was reached on — the notification socket must
	   reuse it for wss (the camera's TLS listener), not a fixed port. */
	int port() const { return port_; }

	/* True when the camera only answers over HTTPS and no TLS backend could be
	   initialised — the operator must switch the camera to HTTP. With the
	   built-in mbedTLS transport this stays false. */
	bool tlsUnavailable() const { return tls_unavailable_; }

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
	/* One parsed HTTP response, transport level. */
	struct RawHttp {
		int status = 0;
		QByteArray headers; /* status line + header block, no blank line */
		QByteArray body;
		QString error; /* non-empty on transport/TLS failure */
		bool timedOut = false;
	};

	using ExchangeDone = std::function<void(int status,
						const QByteArray &headers,
						const QByteArray &body,
						const QString &error,
						bool timedOut)>;

	void enqueue(std::function<void()> run);
	void pump();
	/* Start one request on a fresh connection and report it through `done`. */
	void dispatch(const char *method, const QString &pathWithQuery,
		      const QByteArray &body, int timeoutMs, ExchangeDone done);
	/* Event-loop-driven "synchronous" request for the probes, which the callers
	   already treat as blocking. */
	bool requestSync(const char *method, const QString &pathWithQuery,
			 const QByteArray &body, RawHttp &out, int timeoutMs);
	void doRequest(const char *method, const QString &pathWithQuery,
		       const QByteArray &body, ZcHttpCallback cb, int timeoutMs,
		       int retries);
	QByteArray buildRequest(const char *method,
				const QString &pathWithQuery,
				const QByteArray &body) const;
	/* One leg of the login handshake: GET /login/, optionally with the cached
	   Digest Authorization, over a keep-alive connection. */
	QByteArray buildLoginRequest(bool withAuth) const;
	/* Run the two-step Digest login on a SINGLE connection (the nonce of some
	   firmware is bound to the connection that issued the challenge). Sets
	   `challenged` when leg 1 offered a Digest challenge and `continued` when the
	   authenticated leg 2 was actually sent on that same socket; `out` is the
	   final response (leg 2 when continued, otherwise leg 1). */
	void loginSync(RawHttp &out, bool &challenged, bool &continued,
		       int timeoutMs);
	QString hostHeader() const;
	/* Parse a Digest challenge (WWW-Authenticate-X preferred) out of a raw
	   response header block and cache the request-independent parts. False when
	   no usable challenge is present. */
	bool parseChallenge(const QByteArray &headers);
	/* Build + cache the Authorization header for one request (nc counts up). */
	void buildAuthorization(const QByteArray &method, const QByteArray &uri);
	/* Remember Set-Cookie / x-auth-token for the WS upgrade. */
	void captureSession(const QByteArray &headers);

	QString host_;
	QString base_url_;
	int port_ = 80;
	bool tls_ = false;
	bool auth_required_ = false;
	bool tls_unavailable_ = false;
	ZcCredentials creds_;
	bool busy_ = false;
	std::queue<std::function<void()>> queue_;

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