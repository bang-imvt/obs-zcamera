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

/* Settings engine, ported from ZCamGuiOpen settings-engine.ts.

   Reads use /ctrl/getbatch?catalog=<name> for groups, /ctrl/get?k=<key> for
   per-key reads, and /ctrl/set?<key>=<value> for writes. It maintains a
   value store and, after a write, re-reads the dependent groups per the
   catalog dependency map. */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QHash>
#include <functional>

namespace zc {

class ZcHttpTransport;

struct ZcSettingValue {
	QString key;
	QString value;
	int type = 0;        /* 1 choice, 2 range, 3 text */
	bool readOnly = false;
	bool supported = true;
	QStringList choices;
	double min = 0, max = 0, step = 0;
};

class ZcSettingsEngine : public QObject {
	Q_OBJECT

public:
	explicit ZcSettingsEngine(QObject *parent = nullptr);

	/* The camera client injects its transport before first use. */
	void setTransport(ZcHttpTransport *transport) { transport_ = transport; }

	/* Read a whole catalog (group) and invoke cb(true) when done. */
	void readGroup(const QString &catalog, std::function<void(bool)> cb);

	/* Read one key via /ctrl/get. */
	void readKey(const QString &key, std::function<void(const ZcSettingValue &)> cb);

	/* Write one key, then re-read dependent groups. */
	void writeKey(const QString &key, const QString &value);

	/* Access to the cached values. */
	QString value(const QString &key) const;
	QJsonObject snapshot() const;

	/* Cached metadata for one key (type/opts/min/max/ro), from the last
	   getbatch or per-key read. False when the key was never read, which is
	   how the settings panel tells "not loaded yet" from "unsupported". */
	bool definition(const QString &key, ZcSettingValue *out) const;

	/* The keys the camera returned for a catalog, in its own order. This is
	   the list the settings panel renders: the camera, not the ported schema,
	   decides which settings exist (it reports keys the schema does not know
	   and rejects whole catalogs it does not have). Empty until read. */
	QStringList groupKeys(const QString &catalog) const;

signals:
	void valueChanged(const QString &key, const QString &value);
	void groupRefreshed(const QString &catalog);
	void writeFinished(const QString &key, bool ok);

private:
	void applyBatch(const QString &catalog, const QJsonObject &doc);
	void applyDependencies(const QString &key);
	/* Old-firmware fallback: read every schema key of a catalog individually
	   via /ctrl/get when /ctrl/getbatch is not provided (getbatch returns a
	   non-JSON body or an empty cfgs). */
	void readGroupByKeys(const QString &catalog,
			     std::function<void(bool)> cb);

	QJsonObject values_;
	QHash<QString, ZcSettingValue> defs_;
	QHash<QString, QStringList> groupKeys_;
	ZcHttpTransport *transport_ = nullptr;
};

} // namespace zc