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

#include "zc_credential_store.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QString>

#include <obs-module.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace zc {
namespace {

/* One JSON object per camera, keyed by host:
     { "cameras": { "192.168.10.121": { "user": "admin", "secret": "…" } } }
   `secret` is the base64 of the platform-sealed UTF-8 password, empty when the
   build has no keystore to seal it with. */
const char *kStoreFile = "credentials.json";

QHash<QString, ZcCredentials> &sessionStore()
{
	/* Function-local static: initialised on first use and alive until the
	   process exits, so neither the dock nor a source can free it out from
	   under the other. */
	static QHash<QString, ZcCredentials> store;
	return store;
}

#ifdef _WIN32
/* SEAL/UNSEAL -------------------------------------------------------------

   DPAPI with the current-user scope: the blob is bound to this Windows account
   on this machine, and CRYPTPROTECT_UI_FORBIDDEN keeps a prompt from popping
   out of OBS if the profile ever needs one. */

QByteArray sealPassword(const QString &password)
{
	if (password.isEmpty())
		return QByteArray();

	const QByteArray plain = password.toUtf8();
	DATA_BLOB in;
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
	in.cbData = static_cast<DWORD>(plain.size());

	DATA_BLOB out{};
	if (!CryptProtectData(&in, L"obs-zcamera camera password", nullptr,
			      nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return QByteArray();

	const QByteArray blob(reinterpret_cast<const char *>(out.pbData),
			      static_cast<int>(out.cbData));
	LocalFree(out.pbData);
	return blob.toBase64();
}

QString unsealPassword(const QByteArray &sealed)
{
	if (sealed.isEmpty())
		return QString();

	const QByteArray raw = QByteArray::fromBase64(sealed);
	DATA_BLOB in;
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(raw.constData()));
	in.cbData = static_cast<DWORD>(raw.size());

	DATA_BLOB out{};
	if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
				CRYPTPROTECT_UI_FORBIDDEN, &out))
		return QString();

	const QString password =
		QString::fromUtf8(reinterpret_cast<const char *>(out.pbData),
				  static_cast<int>(out.cbData));
	LocalFree(out.pbData);
	return password;
}
#else
/* No platform keystore on this build: do not write a secret to a file at all.
   The user name is still persisted, so the login form comes up filled in and
   only the password has to be typed. */
QByteArray sealPassword(const QString &)
{
	return QByteArray();
}

QString unsealPassword(const QByteArray &)
{
	return QString();
}
#endif

QString storePath()
{
	char *raw = obs_module_config_path(kStoreFile);
	if (!raw)
		return QString();
	const QString path = QString::fromUtf8(raw);
	bfree(raw);
	/* obs_module_get_config_path() only joins the strings; the directory is
	   created by the frontend for modules it loads, but a fresh install can
	   still race that, so make sure it exists before writing. */
	QDir().mkpath(QFileInfo(path).absolutePath());
	return path;
}

/* The file is read once per process; a write refreshes the cache, so a second
   read never has to go to disk again. */
QHash<QString, ZcCredentials> &diskStore()
{
	static QHash<QString, ZcCredentials> store;
	static bool loaded = false;
	if (loaded)
		return store;
	loaded = true;

	const QString path = storePath();
	if (path.isEmpty())
		return store;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return store;

	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return store;

	const QJsonObject cameras = doc.object().value("cameras").toObject();
	for (auto it = cameras.begin(); it != cameras.end(); ++it) {
		const QJsonObject o = it.value().toObject();
		ZcCredentials creds;
		creds.username = o.value("user").toString();
		creds.password = unsealPassword(o.value("secret").toString().toUtf8());
		if (!creds.username.isEmpty())
			store.insert(it.key(), creds);
	}
	return store;
}

void writeDiskStore()
{
	const QString path = storePath();
	if (path.isEmpty())
		return;

	QJsonObject cameras;
	for (auto it = diskStore().begin(); it != diskStore().end(); ++it) {
		QJsonObject o;
		o.insert("user", it.value().username);
		o.insert("secret",
			 QString::fromLatin1(sealPassword(it.value().password)));
		cameras.insert(it.key(), o);
	}
	QJsonObject root;
	root.insert("cameras", cameras);

	/* QSaveFile: a half-written file would lose every camera's login. */
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return;
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if (!f.commit())
		return;
	/* The file holds a sealed secret; keep it to the current user. The parent
	   directory is already user-scoped, this is belt and braces. */
	QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
}

void persist(const QString &host, const ZcCredentials &creds)
{
	QHash<QString, ZcCredentials> &disk = diskStore();
	disk.insert(host, creds);
	writeDiskStore();
}

} // namespace

void rememberCredentials(const QString &host, const ZcCredentials &creds)
{
	sessionStore().insert(host, creds);
}

void saveCredentials(const QString &host, const ZcCredentials &creds)
{
	if (host.isEmpty() || creds.username.isEmpty())
		return;
	sessionStore().insert(host, creds);
	/* Not a change: every camera switch that lands on a working login runs
	   through here, and rewriting the file each time is pointless I/O. */
	const auto it = diskStore().constFind(host);
	if (it != diskStore().constEnd() && it.value().username == creds.username &&
	    it.value().password == creds.password)
		return;
	persist(host, creds);
}

void forgetCredentials(const QString &host)
{
	sessionStore().remove(host);
	QHash<QString, ZcCredentials> &disk = diskStore();
	if (disk.remove(host) > 0)
		writeDiskStore();
}

ZcCredentials lookupCredentials(const QString &host)
{
	const auto it = sessionStore().constFind(host);
	if (it != sessionStore().constEnd())
		return it.value();
	return diskStore().value(host);
}

} // namespace zc
