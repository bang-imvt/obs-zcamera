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

namespace zc {

static std::vector<CatalogDef> makeCatalogs()
{
	/* Mirrors ZCamGuiOpen settings-schema.ts groups + catalogs. */
	return {
	    {"exposure", {}, {"ev_choice", "flicker", "meter_mode", "iris",
			      "iso", "min_iso", "max_iso", "iso_ctrl",
			      "shutter_angle_ctrl", "sht_operation", "eND",
			      "lock_ae_in_rec", "ae_speed", "bl_comp"}},
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
			   "compose_mode", "rec_proxy_file", "crop_sensor",
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
	    {"system", {}, {"hdmi_fmt", "hdmi_osd", "use_edid", "osd_layout",
			    "led", "desqueeze", "usb_device_role", "tally_on",
		    "ir", "ir_id", "color_bar_enable", "genlock",
		    "sdi", "3g_sdi_mode", "visca_enable", "visca_id",
		    "visca_baud_rate", "lcd_backlight",
		    "gl_shf_coarse", "gl_shf_fine"}},
	    {"network", "system", {"wifi", "wifi_channel"}},
	    {"security", "system", {"http_auth", "https_on",
				    "https_cert_source"}},
	    {"multicam", "system", {"union_ae", "union_awb", "ezlink_mode",
				    "ezlink_trigger"}},
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
	      "shutter_angle_ctrl"}, {"exposure"}},
	    {{"lut"}, {"exposure"}},
	    {{"compose_mode", "movvfr", "video_encoder", "record_mode"},
	     {"record", "video"}},
	    {{"bitrate_level"}, {"video"}},
	    {{"resolution", "project_fps"}, {"record"}},
	    {{"vfr_ctrl"}, {"record"}},
	    {{"audio_channel"}, {"audio"}},
	    {{"wb", "mwb", "tint"}, {"wb"}},
	    {{"wifi"}, {"network"}},
	    {{"https_on"}, {"security"}},
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