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

#include "zc_settings_schema.h"

#include <QRegularExpression>

namespace zc {

static std::vector<CatalogDef> makeCatalogs()
{
	/* Mirrors ZCamGuiOpen settings-schema.ts groups + catalogs. */
	return {
	    {"exposure", {}, {"ev_choice", "flicker", "meter_mode", "iris",
			      "iso", "min_iso", "max_iso", "iso_ctrl", "shutter",
			      "max_shutter", "shutter_angle_ctrl", "sht_operation",
			      "eND", "lock_ae_in_rec", "ae_speed", "bl_comp"}},
	    {"wb", {}, {"wb", "wb_priority", "lock_awb_in_rec", "mwb", "tint",
			"mwb_r", "mwb_g", "mwb_b"}},
	    {"lens", {}, {"focus", "af_mode", "caf", "caf_sens", "af_area",
			  "live_caf", "mf_mag", "mf_recording", "af_speed",
			  "af_adjust_with_ptz", "zoom_mode", "lens_focus_pos",
			  "lens_zoom_pos", "fz_speed"}},
	    {"image", {}, {"lut", "sharpness", "noise_reduction", "luma_level",
			   "brightness", "contrast", "saturation", "hue"}},
	    {"record", {}, {"resolution", "project_fps", "vfr_ctrl", "movvfr",
			    "record_file_format", "rotation", "split_duration",
			    "rec_fps", "preroll", "video_tl_interval",
			    "record_meta", "camera_id", "reelname"}},
	    {"video", {}, {"video_encoder", "bitrate_level", "record_mode",
			   "rec_proxy_file", "crop_sensor",
			   "low_jello", "photo_q", "eis_on_off", "vid_rot"}},
	    /* No stream group: the stream encoders are not a getbatch catalog, and
	       their controls are not a group at all — they sit above the group list,
	       where they are visible without navigating to them (spec §2). */
	    {"audio", {}, {"primary_audio", "audio_channel",
			   "audio_phantom_power", "audio_level_display",
			   "ain_gain_type", "audio_input_level",
			   "audio_input_gain", "audio_noise_reduction",
			   "audio_in_l_gain", "audio_in_r_gain",
			   "audio_output_gain"}},
	    {"ptz", {}, {"pt_speedmode", "ptz_flip", "ptz_limit",
			 "ptz_preset_mode", "ptz_speed_mode",
			 "freeze_during_preset", "pt_pwr_pos", "pt_priv_mode",
			 "preset_adjust_speed_with_zoom",
			 "pt_speed_with_zoom_pos", "ptz_common_speed_unit",
			 "ptz_common_speed", "ptz_common_time"}},
	};
}

static std::vector<DependencyRule> makeDeps()
{
	/* Mirrors ZCamGuiOpen DEPENDENT_GROUPS. */
	return {
	    {{"focus"}, {"lens"}},
	    {{"caf", "caf_sens", "live_caf", "af_mode", "af_area", "af_speed",
	      "af_adjust_with_ptz", "mf_mag", "mf_recording"}, {"lens"}},
	    {{"iso", "iso_ctrl", "min_iso", "max_iso", "sht_operation",
	      "shutter_angle_ctrl", "shutter", "max_shutter", "ev_choice"},
	     {"exposure"}},
	    {{"lut"}, {"exposure"}},
	    {{"movvfr", "video_encoder", "record_mode"}, {"record", "video"}},
	    {{"bitrate_level"}, {"video"}},
	    {{"resolution", "project_fps"}, {"record"}},
	    {{"vfr_ctrl"}, {"record"}},
	    {{"audio_channel"}, {"audio"}},
	    {{"wb", "mwb", "tint"}, {"wb"}},
	};
}

std::vector<CatalogDef> &catalogs()
{
	static std::vector<CatalogDef> c = makeCatalogs();
	return c;
}

static const char *kPerKeys[] = {
    "mwb", "tint", "mwb_r", "mwb_g", "mwb_b", "brightness", "contrast",
    "saturation", "hue", "lens_focus_pos", "lens_zoom_pos", "fz_speed",
    "shutter", "max_shutter",
    "audio_in_l_gain", "audio_in_r_gain", "audio_output_gain", "lcd_backlight",
    "gl_shf_coarse", "gl_shf_fine", "video_tl_interval", "record_meta",
    "camera_id", "reelname", "ptz_common_speed", "ptz_common_time",
    "assitool_zera_th1", "assitool_zera_th2",
};

static const char *kBooleanKeys[] = {
    "lock_ae_in_rec", "lock_awb_in_rec", "mf_mag", "mf_recording",
    "af_adjust_with_ptz", "low_jello", "eis_on_off", "rec_proxy_file",
    "hdmi_osd", "use_edid", "tally_on", "color_bar_enable", "genlock",
    "visca_enable", "auto_off", "auto_standby", "wifi", "https_on",
    "union_ae", "union_awb", "ezlink_mode", "ezlink_trigger",
    "tc_count_up", "tc_drop_frame",
};

bool cameraSupportsPtz(const QJsonObject &info)
{
	const QJsonObject feature = info.value("feature").toObject();
	if (feature.value("lens_ctrl_ptz").toBool(false))
		return true;

	QString model = info.value("model").toString();
	if (model.isEmpty())
		model = info.value("cameraName").toString();
	return model.contains("ptz", Qt::CaseInsensitive);
}

/* Auto framing rides on the /info `feature.autoFraming` flag: it is a product
   capability of the tracking firmware, not of the pan/tilt head, so it has no
   model-name fallback (a fixed PTZ box may lack it, and a box without PTZ may
   have it). Absent flag reads false. */
bool cameraSupportsAutoFraming(const QJsonObject &info)
{
	const QJsonObject feature = info.value("feature").toObject();
	return feature.value("autoFraming").toBool(false);
}

/* One feature flag, whatever shape the firmware wrote it in. */
bool featureFlag(const QJsonObject &feature, const char *name)
{
	const QJsonValue v = feature.value(QLatin1String(name));
	if (v.isBool())
		return v.toBool();
	if (v.isDouble())
		return v.toDouble() != 0.0;
	if (v.isString()) {
		const QString s = v.toString().trimmed().toLower();
		return s == QLatin1String("1") || s == QLatin1String("true") ||
		       s == QLatin1String("on") || s == QLatin1String("yes");
	}
	return false;
}

ZcFeatures::ZcFeatures(const QJsonObject &info)
{
	map_ = info.value(QLatin1String("feature")).toObject();
	declared_ = !map_.isEmpty();
}

bool ZcFeatures::has(const char *name) const
{
	/* No map at all: the body never told us, so nothing is withdrawn. */
	if (!declared_)
		return true;
	return featureFlag(map_, name);
}

/* Catalog -> the one option the group is useless without. */
namespace {
struct CatalogGate {
	const char *id;
	const char *flag;
};

const CatalogGate kCatalogGates[] = {
	/* Pan/tilt configuration. The PTZ *pane* (presets, traces) is gated by
	   the same capability in the dock. */
	{"ptz", "lens_ctrl_ptz"},
};

/* Setting row -> the option behind it. Each entry mirrors the camera page's
   own visibility rule, quoted above it. `hiddenWhenOn` covers the negative
   capabilities: `noFocus` means the body has no focus motor, so it hides the
   focus family instead of revealing it. A key may appear more than once; it
   is shown only when every gate passes. */
struct SettingGate {
	const char *key;
	const char *flag;
	bool hiddenWhenOn;
};

const SettingGate kSettingGates[] = {
	/* $("#bl_comp_info").show() */
	{"bl_comp", "blComp", false},
	/* $("#pre_roll").show() */
	{"preroll", "preRoll", false},
	/* $("#exp_iso_ctrl").show() */
	{"iso_ctrl", "expIsoControl", false},
	/* $("#settings-wifi").show() — a P2-R1-18x reports wifiSupport:false */
	{"wifi", "wifiSupport", false},
	/* $('#visca').show() */
	{"visca_enable", "viscaSupport", false},
	/* $('#zoom_mode_support').show(): the optical/digital zoom switch exists
	   only on a body with digital zoom. */
	{"zoom_mode", "dzoomDigital", false},
	/* $('#audio_phantom_pow').show() */
	{"audio_phantom_power", "audioCodecXLR", false},
	/* $('#audio_line_in').show() */
	{"ain_gain_type", "aLineIn", false},
	{"audio_input_level", "aLineIn", false},
	{"audio_input_gain", "aLineIn", false},
	{"audio_in_l_gain", "aLineIn", false},
	{"audio_in_r_gain", "aLineIn", false},
	/* $('#eis').show() */
	{"eis_on_off", "eisSupport", false},
	/* $("#genlock_ctrl").show() */
	{"genlock", "genlockSupport", false},
	/* $('#adjust_with_ptz').show(), inside the lens_ctrl_ptz branch */
	{"af_adjust_with_ptz", "lens_ctrl_ptz", false},
	/* `mfOnly`: the body takes manual-focus lenses only, so the whole
	   autofocus family is meaningless there (the Avatar reports it; its lens
	   tab then has focus position but no AF rows). Manual focus itself stays.
	   The MF-assist rows go with them: the bodies that report this flag are the
	   cinema ones, whose firmware has no "MF assist while standby / while
	   recording" setting at all (bug 115 — the plugin offered rows the body
	   cannot set). */
	{"af_mode", "mfOnly", false},
	{"caf", "mfOnly", false},
	{"caf_sens", "mfOnly", false},
	{"af_area", "mfOnly", false},
	{"live_caf", "mfOnly", false},
	{"af_speed", "mfOnly", false},
	{"mf_mag", "mfOnly", false},
	{"mf_recording", "mfOnly", false},
	/* The focus family rides the negative `noFocus` flag: a body without a
	   focus motor reports it, and every focus row is meaningless there
	   (bug 115 — MF-assist rows offered on a body that has no such setting). */
	{"focus", "noFocus", true},
	{"af_mode", "noFocus", true},
	{"caf", "noFocus", true},
	{"caf_sens", "noFocus", true},
	{"af_area", "noFocus", true},
	{"live_caf", "noFocus", true},
	{"af_speed", "noFocus", true},
	{"mf_mag", "noFocus", true},
	{"mf_recording", "noFocus", true},
	{"af_adjust_with_ptz", "noFocus", true},
	{"lens_focus_pos", "noFocus", true},
};
} // namespace

bool catalogSupportedByFeatures(const QString &catalogId, const ZcFeatures &f)
{
	for (const CatalogGate &gate : kCatalogGates) {
		if (catalogId == QLatin1String(gate.id))
			return f.has(gate.flag);
	}
	return true;
}

bool settingSupportedByFeatures(const QString &key, const ZcFeatures &f)
{
	for (const SettingGate &gate : kSettingGates) {
		if (key != QLatin1String(gate.key))
			continue;
		const bool on = f.has(gate.flag);
		if (gate.hiddenWhenOn ? on : !on)
			return false;
	}
	return true;
}

bool cameraHasStreamEncoder(const ZcFeatures &f)
{
	return f.has("rtsp") || f.has("rtmp") || f.has("srt") ||
	       f.has("webrtcStream");
}

/* The camera page's value vocabulary, from its own Chinese language pack.
   Only whole values are listed: a firmware that sends "On" shows 开, one that
   sends "Slowest" shows "Slowest" — never a guessed translation.

   These are the page's GENERIC words (its language pack's bare keys: "Medium"
   is 中, "Strong" is 强). The framing-size words are not here: the page keeps
   those under `aframe.*` ("aframe.Medium" is 中景) and the framing tab draws
   them from the .ini, not from a setting's option list. Putting 中景 on the
   generic "Medium" made the sharpness, noise-reduction and bitrate-level
   option lists read 中景 where the page says 中 (bugs 202/203/204). */
QString settingValueLabelZh(const QString &value)
{
	static const QHash<QString, QString> table = {
	{"On", QString::fromUtf8("开")},
	{"Off", QString::fromUtf8("关")},
	{"Auto", QString::fromUtf8("自动")},
	{"Manual", QString::fromUtf8("手动")},
	{"Default", QString::fromUtf8("默认")},
	{"Enable", QString::fromUtf8("启用")},
	{"Disable", QString::fromUtf8("关闭")},
	{"Start", QString::fromUtf8("开始")},
	{"Stop", QString::fromUtf8("停止")},
	{"Full Body", QString::fromUtf8("全身")},
	{"Waist", QString::fromUtf8("半身")},
	{"Close Up", QString::fromUtf8("特写")},
	{"Wide", QString::fromUtf8("广角")},
	/* Strength / size / level words (bugs 202/203/204/205): the page's own
	   generic keys, shared by sharpness (Strong/Medium/None), noise reduction
	   (Medium/Weak/Off), bitrate level (Low/Medium/High) and AF area size
	   (Small/Medium/Large). */
	{"Strong", QString::fromUtf8("强")},
	{"Medium", QString::fromUtf8("中")},
	{"Middle", QString::fromUtf8("中")},
	{"Weak", QString::fromUtf8("弱")},
	{"None", QString::fromUtf8("无")},
	{"Low", QString::fromUtf8("低")},
	{"High", QString::fromUtf8("高")},
	{"Small", QString::fromUtf8("小")},
	{"Large", QString::fromUtf8("大")},
	{"Home", QString::fromUtf8("原点")},
	{"Start Position", QString::fromUtf8("开始位置")},
	{"Hold Position", QString::fromUtf8("保持当前位置")},
	{"Preset 1", QString::fromUtf8("预置位 1")},
	{"Preset 2", QString::fromUtf8("预置位 2")},
	{"Preset 3", QString::fromUtf8("预置位 3")},
	{"Preset 4", QString::fromUtf8("预置位 4")},
	{"Preset 5", QString::fromUtf8("预置位 5")},
	{"General", QString::fromUtf8("常规")},
	{"Sports", QString::fromUtf8("运动")},
	{"Basketball", QString::fromUtf8("篮球")},
	{"Multi", QString::fromUtf8("多目标")},
	{"IDLE", QString::fromUtf8("空闲")},
	{"Detecting", QString::fromUtf8("正在检测")},
	{"Tracking", QString::fromUtf8("正在跟踪")},
	{"Target Lost", QString::fromUtf8("目标丢失")},
	{"Disabled", QString::fromUtf8("禁止")},
	};
	const auto it = table.constFind(value);
	return it == table.constEnd() ? value : it.value();
}

QUrlQuery presetQuery(const QString &action, int index,
		      const QList<QPair<QString, QString>> &extra)
{
	QUrlQuery q;
	q.addQueryItem("action", action);
	q.addQueryItem("index", QString::number(index));
	for (const auto &kv : extra)
		q.addQueryItem(kv.first, kv.second);
	return q;
}

QString presetThumbnailPath(int index)
{
	return QString("/app_data/preset/thm_%1.jpg?act=thm")
	    .arg(index, 3, 10, QChar('0'));
}

QUrlQuery ptraceQuery(const QString &action, int index,
		      const QList<QPair<QString, QString>> &extra)
{
	QUrlQuery q;
	q.addQueryItem("action", action);
	if (index >= 0)
		q.addQueryItem("index", QString::number(index));
	for (const auto &kv : extra)
		q.addQueryItem(kv.first, kv.second);
	return q;
}

QString ptraceThumbnailPath(int index)
{
	return QString("/app_data/trace/thm_%1.jpg?act=thm")
	    .arg(index, 3, 10, QChar('0'));
}

TraceState traceStateFromInt(int value)
{
	switch (value) {
	case 1:
		return TraceState::Recording;
	case 2:
		return TraceState::Preparing;
	case 3:
		return TraceState::ReadyForPlay;
	case 4:
		return TraceState::Playing;
	case 5:
		return TraceState::Deleting;
	case 6:
		return TraceState::ReadyForRecord;
	default:
		return TraceState::None;
	}
}

TraceActions traceActions(TraceState state)
{
	switch (state) {
	case TraceState::Recording:
		/* Only the Record button, which now stops the recording. */
		return {true, false, false, false};
	case TraceState::Playing:
		/* Only the play button, which now stops the playback. */
		return {false, true, false, false};
	case TraceState::Preparing:
		return {false, false, false, true};
	case TraceState::Deleting:
		return {true, true, false, true};
	case TraceState::ReadyForPlay:
	case TraceState::ReadyForRecord:
	case TraceState::None:
	default:
		return {true, true, true, true};
	}
}

QUrlQuery streamSettingQuery(
	const QString &index, const QList<QPair<QString, QString>> &params)
{
	QUrlQuery q;
	/* `query` is also the write verb: it applies every field parameter it is
	   handed. `set` is rejected with code -1. */
	q.addQueryItem("action", "query");
	q.addQueryItem("index", index);
	for (const auto &kv : params)
		q.addQueryItem(kv.first, kv.second);
	return q;
}

bool isStreamDocument(const QJsonObject &doc)
{
	/* An index the camera does not have answers `{"code":0,"streaming":false}`
	   (or code -1); a real document always names its own index. */
	return !doc.value("streamIndex").toString().isEmpty();
}

QList<QPair<QString, QString>> resolutionParams(const QString &resolution)
{
	const int x = resolution.indexOf(QLatin1Char('x'));
	if (x <= 0 || x + 1 >= resolution.size())
		return {};
	bool okW = false;
	bool okH = false;
	const int w = resolution.left(x).toInt(&okW);
	const int h = resolution.mid(x + 1).toInt(&okH);
	if (!okW || !okH || w <= 0 || h <= 0)
		return {};
	return {{QStringLiteral("width"), QString::number(w)},
		{QStringLiteral("height"), QString::number(h)}};
}

QString cameraSourceName(const QString &host)
{
	return QStringLiteral("ZCamera ") + host;
}

QString hostFromSourceName(const QString &sourceName)
{
	static const QString prefix = QStringLiteral("ZCamera ");
	if (!sourceName.startsWith(prefix))
		return QString();
	return sourceName.mid(prefix.size());
}

bool catalogRequiresPtz(const QString &catalogId)
{
	/* Only the `ptz` catalog holds PTZ configuration. The PTZ *pane* (presets
	   and traces) is gated by the same capability in the dock. */
	return catalogId == QStringLiteral("ptz");
}

bool catalogSupportedByModel(const QString &catalogId, const QString &model)
{
	if (catalogId != QStringLiteral("security"))
		return true;
	/* Model names vary ("E2-F6 Pro", "E2_F6_Pro", "Avatar"), so compare on a
	   letters-and-digits-only form. An empty model means "not known yet": the
	   group stays, and the dock rebuilds its list once /info arrives. */
	QString m = model.toLower();
	m.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
	if (m.isEmpty())
		return true;
	return !(m.contains(QStringLiteral("avatar")) ||
		 m.contains(QStringLiteral("e2f6pro")));
}

bool settingRebootsCamera(const QString &key)
{
	/* HTTPS and identity auth restart the camera's web service; a network-type
	   change can move it to another address entirely. Crop Sensor re-times the
	   sensor and reboots the camera (bug list #82 — its reconnect must ride
	   the same 20 s / 40 s / 90 s backoff as the other restart writes). */
	return key == QStringLiteral("https_on") ||
	       key == QStringLiteral("http_auth") ||
	       key == QStringLiteral("ip_mode") ||
	       key == QStringLiteral("net_mode") ||
	       key == QStringLiteral("eth_mode") ||
	       key == QStringLiteral("crop_sensor");
}

bool isPerKey(const QString &key)
{
	for (const char *k : kPerKeys)
		if (key == k)
			return true;
	return false;
}

bool isBooleanSetting(const QString &key)
{
	for (const char *k : kBooleanKeys)
		if (key == k)
			return true;
	return false;
}

const std::vector<DependencyRule> &dependencyRules()
{
	static std::vector<DependencyRule> d = makeDeps();
	return d;
}

QStringList hudKeys()
{
	return {"sht_operation", "live_ae_shutter", "live_ae_shutter_angle",
		"live_ae_iso", "ev", "mwb", "live_ae_fno", "project_fps",
		"resolution", "temp"};
}

} // namespace zc