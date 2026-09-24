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

#include "zc_settings_engine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QJsonValue>
#include <QThread>

#include "zc_settings_schema.h"
#include "zc_http_transport.h"

namespace zc {

/* The camera answers numeric settings (type 2 ranges) with a JSON number and
   string settings (type 1/3) with a JSON string. QJsonValue::toString() is
   empty for a number, so a slider would get an empty initial value; normalise
   both shapes to the textual form the panel and the cache use. */
static QString jsonScalar(const QJsonValue &v)
{
	if (v.isString())
		return v.toString();
	if (v.isDouble())
		return QString::number(v.toDouble());
	if (v.isBool())
		return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
	return QString();
}

ZcSettingsEngine::ZcSettingsEngine(QObject *parent) : QObject(parent) {}

void ZcSettingsEngine::readGroup(const QString &catalog,
				 std::function<void(bool)> cb)
{
	ZcHttpTransport *tr = transport_;
	if (!tr) {
		if (cb)
			cb(false);
		return;
	}

	QUrlQuery query;
	query.addQueryItem("catalog", catalog);
	tr->getQuery("/ctrl/getbatch", query,
		      [this, catalog, cb](const ZcHttpResponse &rsp) {
			      QJsonParseError err{};
			      QJsonDocument doc = QJsonDocument::fromJson(
				      rsp.body, &err);
			      qInfo().noquote() << "[obs-zcamera] getbatch" << catalog
						 << "http" << rsp.statusCode << "parse"
						 << err.error << "bytes" << rsp.body.size();
			      if (err.error == QJsonParseError::NoError && doc.isObject())
				      qInfo().noquote() << "[obs-zcamera] getbatch" << catalog
							 << "cfgs"
							 << doc.object().value("cfgs").toArray().size();
			      /* Old firmware has no /ctrl/getbatch at all: it answers
				 404 (or any non-JSON body). Fall back to per-key reads so
				 such a camera still shows its settings. */
			      if (err.error != QJsonParseError::NoError ||
				  !doc.isObject()) {
				      readGroupByKeys(catalog, cb);
				      return;
			      }
			      const QJsonArray cfgs =
				      doc.object().value("cfgs").toArray();
			      if (cfgs.isEmpty()) {
				      /* getbatch answered but reported no settings for this
					 catalog (either the camera does not have it or it
					 only supports per-key /ctrl/get). Fall back to the
					 per-key path; if the camera really lacks the
					 catalog those reads return code -1 and the group
					 stays empty. */
				      readGroupByKeys(catalog, cb);
				      return;
			      }
			      applyBatch(catalog, doc.object());
			      if (cb)
				      cb(true);
		      });
}

/* Old-firmware path: read each schema key of the catalog through /ctrl/get
   (the camera reads serially, so the replies arrive one after another). Keys
   the camera rejects (code -1) are left out; the ones it returns populate the
   value/definition store and the group key list, exactly as a successful
   getbatch would. */
void ZcSettingsEngine::readGroupByKeys(const QString &catalog,
				       std::function<void(bool)> cb)
{
	QStringList keys;
	for (const auto &cat : zc::catalogs()) {
		if (cat.id == catalog) {
			keys = cat.keys;
			break;
		}
	}
	if (keys.isEmpty()) {
		groupKeys_.insert(catalog, QStringList());
		emit groupRefreshed(catalog);
		if (cb)
			cb(false);
		return;
	}

	/* The replies are serialized by the transport, so both the pending count
	   and the accumulated keys are touched on one thread; shared_ptr keeps the
	   captured state alive past this function. */
	auto pending = std::make_shared<int>(keys.size());
	auto okKeys = std::make_shared<QStringList>();
	for (const QString &key : keys) {
		readKey(key, [this, catalog, pending, okKeys, cb](
				     const ZcSettingValue &v) {
			if (v.supported)
				okKeys->append(v.key);
			if (--*pending == 0) {
				groupKeys_.insert(catalog, *okKeys);
				emit groupRefreshed(catalog);
				if (cb)
					cb(true);
			}
		});
	}
}

void ZcSettingsEngine::readKey(const QString &key,
			       std::function<void(const ZcSettingValue &)> cb)
{
	ZcSettingValue out;
	out.key = key;
	ZcHttpTransport *tr = transport_;
	if (!tr) {
		if (cb)
			cb(out);
		return;
	}

	QUrlQuery query;
	query.addQueryItem("k", key);
	tr->getQuery("/ctrl/get", query,
			      [this, key, out, cb](const ZcHttpResponse &rsp) {
				      ZcSettingValue v = out;
				      QJsonParseError err{};
				      QJsonDocument doc = QJsonDocument::fromJson(
					      rsp.body, &err);
				      if (err.error == QJsonParseError::NoError &&
					  doc.isObject()) {
					      QJsonObject o = doc.object();
					      v.value = jsonScalar(o.value("value"));
					      v.type = o.value("type").toInt();
					      v.readOnly =
						      o.value("ro").toInt() == 1;
					      v.supported =
						      o.value("code").toInt() == 0;
					      QJsonArray opts =
						      o.value("opts").toArray();
					      for (const QJsonValue &ov : opts)
						      v.choices.append(
							      ov.toString());
					      v.min = o.value("min").toDouble();
					      v.max = o.value("max").toDouble();
					      v.step = o.value("step").toDouble();
				      }
				      values_.insert(key, v.value);
				      defs_.insert(key, v);
				      emit valueChanged(key, v.value);
				      if (cb)
					      cb(v);
			      });
}

void ZcSettingsEngine::writeKey(const QString &key, const QString &value)
{
	ZcHttpTransport *tr = transport_;
	if (!tr) {
		emit writeFinished(key, false);
		return;
	}

	QUrlQuery query;
	query.addQueryItem(key, value);
	tr->getQuery("/ctrl/set", query,
			      [this, key, value](const ZcHttpResponse &rsp) {
				      bool ok = (rsp.statusCode == 200);
				      if (ok) {
					      values_.insert(key, value);
					      emit valueChanged(key, value);
					      applyDependencies(key);
				      }
				      emit writeFinished(key, ok);
			      });
}

void ZcSettingsEngine::applyBatch(const QString &catalog,
				  const QJsonObject &doc)
{
	/* getbatch returns { cfgs: [ {code,key,type,ro,value,opts,...} ] }. A
	   catalog the camera does not have answers {"code":-1,...} with no cfgs
	   at all, which correctly leaves the group empty. */
	QJsonArray cfgs = doc.value("cfgs").toArray();
	QStringList keys;
	for (const QJsonValue &cv : cfgs) {
		QJsonObject o = cv.toObject();
		QString key = o.value("key").toString();
		if (key.isEmpty())
			continue;
		QString val = jsonScalar(o.value("value"));
		int type = o.value("type").toInt();
		bool ro = o.value("ro").toInt() == 1;
		int code = o.value("code").toInt();

		/* Keep the metadata as well as the value: the settings panel builds
		   its rows (combo / slider / line edit) from it, and code -1 is how
		   the camera reports a key it does not support. */
		ZcSettingValue def;
		def.key = key;
		def.value = val;
		def.type = type;
		def.readOnly = ro;
		def.supported = (code == 0);
		const QJsonArray opts = o.value("opts").toArray();
		for (const QJsonValue &ov : opts)
			def.choices.append(ov.toString());
		def.min = o.value("min").toDouble();
		def.max = o.value("max").toDouble();
		def.step = o.value("step").toDouble();
		defs_.insert(key, def);

		QString prev = values_.value(key).toString();
		values_.insert(key, val);
		keys.append(key);
		if (prev != val)
			emit valueChanged(key, val);
	}
	groupKeys_.insert(catalog, keys);
	emit groupRefreshed(catalog);
}

void ZcSettingsEngine::applyDependencies(const QString &key)
{
	for (const auto &rule : zc::dependencyRules()) {
		if (!rule.srcKeys.contains(key))
			continue;
		for (const QString &cat : rule.targetCatalogs)
			readGroup(cat, nullptr);
	}
}

QString ZcSettingsEngine::value(const QString &key) const
{
	return values_.value(key).toString();
}

QJsonObject ZcSettingsEngine::snapshot() const
{
	return values_;
}

bool ZcSettingsEngine::definition(const QString &key, ZcSettingValue *out) const
{
	auto it = defs_.constFind(key);
	if (it == defs_.constEnd())
		return false;
	if (out)
		*out = it.value();
	return true;
}

QStringList ZcSettingsEngine::groupKeys(const QString &catalog) const
{
	return groupKeys_.value(catalog);
}

} // namespace zc