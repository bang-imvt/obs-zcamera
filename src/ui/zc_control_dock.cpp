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

#include "zc_control_dock.h"
#include "zc_debug_panel.h"
#include "zc_camera_client.h"
#include "zc_source_bridge.h"
#include "zc_settings_schema.h"

/* Must come before the OBS headers: ssp-mdns.h pulls in winsock2 via <mdns.h>,
   which has to be included before windows.h (brought in by obs-module.h). */
#include "ssp-mdns.h"

#include <obs-module.h>
#include <obs.h>
#include <obs-frontend-api.h>

/* Win32: deliberately after ssp-mdns.h, whose winsock2 has to precede
   windows.h. Kept for the window-handle lookups below. */
#ifdef _WIN32
#include <windows.h>
#endif

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QStackedWidget>
#include <QScrollArea>
#include <QGridLayout>
#include <QGroupBox>
#include <QFrame>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QButtonGroup>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QPointer>
#include <QInputDialog>
#include <QMessageBox>
#include <QDialog>
#include <QApplication>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <functional>
#include <array>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QTimer>
#include <QIcon>
#include <QPixmap>
#include <QPolygonF>
#include <QSet>
#include <QElapsedTimer>
#include <cmath>
#include <algorithm>

namespace zc {

/* Translate a dock string through the OBS locale (.ini). The dock is plain Qt
   widgets with no QTranslator installed, so obs_module_text is what picks the
   language OBS runs in. Values read back from the camera (names, option labels,
   numbers) are never passed through here. */
static QString ztr(const char *key)
{
	/* obs_module_text returns the .ini value verbatim; a literal "\n" in a
	   value (used to open a second tooltip line) is turned into a real
	   newline here. */
	return QString::fromUtf8(obs_module_text(key)).replace(
		QStringLiteral("\\n"), QStringLiteral("\n"));
}

/* The camera's own Chinese word for a setting VALUE ("On" -> 开), when this OBS
   runs in Chinese and the camera's page has that word. The table is the
   camera's language pack, so the two UIs agree; an unknown value stays as the
   camera sent it (bug 35). */
static QString zcValueLabel(const QString &value)
{
	const QByteArray locale = QString::fromUtf8(obs_get_locale()).toUtf8();
	if (!locale.startsWith("zh"))
		return value;
	return zc::settingValueLabelZh(value);
}

/* Human label for a settings row. The camera reports lowercase ids
   ("http_auth"); the locale turns the ones whose id is misleading into readable
   text (bug 5: `http_auth` is the identity-authentication switch, not a second
   HTTPS toggle). A key with no entry keeps its raw id, so a key a newer
   firmware adds is still usable. */
static QString settingLabel(const QString &key)
{
	/* Readable labels following ZCamGuiOpen's settings-schema labels; a key
	   without an entry shows its raw id. The names are kept aligned with the
	   camera's own web UI (bug list "unify setting names"), and every schema key
	   has an entry in the .ini so the row is translated, not a raw camera id
	   (bug list "settings not translated to Chinese"). */
	static const QHash<QString, const char *> kLabels = {
		/* exposure */
		{QStringLiteral("http_auth"), "ZCameraPlugin.Setting.HttpAuth"},
		{QStringLiteral("https_on"), "ZCameraPlugin.Setting.HttpsOn"},
		{QStringLiteral("ev"), "ZCameraPlugin.Setting.Ev"},
		{QStringLiteral("ev_choice"), "ZCameraPlugin.Setting.EvChoice"},
		{QStringLiteral("shutter"), "ZCameraPlugin.Setting.Shutter"},
		{QStringLiteral("max_shutter"), "ZCameraPlugin.Setting.MaxShutter"},
		{QStringLiteral("sht_operation"),
		 "ZCameraPlugin.Setting.ShutterOperation"},
		{QStringLiteral("exposure_window_idx"),
		 "ZCameraPlugin.Setting.ExposureWindow"},
		{QStringLiteral("extended_iso"),
		 "ZCameraPlugin.Setting.ExtendedIso"},
		{QStringLiteral("flicker"), "ZCameraPlugin.Setting.Flicker"},
		{QStringLiteral("meter_mode"), "ZCameraPlugin.Setting.MeterMode"},
		{QStringLiteral("iris"), "ZCameraPlugin.Setting.Iris"},
		{QStringLiteral("iso"), "ZCameraPlugin.Setting.Iso"},
		{QStringLiteral("min_iso"), "ZCameraPlugin.Setting.MinIso"},
		{QStringLiteral("max_iso"), "ZCameraPlugin.Setting.MaxIso"},
		{QStringLiteral("iso_ctrl"), "ZCameraPlugin.Setting.IsoCtrl"},
		{QStringLiteral("shutter_angle_ctrl"),
		 "ZCameraPlugin.Setting.ShutterAngleCtrl"},
		{QStringLiteral("eND"), "ZCameraPlugin.Setting.End"},
		{QStringLiteral("lock_ae_in_rec"), "ZCameraPlugin.Setting.LockAeInRec"},
		{QStringLiteral("ae_speed"), "ZCameraPlugin.Setting.AeSpeed"},
		{QStringLiteral("bl_comp"), "ZCameraPlugin.Setting.BlComp"},
		/* white balance */
		{QStringLiteral("wb"), "ZCameraPlugin.Setting.Wb"},
		{QStringLiteral("wb_priority"), "ZCameraPlugin.Setting.WbPriority"},
		{QStringLiteral("lock_awb_in_rec"),
		 "ZCameraPlugin.Setting.LockAwbInRec"},
		{QStringLiteral("mwb"), "ZCameraPlugin.Setting.Mwb"},
		{QStringLiteral("tint"), "ZCameraPlugin.Setting.Tint"},
		{QStringLiteral("mwb_r"), "ZCameraPlugin.Setting.MwbR"},
		{QStringLiteral("mwb_g"), "ZCameraPlugin.Setting.MwbG"},
		{QStringLiteral("mwb_b"), "ZCameraPlugin.Setting.MwbB"},
		/* Some firmwares name the white-balance offsets with the _offset
		   suffix; both forms get the readable label (bug list #45). */
		{QStringLiteral("mwb_r_offset"),
		 "ZCameraPlugin.Setting.MwbROffset"},
		{QStringLiteral("mwb_g_offset"),
		 "ZCameraPlugin.Setting.MwbGOffset"},
		{QStringLiteral("mwb_b_offset"),
		 "ZCameraPlugin.Setting.MwbBOffset"},
		/* lens / focus */
		{QStringLiteral("focus"), "ZCameraPlugin.Setting.Focus"},
		{QStringLiteral("af_mode"), "ZCameraPlugin.Setting.AfMode"},
		{QStringLiteral("caf"), "ZCameraPlugin.Setting.Caf"},
		{QStringLiteral("caf_sens"), "ZCameraPlugin.Setting.CafSens"},
		{QStringLiteral("af_area"), "ZCameraPlugin.Setting.AfArea"},
		{QStringLiteral("live_caf"), "ZCameraPlugin.Setting.LiveCaf"},
		{QStringLiteral("mf_mag"), "ZCameraPlugin.Setting.MfMag"},
		{QStringLiteral("mf_recording"), "ZCameraPlugin.Setting.MfRecording"},
		{QStringLiteral("af_speed"), "ZCameraPlugin.Setting.AfSpeed"},
		{QStringLiteral("af_far_limit"),
		 "ZCameraPlugin.Setting.AfFarLimit"},
		{QStringLiteral("af_near_limit"),
		 "ZCameraPlugin.Setting.AfNearLimit"},
		{QStringLiteral("af_adjust_with_ptz"),
		 "ZCameraPlugin.Setting.AfAdjustWithPtz"},
		{QStringLiteral("zoom_mode"), "ZCameraPlugin.Setting.ZoomMode"},
		{QStringLiteral("lens_focus_pos"),
		 "ZCameraPlugin.Setting.LensFocusPos"},
		{QStringLiteral("lens_zoom_pos"), "ZCameraPlugin.Setting.LensZoomPos"},
		{QStringLiteral("fz_speed"), "ZCameraPlugin.Setting.FzSpeed"},
		/* image */
		{QStringLiteral("lut"), "ZCameraPlugin.Setting.Lut"},
		{QStringLiteral("sharpness"), "ZCameraPlugin.Setting.Sharpness"},
		{QStringLiteral("noise_reduction"),
		 "ZCameraPlugin.Setting.NoiseReduction"},
		{QStringLiteral("luma_level"), "ZCameraPlugin.Setting.LumaLevel"},
		{QStringLiteral("brightness"), "ZCameraPlugin.Setting.Brightness"},
		{QStringLiteral("contrast"), "ZCameraPlugin.Setting.Contrast"},
		{QStringLiteral("saturation"), "ZCameraPlugin.Setting.Saturation"},
		{QStringLiteral("hue"), "ZCameraPlugin.Setting.Hue"},
		/* record */
		{QStringLiteral("resolution"), "ZCameraPlugin.Setting.Resolution"},
		{QStringLiteral("project_fps"), "ZCameraPlugin.Setting.ProjectFps"},
		{QStringLiteral("vfr_ctrl"), "ZCameraPlugin.Setting.VfrCtrl"},
		{QStringLiteral("movvfr"), "ZCameraPlugin.Setting.MovVfr"},
		{QStringLiteral("record_file_format"),
		 "ZCameraPlugin.Setting.RecordFileFormat"},
		{QStringLiteral("rotation"), "ZCameraPlugin.Setting.Rotation"},
		{QStringLiteral("split_duration"),
		 "ZCameraPlugin.Setting.SplitDuration"},
		{QStringLiteral("rec_fps"), "ZCameraPlugin.Setting.RecFps"},
		{QStringLiteral("preroll"), "ZCameraPlugin.Setting.Preroll"},
		{QStringLiteral("preroll_duration"),
		 "ZCameraPlugin.Setting.PrerollDuration"},
		{QStringLiteral("rec_frame_indicator"),
		 "ZCameraPlugin.Setting.RecFrameIndicator"},
		{QStringLiteral("video_tl_interval"),
		 "ZCameraPlugin.Setting.VideoTlInterval"},
		{QStringLiteral("record_meta"), "ZCameraPlugin.Setting.RecordMeta"},
		{QStringLiteral("camera_id"), "ZCameraPlugin.Setting.CameraId"},
		{QStringLiteral("reelname"), "ZCameraPlugin.Setting.ReelName"},
		/* video */
		{QStringLiteral("video_encoder"),
		 "ZCameraPlugin.Setting.VideoEncoder"},
		{QStringLiteral("bitrate_level"), "ZCameraPlugin.Setting.BitrateLevel"},
		{QStringLiteral("record_mode"), "ZCameraPlugin.Setting.RecordMode"},
		{QStringLiteral("rec_proxy_file"),
		 "ZCameraPlugin.Setting.RecProxyFile"},
		{QStringLiteral("crop_sensor"), "ZCameraPlugin.Setting.CropSensor"},
		{QStringLiteral("low_jello"), "ZCameraPlugin.Setting.LowJello"},
		{QStringLiteral("photo_q"), "ZCameraPlugin.Setting.PhotoQ"},
		{QStringLiteral("eis_on_off"), "ZCameraPlugin.Setting.EisOnOff"},
		{QStringLiteral("vid_rot"), "ZCameraPlugin.Setting.VidRot"},
		/* audio */
		{QStringLiteral("primary_audio"), "ZCameraPlugin.Setting.PrimaryAudio"},
		{QStringLiteral("audio_channel"), "ZCameraPlugin.Setting.AudioChannel"},
		{QStringLiteral("audio_phantom_power"),
		 "ZCameraPlugin.Setting.AudioPhantomPower"},
		{QStringLiteral("audio_level_display"),
		 "ZCameraPlugin.Setting.AudioLevelDisplay"},
		{QStringLiteral("ain_gain_type"), "ZCameraPlugin.Setting.InputGainType"},
		{QStringLiteral("audio_input_level"),
		 "ZCameraPlugin.Setting.AudioInputLevel"},
		{QStringLiteral("xlr_input_level"),
		 "ZCameraPlugin.Setting.XlrInputLevel"},
		{QStringLiteral("audio_input_gain"),
		 "ZCameraPlugin.Setting.AudioInputGain"},
		{QStringLiteral("audio_noise_reduction"),
		 "ZCameraPlugin.Setting.AudioNoiseReduction"},
		{QStringLiteral("audio_in_l_gain"), "ZCameraPlugin.Setting.AudioInLGain"},
		{QStringLiteral("audio_in_r_gain"), "ZCameraPlugin.Setting.AudioInRGain"},
		{QStringLiteral("audio_output_gain"),
		 "ZCameraPlugin.Setting.AudioOutputGain"},
		/* ptz */
		{QStringLiteral("pt_speedmode"), "ZCameraPlugin.Setting.PtSpeedmode"},
		{QStringLiteral("ptz_flip"), "ZCameraPlugin.Setting.PtzFlip"},
		{QStringLiteral("ptz_limit"), "ZCameraPlugin.Setting.PtzLimit"},
		{QStringLiteral("ptz_preset_mode"),
		 "ZCameraPlugin.Setting.PtzPresetMode"},
		{QStringLiteral("ptz_speed_mode"),
		 "ZCameraPlugin.Setting.PtzSpeedMode"},
		{QStringLiteral("freeze_during_preset"),
		 "ZCameraPlugin.Setting.FreezeDuringPreset"},
		{QStringLiteral("pt_pwr_pos"), "ZCameraPlugin.Setting.PtPwrPos"},
		{QStringLiteral("pt_priv_mode"), "ZCameraPlugin.Setting.PtPrivMode"},
		{QStringLiteral("preset_adjust_speed_with_zoom"),
		 "ZCameraPlugin.Setting.PresetAdjustSpeedWithZoom"},
		{QStringLiteral("pt_speed_with_zoom_pos"),
		 "ZCameraPlugin.Setting.PtSpeedWithZoomPos"},
		{QStringLiteral("ptz_common_speed_unit"),
		 "ZCameraPlugin.Setting.PtzCommonSpeedUnit"},
		{QStringLiteral("ptz_common_speed"),
		 "ZCameraPlugin.Setting.PtzCommonSpeed"},
		{QStringLiteral("ptz_common_time"),
		 "ZCameraPlugin.Setting.PtzCommonTime"},
	};
	auto it = kLabels.constFind(key);
	if (it == kLabels.constEnd())
		return key;
	/* obs_module_text echoes the key itself when the locale has no entry. */
	const QString text = ztr(*it);
	return text == QLatin1String(*it) ? key : text;
}

/* Human label for a settings catalog (the sidebar's group tabs). The camera
   exposes lowercase ids ("wb"); the locale maps each to a readable title, and
   an id with no entry keeps its raw form, so a catalog a newer firmware adds is
   still listed. The id itself stays in Qt::UserRole, never shown. */
static QString catalogLabel(const QString &id)
{
	static const QHash<QString, const char *> kLabels = {
		{QStringLiteral("exposure"), "ZCameraPlugin.Catalog.Exposure"},
		{QStringLiteral("wb"), "ZCameraPlugin.Catalog.Wb"},
		{QStringLiteral("lens"), "ZCameraPlugin.Catalog.Lens"},
		{QStringLiteral("image"), "ZCameraPlugin.Catalog.Image"},
		{QStringLiteral("record"), "ZCameraPlugin.Catalog.Record"},
		{QStringLiteral("video"), "ZCameraPlugin.Catalog.Video"},
		{QStringLiteral("audio"), "ZCameraPlugin.Catalog.Audio"},
		{QStringLiteral("ptz"), "ZCameraPlugin.Catalog.Ptz"},
	};
	auto it = kLabels.constFind(id);
	if (it == kLabels.constEnd())
		return id;
	/* obs_module_text echoes the key itself when the locale has no entry. */
	const QString text = ztr(*it);
	return text == QLatin1String(*it) ? id : text;
}

/* Small painted camera glyph so the discovery list needs no bundled asset.
   With `added` a gold check badge is overlaid, marking a camera that already
   exists in OBS as a source (spec §1). With `offline` the body is drawn in the
   muted colour: the camera is only known from its source, discovery is not
   advertising it and it does not answer, so it cannot be controlled. */
static QIcon zcCameraIcon(bool added = false, bool offline = false)
{
	QPixmap pm(24, 24);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	/* body + lens in the dock's accent colour. */
	p.setPen(Qt::NoPen);
	p.setBrush(offline ? QColor("#6b7a90") : QColor("#9db8d2"));
	p.drawRoundedRect(QRectF(2, 9, 20, 11), 2, 2);
	p.drawRect(QRectF(8, 6, 6, 4)); /* top bump */
	p.setBrush(QColor("#1c2733"));
	p.drawEllipse(QRectF(7, 12, 10, 5)); /* lens */

	if (added) {
		/* Gold disc + check, bottom-right. */
		p.setBrush(QColor("#d8b878"));
		p.setPen(Qt::NoPen);
		p.drawEllipse(QRectF(14, 14, 10, 10));
		QPen pen(QColor("#1f2927"));
		pen.setWidthF(1.8);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.drawLine(QPointF(16.6, 19.2), QPointF(18.6, 21.2));
		p.drawLine(QPointF(18.6, 21.2), QPointF(21.6, 17.4));
	}
	p.end();
	return QIcon(pm);
}

/* Rescan glyph for the rail header, turned by `angle` degrees. Drawn rather
   than taken from QStyle: the platform's standard refresh icon is dark-on-dark
   against this rail's header. Built at 24 px like zcCameraIcon(), then shown at
   the tool button's icon size.

   The rotation is applied to the painter, not to the finished pixmap, so the
   canvas stays 24x24: QPixmap::transformed() grows the canvas to the rotated
   bounds instead, and the button (whose iconSize is 15 px) would then scale
   that larger canvas back down and make the glyph visibly shrink on every
   tick. */
static QPixmap zcRefreshPixmap(double angle, const QColor &colour)
{
	QPixmap pm(24, 24);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.translate(12.0, 12.0);
	p.rotate(angle);
	p.translate(-12.0, -12.0);

	const QRectF box(6, 6, 12, 12);
	QPen pen(colour);
	pen.setWidthF(2.0);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	/* Three quarters of a circle, leaving the gap the arrow head points into.
	   Qt sweep angles are 1/16th of a degree, counter-clockwise from 3 o'clock. */
	p.drawArc(box, 70 * 16, 280 * 16);

	/* Head at the arc's start, pointing the way a clockwise reload glyph does:
	   opposite the arc's counter-clockwise travel. */
	const double kPi = 3.14159265358979323846;
	const double a = 70.0 * kPi / 180.0;
	const QPointF centre = box.center();
	const QPointF start(centre.x() + box.width() / 2.0 * std::cos(a),
			    centre.y() - box.height() / 2.0 * std::sin(a));
	const QPointF dir(std::sin(a), std::cos(a));
	const QPointF nrm(-dir.y(), dir.x());
	p.setPen(Qt::NoPen);
	p.setBrush(colour);
	QPolygonF head;
	head << (start + dir * 4.5) << (start - dir * 0.6 + nrm * 3.2)
	     << (start - dir * 0.6 - nrm * 3.2);
	p.drawPolygon(head);
	p.end();
	return pm;
}

/* The rail header's own colour, so the glyph and its spin share one place. */
static const QColor kRailGlyphColour("#9db8d2");

static QIcon zcRefreshIcon()
{
	return QIcon(zcRefreshPixmap(0.0, kRailGlyphColour));
}

/* Bullseye for the PTZ dial's click-to-move button, drawn for the same reason
   as zcRefreshIcon: the platform standard icons are dark-on-dark here. Two
   states, so "armed" is visible without a label: the idle crosshair in the
   dock's link colour, the armed one in the accent gold. */
static QPixmap zcTargetPixmap(const QColor &colour)
{
	QPixmap pm(24, 24);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);

	const QPointF c(12, 12);
	QPen pen(colour);
	pen.setWidthF(1.8);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawEllipse(c, 8.0, 8.0);
	p.drawEllipse(c, 3.0, 3.0);
	/* Crosshair arms, broken at the rim so the two rings stay legible. */
	p.drawLine(QPointF(c.x(), 1.5), QPointF(c.x(), 4.5));
	p.drawLine(QPointF(c.x(), 19.5), QPointF(c.x(), 22.5));
	p.drawLine(QPointF(1.5, c.y()), QPointF(4.5, c.y()));
	p.drawLine(QPointF(19.5, c.y()), QPointF(22.5, c.y()));
	p.end();
	return pm;
}

static QIcon zcTargetIcon()
{
	QIcon icon;
	icon.addPixmap(zcTargetPixmap(QColor("#9db8d2")), QIcon::Normal,
		       QIcon::Off);
	icon.addPixmap(zcTargetPixmap(QColor("#d8b878")), QIcon::Normal,
		       QIcon::On);
	return icon;
}

/* Every window in the process, whatever it is parented to.

   QApplication::topLevelWidgets() is not enough here: it lists only widgets
   with no parent widget at all, and the OBS windows this plugin reaches for
   are constructed with the main window as their parent even though they are
   separate windows — OBSProjector is `OBSQTDisplay(widget, Qt::Window)` and
   OBSBasicInteraction is a QDialog built on the main window. QWidget::isWindow()
   is the honest test for "this is a window", and allWidgets() is the only
   enumeration that does not pre-filter the interesting ones out. */
static QList<QWidget *> zcAllWindows()
{
	QList<QWidget *> out;
	for (QWidget *w : QApplication::allWidgets()) {
		if (w && w->isWindow())
			out.append(w);
	}
	return out;
}

/* The source's Interact window, as OBS made it.

   OBS keeps exactly one per process — OBSBasic::interaction is a single
   QPointer, and its creator closes whatever was there before building a
   replacement — and a plugin can neither construct it nor address it by name.
   It can be recognised, though: the class name survives through the
   meta-object system even though its header is not something a plugin can
   include. And it can be driven, because QDialog::close() is public and the
   window owns its own lifetime (WA_DeleteOnClose, so OBS's QPointer clears
   itself and the next open builds a fresh one). That is enough for the
   click-to-move target to be a real state control: checked means this window
   is up. */
static QWidget *zcFindInteractionWindow()
{
	for (QWidget *w : zcAllWindows()) {
		if (w->isVisible() && w->inherits("OBSBasicInteraction"))
			return w;
	}
	return nullptr;
}

/* Which camera an Interact window was opened for. The class itself carries no
   such notion and its OBSSource member is private, so the dock tags the window
   on creation and reads the tag back to decide whether a request to open one
   can be served by raising the window that is already there. */
static const char *kInteractHostProp = "zcInteractHost";

/* ---------------------------------------------------------------- */
/* ZcCameraRail                                                      */
/* ---------------------------------------------------------------- */

/* Per-row fields. The host doubles as the row's identity everywhere else. */
static constexpr int kHostRole = Qt::UserRole;
static constexpr int kNameRole = Qt::UserRole + 1;
static constexpr int kOfflineRole = Qt::UserRole + 2;
static constexpr int kAddedRole = Qt::UserRole + 3;

/* Row geometry: two lines of text, and room for the action chip on the right. */
static constexpr int kRailRowPadV = 5;
static constexpr int kRailChipW = 26;
static constexpr int kRailChipH = 20;

/* Paints one rail row as two lines — the camera's name above its address — and
   hit-tests the row's own action chip.

   The chip is the row's "add to the current scene" control (spec §1 L5/L6), so
   the rail has no Add button of its own: the operator acts on the camera they
   are looking at instead of having to select it first. It cannot be a
   QPushButton, because a QListWidget row has no widget of its own and an item
   widget would swallow the click that selects the row; it is painted and
   hit-tested here instead. */
class ZcRailDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	/* Called with the row's host and whether it is already an OBS source when
	   its chip is clicked (a gold check removes the source, "＋" adds it). */
	std::function<void(const QString &host, bool added)> onChip;

	QSize sizeHint(const QStyleOptionViewItem &option,
		       const QModelIndex &index) const override
	{
		Q_UNUSED(index);
		return QSize(150, option.fontMetrics.height() * 2 + kRailRowPadV * 2);
	}

	/* The chip's rect inside an item's rect. */
	static QRect chipRect(const QRect &itemRect)
	{
		return QRect(itemRect.right() - kRailChipW - 4,
			     itemRect.center().y() - kRailChipH / 2, kRailChipW,
			     kRailChipH);
	}

	void paint(QPainter *painter, const QStyleOptionViewItem &option,
		   const QModelIndex &index) const override
	{
		QStyleOptionViewItem opt(option);
		initStyleOption(&opt, index);
		/* The base class draws the row background, selection and focus; the
		   text and the glyph are drawn below so the row is two lines. */
		opt.text.clear();
		opt.icon = QIcon();
		const QWidget *widget = opt.widget;
		QStyle *style = widget ? widget->style() : QApplication::style();
		style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

		const QString host = index.data(kHostRole).toString();
		const QString name = index.data(kNameRole).toString();
		const bool offline = index.data(kOfflineRole).toBool();
		const bool added = index.data(kAddedRole).toBool();

		/* The camera the dock is controlling carries the gold mark (spec §1). */
		if (opt.state & QStyle::State_Selected)
			painter->fillRect(QRect(option.rect.left(), option.rect.top(), 3,
						option.rect.height()),
					  QColor("#d8b878"));

		const QRect content =
		    option.rect.adjusted(8, kRailRowPadV, -(kRailChipW + 10), -kRailRowPadV);
		const int glyphW = 24;
		const QRect glyph(content.left(), content.center().y() - glyphW / 2,
				  glyphW, glyphW);
		index.data(Qt::DecorationRole).value<QIcon>().paint(painter, glyph);

		const QRect text = content.adjusted(glyphW + 6, 0, 0, 0);
		const int lineH = text.height() / 2;
		/* Name above, address below: the address is what identifies the OBS
		   source ("ZCamera <ip>", spec §1 L3), and on one line the two made a
		   long label that was hard to scan. A row known only from a source has
		   no name, so its first line is the address. */
		const QString line1 = name.isEmpty() ? host : name;
		const QString line2 = name.isEmpty()
					      ? (offline ? ztr("ZCameraPlugin.Dock.Offline")
							 : ztr("ZCameraPlugin.Dock.NotDiscovered"))
					      : host + (offline ? ztr("ZCameraPlugin.Dock.OfflineMark") : QString());
		const QFontMetrics fm(option.font);
		painter->setFont(option.font);
		painter->setPen(offline ? QColor("#6b7a90") : QColor("#eef2ed"));
		painter->drawText(QRect(text.left(), text.top(), text.width(), lineH),
				  Qt::AlignLeft | Qt::AlignVCenter,
				  fm.elidedText(line1, Qt::ElideRight, text.width()));
		painter->setPen(offline ? QColor("#6b7a90") : QColor("#8a9bb2"));
		painter->drawText(
		    QRect(text.left(), text.top() + lineH, text.width(), lineH),
		    Qt::AlignLeft | Qt::AlignVCenter,
		    fm.elidedText(line2, Qt::ElideRight, text.width()));

		/* The action chip: "＋" while the camera can still be added, a gold
		   check once it is an OBS source (spec §1 L5). */
		const QRect chip = chipRect(option.rect);
		painter->setRenderHint(QPainter::Antialiasing, true);
		painter->setPen(QPen(added ? QColor("#3c514e") : QColor("#d8b878")));
		painter->setBrush(added ? QColor("#232d2b") : QColor("#2b3a38"));
		painter->drawRoundedRect(chip, 3, 3);
		if (added) {
			QPen pen(QColor("#d8b878"));
			pen.setWidthF(1.8);
			pen.setCapStyle(Qt::RoundCap);
			painter->setPen(pen);
			const QPointF c = chip.center();
			painter->drawLine(QPointF(c.x() - 4.5, c.y()), QPointF(c.x() - 1.5, c.y() + 3));
			painter->drawLine(QPointF(c.x() - 1.5, c.y() + 3), QPointF(c.x() + 4.5, c.y() - 3.5));
		} else {
			painter->setPen(QColor("#eef2ed"));
			painter->drawText(chip, Qt::AlignCenter, QStringLiteral("＋"));
		}
	}

	bool editorEvent(QEvent *event, QAbstractItemModel *model,
			 const QStyleOptionViewItem &option,
			 const QModelIndex &index) override
	{
		Q_UNUSED(model);
		const QEvent::Type type = event->type();
		if (type != QEvent::MouseButtonPress &&
		    type != QEvent::MouseButtonRelease)
			return false;
		auto *me = static_cast<QMouseEvent *>(event);
		if (me->button() != Qt::LeftButton ||
		    !chipRect(option.rect).contains(me->pos()))
			return false;
		const QString host = index.data(kHostRole).toString();
		if (host.isEmpty())
			return false;
		/* The chip works on an added row too: a gold check on a camera that is
		   already a source removes it (spec §1, L5). */
		const bool added = index.data(kAddedRole).toBool();
		/* Fire exactly once per click. The press does the work and is remembered;
		   the matching release only clears that memory and must NOT fire again —
		   by the release the row may already show the new state (e.g. a just-added
		   camera now reads "added"), so re-firing would remove what was just
		   added. A release with no tracked press is the only case that fires from
		   release (a view that skips the press). The press always re-arms, so a
		   gesture whose release lands outside the chip — where this method is not
		   called at all — cannot leave the next click on that row swallowed. */
		if (type == QEvent::MouseButtonPress) {
			clickedHost_ = host;
			if (onChip)
				onChip(host, added);
		} else if (clickedHost_ == host) {
			clickedHost_.clear();
		} else if (onChip) {
			onChip(host, added);
		}
		return true;
	}

private:
	/* Host whose chip was pressed, so its release does not fire a second time. */
	mutable QString clickedHost_;
};

ZcCameraRail::ZcCameraRail(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	/* Title row: the rail's name and the rescan button. Discovery polls on its
	   own schedule, but until now there was no way to ask for a scan now — the
	   settings panel's Refresh only re-reads the connected camera's settings and
	   leaves the list alone. */
	auto *header = new QHBoxLayout;
	auto *title = new QLabel(ztr("ZCameraPlugin.Dock.Cameras"), this);
	title->setStyleSheet("color:#8a9bb2;font-size:11px;letter-spacing:1px;");
	header->addWidget(title);
	header->addStretch(1);
	refreshBtn_ = new QToolButton(this);
	refreshBtn_->setIcon(zcRefreshIcon());
	refreshBtn_->setIconSize(QSize(15, 15));
	refreshBtn_->setAutoRaise(true);
	refreshBtn_->setCursor(Qt::PointingHandCursor);
	refreshBtn_->setToolTip(ztr("ZCameraPlugin.Dock.RefreshCamerasTip"));
	refreshBtn_->setAccessibleName(ztr("ZCameraPlugin.Dock.RefreshCameras"));
	refreshBtn_->setStyleSheet(
	    "QToolButton { border:none; padding:1px; background:transparent; }"
	    "QToolButton:hover { background:#354845; border-radius:3px; }");
	header->addWidget(refreshBtn_);
	layout->addLayout(header);
	connect(refreshBtn_, &QToolButton::clicked, this,
		&ZcCameraRail::refreshRequested);

	/* The spin that says a manual rescan is in flight. Disabling the button was
	   not enough on its own: the rail usually already has rows, so neither the
	   busy page nor a changed list follows from the press, and the button only
	   went marginally dimmer — the press read as doing nothing at all. */
	spinTimer_ = new QTimer(this);
	spinTimer_->setInterval(70);
	connect(spinTimer_, &QTimer::timeout, this, [this]() {
		spinAngle_ = (spinAngle_ + 30) % 360;
		refreshBtn_->setIcon(
		    QIcon(zcRefreshPixmap(double(spinAngle_), kRailGlyphColour)));
	});

	list_ = new QListWidget(this);
	list_->setMinimumWidth(180);
	/* Each row carries its own action chip, so the click that acts on a camera
	   never has to go through a selection first (spec §1 L5/L6). */
	auto *delegate = new ZcRailDelegate(list_);
	delegate->onChip = [this](const QString &host, bool added) {
		if (added)
			emit removeRequested(host);
		else
			emit addRequested(host);
	};
	list_->setItemDelegate(delegate);
	layout->addWidget(list_, 1);

	/* The busy page stands in for the list while discovery has yet to name a
	   camera: an empty list gives the operator no way to tell a scan in
	   progress from a scan that found nothing. A sweeping, indeterminate bar is
	   the Qt idle indicator. */
	busyPage_ = new QWidget(this);
	{
		auto *busyLay = new QVBoxLayout(busyPage_);
		busyLay->setContentsMargins(0, 24, 0, 0);
		busyLay->setSpacing(8);
		auto *spin = new QProgressBar(busyPage_);
		spin->setRange(0, 0); /* indeterminate: sweeps until stopped */
		spin->setTextVisible(false);
		spin->setFixedSize(140, 6);
		auto *busyText =
		    new QLabel(ztr("ZCameraPlugin.Dock.Scanning"), busyPage_);
		busyText->setAlignment(Qt::AlignCenter);
		busyText->setStyleSheet("color:#8a9bb2;font-size:11px;");
		busyLay->addWidget(spin, 0, Qt::AlignHCenter);
		busyLay->addWidget(busyText);
		busyLay->addStretch(1);
	}
	layout->addWidget(busyPage_, 1);

	/* Scanning found nothing (or has gone on long enough): say so plainly
	   rather than keep sweeping a bar that will never finish. */
	auto *empty = new QLabel(ztr("ZCameraPlugin.Dock.NoCamera"), this);
	empty->setAlignment(Qt::AlignCenter);
	empty->setStyleSheet("color:#8a9bb2;font-size:11px;");
	emptyPage_ = empty;
	layout->addWidget(emptyPage_, 1);
	updateEmptyState();

	connect(list_, &QListWidget::itemClicked, this,
		[this](QListWidgetItem *item) {
			QString host = item->data(kHostRole).toString();
			emit cameraClicked(host);
		});
}

/* One page at a time: the list has rows, the spin is up, or nothing is left to
   show. Called from setCameras() and setBusy(), the two things that change. */
void ZcCameraRail::updateEmptyState()
{
	const bool showList = hasRows_;
	if (list_)
		list_->setVisible(showList);
	if (busyPage_)
		busyPage_->setVisible(!showList && busy_);
	if (emptyPage_)
		emptyPage_->setVisible(!showList && !busy_);
}

void ZcCameraRail::setBusy(bool busy)
{
	if (busy_ == busy)
		return;
	busy_ = busy;
	updateEmptyState();
}

/* While the rescan is in flight the button spins and its tooltip says what it
   is doing; a second click before the answers land would only queue a duplicate
   query, so it is disabled for the duration too. */
void ZcCameraRail::setRefreshing(bool refreshing)
{
	if (!refreshBtn_)
		return;
	refreshBtn_->setEnabled(!refreshing);
	if (refreshing) {
		spinAngle_ = 0;
		refreshBtn_->setToolTip(ztr("ZCameraPlugin.Dock.Scanning"));
		refreshBtn_->setAccessibleName(ztr("ZCameraPlugin.Dock.Scanning"));
		if (spinTimer_)
			spinTimer_->start();
		return;
	}
	if (spinTimer_)
		spinTimer_->stop();
	refreshBtn_->setIcon(zcRefreshIcon());
	refreshBtn_->setToolTip(ztr("ZCameraPlugin.Dock.RefreshCamerasTip"));
	refreshBtn_->setAccessibleName(ztr("ZCameraPlugin.Dock.RefreshCameras"));
}

/* Fields, glyph and tooltip of one row. The label itself is drawn by
   ZcRailDelegate; the item's text is kept for keyboard search only. */
void ZcCameraRail::applyRow(QListWidgetItem *item)
{
	const QString host = item->data(kHostRole).toString();
	const QString name = item->data(kNameRole).toString();
	const bool offline = item->data(kOfflineRole).toBool();
	const bool added = added_.contains(host);

	/* The OBS source for this camera is named "ZCamera <ip>" (spec §1 L3), so
	   the address belongs in the row: without it a rail entry
	   ("P2-R1-18xx") cannot be matched against the source list
	   ("ZCamera 192.168.10.120"), and two cameras of the same model are
	   indistinguishable. */
	item->setText(name.isEmpty()
			      ? host
			      : QStringLiteral("%1 %2").arg(name, host));
	item->setData(kAddedRole, added);
	item->setIcon(zcCameraIcon(added, offline));

	QString tip = QStringLiteral("ZCamera %1").arg(host);
	if (offline)
		tip += ztr("ZCameraPlugin.Dock.OfflineControl");
	tip += added ? ztr("ZCameraPlugin.Dock.ClickToRemove")
		     : ztr("ZCameraPlugin.Dock.ClickToAdd");
	item->setToolTip(tip);
}

/* Identity of a row as the rail draws it: address, offline mark and name. The
   name is part of it because a source-only row carries none until discovery
   starts advertising the camera, and that transition has to redraw the label. */
static QString rowKey(const QString &host, const QString &name, bool offline)
{
	return host + (offline ? "!" : "") + "|" + name;
}

void ZcCameraRail::setCameras(const QVector<ZcRailCamera> &cameras)
{
	/* Skip the rebuild when the same rows would be produced, so a periodic scan
	   neither drops the current selection nor makes the dock flicker. */
	QSet<QString> current;
	for (int i = 0; i < list_->count(); ++i) {
		const QListWidgetItem *item = list_->item(i);
		current.insert(rowKey(item->data(kHostRole).toString(),
				      item->data(kNameRole).toString(),
				      item->data(kOfflineRole).toBool()));
	}
	QSet<QString> next;
	for (const auto &c : cameras) {
		if (!c.host.isEmpty())
			next.insert(rowKey(c.host, c.name, c.offline));
	}
	if (current == next)
		return;

	/* Remember the highlighted row and put it back after the rebuild: clear()
	   replaces every item and would otherwise drop the operator's selection even
	   though the same camera is still in the list. setCurrentItem() does not
	   emit itemClicked, so this cannot re-open the camera. */
	const QString selectedHost =
	    list_->currentItem()
		? list_->currentItem()->data(kHostRole).toString()
		: QString();

	QListWidgetItem *toSelect = nullptr;
	list_->clear();
	for (const auto &c : cameras) {
		if (c.host.isEmpty())
			continue;
		auto *item = new QListWidgetItem();
		item->setData(kHostRole, c.host);
		item->setData(kNameRole, c.name);
		item->setData(kOfflineRole, c.offline);
		applyRow(item);
		list_->addItem(item);
		if (!selectedHost.isEmpty() && c.host == selectedHost)
			toSelect = item;
	}
	if (toSelect)
		list_->setCurrentItem(toSelect);

	hasRows_ = !next.isEmpty();
	updateEmptyState();
}

/* Spec §1, L11: a row's offline state can change on a click, and rebuilding the
   list for that would drop the selection the operator just made. */
void ZcCameraRail::setHostOffline(const QString &host, bool offline)
{
	for (int i = 0; i < list_->count(); ++i) {
		QListWidgetItem *item = list_->item(i);
		if (item->data(kHostRole).toString() != host)
			continue;
		if (item->data(kOfflineRole).toBool() == offline)
			return;
		item->setData(kOfflineRole, offline);
		applyRow(item);
		return;
	}
}

void ZcCameraRail::setAddedHosts(const QSet<QString> &added)
{
	if (added_ == added)
		return;
	added_ = added;
	/* Update the rows in place: rebuilding the list would drop the selection
	   (and the connected camera) on every OBS source change. */
	for (int i = 0; i < list_->count(); ++i)
		applyRow(list_->item(i));
}

/* ---------------------------------------------------------------- */
/* ZcHud                                                             */
/* ---------------------------------------------------------------- */

ZcHud::ZcHud(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_TransparentForMouseEvents, false);
	setStyleSheet("background:rgba(0,0,0,0);");

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);

	/* Top row: REC badge + mode. */
	auto *topRow = new QHBoxLayout;
	recBadge_ = new QLabel("● REC", this);
	recBadge_->setStyleSheet(
	    "color:#e53935;background:rgba(0,0,0,.45);border-radius:3px;"
	    "padding:2px 8px;font-weight:bold;");
	recBadge_->hide();
	durationLabel_ = new QLabel("", this);
	durationLabel_->setStyleSheet(
	    "color:#fff;background:rgba(0,0,0,.45);border-radius:3px;"
	    "padding:2px 8px;");
	remainingLabel_ = new QLabel("", this);
	remainingLabel_->setStyleSheet(
	    "color:#8a9bb2;background:rgba(0,0,0,.45);border-radius:3px;"
	    "padding:2px 8px;");
	modeLabel_ = new QLabel("", this);
	modeLabel_->setStyleSheet(
	    "color:#eef2ed;background:rgba(0,0,0,.45);border-radius:3px;"
	    "padding:2px 8px;");
	topRow->addWidget(recBadge_);
	topRow->addWidget(durationLabel_);
	topRow->addWidget(remainingLabel_);
	topRow->addStretch();
	topRow->addWidget(modeLabel_);
	layout->addLayout(topRow);

	layout->addStretch();

	/* Bottom info chips row. */
	auto *infoRow = new QHBoxLayout;
	QStringList labels = {"ISO", "SHUTTER", "EV", "WB", "IRIS", "FPS",
			      "RES", "TEMP"};
	for (const QString &lbl : labels) {
		auto *chip = new QLabel(lbl + " —", this);
		chip->setStyleSheet(
		    "color:#eef2ed;background:rgba(0,0,0,.45);"
		    "border-radius:3px;padding:2px 8px;font-size:11px;");
		infoRow->addWidget(chip);
		chips_.append(chip);
	}
	infoRow->addStretch();
	layout->addLayout(infoRow);
}

void ZcHud::setRecording(bool rec)
{
	recBadge_->setVisible(rec);
}

void ZcHud::setMode(const QString &mode)
{
	modeLabel_->setText(mode);
}

void ZcHud::setTemperature(double celsius)
{
	if (celsius < 0)
		return;
	if (chips_.size() < 8)
		return;
	chips_[7]->setText("TEMP " +
			   QString::number(celsius, 'f', 1) + "°C");
}

void ZcHud::setInfo(const QString &iso, const QString &shutter,
		    const QString &ev, const QString &wb, const QString &iris,
		    const QString &fps, const QString &res, const QString &temp)
{
	QStringList vals = {iso, shutter, ev, wb, iris, fps, res, temp};
	QStringList labels = {"ISO", "SHUTTER", "EV", "WB", "IRIS", "FPS",
			      "RES", "TEMP"};
	for (int i = 0; i < chips_.size(); ++i) {
		QString v = i < vals.size() ? vals[i] : QString();
		QString label = labels[i];
		QString display = v.isEmpty() ? (label + " -") : (label + " " + v);
		chips_[i]->setText(display);
		chips_[i]->setProperty("label", label);
		if (!v.isEmpty()) {
			chips_[i]->setStyleSheet(
			    "color:rgba(216,184,120,.9);background:rgba(0,0,0,.45);"
			    "border-radius:3px;padding:2px 8px;font-size:11px;"
			    "font-weight:bold;");
		}
	}
}

void ZcHud::setDuration(int seconds)

{
	int h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
	durationLabel_->setText(QString("%1:%2:%3")
					.arg(h, 2, 10, QChar('0'))
					.arg(m, 2, 10, QChar('0'))
					.arg(s, 2, 10, QChar('0')));
}

void ZcHud::setRemaining(int seconds)
{
	if (seconds <= 0) {
		remainingLabel_->clear();
		return;
	}
	int h = seconds / 3600, m = (seconds % 3600) / 60;
	remainingLabel_->setText(QString("%1h %2m").arg(h).arg(m));
}

/* ---------------------------------------------------------------- */
/* ZcSettingsPanel                                                   */
/* ---------------------------------------------------------------- */

ZcSettingsPanel::ZcSettingsPanel(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);

	auto *header = new QHBoxLayout;
	auto *title = new QLabel(ztr("ZCameraPlugin.Dock.Settings"), this);
	title->setStyleSheet("color:#8a9bb2;font-size:11px;letter-spacing:1px;");
	header->addWidget(title);
	header->addStretch(1);
	auto *refresh = new QPushButton(ztr("ZCameraPlugin.Dock.Refresh"), this);
	refresh->setToolTip(ztr("ZCameraPlugin.Dock.RefreshTip"));
	refresh->setStyleSheet(
	    "QPushButton { color:#eef2ed; background:#2b3a38; border:1px solid "
	    "#3c514e; border-radius:4px; padding:3px 10px; font-size:11px; }"
	    "QPushButton:hover { background:#354845; }");
	header->addWidget(refresh);
	layout->addLayout(header);

	/* The stream bar sits above the group list rather than in it: how the
	   camera encodes the network stream is the setting an operator reaches for
	   most, so it must be there without navigating to a group (spec §2). */
	buildStreamBar(layout);

	/* In-page login / HTTPS fallback: shown when the camera answers 401 (need a
	   login) or is HTTPS-only with no TLS backend; hidden once connected. */
	loginPage_ = new QWidget(this);
	{
		auto *lp = new QVBoxLayout(loginPage_);
		lp->setContentsMargins(8, 8, 8, 8);
		promptHint_ = new QLabel(loginPage_);
		promptHint_->setWordWrap(true);
		promptHint_->setStyleSheet(
		    "color:#eef2ed;background:#2b3a38;border-radius:4px;padding:8px;");
		lp->addWidget(promptHint_);
		/* Rejection notice: hidden until a login fails, then shown in red under
		   the hint so a wrong user/password is reported instead of the form
		   silently staying up (bug list "wrong credentials are not reported"). */
		loginError_ = new QLabel(loginPage_);
		loginError_->setWordWrap(true);
		loginError_->setStyleSheet("color:#e58a8a;padding:0 8px;");
		loginError_->hide();
		lp->addWidget(loginError_);
		/* The username/password rows (labels + inputs) + login button live in
		   their own widget so the HTTPS notice can hide the entire form at once
		   without leaving the column labels behind. */
		loginFields_ = new QWidget(loginPage_);
		auto *lf = new QVBoxLayout(loginFields_);
		lf->setContentsMargins(0, 0, 0, 0);
		auto *uRow = new QHBoxLayout;
		uRow->addWidget(new QLabel(ztr("ZCameraPlugin.Dock.AuthUsername"),
					  loginFields_));
		loginUser_ = new QLineEdit(loginFields_);
		uRow->addWidget(loginUser_, 1);
		lf->addLayout(uRow);
		auto *pRow = new QHBoxLayout;
		pRow->addWidget(new QLabel(ztr("ZCameraPlugin.Dock.AuthPassword"),
					  loginFields_));
		loginPass_ = new QLineEdit(loginFields_);
		loginPass_->setEchoMode(QLineEdit::Password);
		/* A reveal toggle: the typed password stays visible on demand
		   instead of only dots (bug list "no show/hide toggle"). */
		passReveal_ = new QToolButton(loginFields_);
		passReveal_->setCheckable(true);
		passReveal_->setText(ztr("ZCameraPlugin.Dock.AuthShow"));
		passReveal_->setToolTip(ztr("ZCameraPlugin.Dock.AuthShowTip"));
		const auto applyReveal = [this](bool shown) {
			loginPass_->setEchoMode(shown ? QLineEdit::Normal
						      : QLineEdit::Password);
			passReveal_->setText(ztr(shown ? "ZCameraPlugin.Dock."
							 "AuthHide"
						       : "ZCameraPlugin.Dock."
							 "AuthShow"));
		};
		applyReveal(false);
		connect(passReveal_, &QToolButton::toggled, this, applyReveal);
		pRow->addWidget(loginPass_, 1);
		pRow->addWidget(passReveal_);
		lf->addLayout(pRow);
		loginBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.AuthLogin"), loginFields_);
		lf->addWidget(loginBtn_, 0, Qt::AlignRight);
		lp->addWidget(loginFields_);
		connect(loginBtn_, &QPushButton::clicked, this, [this]() {
			emit loginRequested(loginUser_->text(), loginPass_->text());
		});
	}
	loginPage_->hide();
	layout->addWidget(loginPage_);

	subtabList_ = new QListWidget(this);
	subtabList_->setMaximumWidth(150);
	populateGroups();

	rowsContainer_ = new QWidget(this);
	auto *rowLayout = new QVBoxLayout(rowsContainer_);
	rowLayout->setAlignment(Qt::AlignTop);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setWidget(rowsContainer_);

	auto *h = new QHBoxLayout;
	h->addWidget(subtabList_);
	h->addWidget(scroll, 1);
	layout->addLayout(h, 1);

	/* currentTextChanged carries the item's *showing* text, which is the
	   localized label catalogLabel() produced ("wb" -> "White Balance"), not
	   the catalog id. Wiring that into onGroupSelected made every group read
	   ask the camera for `catalog=White Balance`: an unknown catalog answers
	   200 with an empty cfgs list, so the group resolved to no keys and the
	   pane rendered empty (bug report "settings cannot be read, they come back
	   empty"; the OBS log shows the raw reads catalog=Lens, catalog=Exposure,
	   catalog=White%20Balance, ...). The id is the item's Qt::UserRole data;
	   read it from the item instead of its text. */
	connect(subtabList_, &QListWidget::currentItemChanged, this,
		[this](QListWidgetItem *current, QListWidgetItem *) {
			if (current)
				onGroupSelected(
					current->data(Qt::UserRole).toString());
		});
	connect(refresh, &QPushButton::clicked, this,
		&ZcSettingsPanel::refreshGroup);

	/* Default to the first group so its rows are visible immediately rather
	   than leaving the rows pane blank until the user clicks a group. This
	   runs last: it emits currentItemChanged, so the rows pane and the
	   connection above must already exist or the group would never load. */
	if (subtabList_->count() > 0)
		subtabList_->setCurrentRow(0);
}

/* The camera answered 401: show the in-page login form and hide the settings
   rows until a login is entered and accepted. */
void ZcSettingsPanel::showLoginPrompt(const QString &host)
{
	loginHost_ = host;
	if (loginError_)
		loginError_->hide();
	/* Re-arm a form that a previous run of failed logins locked out: a fresh
	   prompt is a fresh chance (bug list #3). */
	loginBtn_->setEnabled(true);
	loginUser_->setEnabled(true);
	loginPass_->setEnabled(true);
	passReveal_->setEnabled(true);
	promptHint_->setText(ztr("ZCameraPlugin.Dock.AuthNeeded").arg(host));
	loginFields_->show();
	loginPage_->show();
	subtabList_->hide();
	rowsContainer_->hide();
	loginUser_->setFocus();
}

/* The camera rejected the submitted credentials: show the form again with a
   notice so the operator knows the login was tried and failed rather than the
   click doing nothing (bug list "wrong credentials are not reported"). The
   typed password is KEPT (bug list #30): clearing it made a retry start from
   nothing and hid what had actually been sent. */
void ZcSettingsPanel::showLoginError()
{
	if (loginError_) {
		loginError_->setText(ztr("ZCameraPlugin.Dock.AuthFailed"));
		loginError_->show();
	}
	loginFields_->show();
	loginPage_->show();
	subtabList_->hide();
	rowsContainer_->hide();
	loginPass_->setFocus();
}

/* Five rejected logins in a row: the camera has locked the account (bug list
   #3 — after 5 wrong passwords the camera stops accepting logins and the old
   build fell through to an empty settings page). Say so, and take the form
   out of action until a fresh prompt re-arms it. */
void ZcSettingsPanel::showLoginLocked()
{
	if (loginError_) {
		loginError_->setText(ztr("ZCameraPlugin.Dock.AuthLocked"));
		loginError_->show();
	}
	loginBtn_->setEnabled(false);
	loginUser_->setEnabled(false);
	loginPass_->setEnabled(false);
	passReveal_->setEnabled(false);
	loginFields_->show();
	loginPage_->show();
	subtabList_->hide();
	rowsContainer_->hide();
}

/* HTTPS-only camera with no TLS backend: tell the operator to switch to HTTP,
   in the pane (no modal dialog). */
void ZcSettingsPanel::showTlsPrompt(const QString &host)
{
	loginHost_ = host;
	promptHint_->setText(ztr("ZCameraPlugin.Dock.TlsHttpHint").arg(host));
	loginFields_->hide();
	loginPage_->show();
	subtabList_->hide();
	rowsContainer_->hide();
}

void ZcSettingsPanel::hidePrompts()
{
	if (loginPage_)
		loginPage_->hide();
	subtabList_->show();
	rowsContainer_->show();
}

void ZcSettingsPanel::setClient(ZcCameraClient *client)
{
	client_ = client;
	/* Another camera's document: the bar must not be painted from it while the
	   new one is being read, and the index has to be discovered again because
	   the two cameras need not run the same firmware. */
	streamIndex_.clear();
	streamDoc_ = QJsonObject();
	updateStreamBar();
	if (client_) {
		connect(client_, &ZcCameraClient::groupRefreshed, this,
			&ZcSettingsPanel::onGroupRefreshed);
		/* The /info reply is what says whether this camera has a pan/tilt
		   head, and therefore whether it has PTZ settings at all. */
		connect(client_, &ZcCameraClient::cameraInfoChanged, this, [this]() {
			populateGroups();
			buildRows(catalog_);
			if (!client_)
				return;
			/* The stream bar is always visible, so it is always re-read. */
			loadStream();
			if (!catalog_.isEmpty())
				client_->readGroup(catalog_, nullptr);
		});
	}
	/* The camera changed: rebuild the group list for it (a camera without a
	   pan/tilt head has no PTZ group), repaint from the (empty) cache, then
	   load. */
	populateGroups();
	buildRows(catalog_);
}

void ZcSettingsPanel::refreshGroup()
{
	if (!client_)
		return;
	loadStream();
	if (!catalog_.isEmpty())
		client_->readGroup(catalog_, [](bool) {});
}

void ZcSettingsPanel::writeKey(const QString &key, const QString &value)
{
	if (!client_)
		return;
	client_->writeSetting(key, value);
	/* Re-read only the key that changed, never the whole PTZ catalog: a catalog
	   read comes back through groupRefreshed and rebuilds every row, which tears
	   down the control the operator is still using. The row already shows the new
	   value; the cache is all that needs to follow the camera. */
	if (client_->settingGroupKeys(QLatin1String("ptz")).contains(key))
		client_->readSetting(key, [](const ZcSettingValue &) {});
	/* HTTPS and identity auth restart the camera's web service, and a
	   network-type change can move it to another address: the reply never
	   arrives and the connection drops. The dock is told to wait for the camera
	   and reconnect instead of leaving the pane dead (bug 8). */
	if (zc::settingRebootsCamera(key))
		emit cameraRestarting();
	/* The camera follows the main resolution / recording fps when it lays out
	   its network stream: changing either here silently changes what stream1
	   offers, so re-read the stream document a moment later and repaint the
	   top bar (bug list "changing the main resolution/fps does not refresh
	   stream1"). The camera needs a beat to apply the write first. */
	if (key == QLatin1String("resolution") ||
	    key == QLatin1String("project_fps"))
		QTimer::singleShot(600, this, [this]() { loadStream(); });
}

/* Read the stream document and repaint the bar. The first read of a camera also
   discovers which index its firmware offers: `app_stream1` when it answers with
   a document, `stream1` otherwise (spec §2 Stream). The index then stays fixed
   for as long as that camera is selected. Every reply is guarded on the client
   and the index it was asked for, so a slow reply can never paint the bar from
   another camera's — or another index's — numbers. */
void ZcSettingsPanel::loadStream()
{
	if (!client_ || !client_->isConnected())
		return;
	ZcCameraClient *c = client_;

	if (streamIndex_.isEmpty()) {
		c->streamSettingQuery(
		    zc::kAppStreamIndex, [this, c](const QJsonObject &doc) {
			    if (c != client_)
				    return;
			    if (zc::isStreamDocument(doc)) {
				    streamIndex_ = zc::kAppStreamIndex;
				    streamDoc_ = doc;
				    updateStreamBar();
				    return;
			    }
			    /* Older firmware: that index does not exist, so read the one
			       it does have. */
			    streamIndex_ = zc::kLegacyStreamIndex;
			    loadStream();
		    });
		return;
	}

	const QString index = streamIndex_;
	c->streamSettingQuery(index, [this, c, index](const QJsonObject &doc) {
		if (c != client_ || index != streamIndex_ ||
		    !zc::isStreamDocument(doc))
			return;
		streamDoc_ = doc;
		updateStreamBar();
	});
}

void ZcSettingsPanel::writeStreamParams(
    const QList<QPair<QString, QString>> &params)
{
	if (!client_ || streamIndex_.isEmpty())
		return;
	ZcCameraClient *c = client_;
	const QString index = streamIndex_;
	c->streamSettingWrite(index, params,
			      [this, c, index](const QJsonObject &doc) {
		/* Repaint from the camera's own answer: a field it refuses (an
		   out-of-range bitrate, a value outside its list) then snaps back
		   instead of showing a value the camera never took. */
		if (c != client_ || index != streamIndex_ ||
		    !zc::isStreamDocument(doc))
			return;
		streamDoc_ = doc;
		updateStreamBar();
	});
}

/* Rebuild the group list from the schema. The PTZ group is camera-side PTZ
   configuration, so a camera without a pan/tilt head has none and must not be
   offered it (spec §2); the group appears as soon as /info says it has one.
   Safe to call before the rows pane exists. */
void ZcSettingsPanel::populateGroups()
{
	const QString keep = catalog_;
	const bool ptz = client_ && client_->cameraSupportsPtz();
	/* Model-specific capability: an E2_F6_Pro or Avatar has no identity-auth or
	   HTTPS setting, so the security group is not offered there. Turning one on
	   through the plugin blanks every setting and cannot be undone from either
	   side (bug 7). An empty model ("not read yet") keeps the group, and the
	   list is rebuilt when /info arrives. */
	const QString model = client_ ? client_->cameraModel() : QString();
	/* The rebuild must not repaint the rows pane through onGroupSelected: the
	   caller decides what to render next. */
	QSignalBlocker blocker(subtabList_);
	subtabList_->clear();
	for (const auto &cat : zc::catalogs()) {
		if (zc::catalogRequiresPtz(cat.id) && !ptz)
			continue;
		/* The camera's own `feature` map is the authority: a body that
		   reports lens_ctrl_ptz off has no PTZ group at all, whatever its
		   model name says. */
		if (!zc::catalogSupportedByFeatures(
			cat.id, client_ ? client_->cameraFeatures()
					: zc::ZcFeatures()))
			continue;
		if (!zc::catalogSupportedByModel(cat.id, model))
			continue;
		auto *item = new QListWidgetItem(catalogLabel(cat.id), subtabList_);
		item->setData(Qt::UserRole, cat.id);
	}
	/* Keep the selection when that group still exists, else fall back to the
	   first one, so a rebuild does not leave the pane showing a group the list
	   no longer offers. */
	int row = -1;
	for (int i = 0; i < subtabList_->count(); ++i) {
		if (subtabList_->item(i)->data(Qt::UserRole).toString() == keep) {
			row = i;
			break;
		}
	}
	if (row < 0 && subtabList_->count() > 0)
		row = 0;
	if (row >= 0)
		subtabList_->setCurrentRow(row);
	catalog_ = row >= 0 ? subtabList_->item(row)->data(Qt::UserRole).toString()
			    : QString();
}

void ZcSettingsPanel::selectGroup(const QString &catalog)
{
	for (int i = 0; i < subtabList_->count(); ++i) {
		if (subtabList_->item(i)->data(Qt::UserRole).toString() ==
		    catalog) {
			subtabList_->setCurrentRow(i);
			return;
		}
	}
}

void ZcSettingsPanel::onGroupSelected(const QString &catalog)
{
	if (catalog.isEmpty())
		return;
	catalog_ = catalog;
	buildRows(catalog);
	/* Rows are built from the cached definitions; the reply that follows
	   arrives as groupRefreshed and rebuilds them with the current values. */
	if (client_)
		client_->readGroup(catalog, nullptr);
}

void ZcSettingsPanel::onGroupRefreshed(const QString &catalog)
{
	/* A late reply for a group the user already left must not repaint the
	   rows pane. */
	if (catalog == catalog_)
		buildRows(catalog);
}

/* Removes every item from a rows layout, including the widgets owned by the
   nested per-row layouts: deleting the nested layout alone would leave its
   widgets parented to the container and still visible. */
static void clearRows(QLayout *layout)
{
	while (QLayoutItem *item = layout->takeAt(0)) {
		if (QWidget *w = item->widget())
			delete w;
		else if (QLayout *l = item->layout())
			clearRows(l);
		delete item;
	}
}

void ZcSettingsPanel::buildRows(const QString &catalog)
{
	/* Clear existing rows: delete every direct child widget and their
	   layouts. */
	QBoxLayout *rowLayout =
		qobject_cast<QBoxLayout *>(rowsContainer_->layout());
	if (!rowLayout)
		return;
	clearRows(rowLayout);
	rowWidgets_.clear();

	auto addHint = [&](const QString &text) {
		auto *hint = new QLabel(text, rowsContainer_);
		hint->setWordWrap(true);
		hint->setStyleSheet("color:#8a9bb2;padding:8px;");
		rowLayout->addWidget(hint);
	};

	if (catalog.isEmpty()) {
		addHint(ztr("ZCameraPlugin.Dock.SelectGroup"));
		return;
	}
	if (!client_) {
		addHint(ztr("ZCameraPlugin.Dock.SelectCamera"));
		return;
	}
	if (!client_->isConnected()) {
		addHint(ztr("ZCameraPlugin.Dock.NotConnected"));
		return;
	}

	/* Render the keys the camera reported for this catalog, in its order.
	   An empty list means the group was not read yet, or the camera does not
	   have this catalog (getbatch answers code -1 with no cfgs). */
	const QStringList keys = client_->settingGroupKeys(catalog);
	if (keys.isEmpty()) {
		addHint(ztr("ZCameraPlugin.Dock.NoSettingsForGroup"));
		return;
	}

	/* The keys the camera only exposes when white balance is in its expert
	   mode (see the read-only check in the loop below). */
	auto isExpertWbKey = [](const QString &key) {
		static const char *const kKeys[] = {"mwb", "tint", "mwb_r",
						    "mwb_g", "mwb_b"};
		for (const char *k : kKeys) {
			if (key == QLatin1String(k))
				return true;
		}
		return false;
	};

	for (const QString &key : keys) {
		/* The WDR setting is not offered by the camera's web UI (it is reported
		   under the raw id `compose_mode`); showing it here only confuses, so it
		   is hidden (bug list "remove WDR setting"). Likewise the raw `ev` row
		   duplicates `ev_choice` (both set EV, and editing one does not update the
		   other); only `ev_choice` is shown (bug list "keep EV Choice"). The image
		   group also reports `ois_mode` and `vignette`, which have no counterpart
		   on the camera or its web UI (they only ever come back read-only), so
		   they are hidden too (bug list "ois_mode/vignette are not on the
		   camera"). `raw_over_hdmi` is removed from the UI on purpose: RAW has
		   nothing to do with live streaming, and toggling it makes the camera
		   restart its web service, which breaks the stream/audio in the plugin
		   (bug list "remove RAW over HDMI", RD feedback). `ptz_limit` is the
		   Range Limit status, which the PTZ tab's own Range Limit group owns
		   (checkbox + the four edge sliders); rendering it here as a second,
		   independent combo duplicated the control and let the two disagree
		   (bug list "the range limit status appears twice"). The PTZ group
		   still reports `preset_adjust_speed_with_zoom` ("Adjust speed with
		   zoom"), but the camera's own page never renders that row on the PTZ
		   models we ship for, so the plugin drops it too instead of showing a
		   setting the operator cannot find on the camera (bug 63). */
		if (key == QLatin1String("compose_mode") ||
		    key == QLatin1String("ev") ||
		    key == QLatin1String("ois_mode") ||
		    key == QLatin1String("vignette") ||
		    key == QLatin1String("raw_over_hdmi") ||
		    key == QLatin1String("ptz_limit") ||
		    key == QLatin1String("preset_adjust_speed_with_zoom") ||
		    /* The 1.0.6 firmware lists these four PTZ keys in getbatch
		       but silently ignores writes to them (bug list #84: the PTZ
		       tab's flip-auto and rise/fall/s-curve rows accept input
		       without any effect). Until a firmware honors them, hide
		       them rather than show dead controls. */
		    key == QLatin1String("ptz_flip_auto") ||
		    key == QLatin1String("pt_rise_curve") ||
		    key == QLatin1String("pt_fall_curve") ||
		    key == QLatin1String("pt_s_curve"))
			continue;
		/* A row the body has no option for is not offered: the camera's own
		   `feature` map says which controls exist (Wi-Fi, pre-roll, EIS,
		   genlock, line-in, VISCA, the focus family on a body with no focus
		   motor, ...). The table is opt-in — a key it does not name is always
		   shown — so nothing is withdrawn on a camera that does not report
		   the flag at all. */
		if (!zc::settingSupportedByFeatures(key,
						   client_->cameraFeatures()))
			continue;
		ZcSettingValue def;
		if (!client_->settingDefinition(key, &def))
			continue;
		/* Unsupported (code -1) keys are hidden; read-only (ro=1) keys are shown
		   but disabled (greyed) so the operator can see the value without being
		   able to change it. */
		if (!def.supported)
			continue;
		/* Expert white balance — `mwb` (manual Kelvin), `tint` and the three
		   expert gains — exists only when the body's white-balance mode has
		   that mode, and the camera says so itself: in a mode without them it
		   answers these keys read-only. So a read-only answer hides the row
		   instead of greying it (bug 40: the E2_F6_Pro has no expert mode,
		   yet the R/G/B gains were drawn, greyed and useless). The rows come
		   back on the next catalog read when the mode offers them again. */
		if (def.readOnly && isExpertWbKey(key))
			continue;
		const bool readOnly = def.readOnly ||
				      /* A time-lapse interval only matters when the camera
					 is actually time-lapsing; in the normal record mode the
					 field is meaningless and must be greyed (bug list
					 "Video_tl_interval grey when record_mode = Normal"). */
				      (key == QLatin1String("video_tl_interval") &&
				       client_->settingValue(QLatin1String("record_mode")) ==
					       QLatin1String("Normal"));

		auto *row = new QHBoxLayout;
		/* `ptz_common_time` carries milliseconds on the wire but the camera's own
		   web UI shows seconds; display it in seconds and convert on write, and
		   make the unit explicit (bug list "common time unit differs from the web
		   and no unit is shown"). */
		const bool timeInSeconds = key == QLatin1String("ptz_common_time");
		const double scale = timeInSeconds ? 1000.0 : 1.0;
		auto *label = new QLabel(
		    timeInSeconds ? settingLabel(key) + QStringLiteral(" (s)")
				  : settingLabel(key),
		    rowsContainer_);
		label->setMinimumWidth(170);
		label->setStyleSheet("color:#eef2ed;");
		/* Bug list #46: in the Chinese UI the tooltip must read Chinese too —
		   it repeats the localized label (a row whose label is elided by a
		   narrow pane stays readable), not the raw camera id. */
		label->setToolTip(settingLabel(key));
		row->addWidget(label);
		row->addStretch(1);

		QWidget *control = nullptr;
		if (def.type == 1 && !def.choices.isEmpty()) {
			/* Choice -> combo box; set the value before connecting so the
			   initial fill does not fire a spurious write. The face of each
			   entry is the camera's own Chinese word where its language pack
			   has one (bug 35); the value written back is always the raw
			   string the camera sent, carried as the entry's data. */
			auto *combo = new QComboBox(rowsContainer_);
			for (const QString &choice : def.choices)
				combo->addItem(zcValueLabel(choice), choice);
			const int current = combo->findData(def.value);
			if (current >= 0)
				combo->setCurrentIndex(current);
			connect(combo, &QComboBox::currentIndexChanged, this,
				[this, key, combo](int row) {
					if (row < 0)
						return;
					writeKey(key, combo->itemData(row).toString());
				});
			/* Changing the unit must reveal the matching speed/time row right
			   away instead of waiting for a refresh (bug list "unit switch does
			   not show the matching speed/time row"). */
			if (key == QLatin1String("ptz_common_speed_unit"))
				connect(combo, &QComboBox::currentTextChanged, this,
					[this](const QString &) {
						updatePtzCommonRows();
					});
			control = combo;
		} else if (def.type == 2 && def.max > def.min && def.step > 0 &&
			   def.step == std::floor(def.step)) {
			/* Range -> slider + live gold value label. */
			auto *wrap = new QWidget(rowsContainer_);
			auto *wl = new QHBoxLayout(wrap);
			wl->setContentsMargins(0, 0, 0, 0);
			auto *slider = new QSlider(Qt::Horizontal, wrap);
			slider->setMinimum(static_cast<int>(def.min / scale));
			slider->setMaximum(static_cast<int>(def.max / scale));
			slider->setSingleStep(
			    qMax(1, static_cast<int>(def.step / scale)));
			slider->setValue(static_cast<int>(def.value.toInt() / scale));
			auto *val = new QLabel(QString::number(slider->value()), wrap);
			val->setMinimumWidth(48);
			val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			val->setStyleSheet("color:#d8b878;");
			connect(slider, &QSlider::valueChanged, val,
				[val](int v) { val->setText(QString::number(v)); });
			connect(slider, &QSlider::sliderReleased, this,
				[this, key, slider, scale] {
					writeKey(key, QString::number(
							  static_cast<qint64>(
							      slider->value() * scale)));
				});
			wl->addWidget(slider, 1);
			wl->addWidget(val);
			control = wrap;
		} else {
			/* Text (type 3), and any key whose metadata cannot drive a
			   combo or slider. */
			auto *edit = new QLineEdit(def.value, rowsContainer_);
			connect(edit, &QLineEdit::editingFinished, this,
				[this, key, edit] {
					writeKey(key, edit->text());
				});
			control = edit;
		}

		if (readOnly)
			control->setEnabled(false);

		control->setMinimumWidth(200);
		control->setMaximumWidth(260);
		row->addWidget(control);
		rowLayout->addLayout(row);
		rowWidgets_.insert(key, {label, control});
	}

	/* Apply the PTZ common-speed-unit linkage to the rows just built. */
	updatePtzCommonRows();
}

/* The PTZ group's "common speed unit" selects which of the two common values
   (speed index or recall time) applies. The camera's web UI shows only the one
   in use; until now both stayed visible here (bug list "switching the common
   speed unit does not show the matching speed/time row"). */
void ZcSettingsPanel::updatePtzCommonRows()
{
	const auto unitRow =
	    rowWidgets_.constFind(QLatin1String("ptz_common_speed_unit"));
	if (unitRow == rowWidgets_.constEnd())
		return; /* not the PTZ group */

	/* Prefer the combo on screen (a click has not been round-tripped through the
	   camera yet); fall back to the cached value. unit 1 = Time. */
	QString unitText;
	if (unitRow.value().size() > 1)
		if (auto *combo = qobject_cast<QComboBox *>(unitRow.value().at(1)))
			unitText = combo->currentText();
	if (unitText.isEmpty() && client_)
		unitText = client_->settingValue(
		    QLatin1String("ptz_common_speed_unit"));

	const bool timeMode =
	    unitText == QLatin1String("1") ||
	    unitText.contains(QLatin1String("time"), Qt::CaseInsensitive);

	auto show = [this](const char *key, bool visible) {
		for (QWidget *w : rowWidgets_.value(QLatin1String(key)))
			w->setVisible(visible);
	};
	show("ptz_common_speed", !timeMode);
	show("ptz_common_time", timeMode);
}

/* A JSON array of choices as a string list; the lists are numbers for fps and
   strings for the encoders and resolutions. */
static QStringList jsonStringList(const QJsonValue &value)
{
	QStringList out;
	for (const QJsonValue &entry : value.toArray())
		out << (entry.isString() ? entry.toString()
					 : QString::number(entry.toInt()));
	return out;
}

/* Fill one choice box from the camera's own list. A current value the list does
   not mention is added, so the box can never misrepresent the camera; a box the
   camera offers no list for (older firmware) or marks read-only is shown but not
   selectable, because that value is the camera's own setting. */
static void fillChoice(QComboBox *box, const QStringList &choices,
		       const QString &current, bool readOnly)
{
	box->clear();
	if (choices.isEmpty()) {
		if (!current.isEmpty())
			box->addItem(current);
		box->setEnabled(false);
		return;
	}
	QStringList items = choices;
	if (!current.isEmpty() && !items.contains(current))
		items.prepend(current);
	box->addItems(items);
	box->setCurrentText(current);
	box->setEnabled(!readOnly);
}

/* The stream bar, at the top of the Settings tab (spec §2).

   It is not a settings group: the encoder's document is not a getbatch catalog,
   and how the camera encodes the network stream is the first thing an operator
   changes, so it must be visible without navigating to a group. The controls are
   created once and only ever updated in place — a write's reply arrives while
   the operator may still have a box open, and rebuilding the row would close
   it. */
void ZcSettingsPanel::buildStreamBar(QBoxLayout *outer)
{
	auto *frame = new QFrame(this);
	frame->setObjectName("StreamBar");
	frame->setStyleSheet(
	    "#StreamBar { background:#1b2422; border:1px solid #3c514e;"
	    " border-radius:4px; }"
	    "#StreamBar QLabel { color:#8a9bb2; font-size:11px; }"
	    "#StreamBar QComboBox { color:#eef2ed; background:#2b3a38;"
	    " border:1px solid #3c514e; border-radius:3px; padding:1px 4px;"
	    " font-size:11px; }"
	    "#StreamBar QComboBox:disabled { color:#6b7a90; }"
	    "#StreamBar QComboBox QAbstractItemView { color:#eef2ed;"
	    " background:#2b3a38; selection-background-color:#d8b878;"
	    " selection-color:#1f2927; }");
	streamBar_ = frame;

	auto *bar = new QVBoxLayout(frame);
	bar->setContentsMargins(8, 6, 8, 6);
	bar->setSpacing(4);

	/* Row 1: what the stream is encoded as, plus the index it belongs to. */
	auto *row1 = new QHBoxLayout;
	row1->setSpacing(6);
	auto *title = new QLabel(ztr("ZCameraPlugin.Dock.Stream"), frame);
	title->setStyleSheet("color:#8a9bb2;font-size:11px;letter-spacing:1px;");
	row1->addWidget(title);

	auto addCombo = [&](const QString &label, QComboBox **box) {
		row1->addWidget(new QLabel(label, frame));
		auto *combo = new QComboBox(frame);
		combo->setMinimumWidth(92);
		row1->addWidget(combo);
		*box = combo;
	};
	addCombo(ztr("ZCameraPlugin.Dock.Codec"), &streamCodec_);
	addCombo(ztr("ZCameraPlugin.Dock.Resolution"), &streamRes_);
	addCombo(ztr("ZCameraPlugin.Dock.Fps"), &streamFps_);
	row1->addStretch(1);
	streamInfo_ = new QLabel(frame);
	streamInfo_->setStyleSheet("color:#8a9bb2;font-size:11px;");
	row1->addWidget(streamInfo_);
	bar->addLayout(row1);

	/* Row 2: the two numbers, each with its live gold value. */
	auto *row2 = new QHBoxLayout;
	row2->setSpacing(6);
	auto addSlider = [&](const QString &label, QSlider **slider, QLabel **value) {
		row2->addWidget(new QLabel(label, frame));
		auto *s = new QSlider(Qt::Horizontal, frame);
		s->setMinimumWidth(140);
		s->setMaximumWidth(220);
		row2->addWidget(s);
		auto *v = new QLabel(frame);
		v->setMinimumWidth(60);
		v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		v->setStyleSheet("color:#d8b878;font-size:11px;");
		row2->addWidget(v);
		*slider = s;
		*value = v;
	};
	addSlider(ztr("ZCameraPlugin.Dock.Bitrate"), &streamBitrate_, &streamBitrateValue_);
	addSlider(ztr("ZCameraPlugin.Dock.Gop"), &streamGop_, &streamGopValue_);
	row2->addStretch(1);
	bar->addLayout(row2);

	/* Writes go out on release (a drag must not send a request per pixel) and
	   the bar then repaints from the camera's own answer. */
	connect(streamBitrate_, &QSlider::valueChanged, this, [this](int v) {
		streamBitrateValue_->setText(ztr("ZCameraPlugin.Dock.Kbps").arg(v));
	});
	connect(streamBitrate_, &QSlider::sliderReleased, this, [this]() {
		writeStreamParams({{QStringLiteral("bitrate"),
				    QString::number(streamBitrate_->value() *
						    zc::kBitrateWriteScale)}});
	});
	connect(streamGop_, &QSlider::valueChanged, this, [this](int v) {
		streamGopValue_->setText(QString::number(v));
	});
	connect(streamGop_, &QSlider::sliderReleased, this, [this]() {
		writeStreamParams({{QStringLiteral("gop_n"),
				    QString::number(streamGop_->value())}});
	});
	/* The write names are the firmware's, not the document's field names
	   (spec §2 Stream): `venc` for the encoder, `width`+`height` for a
	   resolution. */
	connect(streamCodec_, &QComboBox::currentTextChanged, this,
		[this](const QString &text) {
			writeStreamParams({{QStringLiteral("venc"), text}});
		});
	connect(streamRes_, &QComboBox::currentTextChanged, this,
		[this](const QString &text) {
			writeStreamParams(zc::resolutionParams(text));
		});
	connect(streamFps_, &QComboBox::currentTextChanged, this,
		[this](const QString &text) {
			writeStreamParams({{QStringLiteral("fps"), text}});
		});

	outer->addWidget(frame);
	updateStreamBar();
}

/* Repaint the bar from the last document. Never a write: every control is
   filled with its signals blocked, so the operator's own change is the only
   thing that reaches the camera. */
void ZcSettingsPanel::updateStreamBar()
{
	if (!streamBar_)
		return;
	/* A body with no network encoder (the Avatar, bug 107) has no stream to
	   configure. Its /info names none of rtsp / rtmp / srt / webrtc, so the
	   bar is hidden outright instead of sitting there inert and accepting
	   writes that do nothing. */
	if (client_ && !zc::cameraHasStreamEncoder(client_->cameraFeatures())) {
		streamBar_->setVisible(false);
		return;
	}
	streamBar_->setVisible(true);
	QSignalBlocker blockCodec(streamCodec_);
	QSignalBlocker blockRes(streamRes_);
	QSignalBlocker blockFps(streamFps_);
	QSignalBlocker blockBitrate(streamBitrate_);
	QSignalBlocker blockGop(streamGop_);

	if (streamDoc_.isEmpty()) {
		/* Nothing read yet: leave the bar inert rather than showing values no
		   camera reported. */
		streamCodec_->clear();
		streamRes_->clear();
		streamFps_->clear();
		streamBitrate_->setRange(0, 0);
		streamGop_->setRange(0, 0);
		streamBitrateValue_->setText(QStringLiteral("—"));
		streamGopValue_->setText(QStringLiteral("—"));
		streamInfo_->setText(client_ && client_->isConnected() ? ztr("ZCameraPlugin.Dock.Reading")
								      : ztr("ZCameraPlugin.Dock.NoCamera"));
		streamBar_->setEnabled(false);
		return;
	}

	streamBar_->setEnabled(true);

	/* bitrate is kbps in the document and bps on the wire, and maxBitrate is not
	   a hard bound (an idle stream can report more than it), so the current
	   value is always kept representable. The minimum is the web UI's own
	   floor, 0.2 Mbps: the camera refuses anything lower, and with a smaller
	   minimum the bar snapped back from the far left, which read as "the
	   slider cannot reach its left end" (bug list "bitrate slider bounces at
	   the far left").

	   The minimum must NOT follow the current value. Using `bitrate` as the
	   lower bound made the thumb sit at the far-left end after every write
	   (the left end *was* the value just stored): a rightward drag then wrote
	   the minimum, and a drag to the maximum left a degenerate min == max
	   range that read as a permanently greyed bar (bug lists "the slider
	   jumps to its left end on every rightward drag" and "it greys at the
	   maximum"). */
	const int bitrate = streamDoc_.value("bitrate").toInt();
	const int bitrateMax = qMax(qMax(streamDoc_.value("maxBitrate").toInt(),
					 bitrate),
				    kStreamBitrateMin);
	streamBitrate_->setRange(kStreamBitrateMin, bitrateMax);
	streamBitrate_->setValue(qBound(kStreamBitrateMin, bitrate, bitrateMax));
	/* The encoder's bitrate and GOP cannot be changed while the stream is
	   running: the camera refuses the write and only reports the field
	   read-only afterwards, so on a freshly opened camera the two sliders used
	   to look writable and greyed themselves only after the first drag (bug
	   102, E2_F6_Pro). The running state is in the same document, so it is
	   what greys them from the start. */
	const bool streamRunning =
	    streamDoc_.value(QLatin1String("streaming")).toBool() ||
	    streamDoc_.value(QLatin1String("status"))
		.toString()
		.contains(QLatin1String("streaming"), Qt::CaseInsensitive);
	const QString lockedTip =
	    streamRunning ? ztr("ZCameraPlugin.Dock.StreamRunningTip")
			  : QString();
	streamBitrate_->setEnabled(!streamDoc_.value("bitrateRo").toBool() &&
				   !streamRunning);
	streamBitrate_->setToolTip(lockedTip);
	streamBitrateValue_->setText(ztr("ZCameraPlugin.Dock.Kbps").arg(bitrate));

	const int gop = streamDoc_.value("gop_n").toInt();
	streamGop_->setRange(1, qMax(streamDoc_.value("maxGop").toInt(), gop));
	streamGop_->setValue(gop);
	streamGop_->setEnabled(!streamDoc_.value("gopRo").toBool() &&
			       !streamRunning);
	streamGop_->setToolTip(lockedTip);
	streamGopValue_->setText(QString::number(gop));

	const QString resolution =
	    QStringLiteral("%1x%2")
		.arg(streamDoc_.value("width").toInt())
		.arg(streamDoc_.value("height").toInt());
	fillChoice(streamRes_, jsonStringList(streamDoc_.value("resolutionList")),
		   resolution, streamDoc_.value("resolutionRo").toBool());
	fillChoice(streamFps_, jsonStringList(streamDoc_.value("fpsList")),
		   QString::number(streamDoc_.value("fps").toInt()),
		   streamDoc_.value("fpsRo").toBool());
	/* The H.265 10-bit encoder is offered by newer firmware but selecting it
	   does not take effect on the camera side (bug list "H265_10bit cannot be
	   selected"; RD: block the option until the camera accepts it). The
	   firmware's label is not fixed — "H265_10bit", "H.265 10bit", a bare
	   "h265_10" have all been seen — so match any 265 label that carries a
	   10-bit marker rather than one exact string. */
	QStringList encoders = jsonStringList(streamDoc_.value("videoEncoderList"));
	encoders.erase(std::remove_if(encoders.begin(), encoders.end(),
				      [](const QString &e) {
					      const QString low = e.toLower();
					      return low.contains(QStringLiteral("10bit")) ||
						     low.contains(QStringLiteral("10-bit")) ||
						     low.contains(QStringLiteral("10_bit")) ||
						     (low.contains(QStringLiteral("265")) &&
						      low.contains(QStringLiteral("10")));
				      }),
		       encoders.end());
	fillChoice(streamCodec_, encoders,
		   streamDoc_.value("encoderType").toString(),
		   streamDoc_.value("encoderRo").toBool());

	/* Which index the firmware offered, how deep the stream is and whether it is
	   running: read-only, and the context for everything above. */
	QStringList info;
	info << streamIndex_;
	const QString bitwidth = streamDoc_.value("bitwidth").toString();
	if (!bitwidth.isEmpty())
		info << bitwidth;
	const QString status = streamDoc_.value("status").toString();
	if (!status.isEmpty())
		info << status;
	streamInfo_->setText(info.join(QStringLiteral(" · ")));
}

/* ---------------------------------------------------------------- */
/* ZcPtzPad                                                          */
/* ---------------------------------------------------------------- */

namespace {
constexpr qreal kPi = 3.1415926535897932384626433832795;

/* Per-sector (index 0..7) pan/tilt unit deltas; positive tilt = down,
   positive pan = right (matches ZcCameraClient::ptzMove). */
constexpr int kPanDelta[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kTiltDelta[8] = {0, 1, 1, 1, 0, -1, -1, -1};

/* The camera's /ctrl/pt takes a direction action, not pan/tilt numbers. Map a
   pan/tilt pair (each -1/0/1; +tilt = down, +pan = right) to that action. */
static const char *ptzActionFor(int pan, int tilt)
{
	static const char *const kActions[3][3] = {
		{"leftup", "up", "rightup"},       /* tilt -1 (up)   */
		{"left", "stop", "right"},         /* tilt 0         */
		{"leftdown", "down", "rightdown"}, /* tilt +1 (down) */
	};
	const int p = (pan > 0) - (pan < 0);
	const int t = (tilt > 0) - (tilt < 0);
	return kActions[t + 1][p + 1];
}

const QString kPtzPanelStyle =
    "QWidget#PtzPadWrapper { background:#1f2927; }"
    "QLabel { color:#8a9bb2; font-size:11px; letter-spacing:1px; }"
    "QPushButton { color:#eef2ed; background:#2b3a38; border:1px solid "
    "#3c514e; border-radius:4px; padding:6px 10px; font-size:11px; }"
    "QPushButton:hover { background:#354845; }"
    "QPushButton:pressed { background:#d8b878; color:#1f2927; }"
    "QSlider::groove:horizontal { height:4px; background:#3c514e; "
    "border-radius:2px; }"
    "QSlider::handle:horizontal { width:14px; height:14px; margin:-5px 0; "
    "border-radius:7px; background:#d8b878; }"
    /* The zoom rocker is vertical (see ZcPtzPad's constructor), so the groove
       and handle need their vertical rules too — the horizontal ones alone
       left it drawn as a bare 14px column. */
    "QSlider::groove:vertical { width:4px; background:#3c514e; "
    "border-radius:2px; }"
    "QSlider::handle:vertical { width:14px; height:14px; margin:0 -5px; "
    "border-radius:7px; background:#d8b878; }"
    /* The dial row's click-to-move target: a bare icon that fills in while it
       is armed, so the state reads at a glance without a label. */
    "QToolButton#ptzTarget { border:1px solid transparent; "
    "background:transparent; border-radius:4px; padding:3px; }"
    "QToolButton#ptzTarget:hover { background:#354845; }"
    "QToolButton#ptzTarget:checked { background:#2b3a38; "
    "border:1px solid #d8b878; }";
}

/* ------------------------------------------------------------------ */
/* ZcOverlayLink                                                       */
/* ------------------------------------------------------------------ */

/* A half-frame of click travel maps to this many degrees. Nothing in any
   snapshot on this firmware carries an absolute field of view (the lens
   catalog has no zoom position either), so the mapping is a fixed gain and
   the move is a *relative* goto: click, the head goes part of the way, click
   again to refine. That is how the camera's own page behaves too — its
   EzFraming MODE_CLICK crosshair writes nothing but a relative target. */
constexpr double kClickPanDeg = 25.0;
constexpr double kClickTiltDeg = 15.0;
/* Down the middle of the rocker: fast enough to feel immediate, slow enough
   that a click does not overshoot. */
constexpr float kClickMoveSpeed = 0.6f;

/* The one object that joins the dock's camera control to the source's
   Interact window (src/zc_source_bridge.h explains why the two do not talk
   directly). Owned by ZcControlDock, shared with the PTZ pad and the framing
   panel, and registered as the global pointer sink for its lifetime. */
class ZcOverlayLink : public QObject, public zc::OverlayPointerSink {
public:
	explicit ZcOverlayLink(QObject *owner);
	~ZcOverlayLink() override;

	void setClient(ZcCameraClient *client, const QString &host);
	void setPtzSpeed(float speed);
	/* Arm one mode, disarming whatever was armed before. */
	void armMode(zc::OverlayInteraction mode);
	zc::OverlayInteraction mode() const { return mode_; }
	void disarm();
	/* The camera's rectangle, from a snapshot (all_status or /mask_area). */
	void setMask(bool enabled, float left, float top, float right,
		     float bottom);
	/* Take the rectangle off (or back onto) the picture without waiting for
	   the camera's read-back — used the moment the detection-area switch is
	   turned off (bug list 28). */
	void setMaskVisible(bool visible);
	/* Whether a rectangle is known at all. */
	bool hasMask() const { return hasMask_; }
	/* Put a rectangle on the picture without writing it to the camera, for an
	   editor that would otherwise open with nothing to grab (bug 29). */
	void seedMask(float left, float top, float right, float bottom);
	/* The camera's tracking-box ("range") frame, mirrored into the source so
	   its overlay draws the region line the web preview shows (bug 17). */
	void setTrackBox(bool enabled, float left, float top, float width,
			 float height);
	void onOverlayPointer(int phase, int button, float nx, float ny) override;

	/* Opens the source's Interact window. Set by the dock, which owns the
	   source lookup. */
	std::function<void()> openInteract;
	/* Closes it again. The click-to-move target is a state control now, so its
	   second press has to take the window down rather than just unarm. */
	std::function<void()> closeInteract;
	/* Fired whenever the armed mode changes, so the two toggles (PTZ pad and
	   framing panel) cannot both look armed. */
	std::function<void(zc::OverlayInteraction)> onModeChanged;

private:
	void pushMask();
	void pushMarker();
	/* Write the rectangle and pin it against a camera that ignores it. */
	void commitMask(float left, float top, float right, float bottom);

	ZcCameraClient *client_ = nullptr;
	QString host_;
	zc::OverlayInteraction mode_ = zc::OverlayInteraction::None;
	float ptzSpeed_ = kClickMoveSpeed;

	bool hasMask_ = false;
	bool maskOn_ = false;
	float ml_ = 0.0f, mt_ = 0.0f, mr_ = 1.0f, mb_ = 1.0f;
	/* What the camera last reported, and whether a local edit is pinned
	   because the camera did not take it. */
	float camL_ = 0.0f, camT_ = 0.0f, camR_ = 1.0f, camB_ = 1.0f;
	/* The camera's own enable flag, so a change in it counts as news (bug 28). */
	bool camOn_ = false;
	bool maskLocalOverride_ = false;

	bool markerOn_ = false;
	float markerX_ = 0.5f, markerY_ = 0.5f;
	bool dragging_ = false;
	float anchorX_ = 0.0f, anchorY_ = 0.0f;

	bool trackOn_ = false;
	float tbL_ = 0.0f, tbT_ = 0.0f, tbW_ = 0.0f, tbH_ = 0.0f;
};

ZcOverlayLink::ZcOverlayLink(QObject *owner) : QObject(owner) {}

ZcOverlayLink::~ZcOverlayLink()
{
	/* Never leave a mode armed on the source after the dock is gone. */
	disarm();
}

void ZcOverlayLink::setClient(ZcCameraClient *client, const QString &host)
{
	if (client_ == client && host_ == host)
		return;
	/* The old camera's source keeps no mode: disarm before switching so a
	   stale crosshair cannot be left painted on the previous source. */
	disarm();
	/* Nor its range frame (bug 17's drawing must follow the camera that
	   owns it). */
	if (zc::OverlayTarget *t = zc::overlayFindTarget(host_))
		t->setOverlayTrackBox(false, 0.0f, 0.0f, 0.0f, 0.0f);
	trackOn_ = false;
	client_ = client;
	host_ = host;
	hasMask_ = false;
	maskLocalOverride_ = false;
}

void ZcOverlayLink::setTrackBox(bool enabled, float left, float top,
				float width, float height)
{
	trackOn_ = enabled;
	tbL_ = left;
	tbT_ = top;
	tbW_ = width;
	tbH_ = height;
	zc::OverlayTarget *t = zc::overlayFindTarget(host_);
	if (t)
		t->setOverlayTrackBox(enabled, left, top, width, height);
}

void ZcOverlayLink::setPtzSpeed(float speed)
{
	ptzSpeed_ = qBound(0.05f, speed, 1.0f);
}

void ZcOverlayLink::pushMask()
{
	zc::OverlayTarget *t = zc::overlayFindTarget(host_);
	if (!t) {
		blog(LOG_INFO,
		     "[obs-zcamera] overlay push: no source is registered for "
		     "host '%s'",
		     host_.toUtf8().constData());
		return;
	}
	t->setOverlayMask(hasMask_ && maskOn_, ml_, mt_, mr_, mb_);
}

void ZcOverlayLink::pushMarker()
{
	zc::OverlayTarget *t = zc::overlayFindTarget(host_);
	if (!t)
		return;
	t->setOverlayMarker(markerOn_, markerX_, markerY_);
}

void ZcOverlayLink::armMode(zc::OverlayInteraction mode)
{
	if (mode_ == mode)
		return;
	mode_ = mode;
	/* A fresh mode starts with a clean crosshair and no half-drawn drag. */
	dragging_ = false;
	markerOn_ = false;
	if (onModeChanged)
		onModeChanged(mode_);
	zc::OverlayTarget *t = zc::overlayFindTarget(host_);
	blog(LOG_INFO, "[obs-zcamera] armMode %d -> target %s", (int)mode,
	     t ? "found" : "MISSING");
	if (!t)
		return;
	t->setOverlayInteraction(mode);
	t->setOverlayMarker(false, 0.5f, 0.5f);
	if (mode != zc::OverlayInteraction::None)
		pushMask();
}

void ZcOverlayLink::disarm()
{
	mode_ = zc::OverlayInteraction::None;
	dragging_ = false;
	markerOn_ = false;
	if (zc::OverlayTarget *t = zc::overlayFindTarget(host_)) {
		t->setOverlayInteraction(zc::OverlayInteraction::None);
		t->setOverlayMarker(false, 0.5f, 0.5f);
	}
}

/* The camera's own rectangle arriving from a snapshot (all_status or
   /mask_area). A local edit the camera did not take is pinned: firmware that
   answers {"code":0} but keeps its old geometry must not have the operator's
   rectangle pulled out from under them on the next refresh. Only the camera
   switching the feature itself (on or off) overrides that pin. */
void ZcOverlayLink::setMask(bool enabled, float left, float top, float right,
			    float bottom)
{
	/* The enable flag is part of "the camera said something new": the
	   operator's own click pins a local override, and if the camera later
	   reports the flag the other way (its page, or a rejected write being
	   corrected) the rectangle has to follow. Without this, a flapped switch
	   left the overlay stuck on the last local value for good (bug 88). */
	const bool enableChanged = (enabled != camOn_);
	camL_ = left;
	camT_ = top;
	camR_ = right;
	camB_ = bottom;
	camOn_ = enabled;

	/* A snapshot may only end a local override when the camera's own switch
	   flipped. Geometry alone is not enough: firmware that answers {"code":0}
	   yet keeps its old rectangle reports the drift on the very next poll, and
	   that was wiping the rectangle out from under the operator one second
	   after the editor opened (bug 28). */
	if (maskLocalOverride_) {
		if (!enableChanged)
			return; /* keep what the operator drew */
		maskLocalOverride_ = false;
	}

	hasMask_ = true;
	maskOn_ = enabled;
	ml_ = left;
	mt_ = top;
	mr_ = right;
	mb_ = bottom;
	pushMask();
}

void ZcOverlayLink::seedMask(float left, float top, float right, float bottom)
{
	hasMask_ = true;
	maskOn_ = true;
	ml_ = left;
	mt_ = top;
	mr_ = right;
	mb_ = bottom;
	/* Local until the first drag writes it, so a read-back cannot take the
	   seed away before the operator has touched anything. */
	maskLocalOverride_ = true;
	pushMask();
}

void ZcOverlayLink::setMaskVisible(bool visible)
{
	maskOn_ = visible;
	/* A local decision, so a following read-back that still says "on" must
	   not undo it before the camera has accepted the write. */
	maskLocalOverride_ = true;
	pushMask();
}

void ZcOverlayLink::commitMask(float left, float top, float right, float bottom)
{
	hasMask_ = true;
	maskOn_ = true;
	ml_ = left;
	mt_ = top;
	mr_ = right;
	mb_ = bottom;
	/* Pinned until the camera reports something different. */
	maskLocalOverride_ = true;
	if (client_) {
		/* Two requests, the way the page does it: the rectangle first, then the
		   switch that makes the camera use it (bug 29). */
		client_->setFramingMaskRect(left, top, right, bottom);
		client_->setFramingMaskEnable(true);
	}
	pushMask();
}

void ZcOverlayLink::onOverlayPointer(int phase, int button, float nx, float ny)
{
	if (mode_ == zc::OverlayInteraction::None)
		return;

	if (mode_ == zc::OverlayInteraction::PtzClick) {
		markerOn_ = true;
		markerX_ = nx;
		markerY_ = ny;
		pushMarker();
		if (phase != zc::OverlayPointerRelease || !client_)
			return;
		/* The image is y-down and the head's tilt is y-up, so the vertical
		   delta is negated; pan matches (positive = to the operator's
		   right). */
		const double pan = (double(nx) - 0.5) * 2.0 * kClickPanDeg;
		const double tilt = -(double(ny) - 0.5) * 2.0 * kClickTiltDeg;
		client_->ptzGoto((float)pan, (float)tilt, ptzSpeed_,
				 /*relative=*/true, /*speedWithZoom=*/true);
		return;
	}

	/* MaskEdit. The source draws the live rectangle itself; the dock only
	   needs the corners, and it computes them from the same coordinates so
	   the write and the drawing cannot disagree. */
	if (button != MOUSE_LEFT && button != -1)
		return;
	if (phase == zc::OverlayPointerPress) {
		dragging_ = true;
		anchorX_ = nx;
		anchorY_ = ny;
	} else if (phase == zc::OverlayPointerDrag) {
		if (!dragging_)
			return;
	} else if (phase == zc::OverlayPointerRelease) {
		if (!dragging_)
			return;
		dragging_ = false;
		const float l = qMin(anchorX_, nx), r = qMax(anchorX_, nx);
		const float t = qMin(anchorY_, ny), b = qMax(anchorY_, ny);
		/* A stray click with no drag is not a rectangle: ignore it rather
		   than write a zero-area mask the camera will refuse. */
		if (r - l < 0.02f && b - t < 0.02f)
			return;
		commitMask(l, t, r, b);
	}
}

ZcPtzPad::ZcPtzPad(QWidget *parent) : QWidget(parent)
{
	setObjectName("PtzPadWrapper");
	setStyleSheet(kPtzPanelStyle);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	auto *title = new QLabel("PTZ", this);
	layout->addWidget(title, 0, Qt::AlignHCenter);

	/* The pad and the zoom rocker on one row, which is how the camera's own
	   page draws them: the round joystick on the left, a narrow vertical
	   T-over-W column on its right (#tab-ctrl-af-zoom-pos). Both are lens
	   moves and read as one instrument; the speed slider below is a different
	   thing — it sets how fast the head travels, not where the lens goes — so
	   it keeps a row of its own rather than being paired with the zoom. */
	auto *stickRow = new QHBoxLayout;
	stickRow->setSpacing(8);

	/* The painted joystick occupies this fixed, transparent area. */
	joystickArea_ = new QWidget(this);
	joystickArea_->setFixedSize(220, 220);
	joystickArea_->setStyleSheet("background:transparent;");
	joystickArea_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	stickRow->addWidget(joystickArea_, 0, Qt::AlignTop);

	/* Lens zoom (bug list "the PTZ tab has no zoom in/out control", the camera's
	   own page has one next to the joystick). The camera's
	   /ctrl/lens?action=zoomin|zoomout&fspeed= is a continuous move that only
	   stops on zoomstop, and this firmware reports no absolute zoom position
	   (`z_status` is {"moving":0} and its lens catalog has no `lens_zoom_pos`),
	   so a position slider would display a number it cannot verify. The slider
	   is therefore a spring-return rocker — up zooms in towards T, down zooms
	   out towards W, the distance from the centre is the speed, and releasing
	   recentres it and sends zoomstop. T, W and the centre tick make the
	   resting position read as "centred, not moving" instead of as a zoom
	   level of zero: there is no number in the middle any more, only the
	   direction and amount while the rocker is actually held. */
	auto *zoomBox = new QWidget(this);
	zoomBox->setFixedSize(52, 220);
	auto *zoomCol = new QVBoxLayout(zoomBox);
	zoomCol->setContentsMargins(0, 0, 0, 0);
	zoomCol->setSpacing(2);

	auto *zoomTele = new QLabel(QStringLiteral("T"), zoomBox);
	zoomTele->setAlignment(Qt::AlignHCenter);
	zoomTele->setToolTip(ztr("ZCameraPlugin.Dock.ZoomInTip"));
	zoomSlider_ = new QSlider(Qt::Vertical, zoomBox);
	zoomSlider_->setObjectName("ptzZoom");
	zoomSlider_->setRange(-100, 100);
	zoomSlider_->setValue(0);
	/* A tick a the centre only: it is the neutral the rocker returns to, not a
	   scale anyone reads a value off. */
	zoomSlider_->setTickPosition(QSlider::TicksBothSides);
	zoomSlider_->setTickInterval(100);
	zoomSlider_->setToolTip(ztr("ZCameraPlugin.Dock.ZoomTip"));
	auto *zoomWide = new QLabel(QStringLiteral("W"), zoomBox);
	zoomWide->setAlignment(Qt::AlignHCenter);
	zoomWide->setToolTip(ztr("ZCameraPlugin.Dock.ZoomOutTip"));
	zoomValue_ = new QLabel(zoomBox);
	zoomValue_->setObjectName("PtzZoomValue");
	zoomValue_->setAlignment(Qt::AlignHCenter);
	zoomValue_->setFixedWidth(48);

	zoomCol->addWidget(zoomTele, 0, Qt::AlignHCenter);
	zoomCol->addWidget(zoomSlider_, 1, Qt::AlignHCenter);
	zoomCol->addWidget(zoomWide, 0, Qt::AlignHCenter);
	zoomCol->addWidget(zoomValue_, 0, Qt::AlignHCenter);

	/* Click-to-move, as one button at the top-right of this row — stacked above
	   the zoom column rather than placed beside it. The row is already as wide
	   as the scroll pane allows (joystick 220 + rocker 52 + the pad's 20 of
	   margin fills 292 of a 320 px pane), so a button added after the rocker had
	   no room at all and was clipped to a sliver at the viewport's right edge.
	   Stacking costs no width: the button's column is the rocker's own 52 px.
	   It used to be two buttons lower down — a "click to move" tick plus an
	   "Interact…" button — which asked the operator to turn one on and then go
	   find the other. The target does both in one press, and undoes both too:
	   armed, it arms the click mode and opens the Interact pane, which is a live
	   view of this camera and the only pane libobs forwards mouse events to (see
	   zc_source_bridge.h), so a click on the picture turns the head to that
	   point. Pressed again it unarms and closes the pane, so its tick reports
	   the state of the window as well as of the mode. */
	targetBtn_ = new QToolButton(this);
	targetBtn_->setObjectName("ptzTarget");
	targetBtn_->setIcon(zcTargetIcon());
	targetBtn_->setIconSize(QSize(20, 20));
	/* Fixed: no layout may squeeze it back down to a sliver. */
	targetBtn_->setFixedSize(28, 28);
	targetBtn_->setCheckable(true);
	targetBtn_->setAutoRaise(true);
	targetBtn_->setCursor(Qt::PointingHandCursor);
	targetBtn_->setToolTip(ztr("ZCameraPlugin.Dock.ClickMoveTip"));
	targetBtn_->setAccessibleName(ztr("ZCameraPlugin.Dock.ClickMove"));

	auto *rightCol = new QVBoxLayout;
	rightCol->setContentsMargins(0, 0, 0, 0);
	rightCol->setSpacing(6);
	rightCol->addWidget(targetBtn_, 0, Qt::AlignRight);
	rightCol->addWidget(zoomBox, 0, Qt::AlignHCenter);
	stickRow->addStretch(1);
	stickRow->addLayout(rightCol);
	layout->addLayout(stickRow);
	updateZoomLabel(0);

	connect(targetBtn_, &QToolButton::toggled, this, [this](bool on) {
		if (!link_)
			return;
		link_->armMode(on ? zc::OverlayInteraction::PtzClick
				  : zc::OverlayInteraction::None);
		if (!on) {
			if (link_->closeInteract)
				link_->closeInteract();
			return;
		}
		/* Opening the pane is what makes the mode usable: nothing else in
		   libobs forwards a click to the source. */
		if (link_->openInteract)
			link_->openInteract();
		/* No source for this camera, so no pane could be opened: drop back
		   to off rather than sit there claiming a window that is not
		   there. */
		if (!zcFindInteractionWindow())
			targetBtn_->setChecked(false);
	});
	/* The pane can also be closed by its own X, or by OBS when the source it
	   belongs to goes away — neither goes through this button. Poll so the tick
	   cannot claim a window that is gone. */
	interactTimer_ = new QTimer(this);
	interactTimer_->setInterval(1000);
	connect(interactTimer_, &QTimer::timeout, this, [this]() {
		if (targetBtn_ && targetBtn_->isChecked() &&
		    !zcFindInteractionWindow())
			targetBtn_->setChecked(false);
	});
	interactTimer_->start();

	/* A drag must zoom continuously, so the pointer's pressure is what starts
	   and stops the move: sliderPressed/sliderReleased bracket the drag, and
	   only the values produced inside it are sent (a keyboard or programmatic
	   change must not move the lens). */
	connect(zoomSlider_, &QSlider::sliderPressed, this,
		[this]() { zoomDragging_ = true; });
	connect(zoomSlider_, &QSlider::valueChanged, this, [this](int v) {
		if (zoomDragging_)
			pushZoom(v);
	});
	connect(zoomSlider_, &QSlider::sliderReleased, this, [this]() {
		zoomDragging_ = false;
		if (client_)
			client_->lensZoomStop();
		zoomDir_ = 0;
		zoomBucket_ = 0;
		QSignalBlocker blocker(zoomSlider_);
		zoomSlider_->setValue(0);
		updateZoomLabel(0);
	});

	/* Pan/tilt speed: how fast the head travels, the same "Speed" the camera's
	   own page shows as a vertical slider beside the joystick. */
	auto *speedRow = new QHBoxLayout;
	auto *speedLbl = new QLabel(ztr("ZCameraPlugin.Dock.Speed"), this);
	speedSlider_ = new QSlider(Qt::Horizontal, this);
	speedSlider_->setObjectName("ptzSpeed");
	speedSlider_->setRange(1, 100);
	speedSlider_->setValue(50);
	/* The same speed drives a click-to-move goto: the operator's idea of
	   "how fast the head travels" should not change between the joystick and
	   a click on the picture. */
	connect(speedSlider_, &QSlider::valueChanged, this, [this](int v) {
		if (link_)
			link_->setPtzSpeed(float(v) / 100.0f);
	});
	speedRow->addWidget(speedLbl);
	speedRow->addWidget(speedSlider_, 1);
	layout->addLayout(speedRow);

	/* Home / Stop: the two camera-wide moves. Presets live in the adjacent
	   ZcPresetPanel, not here. */
	auto *actionRow = new QHBoxLayout;
	homeButton_ = new QPushButton(ztr("ZCameraPlugin.Dock.Home"), this);
	stopButton_ = new QPushButton(ztr("ZCameraPlugin.Dock.Stop"), this);
	actionRow->addWidget(homeButton_);
	actionRow->addWidget(stopButton_);
	layout->addLayout(actionRow);

	connect(homeButton_, &QPushButton::clicked, this,
		[this]() { emit homeRequested(); });
	connect(stopButton_, &QPushButton::clicked, this, [this]() {
		if (client_)
			client_->ptzStop();
	});

	/* No position / GOTO row here: the absolute / relative GOTO spins are not
	   wanted on this panel — the joystick, the presets and the traces already
	   cover every move it needs. ptzGoto() is still used, by click-to-move: a
	   click on the picture is a relative goto to that point.
	   A read-only Current Position read-out is kept below, though: the
	   operator needs to see where the head is. The click-to-move control
	   itself lives in the dial row above (targetBtn_). */

	/* Current Position (bug list: the PTZ tab had no position read-out). Kept
	   live by the ptzfChange notification, so it follows the joystick. */
	positionLabel_ = new QLabel(this);
	positionLabel_->setObjectName("PtzPosition");
	layout->addWidget(positionLabel_);
	setPosition(0.0, 0.0);

	/* No auto-framing controls here: they live in the dock's dedicated
	   ZcFramingPanel tab, matching the camera's web UI layout. */

	/* Range Limit: the enable flag plus the four edge sliders and their
	   "set to current" buttons, mirroring the camera's web UI
	   "Pan tilt -> Range Limit" group (bug list: the four range items were
	   missing). Up/Right write the "upright" corner, Down/Left "downleft",
	   exactly as the web UI does. */
	limitCheck_ = new QCheckBox(ztr("ZCameraPlugin.Dock.PtzRangeLimit"), this);
	layout->addWidget(limitCheck_);

	static const char *const kEdgeLabels[EdgeCount] = {
		"ZCameraPlugin.Dock.EdgeUp", "ZCameraPlugin.Dock.EdgeRight",
		"ZCameraPlugin.Dock.EdgeDown", "ZCameraPlugin.Dock.EdgeLeft"};
	for (int e = 0; e < EdgeCount; ++e) {
		auto *row = new QHBoxLayout;
		row->setSpacing(4);
		auto *lbl = new QLabel(ztr(kEdgeLabels[e]), this);
		/* One character ("上" / "Up") plus the two buttons leave the slider
		   about 120 px of the pane's fixed 320 px column. */
		lbl->setFixedWidth(28);
		limitSlider_[e] = new QSlider(Qt::Horizontal, this);
		limitSlider_[e]->setRange(0, 1);
		/* Bug list 56: grey until the camera's Range Limit state arrives;
		   applyLimitBox() then enables or keeps them disabled. */
		limitSlider_[e]->setEnabled(false);
		limitValue_[e] = new QLabel(QStringLiteral("0\u00b0"), this);
		limitValue_[e]->setFixedWidth(30);
		limitValue_[e]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		/* Labelled buttons, exactly the pair the camera's page puts next to
		   each edge ("Current" and "Reset"). A bare glyph read as a reset
		   button, and there was no reset at all (bug 98). */
		limitCurrent_[e] = new QPushButton(
		    ztr("ZCameraPlugin.Dock.PtzEdgeCurrent"), this);
		limitCurrent_[e]->setToolTip(
		    ztr("ZCameraPlugin.Dock.PtzSetCurrent"));
		limitCurrent_[e]->setEnabled(false);
		limitReset_[e] = new QPushButton(
		    ztr("ZCameraPlugin.Dock.PtzEdgeReset"), this);
		limitReset_[e]->setToolTip(ztr("ZCameraPlugin.Dock.PtzEdgeResetTip"));
		limitReset_[e]->setEnabled(false);
		for (QPushButton *b : {limitCurrent_[e], limitReset_[e]}) {
			/* The pane is a fixed 320 px column: keep the pair compact so
			   the slider still has room to be dragged. */
			b->setFixedHeight(22);
			b->setStyleSheet(QStringLiteral("padding:0 6px;"));
		}
		row->addWidget(lbl);
		row->addWidget(limitSlider_[e], 1);
		row->addWidget(limitValue_[e]);
		row->addWidget(limitCurrent_[e]);
		row->addWidget(limitReset_[e]);
		layout->addLayout(row);

		connect(limitSlider_[e], &QSlider::valueChanged, this,
			[this, e](int v) {
				updateLimitLabel((Edge)e);
				if (!limitLoaded_)
					return;
				/* Keep the opposite edge one degree clear of this one,
				   without re-entering this handler. */
				const bool low = (e == EdgeDown || e == EdgeLeft);
				QSlider *opp = (e == EdgeUp)     ? limitSlider_[EdgeDown]
					     : (e == EdgeRight) ? limitSlider_[EdgeLeft]
					     : (e == EdgeDown)  ? limitSlider_[EdgeUp]
								: limitSlider_[EdgeRight];
				{
					QSignalBlocker b(opp);
					if (low)
						opp->setMinimum(v + 1);
					else
						opp->setMaximum(v - 1);
				}
				updateLimitLabel((Edge)((e + 2) % EdgeCount));
			});
		/* Write only when the drag ends: every other slider in the dock does the
		   same, and one request per notch floods the camera's single-threaded
		   httpd and can store intermediate limits (bug list: range-limit
		   sliders). */
		connect(limitSlider_[e], &QSlider::sliderReleased, this,
			[this, e]() {
				if (!limitLoaded_)
					return;
				pushLimit((Edge)e);
			});
		connect(limitCurrent_[e], &QPushButton::clicked, this,
			[this, e]() { setEdgeToCurrent((Edge)e); });
		connect(limitReset_[e], &QPushButton::clicked, this,
			[this, e]() { setEdgeToReset((Edge)e); });
	}

	connect(limitCheck_, &QCheckBox::toggled, this, [this](bool on) {
		/* Repaint the four sliders from the limit / unlimit box right away, the
		   way the camera's web UI does when its Range Limit status changes: with
		   the limit on an edge sits on the stored boundary, with it off on the
		   head's full travel. */
		if (limitLoaded_)
			applyLimitBox(on);
		if (client_)
			client_->writeSetting(QLatin1String("ptz_limit"),
					      on ? QStringLiteral("On")
						 : QStringLiteral("Off"));
	});

	layout->addStretch();
}

void ZcPtzPad::setClient(ZcCameraClient *client)
{
	client_ = client;
	/* The zoom rocker is stateless, but its "last command sent" memory must not
	   survive a camera change: the next drag has to be able to send the same
	   bucket again. Recentre it too, so a drag interrupted by the switch does
	   not leave the handle off centre. */
	zoomDir_ = 0;
	zoomBucket_ = 0;
	zoomDragging_ = false;
	if (zoomSlider_) {
		QSignalBlocker blocker(zoomSlider_);
		zoomSlider_->setValue(0);
		updateZoomLabel(0);
	}
	if (client_) {
		/* A change made from the Settings tab (or the camera's own web UI)
		   reaches the engine as a refreshed PTZ catalog. Mirror its `ptz_limit`
		   so the Range Limit checkbox and the four sliders below stay in step
		   with it, instead of the two controls disagreeing (bug list: the range
		   limit status did not drive the range limit adjustment). */
		connect(client_, &ZcCameraClient::groupRefreshed, this,
			[this](const QString &catalog) {
				if (!client_ || catalog != QLatin1String("ptz"))
					return;
				const QString value = client_->settingValue(
				    QLatin1String("ptz_limit"));
				if (value.isEmpty())
					return;
				const bool enabled = value == QLatin1String("On");
				if (limitCheck_) {
					QSignalBlocker b(limitCheck_);
					limitCheck_->setChecked(enabled);
				}
				if (limitLoaded_)
					applyLimitBox(enabled);
			});
		return;
	}
	/* No client: clear the live values so a stale position/limit from the
	   previous camera is not left on screen. */
	limitLoaded_ = false;
	if (limitCheck_) {
		QSignalBlocker b(limitCheck_);
		limitCheck_->setChecked(false);
	}
	for (int e = 0; e < EdgeCount; ++e) {
		if (!limitSlider_[e])
			continue;
		QSignalBlocker b(limitSlider_[e]);
		limitSlider_[e]->setRange(0, 1);
		limitSlider_[e]->setValue(0);
		limitSlider_[e]->setEnabled(false);
		if (limitCurrent_[e])
			limitCurrent_[e]->setEnabled(false);
		if (limitReset_[e])
			limitReset_[e]->setEnabled(false);
		updateLimitLabel((Edge)e);
	}
	if (positionLabel_)
		positionLabel_->setText(ztr("ZCameraPlugin.Dock.PtzPosition") +
					QStringLiteral(" \u2014"));
}

/* Whole-degree slider helper: a degenerate range (an edge that reaches the
   camera's own travel) collapses to a single point rather than inverting. */
static void setLimitRange(QSlider *s, int lo, int hi, int value)
{
	if (!s)
		return;
	if (hi < lo)
		hi = lo;
	s->setRange(lo, hi);
	s->setValue(value);
}

void ZcPtzPad::applyLimitBox(bool enabled)
{
	/* Bug list 56: with the Range Limit switch off the camera ignores the
	   four edges, so they grey; switching it on restores them. */
	for (int e = 0; e < EdgeCount; ++e) {
		if (limitSlider_[e])
			limitSlider_[e]->setEnabled(enabled);
		if (limitCurrent_[e])
			limitCurrent_[e]->setEnabled(enabled);
		if (limitReset_[e])
			limitReset_[e]->setEnabled(enabled);
	}

	/* Ranges and values mirror the web UI's updatePtzLimit(): with the limit on
	   an edge sits on the stored boundary, with it off on the camera's full
	   travel. The opposite edge is kept one degree clear so the box cannot
	   invert. */
	const int upVal = enabled ? tiltMax_ : utiltMax_;
	const int rightVal = enabled ? panMax_ : upanMax_;
	const int downVal = enabled ? tiltMin_ : utiltMin_;
	const int leftVal = enabled ? panMin_ : upanMin_;
	for (int e = 0; e < EdgeCount; ++e) {
		if (limitSlider_[e])
			limitSlider_[e]->blockSignals(true);
	}
	setLimitRange(limitSlider_[EdgeUp], tiltMin_ + 1, utiltMax_, upVal);
	setLimitRange(limitSlider_[EdgeRight], panMin_ + 1, upanMax_, rightVal);
	setLimitRange(limitSlider_[EdgeDown], utiltMin_, tiltMax_ - 1, downVal);
	setLimitRange(limitSlider_[EdgeLeft], upanMin_, panMax_ - 1, leftVal);
	for (int e = 0; e < EdgeCount; ++e) {
		if (limitSlider_[e])
			limitSlider_[e]->blockSignals(false);
		updateLimitLabel((Edge)e);
	}
}

void ZcPtzPad::pushLimit(Edge edge)
{
	if (!client_)
		return;
	const int up = limitSlider_[EdgeUp]->value();
	const int right = limitSlider_[EdgeRight]->value();
	const int down = limitSlider_[EdgeDown]->value();
	const int left = limitSlider_[EdgeLeft]->value();
	/* Up and Right are the origin corner's maximums, Down and Left its
	   minimums; the camera takes the whole corner in one request. */
	if (edge == EdgeUp || edge == EdgeRight)
		client_->ptzLimitUpdate(QStringLiteral("upright"), right, up);
	else
		client_->ptzLimitUpdate(QStringLiteral("downleft"), left, down);
	/* Re-read the limit box the camera now holds: the transport serializes
	   requests, so this reply reflects the push. Without it the cached box
	   goes stale and the next Range-Limit checkbox toggle (applyLimitBox)
	   repaints the sliders back to the pre-drag limits. */
	refreshState();
}

void ZcPtzPad::updateLimitLabel(Edge edge)
{
	if (limitValue_[edge] && limitSlider_[edge])
		limitValue_[edge]->setText(
		    QString::number(limitSlider_[edge]->value()) +
		    QStringLiteral("\u00b0"));
}

/* The two edge buttons, written to do exactly what the camera's page does with
   the same click (its btn_*_current / btn_*_reset handlers). Both were wrong
   before:

    - "Current" pushed the raw position into the slider, which clamped it to
      the slider's own range and then sent the CLAMPED value to the camera;
      the page instead refuses the click when the position would cross the
      opposite edge, and widens the opposite edge's bound first. That is why
      the stored value and the page's value disagreed (bug 97).
    - There was no "Reset" at all: the single glyph button only ever did
      "current", so testers took it for reset (bug 98).

   Both need a connected head: a stale 0/0 (the reply the camera sends while
   the head is off-line) would otherwise be stored as a limit. */
void ZcPtzPad::setEdgeToCurrent(Edge edge)
{
	if (!client_)
		return;
	client_->ptzQuery(true, [this, edge](const QJsonObject &o) {
		if (!client_ || !limitLoaded_ || !limitSlider_[edge])
			return;
		if (o.contains("connected") && !o.value("connected").toBool())
			return;
		const int pan = qRound(o.value("pan_pos").toDouble());
		const int tilt = qRound(o.value("tilt_pos").toDouble());
		QSlider *self = limitSlider_[edge];
		/* Up/Right own the far corner's maximum, Down/Left its minimum, so
		   the opposite edge is always the one that has to stay clear. */
		QSlider *opp = (edge == EdgeUp)     ? limitSlider_[EdgeDown]
			       : (edge == EdgeRight) ? limitSlider_[EdgeLeft]
			       : (edge == EdgeDown)  ? limitSlider_[EdgeUp]
						     : limitSlider_[EdgeRight];
		const int pos = (edge == EdgeUp || edge == EdgeDown) ? tilt : pan;
		const bool low = (edge == EdgeDown || edge == EdgeLeft);
		if (!opp)
			return;
		/* Refuse instead of collapsing the box: the page checks the same
		   two conditions and does nothing when they fail. */
		if (low ? (opp->value() < pos + 1) : (opp->value() > pos - 1))
			return;
		{
			QSignalBlocker b(opp);
			/* Clicking a low edge moves the high edge's floor up; clicking
			   a high edge moves the low edge's ceiling down. */
			if (low)
				opp->setMinimum(qMin(opp->maximum(), pos + 1));
			else
				opp->setMaximum(qMax(opp->minimum(), pos - 1));
		}
		{
			QSignalBlocker b(self);
			self->setValue(qBound(self->minimum(), pos,
					      self->maximum()));
		}
		updateLimitLabel(edge);
		updateLimitLabel((Edge)((edge + 2) % EdgeCount));
		pushLimit(edge);
	});
}

/* Put one edge back on the head's own travel limit. The page re-reads the
   unlimit box for this (its btn_*_reset handlers) rather than reusing a cached
   one, so a box the head changed since the last status read cannot be stored
   back by mistake. */
void ZcPtzPad::setEdgeToReset(Edge edge)
{
	if (!client_)
		return;
	client_->ptzQuery(false, [this, edge](const QJsonObject &o) {
		if (!client_ || !limitSlider_[edge])
			return;
		if (o.contains("connected") && !o.value("connected").toBool())
			return;
		const QJsonObject un = o.value("unlimit").toObject();
		if (un.isEmpty())
			return;
		upanMin_ = qRound(un.value("pan_min").toDouble());
		upanMax_ = qRound(un.value("pan_max").toDouble());
		utiltMin_ = qRound(un.value("tilt_min").toDouble());
		utiltMax_ = qRound(un.value("tilt_max").toDouble());
		limitLoaded_ = true;

		const int pos = (edge == EdgeUp)     ? utiltMax_
				: (edge == EdgeDown) ? utiltMin_
				: (edge == EdgeRight) ? upanMax_
						      : upanMin_;
		QSlider *opp = (edge == EdgeUp)     ? limitSlider_[EdgeDown]
			       : (edge == EdgeRight) ? limitSlider_[EdgeLeft]
			       : (edge == EdgeDown)  ? limitSlider_[EdgeUp]
						     : limitSlider_[EdgeRight];
		const bool low = (edge == EdgeDown || edge == EdgeLeft);
		if (opp) {
			QSignalBlocker b(opp);
			if (low)
				opp->setMinimum(qMin(opp->maximum(), pos + 1));
			else
				opp->setMaximum(qMax(opp->minimum(), pos - 1));
		}		{
			QSignalBlocker b(limitSlider_[edge]);
			limitSlider_[edge]->setValue(pos);
		}
		updateLimitLabel(edge);
		updateLimitLabel((Edge)((edge + 2) % EdgeCount));
		pushLimit(edge);
	});
}

void ZcPtzPad::refreshState()
{
	if (!client_)
		return;
	client_->ptzQuery(true, [this](const QJsonObject &o) {
		if (!client_)
			return;
		if (o.contains("pan_pos") || o.contains("tilt_pos"))
			setPosition(o.value("pan_pos").toDouble(),
				    o.value("tilt_pos").toDouble());
		const QJsonObject lim = o.value("limit").toObject();
		const QJsonObject un = o.value("unlimit").toObject();
		if (lim.isEmpty() || un.isEmpty())
			return;
		panMin_ = qRound(lim.value("pan_min").toDouble());
		panMax_ = qRound(lim.value("pan_max").toDouble());
		tiltMin_ = qRound(lim.value("tilt_min").toDouble());
		tiltMax_ = qRound(lim.value("tilt_max").toDouble());
		upanMin_ = qRound(un.value("pan_min").toDouble());
		upanMax_ = qRound(un.value("pan_max").toDouble());
		utiltMin_ = qRound(un.value("tilt_min").toDouble());
		utiltMax_ = qRound(un.value("tilt_max").toDouble());
		const bool enabled = lim.value("enable").toBool();
		if (limitCheck_) {
			QSignalBlocker b(limitCheck_);
			limitCheck_->setChecked(enabled);
		}
		limitLoaded_ = true;
		applyLimitBox(enabled);
	});
}

void ZcPtzPad::setPosition(double pan, double tilt)
{
	if (!positionLabel_)
		return;
	positionLabel_->setText(
	    QStringLiteral("%1  Pan %2\u00b0   Tilt %3\u00b0")
		.arg(ztr("ZCameraPlugin.Dock.PtzPosition"),
		     QString::number(pan, 'f', 1),
		     QString::number(tilt, 'f', 1)));
}

void ZcPtzPad::updateGeometry()
{
	joystickRect_ = QRectF(joystickArea_->geometry());
	if (joystickRect_.isEmpty())
		return;
	joyCenter_ = joystickRect_.center();
	joyOuterRadius_ = joystickRect_.width() / 2.0 - 12.0;
	joyInnerRadius_ = joyOuterRadius_ * 0.46;
}

QPainterPath ZcPtzPad::sectorPath(int i) const
{
	const qreal a0 = i * 45.0 - 22.5;
	const qreal a1 = i * 45.0 + 22.5;
	QPainterPath path;
	const int steps = 5; /* ~9° per chord is smooth enough */
	for (int s = 0; s <= steps; ++s) {
		qreal a = a0 + (a1 - a0) * s / steps;
		qreal rad = a * kPi / 180.0;
		QPointF pt(joyCenter_.x() + joyOuterRadius_ * std::cos(rad),
			   joyCenter_.y() + joyOuterRadius_ * std::sin(rad));
		if (s == 0)
			path.moveTo(pt);
		else
			path.lineTo(pt);
	}
	for (int s = steps; s >= 0; --s) {
		qreal a = a0 + (a1 - a0) * s / steps;
		qreal rad = a * kPi / 180.0;
		QPointF pt(joyCenter_.x() + joyInnerRadius_ * std::cos(rad),
			   joyCenter_.y() + joyInnerRadius_ * std::sin(rad));
		path.lineTo(pt);
	}
	path.closeSubpath();
	return path;
}

void ZcPtzPad::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	updateGeometry();

	if (joystickRect_.isEmpty())
		return;

	/* Background ring. */
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x2a, 0x38, 0x36));
	p.drawEllipse(joyCenter_, joyOuterRadius_, joyOuterRadius_);

	/* 8 animated/directional sectors. */
	for (int i = 0; i < 8; ++i) {
		bool active = (mode_ == JoystickMode::Sector && activeDir_ == i);
		p.setBrush(active ? QColor(0xd8, 0xb8, 0x78)
				  : QColor(0x22, 0x2c, 0x2a));
		p.drawPath(sectorPath(i));

		/* Directional arrow pointing outward at the sector's mid angle. */
		qreal mid = i * 45.0;
		qreal rad = mid * kPi / 180.0;
		qreal ar = (joyInnerRadius_ + joyOuterRadius_) / 2.0;
		p.save();
		p.translate(joyCenter_.x() + ar * std::cos(rad),
			    joyCenter_.y() + ar * std::sin(rad));
		p.rotate(mid); /* screen coords: rotate() is clockwise, matches */
		QPainterPath arrow;
		arrow.moveTo(5, 0);
		arrow.lineTo(-3, -3.2);
		arrow.lineTo(-3, 3.2);
		arrow.closeSubpath();
		p.setBrush(active ? QColor(0x1f, 0x29, 0x27)
				  : QColor(0x8a, 0x9b, 0xb2));
		p.drawPath(arrow);
		p.restore();
	}

	/* Inner hole. */
	p.setBrush(QColor(0x18, 0x1e, 0x1c));
	p.drawEllipse(joyCenter_, joyInnerRadius_, joyInnerRadius_);

	/* Center draggable knob. */
	QPointF knob = joyCenter_ + dragOffset_;
	qreal knobR = qMax(6.0, joyInnerRadius_ * 0.42);
	bool dragging = (mode_ == JoystickMode::FreeDrag);
	p.setBrush(dragging ? QColor(0xd8, 0xb8, 0x78)
			    : QColor(0x3c, 0x51, 0x4e));
	p.drawEllipse(knob, knobR, knobR);
	p.setBrush(dragging ? QColor(0x1f, 0x29, 0x27)
			    : QColor(0x8a, 0x9b, 0xb2));
	qreal dotR = knobR * 0.34;
	p.drawEllipse(knob, dotR, dotR);
}

int ZcPtzPad::directionAt(const QPoint &pos) const
{
	QPointF d = QPointF(pos) - joyCenter_;
	qreal dist = std::sqrt(d.x() * d.x() + d.y() * d.y());
	if (dist < joyInnerRadius_ || dist > joyOuterRadius_ + 6.0)
		return -1;
	qreal a = std::atan2(d.y(), d.x()) * 180.0 / kPi; /* [-180,180] */
	int idx = qRound(a / 45.0) % 8;
	if (idx < 0)
		idx += 8;
	return idx;
}

bool ZcPtzPad::inCenter(const QPoint &pos) const
{
	QPointF d = QPointF(pos) - joyCenter_;
	return std::sqrt(d.x() * d.x() + d.y() * d.y()) <= joyInnerRadius_ * 0.6;
}

void ZcPtzPad::moveInDirection(int dirIndex)
{
	if (dirIndex < 0 || dirIndex > 7)
		return;
	const int pan = kPanDelta[dirIndex];
	const int tilt = kTiltDelta[dirIndex];
	/* fspeed is 0-1; the slider is 0-100. */
	const float fspeed = speedSlider_->value() / 100.0f;
	if (client_)
		client_->ptzMoveAction(QLatin1String(ptzActionFor(pan, tilt)), fspeed);
	emit moveRequested(pan, tilt);
}

void ZcPtzPad::moveAnalog(const QPointF &offset)
{
	/* The camera page's own centre-knob math (controller.js, the PTZ
	   joystick): the drag vector is clamped radially, split per axis with the
	   sign carrying the direction -- positive pan = right, positive tilt = UP
	   (the page negates screen-down) -- and scaled by sqrt-2 so a
	   full-length diagonal still drives both axes at full speed. The page
	   sends it as `action=pt&pan_speed=&tilt_speed=` (Api.pt.ptMove), so the
	   centre knob sends exactly that now instead of a direction word with
	   one fspeed (bug list #28). The sector keys keep the fspeed path. */
	const qreal lim = qMax(8.0, joyInnerRadius_ * 0.42);
	qreal px = qBound(-1.0, offset.x() / lim, 1.0);
	qreal py = qBound(-1.0, offset.y() / lim, 1.0);
	const qreal len = std::hypot(px, py);
	if (len > 1.0) {
		px /= len;
		py /= len;
	}
	const qreal kSqrt2 = 1.4142135623730951;
	qreal pan = qBound(-1.0, px * kSqrt2, 1.0);
	qreal tilt = qBound(-1.0, -py * kSqrt2, 1.0);
	/* The page's 3% centre dead zone; inside it the head is stopped, and the
	   stop is sent once, not on every mouse move. */
	const qreal kDead = 0.03;
	if (std::abs(pan) < kDead)
		pan = 0.0;
	if (std::abs(tilt) < kDead)
		tilt = 0.0;
	if (pan == 0.0 && tilt == 0.0) {
		if (analogMoving_) {
			analogMoving_ = false;
			stopMoving();
		}
		return;
	}
	/* Mouse moves arrive far faster than the single-threaded httpd can
	   answer, so quantize to 1/20 steps and skip repeats of the same pair. */
	const float qPan = float(qRound(pan * 20.0)) / 20.0f;
	const float qTilt = float(qRound(tilt * 20.0)) / 20.0f;
	if (analogMoving_ && qPan == analogPan_ && qTilt == analogTilt_)
		return;
	analogMoving_ = true;
	analogPan_ = qPan;
	analogTilt_ = qTilt;
	if (client_)
		client_->ptzMoveAnalog(qPan, qTilt);
	emit moveRequested((px > 0) - (px < 0), (py > 0) - (py < 0));
}

void ZcPtzPad::stopMoving()
{
	if (client_)
		client_->ptzStop();
}

/* One zoom command per (direction, speed bucket) change — see the header. The
   camera keeps zooming after the request returns, so repeating it would only
   queue work on its single-threaded httpd. */
void ZcPtzPad::pushZoom(int value)
{
	updateZoomLabel(value);
	if (!client_)
		return;

	/* A small dead zone keeps a hand that lands a notch off centre from
	   starting a permanent crawl. */
	const int dead = 4;
	const int dir = (value > dead) ? 1 : (value < -dead ? -1 : 0);
	/* Five buckets across the 100 steps: fspeed is a percentage, and five
	   distinct speeds are all a zoom rocker needs. */
	const int bucket = (dir == 0) ? 0 : qBound(1, (qAbs(value) + 19) / 20, 5);
	if (dir == zoomDir_ && bucket == zoomBucket_)
		return;
	zoomDir_ = dir;
	zoomBucket_ = bucket;

	if (dir == 0) {
		client_->lensZoomStop();
		return;
	}
	const int speed = qBound(1, bucket * 20, 100);
	if (dir > 0)
		client_->lensZoomIn(speed);
	else
		client_->lensZoomOut(speed);
}

void ZcPtzPad::updateZoomLabel(int value)
{
	if (!zoomValue_)
		return;
	/* The direction and how hard the rocker is being pushed — never the raw
	   signed slider position. "0" in the middle of the track read as "the zoom
	   level is zero, i.e. the minimum", which is the opposite of what the
	   centre means here (the lens is idle). A centred rocker now says so. */
	const int dead = 4;
	QString text = QStringLiteral("\u00b7");
	if (value > dead)
		text = QStringLiteral("T %1").arg(qMin(100, value));
	else if (value < -dead)
		text = QStringLiteral("W %1").arg(qMin(100, -value));
	zoomValue_->setText(text);
}

void ZcPtzPad::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || mode_ != JoystickMode::None) {
		QWidget::mousePressEvent(event);
		return;
	}
	updateGeometry();
	if (inCenter(event->pos())) {
		mode_ = JoystickMode::FreeDrag;
		dragOffset_ = QPointF(0, 0);
	} else {
		int idx = directionAt(event->pos());
		if (idx >= 0) {
			mode_ = JoystickMode::Sector;
			activeDir_ = idx;
			moveInDirection(idx);
			update();
		}
	}
	if (mode_ != JoystickMode::None) {
		grabMouse();
		event->accept();
	} else {
		QWidget::mousePressEvent(event);
	}
}

void ZcPtzPad::mouseMoveEvent(QMouseEvent *event)
{
	if (mode_ == JoystickMode::None) {
		QWidget::mouseMoveEvent(event);
		return;
	}
	if (mode_ == JoystickMode::Sector) {
		int idx = directionAt(event->pos());
		if (idx >= 0 && idx != activeDir_) {
			activeDir_ = idx;
			moveInDirection(idx);
		} else if (idx < 0) {
			if (activeDir_ >= 0) {
				stopMoving();
				activeDir_ = -1;
			}
		}
		update();
	} else if (mode_ == JoystickMode::FreeDrag) {
		QPointF d = QPointF(event->pos()) - joyCenter_;
		qreal lim = qMax(8.0, joyInnerRadius_ * 0.42);
		qreal dist = std::sqrt(d.x() * d.x() + d.y() * d.y());
		if (dist > lim) {
			d = d * (lim / dist);
		}
		dragOffset_ = d;
		moveAnalog(d);
		update();
	}
	event->accept();
}

void ZcPtzPad::mouseReleaseEvent(QMouseEvent *event)
{
	if (mode_ == JoystickMode::None) {
		QWidget::mouseReleaseEvent(event);
		return;
	}
	if (event->button() == Qt::LeftButton) {
		stopMoving();
		mode_ = JoystickMode::None;
		activeDir_ = -1;
		dragOffset_ = QPointF(0, 0);
		analogMoving_ = false;
		releaseMouse();
		update();
	}
	event->accept();
}

/* ---------------------------------------------------------------- */
/* ZcFramingPanel                                                    */
/* ---------------------------------------------------------------- */

namespace {
/* Re-read the framing state a moment after a change, once the camera has
   stored it. */
constexpr int kFramingReloadMs = 400;

/* How long a framing-pad arrow must be held before it starts repeating, and how
   fast it repeats after that — the page's own LONG_PRESS_DELAY and
   LONG_PRESS_INTERVAL. */
constexpr int kPadRepeatDelayMs = 500;
constexpr int kPadRepeatIntervalMs = 100;

/* The auto-framing panel speaks the whole /ctrl/framing API the camera's own
   web page uses, and is laid out like it. The tab is shown for a camera whose
   /info reports auto framing; the gate is cameraSupportsAutoFraming() in
   updatePtzTab(). */
constexpr bool kFramingPanelVisible = true;

/* Option lists lifted from the camera's web UI (framing.js constants
   AI_FRAMING_SCENE / AI_TARET_NUM / AI_TARGET_LOST_ACTION / AI_START_POSITION);
   /ctrl/framing/all_status reports the same lists. */
const char *const kFramingScenes[] = {"General", "Sports"};
const char *const kFramingLostActions[] = {"Remain", "Wide", "Start Position"};
constexpr int kFramingTargetNumMin = 1;
constexpr int kFramingTargetNumMax = 8;
/* The camera ships five built-in composition presets (bug list #12.4): they
   are the factory defaults and its page refuses to delete them, so Delete on
   their rows is refused too. */
constexpr int kFramingPresetDefaultCount = 5;
/* The camera's speed sliders run the full signed range on this firmware. */
constexpr int kFramingSpeedMin = -100;
constexpr int kFramingSpeedMax = 100;
/* The two composition offsets (AI_FOV_PLACEMENT / AI_FOV_FACE_HEIGHT) are a
   -12..12 grid on the camera's page — and the grid the framing pad walks. */
constexpr int kFramingOffsetMin = -12;
constexpr int kFramingOffsetMax = 12;
/* The page's "View Angle" dropdown (framing.js AI_FOV_TYPE) is the /fov?type=
   index 0..3; "Remain" is the fourth entry, not an extra control. */
const char *const kFramingFovTypes[] = {
    "ZCameraPlugin.Framing.FovFullBody", "ZCameraPlugin.Framing.FovWaist",
    "ZCameraPlugin.Framing.FovCloseUp", "ZCameraPlugin.Framing.FovRemain"};
/* The page's "Size" stepper walks /fov?body_size= 0..4, which its option list
   (AI_FOV_BODY_SIZE) names as these percentages: index * 10 + 80. Step 10 so a
   click moves one entry, and the spinner shows the percentage the page shows,
   never the raw index. */
constexpr int kFramingBodySizeMin = 80;
constexpr int kFramingBodySizeMax = 120;
constexpr int kFramingBodySizeStep = 10;
/* Fallback start-position labels by index, used when all_status carries no
   start_pos_opts (the camera's own list is zero-padded: "Preset 001"…). */
const char *const kFramingStartPosFallback[] = {
    "Home", "Preset 001", "Preset 002", "Preset 003", "Preset 004",
    "Preset 005"};

/* What the camera prints next to its three buttons, in its own order
   (framing.js AI_STATUS_LABELS). The index is the `status` field of
   /ctrl/framing/status and the `value` of a FramingStatusChanged message. */
constexpr int kFramingStatusCount = 5;
const char *const kFramingStatusKeys[kFramingStatusCount] = {
    "ZCameraPlugin.Framing.StatusIdle", "ZCameraPlugin.Framing.StatusDetecting",
    "ZCameraPlugin.Framing.StatusTracking", "ZCameraPlugin.Framing.StatusLost",
    "ZCameraPlugin.Framing.StatusDisabled"};
/* The same five hues the page uses (AI_STATUS_COLORS), re-tinted for this
   panel's dark background: yellow while working, green while tracking, amber
   once the subject is gone, and the panel's own muted grey when nothing runs. */
const char *const kFramingStatusColors[kFramingStatusCount] = {
    "#8a9bb2", "#ffe066", "#4ade80", "#ffb020", "#5d6b7d"};

/* The camera limits preset names by UTF-8 byte count (max_name_len), so a
   Chinese name has fewer characters than an English one. Clamp whole
   characters until it fits, like the web UI does. */
QString framingClampName(const QString &name, int maxBytes)
{
	if (maxBytes <= 0 || name.toUtf8().size() <= maxBytes)
		return name;
	QString out = name;
	while (!out.isEmpty() && out.toUtf8().size() > maxBytes)
		out.chop(1);
	return out;
}

/* Finding the dock's camera among OBS's sources.
   The dock knows the camera by address and a source knows it by its own
   `zc_source_ip`, so that is what they are matched on: a source pointing at a
   different camera is a different camera and must not be opened. Sources seen
   by the enumeration are owned by it (no release), but the settings object
   obs_source_get_settings() hands out is a new reference that has to go back. */
struct ZcCameraSourceLookup {
	QString want;
	obs_source_t *found = nullptr;
};

/* Compare two camera addresses the way an operator writes them: the source
   stores whatever was typed into its address field (`192.168.10.121`,
   `http://192.168.10.121`, `192.168.10.121:80`, with or without a trailing
   slash) while the dock knows the bare host. Matching the raw strings meant a
   correctly added source could be reported as "not in OBS", which greys the
   Detected Indicator for no visible reason (bug 21). */
QString zcNormalizeHost(const QString &address)
{
	QString h = address.trimmed().toLower();
	if (h.startsWith(QLatin1String("http://")))
		h.remove(0, 7);
	else if (h.startsWith(QLatin1String("https://")))
		h.remove(0, 8);
	while (h.endsWith(QLatin1Char('/')))
		h.chop(1);
	if (h.endsWith(QLatin1String(":80")))
		h.chop(3);
	else if (h.endsWith(QLatin1String(":443")))
		h.chop(4);
	return h;
}

bool zcFindCameraSourceProc(void *param, obs_source_t *source)
{
	auto *lookup = static_cast<ZcCameraSourceLookup *>(param);
	if (!source || !lookup || lookup->found)
		return true;
	if (QString::fromUtf8(obs_source_get_id(source)) !=
	    QStringLiteral("zcamera_source"))
		return true;

	obs_data_t *settings = obs_source_get_settings(source);
	const QString ip =
	    settings ? QString::fromUtf8(obs_data_get_string(settings,
							      "zc_source_ip"))
		     : QString();
	if (settings)
		obs_data_release(settings);

	if (zcNormalizeHost(ip) != zcNormalizeHost(lookup->want))
		return true;
	lookup->found = source;
	return false; /* stop: the first match is the one to use */
}

/* The AI-box master switch of the source that points at `host` (zc_source.cpp's
   zc_ai_overlay), or `def` when this camera has no source. The dock's Detected
   Indicator drives this rather than the camera's own /ctrl/framing/marker: the
   boxes this plugin draws come from the source's own AI feed, so the source's
   property is what actually decides whether anything is painted. */
bool cameraSourceOverlay(const QString &host, bool def)
{
	ZcCameraSourceLookup lookup;
	lookup.want = host;
	obs_enum_sources(zcFindCameraSourceProc, &lookup);
	if (!lookup.found)
		return def;
	obs_data_t *settings = obs_source_get_settings(lookup.found);
	const bool on =
	    settings && obs_data_get_bool(settings, "zc_ai_overlay");
	if (settings)
		obs_data_release(settings);
	return on;
}

/* Whether this camera exists in OBS as a source at all — the precondition for
   the Detected Indicator to have anywhere to take effect. */
bool cameraSourceExists(const QString &host)
{
	ZcCameraSourceLookup lookup;
	lookup.want = host;
	obs_enum_sources(zcFindCameraSourceProc, &lookup);
	return lookup.found != nullptr;
}

/* Write the source's AI-box master switch. A no-op when this camera is not in
   OBS: there is nothing to draw on. The source's whole settings object is read
   first and only that one key is changed, because obs_source_update() hands the
   data straight to the source's update callback, which reads every key it knows
   — a bare one-key object would blank the address and the rest. */
void setCameraSourceOverlay(const QString &host, bool on)
{
	ZcCameraSourceLookup lookup;
	lookup.want = host;
	obs_enum_sources(zcFindCameraSourceProc, &lookup);
	if (!lookup.found)
		return;
	obs_data_t *settings = obs_source_get_settings(lookup.found);
	if (!settings)
		return;
	obs_data_set_bool(settings, "zc_ai_overlay", on);
	obs_source_update(lookup.found, settings);
	obs_data_release(settings);
}
} // namespace

/* ---- The page's field metrics. ----
   Every control on the camera's page sits in one column of a grid about this
   wide, with its label on its own line above it, and the columns are laid out
   side by side. Reusing that width here is what stops an Off/On segment or a
   button from stretching across the whole dock, which reads nothing like the
   page. The pad and the action buttons get the two widths the page gives
   them. */
constexpr int kFramingTileWidth = 176;
constexpr int kFramingPadWidth = 208;
constexpr int kFramingActionWidth = 140;
/* Every live column is drawn inside its own thin frame, so the strip reads as
   the separate blocks the page lays out instead of one undifferentiated wall of
   controls. The frame and its inner margins take this much off a column's
   width; the fields are sized to what is left, so nothing overflows the box. */
constexpr int kFramingColumnInset = 18;

/* A row that wraps: every item keeps its own width and starts a new line when
   the dock is too narrow for the next one. QHBoxLayout would stretch them
   instead, and Qt has no flow layout of its own, so this is the customary one
   (QLayout subclass, no moc needed). It is what lets the panel present the
   page's several narrow columns in a dock that may only fit one. */
class ZcFlowLayout : public QLayout {
public:
	explicit ZcFlowLayout(QWidget *parent = nullptr, int margin = 0,
			      int hSpacing = 8, int vSpacing = 8)
		: QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing)
	{
		setContentsMargins(margin, margin, margin, margin);
	}

	~ZcFlowLayout() override
	{
		QLayoutItem *item;
		while ((item = takeAt(0)) != nullptr)
			delete item;
	}

	void addItem(QLayoutItem *item) override { itemList_ << item; }

	/* Only used to drop the preset block back where the page has it, between
	   Auto Target and Lock Zoom. */
	void insertWidget(int index, QWidget *widget)
	{
		if (!widget)
			return;
		if (index < 0 || index > itemList_.size())
			index = itemList_.size();
		addChildWidget(widget);
		itemList_.insert(index, new QWidgetItem(widget));
		invalidate();
	}

	int count() const override { return int(itemList_.size()); }

	QLayoutItem *itemAt(int index) const override
	{
		return (index >= 0 && index < itemList_.size())
			   ? itemList_.at(index)
			   : nullptr;
	}

	QLayoutItem *takeAt(int index) override
	{
		if (index < 0 || index >= itemList_.size())
			return nullptr;
		return itemList_.takeAt(index);
	}

	Qt::Orientations expandingDirections() const override { return {}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override
	{
		return doLayout(QRect(0, 0, width, 0), true);
	}

	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		doLayout(rect, false);
	}

	QSize sizeHint() const override { return minimumSize(); }

	QSize minimumSize() const override
	{
		QSize size;
		for (QLayoutItem *item : itemList_)
			size = size.expandedTo(item->minimumSize());
		const QMargins m = contentsMargins();
		size += QSize(m.left() + m.right(), m.top() + m.bottom());
		return size;
	}

private:
	int doLayout(const QRect &rect, bool testOnly) const
	{
		const QMargins m = contentsMargins();
		const QRect effective = rect.adjusted(m.left(), m.top(),
						      -m.right(), -m.bottom());
		int x = effective.x();
		int y = effective.y();
		int lineHeight = 0;

		for (QLayoutItem *item : itemList_) {
			QWidget *widget = item->widget();
			if (widget && widget->isHidden())
				continue;
			const QSize hint = item->sizeHint();
			int nextX = x + hint.width() + hSpace_;
			if (nextX - hSpace_ > effective.right() + 1 &&
			    lineHeight > 0) {
				x = effective.x();
				y += lineHeight + vSpace_;
				nextX = x + hint.width() + hSpace_;
				lineHeight = 0;
			}
			if (!testOnly)
				item->setGeometry(QRect(QPoint(x, y), hint));
			x = nextX;
			lineHeight = qMax(lineHeight, hint.height());
		}
		return y + lineHeight - rect.y() + m.bottom();
	}

	QList<QLayoutItem *> itemList_;
	int hSpace_;
	int vSpace_;
};

/* ---- The three small widgets the panel is built from. ----
   The page's own controls are a jquery-ui "segment" (joined radio cells), a
   slider+spinner pair and a four-arrow pad; these are those same three shapes,
   so the dock and the page are read the same way. They use plain std::function
   callbacks rather than Qt signals, so no moc is needed for them. */

/* Joined Off/On cells with the live one filled. A checkbox says the same thing
   in a different shape; the page uses a segment, so this does too. */
class ZcSegmentedControl : public QWidget {
public:
	std::function<void(int)> onActivated;

	explicit ZcSegmentedControl(const QStringList &options,
				    QWidget *parent = nullptr)
		: QWidget(parent)
	{
		auto *lay = new QHBoxLayout(this);
		lay->setContentsMargins(0, 0, 0, 0);
		lay->setSpacing(0);
		group_ = new QButtonGroup(this);
		group_->setExclusive(true);
		const int count = int(options.size());
		for (int i = 0; i < count; i++) {
			auto *btn = new QToolButton(this);
			btn->setText(options.at(i));
			btn->setCheckable(true);
			btn->setCursor(Qt::PointingHandCursor);
			btn->setSizePolicy(QSizePolicy::Expanding,
					   QSizePolicy::Fixed);
			/* Only the outer cells round their edges; the stylesheet keys
			   off this property. */
			if (count == 1)
				btn->setProperty("zcCell", "only");
			else if (i == 0)
				btn->setProperty("zcCell", "first");
			else if (i == count - 1)
				btn->setProperty("zcCell", "last");
			group_->addButton(btn, i);
			lay->addWidget(btn);
			buttons_ << btn;
		}
		connect(group_, &QButtonGroup::idClicked, this, [this](int id) {
			if (onActivated)
				onActivated(id);
		});
	}

	/* Painting from a snapshot. setChecked() never reaches idClicked — the
	   group only emits that for a real click — so this cannot echo back out as
	   a write. */
	void setCurrentIndex(int index)
	{
		for (int i = 0; i < int(buttons_.size()); i++)
			buttons_.at(i)->setChecked(i == index);
	}

	int currentIndex() const
	{
		for (int i = 0; i < int(buttons_.size()); i++) {
			if (buttons_.at(i)->isChecked())
				return i;
		}
		return 0;
	}

private:
	QList<QToolButton *> buttons_;
	QButtonGroup *group_ = nullptr;
};

/* The page's slider+spinner pair. A drag commits once, on release, and a typed
   value commits when it is committed — not once per drag tick: the camera's
   httpd is single-threaded, so one write per tick would only queue behind
   itself. */
class ZcSliderSpin : public QWidget {
public:
	std::function<void(int)> onCommitted;

	explicit ZcSliderSpin(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *lay = new QHBoxLayout(this);
		lay->setContentsMargins(0, 0, 0, 0);
		lay->setSpacing(6);
		slider_ = new QSlider(Qt::Horizontal, this);
		spin_ = new QSpinBox(this);
		spin_->setKeyboardTracking(false);
		/* Width follows the widest value+suffix so a unit like " 秒" is not
		   clipped (bug list "lost-timeout unit truncated"). */
		spin_->setMinimumWidth(60);
		lay->addWidget(slider_, 1);
		lay->addWidget(spin_, 0);

		connect(slider_, &QSlider::valueChanged, this, [this](int v) {
			QSignalBlocker block(spin_);
			spin_->setValue(v);
		});
		connect(spin_, &QSpinBox::valueChanged, this, [this](int v) {
			{
				QSignalBlocker block(slider_);
				slider_->setValue(v);
			}
			if (onCommitted)
				onCommitted(v);
		});
		connect(slider_, &QSlider::sliderReleased, this, [this]() {
			if (onCommitted)
				onCommitted(slider_->value());
		});
	}

	void setRange(int minimum, int maximum, int step = 1)
	{
		slider_->setRange(minimum, maximum);
		slider_->setSingleStep(step);
		slider_->setPageStep(qMax(step, (maximum - minimum) / 10));
		spin_->setRange(minimum, maximum);
		spin_->setSingleStep(step);
		updateSpinWidth();
	}

	void setSuffix(const QString &suffix)
	{
		spin_->setSuffix(suffix);
		updateSpinWidth();
	}

	void updateSpinWidth()
	{
		const int w = spin_->fontMetrics().horizontalAdvance(
				      QString::number(spin_->maximum()) +
				      spin_->suffix()) +
			      28; /* arrows + frame */
		spin_->setMinimumWidth(w);
	}

	void setValue(int value)
	{
		QSignalBlocker blockSlider(slider_);
		QSignalBlocker blockSpin(spin_);
		slider_->setValue(value);
		spin_->setValue(value);
	}

	int value() const { return slider_->value(); }

private:
	QSlider *slider_ = nullptr;
	QSpinBox *spin_ = nullptr;
};

/* The camera's live framing pad: a preview tile ringed by four arrows that
   nudge where the subject sits in frame. Up/down walk Face Height, left/right
   walk Placement — and left *raises* the value, exactly as the page's own
   `leftIncrement` does. A held arrow repeats after 500 ms every 100 ms, again
   as the page does.

   The page paints the live camera frame behind the tile. That frame belongs to
   the OBS source, not to this dock, so the tile paints a schematic frame with
   the current window on it and the two offsets printed below. The page's own
   overlay works off the normalised /ctrl/framing/info values while the pad
   walks the -12..12 index /ctrl/framing/fov takes, so the window is an
   indication only — the printed numbers are what is actually sent. */
class ZcFramingPad : public QWidget {
public:
	std::function<void(bool placement, int delta)> onNudge;

	explicit ZcFramingPad(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *grid = new QGridLayout(this);
		grid->setContentsMargins(0, 0, 0, 0);
		grid->setSpacing(2);
		up_ = makeArrow(QStringLiteral("\u25B2"), false, 1);
		down_ = makeArrow(QStringLiteral("\u25BC"), false, -1);
		left_ = makeArrow(QStringLiteral("\u25C0"), true, 1);
		right_ = makeArrow(QStringLiteral("\u25B6"), true, -1);
		grid->addWidget(up_, 0, 0, 1, 3, Qt::AlignHCenter);
		grid->addWidget(left_, 1, 0);
		grid->addWidget(right_, 1, 2);
		grid->addWidget(down_, 2, 0, 1, 3, Qt::AlignHCenter);
		grid->setColumnStretch(1, 1);
		/* Tall enough for the middle cell to read as a frame: the arrows take
		   22 px top and bottom plus 2 px of spacing, which leaves the tile the
		   rest. The tile is the one thing on this field an operator reads at a
		   glance, so it is given more room than the arrows need. */
		setFixedHeight(136);
		setMinimumWidth(152);
	}

	void setOffsets(int placement, int faceHeight)
	{
		placement_ = placement;
		faceHeight_ = faceHeight;
		update();
	}

	/* How large the subject is drawn, from the composition Size (0..100).
	   The two offsets only *move* the subject; Size is what makes it fill
	   more of the frame, so this is the one input that may change its
	   dimensions (bug lists 18/19/56 — the figure used to scale with Face
	   Height, which is a position, so nudging the subject up also grew it). */
	void setSubjectSize(int pct)
	{
		subjectSize_ = qBound(0, pct, 100);
		update();
	}

	/* Bug list 13: with Tilt Lock on the camera refuses face-height (up/down)
	   nudges, so those two arrows grey out; left/right keeps working. */
	void setVerticalEnabled(bool on)
	{
		up_->setEnabled(on);
		down_->setEnabled(on);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		/* The middle cell is left empty for the tile; its rect is whatever the
		   arrows leave. */
		const QRect tile(left_->geometry().right() + 3,
				 up_->geometry().bottom() + 3,
				 right_->geometry().left() -
				     left_->geometry().right() - 6,
				 down_->geometry().top() -
				     up_->geometry().bottom() - 6);
		if (tile.width() < 8 || tile.height() < 8)
			return;

		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, false);
		p.fillRect(tile, QColor("#1b2422"));
		p.setPen(QColor("#3c514e"));
		p.drawRect(tile.adjusted(0, 0, -1, -1));

		/* A faint cross: the frame centre the subject is placed against. */
		p.setPen(QColor("#2c3a37"));
		p.drawLine(tile.center().x(), tile.top() + 2, tile.center().x(),
			   tile.bottom() - 2);
		p.drawLine(tile.left() + 2, tile.center().y(), tile.right() - 2,
			   tile.center().y());

		/* The subject. The page's tile shows a person, not a window: a
		   rectangle sliding round reads as the aperture moving, while what the
		   two offsets actually place is the subject. Its shape is fixed — only
		   its position follows the offsets.

		   Drawn as a stick figure rather than the filled silhouette this used
		   to be: the tile is too small to hold a solid body *and* four limbs
		   without all five merging into one blob, which is what the old big
		   head and tightly stacked strokes produced. A small head, a torso
		   stroke, and four limbs that start at the torso's ends and fan away
		   from it read as a person instead.

		   Horizontal is mapped against its arrow and vertical with it, which
		   is what the camera's own page does: its left arrow raises Placement
		   (the `leftIncrement` handler), and raising Placement moves the
		   subject to the *right* of the frame — so the figure crosses the tile
		   away from the arrow that nudged it. FaceHeight runs the other way
		   (raising it lifts the subject), so that axis follows its arrow. */
		const double span = double(kFramingOffsetMax - kFramingOffsetMin);
		const double fx = double(placement_ - kFramingOffsetMin) / span;
		const double fy = double(faceHeight_ - kFramingOffsetMin) / span;

		/* The figure's size follows the composition Size, not an offset: the
		   tile is a picture of the frame, so a subject the camera has been
		   told to make larger has to be drawn larger (bug lists 18/19/56).
		   Face Height and Placement only place it — reading the size off
		   Face Height (what this did) made every up/down nudge resize the
		   subject, which is not what that control does (bug list 181). */
		const double sizeT = qBound(0.0, subjectSize_ / 100.0, 1.0);
		const int fw = qMax(10, int(tile.width() * (0.14 + 0.22 * sizeT)));
		const int fh = qMax(18, int(tile.height() * (0.34 + 0.52 * sizeT)));
		const int margin = 3;
		const double left =
		    tile.left() + margin +
		    (tile.width() - 2 * margin - fw) * qBound(0.0, fx, 1.0);
		const double top =
		    tile.bottom() - margin - fh -
		    (tile.height() - 2 * margin - fh) * qBound(0.0, fy, 1.0);

		const double cx = left + fw / 2.0;
		const double headD = fw * 0.28;
		const double headCy = top + headD / 2.0;
		/* A gap under the head, so it does not sit on the shoulders. */
		const double neckY = top + headD * 1.3;
		const double hipY = top + fh * 0.64;
		const double shoulderY = neckY + (hipY - neckY) * 0.18;
		const double halfSpan = fw * 0.42;

		const QColor subject("#d8b878");
		const double limbW = qMax(1.6, fw * 0.13);
		p.setRenderHint(QPainter::Antialiasing, true);

		p.setPen(Qt::NoPen);
		p.setBrush(subject);
		p.drawEllipse(QPointF(cx, headCy), headD / 2.0, headD / 2.0);

		p.setBrush(Qt::NoBrush);
		/* Torso: neck to hips. */
		p.setPen(QPen(subject, limbW * 1.15, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(cx, neckY), QPointF(cx, hipY));

		/* Arms off the shoulder point, legs off the hip point. The arms fan
		   out wide, the legs much less: at the tile's size a wide stance
		   reads as a jumping jack rather than as someone standing, so the
		   feet stay close to the hip line and the two legs run nearly
		   parallel. */
		p.setPen(QPen(subject, limbW, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(cx, shoulderY),
			   QPointF(cx - halfSpan, shoulderY + fh * 0.3));
		p.drawLine(QPointF(cx, shoulderY),
			   QPointF(cx + halfSpan, shoulderY + fh * 0.3));
		p.drawLine(QPointF(cx, hipY),
			   QPointF(cx - halfSpan * 0.48, top + fh));
		p.drawLine(QPointF(cx, hipY),
			   QPointF(cx + halfSpan * 0.48, top + fh));
	}

private:
	QToolButton *makeArrow(const QString &glyph, bool placement, int delta)
	{
		auto *btn = new QToolButton(this);
		btn->setText(glyph);
		btn->setAutoRepeat(true);
		btn->setAutoRepeatDelay(kPadRepeatDelayMs);
		btn->setAutoRepeatInterval(kPadRepeatIntervalMs);
		btn->setFixedSize(26, 22);
		btn->setCursor(Qt::PointingHandCursor);
		btn->setProperty("zcPadArrow", true);
		connect(btn, &QToolButton::clicked, this, [this, placement, delta]() {
			if (onNudge)
				onNudge(placement, delta);
		});
		return btn;
	}

	QToolButton *up_ = nullptr;
	QToolButton *down_ = nullptr;
	QToolButton *left_ = nullptr;
	QToolButton *right_ = nullptr;
	int placement_ = 0;
	int faceHeight_ = 0;
	/* Composition Size, 0..100. Starts mid-range so a camera that has never
	   reported one still draws a plausible subject. */
	int subjectSize_ = 50;
};

const QString kFramingPanelStyle =
    "QWidget#FramingPanel, QTabWidget#FramingPages::pane { background:#1f2927; "
    "border:none; }"
    "QTabBar::tab { color:#8a9bb2; background:#22302e; padding:5px 14px; "
    "border:1px solid #3c514e; border-bottom:none; font-size:11px; "
    "letter-spacing:1px; }"
    "QTabBar::tab:selected { color:#eef2ed; background:#1f2927; "
    "border-top:2px solid #d8b878; }"
    "QTabBar::tab:hover:!selected { color:#eef2ed; }"
    "QLabel { color:#8a9bb2; font-size:11px; letter-spacing:1px; }"
    "QLabel#FramingStatus { color:#8a9bb2; font-size:13px; font-weight:bold; "
    "letter-spacing:1px; }"
    "QLabel#FramingHint { color:#6f7f92; font-size:10px; letter-spacing:0px; }"
    /* The page's field caption: a quiet line above the control. */
    "QLabel#FramingFieldLabel { color:#8a9bb2; font-size:11px; "
    "letter-spacing:0px; }"
    "QLabel#FramingReadOut { color:#d8b878; font-size:11px; "
    "letter-spacing:0px; }"
    "QPushButton { color:#eef2ed; background:#2b3a38; border:1px solid "
    "#3c514e; border-radius:4px; padding:6px 12px; font-size:11px; }"
    "QPushButton:hover { background:#354845; }"
    "QPushButton:pressed { background:#d8b878; color:#1f2927; }"
    "QPushButton:disabled { color:#5d6b7d; background:#242f2d; }"
    /* A checkable button has to *look* checked: the panel draws its own
       background, so without this an on button is indistinguishable from an
       off one. Used by the monitor-window toggle in the status row. */
    "QPushButton:checked { background:#d8b878; color:#1f2927; "
    "border-color:#d8b878; font-weight:bold; }"
    "QPushButton#framingEnable { font-weight:bold; }"
    /* The page's segment: joined cells, the live one filled. */
    "QToolButton[zcCell] { color:#cfe0dc; background:#2b3a38; "
    "border:1px solid #3c514e; padding:3px 8px; font-size:11px; }"
    "QToolButton[zcCell=\"first\"] { border-top-left-radius:4px; "
    "border-bottom-left-radius:4px; }"
    "QToolButton[zcCell=\"last\"] { border-top-right-radius:4px; "
    "border-bottom-right-radius:4px; }"
    "QToolButton[zcCell=\"only\"] { border-radius:4px; }"
    "QToolButton[zcCell]:checked { background:#d8b878; color:#1f2927; "
    "font-weight:bold; }"
    "QToolButton[zcCell]:hover:!checked { background:#354845; }"
    "QToolButton[zcCell]:disabled { color:#5d6b7d; background:#242f2d; }"
    "QToolButton[zcPadArrow] { color:#cfe0dc; background:#2b3a38; "
    "border:1px solid #3c514e; border-radius:3px; font-size:9px; }"
    "QToolButton[zcPadArrow]:hover { background:#354845; }"
    "QToolButton[zcPadArrow]:pressed { background:#d8b878; color:#1f2927; }"
    "QToolButton[zcPadArrow]:disabled { color:#5d6b7d; background:#242f2d; }"
    "QGroupBox { color:#d8b878; font-size:11px; border:1px solid #33413f; "
    "border-radius:4px; margin-top:8px; padding:8px 6px 6px 6px; }"
    "QGroupBox::title { subcontrol-origin: margin; left:8px; padding:0 4px; }"
    "QGroupBox:disabled { color:#6f7f92; }"
    /* The live columns: a quiet rounded frame with no title, so each block of
       controls reads as its own card against the panel. */
    "QFrame#framingColumn { border:1px solid #33413f; border-radius:6px; "
    "background:#232e2c; }"
    "QComboBox, QSpinBox, QLineEdit { color:#eef2ed; background:#2b3a38; "
    "border:1px solid #3c514e; border-radius:4px; padding:3px 6px; "
    "font-size:11px; }"
    "QComboBox:disabled, QSpinBox:disabled { color:#5d6b7d; "
    "background:#242f2d; }"
    "QListWidget { color:#eef2ed; background:#232e2c; "
    "border:1px solid #3c514e; border-radius:4px; font-size:11px; }"
    "QListWidget::item:selected { background:#3c514e; }"
    "QSlider::groove:horizontal { height:4px; background:#2b3a38; "
    "border-radius:2px; }"
    "QSlider::handle:horizontal { width:10px; margin:-5px 0; "
    "background:#d8b878; border-radius:5px; }"
    "QSlider::sub-page:horizontal { background:#8a9bb2; border-radius:2px; }"
    "QSlider:disabled::handle:horizontal { background:#5d6b7d; }";

/* Auto-framing controls in their own tab, laid out like the camera's own page
   so an operator who knows that page can read this one without relearning it.

   The page splits Auto Framing across two places and so does this panel: the
   live page and the settings page (the power-on and gesture switches, the
   delays, the framing-box control and the profile export). The tab is gated on
   cameraSupportsAutoFraming() in updatePtzTab().

   The live page is the page's own landscape strip: a status word across the
   top, then narrow captioned columns laid side by side -- [view angle, size,
   scene] [start position, lock zoom, lock tilt] [the subject pad] [number of
   targets, registered face tracking] [auto target, lost timeout, lost action]
   [detection area, sensitivity, acceleration] and, at the far right, [enable,
   start, restart]. A wrapping flow holds one column per item, so a dock too
   narrow for the whole strip moves a whole column onto the next row rather
   than half of one, and no control is ever stretched across the dock.

   Writes happen on change, as on the page: there is no Apply button, so a
   control can never be written with a value it is not showing. */
/* ------------------------------------------------------------------ */
/* ZcFaceRegDialog                                                     */
/* ------------------------------------------------------------------ */

/* The camera's registered-face database (/ctrl/facedb/*). The web UI edits it
   in a dialog of its own, and so does this: list, capture-and-add, rename /
   re-prioritise, delete. Capturing is the two-step flow the camera imposes —
   /prepare takes the face out of the live view and answers with a uid and a
   thumbnail, /add then stores that uid under the operator's name. Without a
   face in frame, /prepare answers status 2 "face not found", so the dialog
   keeps the capture button and shows the camera's own wording.

   Nothing here is drawn when the camera has no face database: the first
   /query that comes back with code < 0 turns the dialog into a one-line
   "not supported" notice instead of a dead set of controls. */
class ZcFaceRegDialog : public QDialog {
public:
	explicit ZcFaceRegDialog(ZcCameraClient *client, QWidget *parent)
		: QDialog(parent), client_(client)
	{
		setWindowTitle(ztr("ZCameraPlugin.Framing.FaceRegTitle"));
		setMinimumWidth(420);
		auto *lay = new QVBoxLayout(this);

		info_ = new QLabel(this);
		info_->setWordWrap(true);
		info_->setStyleSheet("color:#8a9bb2;");
		lay->addWidget(info_);

		list_ = new QListWidget(this);
		list_->setMinimumHeight(160);
		lay->addWidget(list_, 1);

		auto *form = new QGridLayout;
		form->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.FaceRegName"), this),
				0, 0);
		name_ = new QLineEdit(this);
		name_->setMaxLength(18);
		form->addWidget(name_, 0, 1);
		form->addWidget(
		    new QLabel(ztr("ZCameraPlugin.Framing.FaceRegPriority"), this),
		    1, 0);
		priority_ = new QSpinBox(this);
		priority_->setRange(1, 99);
		priority_->setValue(1);
		form->addWidget(priority_, 1, 1);
		/* Bug list #8: the page's entry form carries the "activate" flag
		   (it is the `activate=` parameter of add/update), and this dialog
		   had no way to set it — every edit silently re-activated the face.
		   Filled from the camera's own per-face record, sent back on both
		   capture-and-add and apply. */
		active_ = new QCheckBox(
		    ztr("ZCameraPlugin.Framing.FaceRegActive"), this);
		active_->setChecked(true);
		active_->setToolTip(
		    ztr("ZCameraPlugin.Framing.FaceRegActiveTip"));
		form->addWidget(active_, 2, 0, 1, 2);
		lay->addLayout(form);

		auto *row = new QHBoxLayout;
		capture_ = new QPushButton(
		    ztr("ZCameraPlugin.Framing.FaceRegCapture"), this);
		capture_->setToolTip(ztr("ZCameraPlugin.Framing.FaceRegCaptureTip"));
		apply_ = new QPushButton(
		    ztr("ZCameraPlugin.Framing.FaceRegApply"), this);
		del_ = new QPushButton(ztr("ZCameraPlugin.Framing.FaceRegDelete"),
				       this);
		refresh_ = new QPushButton(
		    ztr("ZCameraPlugin.Framing.FaceRegRefresh"), this);
		row->addWidget(capture_);
		row->addWidget(apply_);
		row->addWidget(del_);
		row->addWidget(refresh_);
		lay->addLayout(row);

		connect(refresh_, &QPushButton::clicked, this,
			[this]() { refresh(); });
		connect(capture_, &QPushButton::clicked, this,
			[this]() { captureAndAdd(); });
		connect(apply_, &QPushButton::clicked, this,
			[this]() { applyEdit(); });
		connect(del_, &QPushButton::clicked, this, [this]() { remove(); });
		connect(list_, &QListWidget::currentRowChanged, this,
			[this](int row) { fillFromRow(row); });

		/* The camera's own page turns the store on before it captures;
		   parity, and harmless where the firmware ignores it. */
		if (client_)
			client_->setFramingFaceEnable(true);
		refresh();
	}

private:
	void setBodyEnabled(bool on)
	{
		list_->setEnabled(on);
		name_->setEnabled(on);
		priority_->setEnabled(on);
		active_->setEnabled(on);
		capture_->setEnabled(on);
		apply_->setEnabled(on);
		del_->setEnabled(on);
	}

	void refresh()
	{
		if (!client_)
			return;
		QPointer<QObject> alive(alive_);
		client_->framingFaceQuery([this, alive](const QJsonObject &doc) {
			if (!alive)
				return;
			if (doc.contains(QLatin1String("code")) &&
			    doc.value(QLatin1String("code")).toInt() < 0) {
				supported_ = false;
				info_->setText(ztr(
				    "ZCameraPlugin.Framing.FaceRegUnsupported"));
				setBodyEnabled(false);
				return;
			}
			supported_ = true;
			setBodyEnabled(true);
			max_ = doc.value(QLatin1String("max")).toInt(max_);
			maxActive_ = doc.value(QLatin1String("max_active"))
					 .toInt(maxActive_);
			uids_.clear();
			names_.clear();
			priorities_.clear();
			actives_.clear();
			const QJsonArray faces =
			    doc.value(QLatin1String("faces")).toArray();
			list_->clear();
			for (const QJsonValue &v : faces) {
				const QJsonObject f = v.toObject();
				uids_ << f.value(QLatin1String("uid")).toInt();
				names_ << f.value(QLatin1String("name"))
					      .toString();
				priorities_ << f.value(QLatin1String("priority"))
						   .toInt();
				/* The flag arrives as a bool on some builds and as
				   1/0 on others; a record that carries neither is
				   taken as active, which is what the camera does
				   with a stored face. */
				/* The flag is `activate` on the firmware whose page
				   was read, but a differently-worded reply would have
				   fallen through to "always active" — which is
				   exactly what "the activate switch does nothing"
				   looked like (bug 191). Accept the spellings. */
				QJsonValue act = f.value(QLatin1String("activate"));
				if (act.isUndefined())
					act = f.value(QLatin1String("active"));
				if (act.isUndefined())
					act = f.value(QLatin1String("act"));
				if (act.isBool())
					actives_ << act.toBool();
				else if (act.isDouble())
					actives_ << (act.toInt() != 0);
				else if (act.isString())
					actives_ << (act.toString() !=
						     QLatin1String("0"));
				else
					actives_ << true;
				list_->addItem(names_.last());
			}
			/* Bug 191: the camera is the truth. If the entry it just sent
			   back is not what was written, say so instead of leaving the
			   operator to wonder whether the switch worked. */
			if (pendingUid_ >= 0) {
				const int at = uids_.indexOf(pendingUid_);
				const bool held =
				    at >= 0 && names_.at(at) == pendingName_ &&
				    priorities_.at(at) == pendingPriority_ &&
				    actives_.at(at) == pendingActive_;
				if (!held)
					info_->setText(ztr(
					    "ZCameraPlugin.Framing.FaceRegRejected"));
				pendingUid_ = -1;
			}
			if (pendingUid_ < 0 &&
			    info_->text().isEmpty())
				info_->setText(
				    ztr("ZCameraPlugin.Framing.FaceRegInfo")
					.arg(uids_.size())
					.arg(max_)
					.arg(maxActive_));
			/* Bug 190: a new entry gets the next free priority instead of
			   repeating 1. Priority is what decides between two faces in
			   frame, so two entries sharing one leaves that choice to the
			   camera. */
			if (list_->currentRow() < 0) {
				int next = 1;
				for (int p : priorities_)
					next = qMax(next, p + 1);
				priority_->setValue(
				    qBound(1, next, priority_->maximum()));
			}
			/* Bug 192: opening the dialog must not land on the first entry
			   with its name and priority filled in — that reads as "these are
			   the values for the face I am about to add". Start unselected
			   (the fields belong to an explicit selection or to a new
			   capture). */
		});
	}

	/* The limits this firmware actually has, set by the caller before show():
	   1.0.5 registers and tracks 20 faces, the 1.0.3 generation's UI says 10
	   and 8 (bug 13). A face-database reply that carries max / max_active
	   still overrides these. */
public:
	void setDefaults(int max, int maxActive)
	{
		if (max > 0)
			max_ = max;
		if (maxActive > 0)
			maxActive_ = maxActive;
		if (info_)
			/* Bug 191: the camera is the truth. If the entry it just sent
			   back is not what was written, say so instead of leaving the
			   operator to wonder whether the switch worked. */
			if (pendingUid_ >= 0) {
				const int at = uids_.indexOf(pendingUid_);
				const bool held =
				    at >= 0 && names_.at(at) == pendingName_ &&
				    priorities_.at(at) == pendingPriority_ &&
				    actives_.at(at) == pendingActive_;
				if (!held)
					info_->setText(ztr(
					    "ZCameraPlugin.Framing.FaceRegRejected"));
				pendingUid_ = -1;
			}
			if (pendingUid_ < 0 &&
			    info_->text().isEmpty())
				info_->setText(
				    ztr("ZCameraPlugin.Framing.FaceRegInfo")
					.arg(uids_.size())
					.arg(max_)
					.arg(maxActive_));
			/* Bug 190: a new entry gets the next free priority instead of
			   repeating 1. Priority is what decides between two faces in
			   frame, so two entries sharing one leaves that choice to the
			   camera. */
			if (list_->currentRow() < 0) {
				int next = 1;
				for (int p : priorities_)
					next = qMax(next, p + 1);
				priority_->setValue(
				    qBound(1, next, priority_->maximum()));
			}
	}

	void fillFromRow(int row)
	{
		if (row < 0 || row >= names_.size())
			return;
		name_->setText(names_.at(row));
		priority_->setValue(qMax(1, priorities_.at(row)));
		active_->setChecked(row < actives_.size() ? actives_.at(row)
							  : true);
	}

	void captureAndAdd()
	{
		if (!client_ || !supported_)
			return;
		const QString name = name_->text().trimmed();
		if (name.isEmpty()) {
			info_->setText(
			    ztr("ZCameraPlugin.Framing.FaceRegNeedName"));
			return;
		}
		const int priority = priority_->value();
		/* The first slot the list does not already hold; the camera
		   captures INTO a slot, so add() and prepare() have to agree on
		   one (bug list #3: without a slot the camera said "face not
		   found" for every capture). All slots taken falls back to 0 —
		   the camera rejects the store itself if it is really full. */
		int slot = 0;
		while (slot < max_ && uids_.contains(slot))
			++slot;
		if (slot >= max_)
			slot = 0;
		info_->setText(ztr("ZCameraPlugin.Framing.FaceRegCapturing"));
		QPointer<QObject> alive(alive_);
		client_->framingFacePrepare(slot, [this, alive, name, priority](
							const QJsonObject &doc) {
			if (!alive)
				return;
			const int status =
			    doc.value(QLatin1String("status")).toInt(-1);
			const int uid =
			    doc.value(QLatin1String("uid")).toInt(-1);
			if (status != 0 || uid < 0) {
				/* The camera's own wording ("face not found",
				   "face too small") is the useful part. */
				QString why = doc.value(QLatin1String("detail"))
						  .toString();
				if (why.isEmpty())
					why = ztr("ZCameraPlugin.Framing."
						  "FaceRegCaptureFailed");
				info_->setText(why);
				return;
			}
			if (client_) {
				/* `which` is the uid prepare just handed back;
				   activate makes the camera adopt the new entry
				   at once, as the page does. The thumbnail in
				   the reply is the camera's own preview and is
				   not needed again by `add`. */
				client_->framingFaceAdd(uid, name, priority,
							active_->isChecked());
				/* The store is asynchronous; give the camera a
				   beat before reading the list back. */
				QTimer::singleShot(400, this,
						   [this]() { refresh(); });
			}
		});
	}

	void applyEdit()
	{
		if (!client_ || !supported_)
			return;
		const int row = list_->currentRow();
		if (row < 0 || row >= uids_.size()) {
			info_->setText(
			    ztr("ZCameraPlugin.Framing.FaceRegNoSelection"));
			return;
		}
		/* Remember what was asked for: the refresh that follows compares the
		   camera's own values against it, so a refused update shows as a
		   refusal instead of silently looking applied (bug 191). */
		pendingUid_ = uids_.at(row);
		pendingName_ = name_->text().trimmed();
		pendingPriority_ = priority_->value();
		pendingActive_ = active_->isChecked();
		client_->framingFaceUpdate(uids_.at(row), name_->text().trimmed(),
					   priority_->value(),
					   active_->isChecked());
		QTimer::singleShot(400, this, [this]() { refresh(); });
	}

	void remove()
	{
		if (!client_ || !supported_)
			return;
		const int row = list_->currentRow();
		if (row < 0 || row >= uids_.size()) {
			info_->setText(
			    ztr("ZCameraPlugin.Framing.FaceRegNoSelection"));
			return;
		}
		client_->framingFaceDelete(uids_.at(row));
		QTimer::singleShot(400, this, [this]() { refresh(); });
	}

	ZcCameraClient *client_ = nullptr;
	QLabel *info_ = nullptr;
	/* The entry an applyEdit() asked for, -1 when nothing is pending. */
	int pendingUid_ = -1;
	QString pendingName_;
	int pendingPriority_ = 0;
	bool pendingActive_ = false;
	QListWidget *list_ = nullptr;
	QLineEdit *name_ = nullptr;
	QSpinBox *priority_ = nullptr;
	QCheckBox *active_ = nullptr;
	QPushButton *capture_ = nullptr;
	QPushButton *apply_ = nullptr;
	QPushButton *del_ = nullptr;
	QPushButton *refresh_ = nullptr;
	/* Liveness token. The dialog is WA_DeleteOnClose, yet the client's HTTP
	   replies keep arriving after it closes - the callbacks captured below
	   have no context object of their own, so a late reply would otherwise
	   write into a destroyed widget. Every asynchronous callback takes a
	   *copy* of this by value and bails once it has nulled out; the copy
	   must be by value because reading a member through a dangling `this`
	   is already undefined. */
	QPointer<QObject> alive_{this};

	QVector<int> uids_;
	QStringList names_;
	QVector<int> priorities_;
	/* Per-face `activate` flag, in the same order as uids_. */
	QVector<bool> actives_;
	int max_ = 10;
	int maxActive_ = 8;
	bool supported_ = true;
};

/* The camera's page explains a field with a "?" beside it and shows the text
   when it is clicked, not on a hover. Used by the rows whose explanation the
   tester asked to be clickable (bug 43). */
static void zcShowHelpText(QWidget *parent, const QString &title,
			   const QString &text)
{
	QDialog dlg(parent);
	dlg.setWindowTitle(title);
	auto *lay = new QVBoxLayout(&dlg);
	auto *body = new QLabel(text, &dlg);
	body->setWordWrap(true);
	body->setMaximumWidth(420);
	lay->addWidget(body);
	auto *box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	QObject::connect(box, &QDialogButtonBox::rejected, &dlg,
			 &QDialog::reject);
	lay->addWidget(box);
	dlg.exec();
}

/* A control with its "?" beside it; the wrapper is what the row shows. */
static QWidget *zcWithHelp(QWidget *parent, QWidget *control,
			   const QString &title, const QString &text)
{
	auto *wrap = new QWidget(parent);
	auto *row = new QHBoxLayout(wrap);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(4);
	row->addWidget(control);
	auto *btn = new QToolButton(wrap);
	btn->setText(QStringLiteral("?"));
	btn->setAutoRaise(true);
	btn->setToolTip(title);
	QObject::connect(btn, &QToolButton::clicked, wrap,
			 [wrap, title, text]() {
				 zcShowHelpText(wrap, title, text);
			 });
	row->addWidget(btn);
	return wrap;
}

ZcFramingPanel::ZcFramingPanel(QWidget *parent) : QWidget(parent)
{
	setObjectName("FramingPanel");
	setStyleSheet(kFramingPanelStyle);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	outer->addWidget(scroll);

	auto *body = new QWidget;
	scroll->setWidget(body);
	auto *layout = new QVBoxLayout(body);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	auto *title = new QLabel(ztr("ZCameraPlugin.Framing.Title"), body);
	layout->addWidget(title, 0, Qt::AlignHCenter);

	auto *pages = new QTabWidget(body);
	pages->setObjectName("FramingPages");
	pages->setDocumentMode(true);
	layout->addWidget(pages, 1);

	/* One field of the page's grid: the caption on a line of its own, the
	   control under it, and the pair exactly as wide as the caller asks. A
	   fixed width is what keeps an Off/On segment or a button from being
	   stretched across the whole dock — the page gives every field a column
	   of its own and the controls are that wide and no wider. */
	const auto makeRow = [](QWidget *parentWidget, const QString &text,
				QWidget *control, int width) {
		auto *w = new QWidget(parentWidget);
		w->setFixedWidth(width);
		auto *lay = new QVBoxLayout(w);
		lay->setContentsMargins(0, 0, 0, 0);
		lay->setSpacing(3);
		auto *label = new QLabel(text, w);
		label->setObjectName("FramingFieldLabel");
		label->setWordWrap(true);
		lay->addWidget(label);
		lay->addWidget(control);
		return w;
	};
	/* A field built from parts rather than from one control. */
	const auto makeField = [](QWidget *parentWidget, const QString &text,
				  QWidget *body, int width) {
		auto *w = new QWidget(parentWidget);
		w->setFixedWidth(width);
		auto *lay = new QVBoxLayout(w);
		lay->setContentsMargins(0, 0, 0, 0);
		lay->setSpacing(3);
		auto *label = new QLabel(text, w);
		label->setObjectName("FramingFieldLabel");
		label->setWordWrap(true);
		lay->addWidget(label);
		lay->addWidget(body);
		return w;
	};
	const auto segment = [](QWidget *parentWidget) {
		return new ZcSegmentedControl({ztr("ZCameraPlugin.Framing.Off"),
						ztr("ZCameraPlugin.Framing.On")},
					       parentWidget);
	};
	/* The page's segments all open on Off (a switch nobody has set yet). */
	const auto offSegment = [&segment](QWidget *parentWidget) {
		auto *seg = segment(parentWidget);
		seg->setCurrentIndex(0);
		return seg;
	};

	/* ============ Live: the page's live Auto Framing panel ================ */
	auto *live = new QWidget(pages);
	auto *liveLay = new QVBoxLayout(live);
	liveLay->setContentsMargins(8, 8, 8, 8);
	liveLay->setSpacing(8);

	/* The camera's status word ("Idle" / "Detecting" / "Tracking" /
	   "Target Lost" / "Disabled"), repainted on refresh, on every
	   FramingStatusChanged and after every button. The camera's three live
	   buttons share this row instead of sitting in a column of their own at the
	   far right of the grid: the word reads from the left and the buttons sit
	   at the right end of the same line, which scans better than a lone word
	   followed by a separate stack.

	   The monitor window's button leads the row. It is the one control here
	   that is about *watching* rather than operating, and it is a toggle: its
	   checked face means the window is up, so its state answers "is the monitor
	   window open?" without opening the View menu, and a second press closes
	   it. It used to be a plain button in a row of its own below the grid. */
	auto *statusRow = new QHBoxLayout;
	statusRow->setSpacing(8);
	monitorBtn_ = new QPushButton(ztr("ZCameraPlugin.Framing.OpenMonitor"), live);
	monitorBtn_->setCheckable(true);
	monitorBtn_->setToolTip(ztr("ZCameraPlugin.Framing.OpenMonitorTip"));
	statusRow->addWidget(monitorBtn_);
	statusLabel_ = new QLabel(ztr("ZCameraPlugin.Framing.StatusIdle"), live);
	statusLabel_->setObjectName("FramingStatus");
	statusLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	statusLabel_->setWordWrap(true);
	statusRow->addWidget(statusLabel_, 1);

	/* Enable/Disable toggles the camera's detection, Start/Stop its tracking,
	   Restart re-runs detection. Built here so the row above can hold them; the
	   connects and the gating are below. */
	enableBtn_ = new QPushButton(live);
	enableBtn_->setObjectName("framingEnable");
	startBtn_ = new QPushButton(live);
	startBtn_->setObjectName("framingStart");
	restartBtn_ = new QPushButton(ztr("ZCameraPlugin.Framing.Restart"), live);
	restartBtn_->setToolTip(ztr("ZCameraPlugin.Framing.RestartTip"));
	statusRow->addWidget(enableBtn_);
	statusRow->addWidget(startBtn_);
	statusRow->addWidget(restartBtn_);
	liveLay->addLayout(statusRow);

	/* The hint that explains the Enable-then-Start order. The page has no such
	   line -- its gate is only implied -- but the switch really does stop
	   Start and Restart from doing anything, which is not obvious from the
	   page either. */
	QLabel *gateHint = nullptr;
	{
		auto *hint = new QLabel(ztr("ZCameraPlugin.Framing.GateHint"), live);
		hint->setObjectName("FramingHint");
		hint->setWordWrap(true);
		gateHint = hint;
	}

	/* ---- The page's live panel, reproduced as the landscape grid it is. ----
	   The camera's page never stacks full-width rows. It lays its live
	   controls out as a strip of narrow captioned columns and wraps that strip
	   when the window is narrow: [view angle, size, scene] [start position,
	   lock zoom, lock tilt] [the subject pad] [number of targets, registered
	   face tracking] [auto target, lost timeout, lost action] [detection area,
	   sensitivity, acceleration]. The page's own far-right [enable, start,
	   restart] column is not reproduced here: those buttons live in the status
	   row at the top of the page instead. One wrapping flow holding one item
	   per column reproduces that
	   reading order, keeps every column intact, and -- on a dock too narrow
	   for the whole strip -- moves a whole column onto the next row instead of
	   half of one, so the grid always reads the way the page does.

	   Captions sit above their controls, as on the page, and every field is
	   one column wide. That width is the point: it is what stops an Off/On
	   segment or a button from being stretched across the whole dock, which
	   reads nothing like the page. */
	auto *grid = new QWidget(live);
	auto *flow = new ZcFlowLayout(grid, 0, 12, 8);

	/* One of the page's columns: a fixed-width stack of fields, wrapped in its
	   own frame so the strip reads as separate blocks. The column widget is the
	   layout's parent; QFrame draws the frame the stylesheet gives it. */
	const auto makeColumn = [](QWidget *parentWidget, int width) {
		auto *col = new QFrame(parentWidget);
		col->setObjectName("framingColumn");
		col->setFrameShape(QFrame::NoFrame);
		col->setFixedWidth(width);
		auto *lay = new QVBoxLayout(col);
		lay->setContentsMargins(8, 8, 8, 8);
		lay->setSpacing(8);
		return lay;
	};
	/* One field in a column, exactly as wide as the column. `gated` adds it to
	   the live block the camera greys out while detection is off; the fields
	   the camera leaves live -- the view angle, the size and the pad -- pass
	   false, because the page leaves them live too. */
	const auto addField = [&](QVBoxLayout *column, const QString &text,
				  QWidget *control, bool gated) {
		QWidget *field = makeRow(column->parentWidget(), text, control,
					 column->parentWidget()->minimumWidth() -
					     kFramingColumnInset);
		column->addWidget(field);
		if (gated)
			gatedGroups_ << field;
		return field;
	};
	/* The same, for a field built from several parts rather than from one
	   control. */
	const auto addFieldBody = [&](QVBoxLayout *column, const QString &text,
				      QWidget *body, bool gated) {
		QWidget *field = makeField(column->parentWidget(), text, body,
					   column->parentWidget()->minimumWidth() -
					       kFramingColumnInset);
		column->addWidget(field);
		if (gated)
			gatedGroups_ << field;
		return field;
	};
	const auto placeColumn = [&](QVBoxLayout *column) {
		flow->addWidget(column->parentWidget());
	};

	/* --- Column 1: the view angle, its size, and the scene. The page's first
	   three controls, in its own order: "View Angle" is the /fov?type=
	   dropdown, "Size" the /fov?body_size= stepper behind it, and "Scene" the
	   /scene?idx= choice. All three stay live while detection is off, exactly
	   as the page leaves them. --- */
	QVBoxLayout *colView = makeColumn(grid, kFramingTileWidth);
	viewAngleCombo_ = new QComboBox(colView->parentWidget());
	for (const char *key : kFramingFovTypes)
		viewAngleCombo_->addItem(ztr(key));
	viewAngleField_ = addField(colView,
		ztr("ZCameraPlugin.Framing.FovSize"), viewAngleCombo_, false);
	connect(viewAngleCombo_, &QComboBox::currentIndexChanged, this,
		[this](int index) {
			if (painting_ || !client_)
				return;
			client_->setFramingFovType(index);
		});

	/* The page's "Size" stepper walks /fov?body_size= 0..4 but prints the
	   percentage its option list names (index * 10 + 80), so the spinner shows
	   the percentage and the write sends the index back. Built once and added
	   once: an earlier double addField() reparented the spinner into a second
	   field and left the first one as a caption with no control, which is the
	   duplicated "Size" line the testers reported (bug list 36). */
	bodySizeSp_ = new ZcSliderSpin(colView->parentWidget());
	bodySizeSp_->setRange(kFramingBodySizeMin, kFramingBodySizeMax,
			      kFramingBodySizeStep);
	bodySizeSp_->setSuffix(QStringLiteral("%"));
	bodySizeSp_->onCommitted = [this](int v) {
		if (painting_ || !client_)
			return;
		client_->setFramingFovBodySize((v - kFramingBodySizeMin) /
					       kFramingBodySizeStep);
	};
	bodySizeField_ = addField(colView,
		ztr("ZCameraPlugin.Framing.FovScale"), bodySizeSp_, false);

	/* The >=1.0.5 firmware replaces View Angle + Size with one 0..100 "Size"
	   slider plus Full-body / Waist / Close-up buttons that recall stored
	   sizes (bug list #10). all_status carries `fov_size` only on such a
	   firmware, so the row starts hidden and a snapshot decides. */
	{
		auto *sizeCol = new QWidget(colView->parentWidget());
		auto *sizeLay = new QVBoxLayout(sizeCol);
		sizeLay->setContentsMargins(0, 0, 0, 0);
		fovSizeSp_ = new ZcSliderSpin(sizeCol);
		fovSizeSp_->setRange(0, 100, 1);
		fovSizeSp_->setSuffix(QStringLiteral("%"));
		fovSizeSp_->onCommitted = [this](int v) {
			if (painting_ || !client_)
				return;
			client_->setFramingFovSize(v);
		};
		sizeLay->addWidget(fovSizeSp_);
		auto *presetRow = new QHBoxLayout();
		presetRow->setSpacing(4);
		const auto presetBtn = [this, sizeCol, presetRow](const char *key,
							QPushButton **slot,
							int *preset) {
			auto *btn = new QPushButton(ztr(key), sizeCol);
			btn->setToolTip(ztr(key));
			/* Checkable, because the page marks the preset the current
			   Size corresponds to; the tick is painted in
			   updateFovPresetButtons() (bug lists 16/17/20). */
			btn->setCheckable(true);
			connect(btn, &QPushButton::clicked, this,
				[this, preset]() {
					if (painting_ || !client_)
						return;
					/* The page moves its Size control as soon as a
					   preset is picked. Waiting for the camera's
					   read-back left the slider showing the old value
					   and no button ticked, which read as "the
					   button did nothing" (bug lists 16/17). */
					if (fovSizeSp_)
						fovSizeSp_->setValue(*preset);
					client_->setFramingFovSize(*preset);
					if (fovPresetButtons_)
						fovPresetButtons_();
				});
			presetRow->addWidget(btn);
			*slot = btn;
			return btn;
		};
		presetBtn("ZCameraPlugin.Framing.FovFullBody", &fovFullBodyBtn_,
			  &fovPresetFullBody_);
		presetBtn("ZCameraPlugin.Framing.FovWaist", &fovWaistBtn_,
			  &fovPresetWaist_);
		presetBtn("ZCameraPlugin.Framing.FovCloseUp", &fovCloseupBtn_,
			  &fovPresetCloseup_);
		/* Exactly one of the three carries the tick: the one whose stored
		   size equals the current Size. The sizes come from all_status
		   (fullbody / waist / closeup), so this follows the camera rather
		   than a hard-coded 20/80/90 triple. */
		fovPresetButtons_ = [this]() {
			const int size = fovSizeSp_ ? fovSizeSp_->value() : -1;
			/* Exactly one button carries the tick. The page marks the
			   preset whose stored size equals the current Size; two
			   presets may legitimately hold the same value, and then
			   only the first of them lights — otherwise the row read as
			   "all three selected" (bug list 180). */
			int hit = -1;
			if (size >= 0) {
				const int sizes[3] = {fovPresetFullBody_,
						      fovPresetWaist_,
						      fovPresetCloseup_};
				for (int i = 0; i < 3; i++) {
					if (sizes[i] == size) {
						hit = i;
						break;
					}
				}
			}
			const auto tick = [hit](QPushButton *btn, int index) {
				if (!btn)
					return;
				QSignalBlocker block(btn);
				btn->setChecked(hit == index);
			};
			tick(fovFullBodyBtn_, 0);
			tick(fovWaistBtn_, 1);
			tick(fovCloseupBtn_, 2);
			/* The figure follows the same Size these buttons mark. */
			paintPad();
		};
		sizeLay->addLayout(presetRow);
		fovSizeField_ = addField(
			colView, ztr("ZCameraPlugin.Framing.FovScale"),
			sizeCol, false);
		fovSizeField_->setVisible(false);
	}
	sceneCombo_ = new QComboBox(colView->parentWidget());
	for (const char *s : kFramingScenes)
		sceneCombo_->addItem(QString::fromUtf8(s));
	addField(colView, ztr("ZCameraPlugin.Framing.Scene"), sceneCombo_, true);
	connect(sceneCombo_, &QComboBox::currentIndexChanged, this,
		[this](int index) {
			if (painting_ || !client_)
				return;
			client_->setFramingScene(index);
		});
	placeColumn(colView);

	/* --- Column 3: where the subject starts, and what the camera is allowed
	   to move while it follows. --- */
	QVBoxLayout *colStart = makeColumn(grid, kFramingTileWidth);
	startPosCombo_ = new QComboBox(colStart->parentWidget());
	for (int i = 0;
	     i < int(sizeof(kFramingStartPosFallback) /
		     sizeof(kFramingStartPosFallback[0]));
	     i++)
		startPosCombo_->addItem(
		    QString::fromUtf8(kFramingStartPosFallback[i]), i);
	addField(colStart, ztr("ZCameraPlugin.Framing.StartPos"), startPosCombo_,
		 true);
	connect(startPosCombo_, &QComboBox::currentIndexChanged, this, [this]() {
		if (painting_ || !client_)
			return;
		client_->setFramingStartPos(startPosCombo_->currentData().toInt());
	});

	tiltLockSeg_ = offSegment(colStart->parentWidget());
	addField(colStart, ztr("ZCameraPlugin.Framing.TiltLock"), tiltLockSeg_,
		 true);
	tiltLockSeg_->onActivated = [this](int index) {
		if (painting_ || !client_)
			return;
		client_->setFramingTiltLock(index == 1);
	};

	/* Bug 68: the page keeps Lock Zoom and Lock Tilt side by side in its live
	   block (its aframe_lock_zoom and aframe_lock_tilt), so this row belongs
	   here rather than on the Settings page. A firmware without the key hides
	   it on the first snapshot (applyStatus → hideRow). */
	zoomLockSeg_ = offSegment(colStart->parentWidget());
	zoomLockRow_ = addField(colStart,
				ztr("ZCameraPlugin.Framing.ZoomLock"), zoomLockSeg_,
				false);
	zoomLockSeg_->onActivated = [this](int index) {
		if (painting_ || !client_)
			return;
		client_->setFramingZoomLock(index == 1);
		/* Bug list #13: Lock Zoom greys the Size row. */
		updateGating();
	};

	/* --- Column 2: the pad that moves the subject in frame. The page puts it
	   between the two control blocks and leaves it live, and so does this: it
	   writes a camera offset, not anything the detector owns. --- */
	QVBoxLayout *colPad = makeColumn(grid, kFramingPadWidth);
	{
		auto *padBody = new QWidget(colPad->parentWidget());
		auto *padLay = new QVBoxLayout(padBody);
		padLay->setContentsMargins(0, 0, 0, 0);
		padLay->setSpacing(3);
		/* The pad on its own: the camera's page puts nothing under its tile,
		   and a numeric read-out of the two offsets was a line of text to read
		   past to reach the thing the operator came for. The offsets are still
		   what the arrows write; they are just not restated here. */
		pad_ = new ZcFramingPad(padBody);
		padLay->addWidget(pad_);
		/* The live numbers: Face Height and Size are what the arrows move and
		   what the operator is aiming at, so they are restated under the tile
		   and repaint with every nudge (bug 185). */
		padReadout_ = new QLabel(padBody);
		padReadout_->setWordWrap(true);
		padReadout_->setStyleSheet("color:#8a9bb2;font-size:11px;");
		padLay->addWidget(padReadout_);

		QWidget *padField =
		    addFieldBody(colPad, ztr("ZCameraPlugin.Framing.PositionPad"),
				 padBody, false);
		padField_ = padField;
		padField->setToolTip(
		    ztr("ZCameraPlugin.Framing.PositionPadTip"));

		pad_->onNudge = [this](bool isPlacement, int delta) {
			if (painting_)
				return;
			nudgeOffset(isPlacement, delta);
		};
	}
	placeColumn(colPad);
	placeColumn(colStart);

	/* --- Column 4: how many targets to follow, and whether a registered face
	   counts as one. --- */
	QVBoxLayout *colTargets = makeColumn(grid, kFramingTileWidth);
	targetNumCombo_ = new QComboBox(colTargets->parentWidget());
	for (int i = kFramingTargetNumMin; i <= kFramingTargetNumMax; i++)
		targetNumCombo_->addItem(QString::number(i), i);
	addField(colTargets, ztr("ZCameraPlugin.Framing.TargetNum"),
		 targetNumCombo_, true);
	connect(targetNumCombo_, &QComboBox::currentIndexChanged, this, [this]() {
		if (painting_ || !client_)
			return;
		client_->setFramingTargetNum(
		    targetNumCombo_->currentData().toInt());
	});

	faceTrackSeg_ = offSegment(colTargets->parentWidget());
	addField(colTargets, ztr("ZCameraPlugin.Framing.FaceTrack"), faceTrackSeg_,
		 true);
	faceTrackSeg_->onActivated = [this](int index) {
		if (painting_ || !client_)
			return;
		client_->setFramingTrackActiveFace(index == 1);
	};
	placeColumn(colTargets);

	/* --- Column 5: how the target is chosen, and what happens when it is
	   lost. --- */
	QVBoxLayout *colLost = makeColumn(grid, kFramingTileWidth);
	autoTargetSeg_ = offSegment(colLost->parentWidget());
	addField(colLost, ztr("ZCameraPlugin.Framing.AutoTarget"), autoTargetSeg_,
		 true);
	autoTargetSeg_->onActivated = [this](int index) {
		if (painting_ || !client_)
			return;
		client_->setFramingAutoTarget(index == 1);
		/* Mirror the page at once (#15/#16): the refresh that follows the
		   write re-states this from the camera, but the click should not
		   leave a dead control looking live for a second. */
		if (waitTimeSp_)
			waitTimeSp_->setEnabled(index != 1);
		if (detectRestartSp_)
			detectRestartSp_->setEnabled(index == 1);
		/* Bug list #48: on a >=1.0.5 firmware the detection-area button
		   greys while Auto Target is off. */
		updateGating();
	};

	lostTimeoutSp_ = new ZcSliderSpin(colLost->parentWidget());
	lostTimeoutSp_->setRange(0, 60);
	lostTimeoutSp_->setSuffix(ztr("ZCameraPlugin.Framing.SecondsSuffix"));
	addField(colLost, ztr("ZCameraPlugin.Framing.LostTimeout"), lostTimeoutSp_,
		 true);
	lostTimeoutSp_->onCommitted = [this](int v) {
		if (painting_ || !client_)
			return;
		client_->setFramingLostTimeout(v);
	};

	lostActionCombo_ = new QComboBox(colLost->parentWidget());
	for (const char *s : kFramingLostActions)
		lostActionCombo_->addItem(QString::fromUtf8(s));
	addField(colLost, ztr("ZCameraPlugin.Framing.LostAction"), lostActionCombo_,
		 true);
	connect(lostActionCombo_, &QComboBox::currentIndexChanged, this,
		[this](int index) {
			if (painting_ || !client_)
				return;
			client_->setFramingLostAction(index);
		});
	placeColumn(colLost);

	/* --- Column 6: what part of the frame is looked at or marked. The page
	   pairs "Detection area" (the masked rectangle) with the "Detected
	   indicator" that draws the boxes; both are switches, both stay live while
	   detection is off, exactly as the page leaves them. --- */
	QVBoxLayout *colDetect = makeColumn(grid, kFramingTileWidth);
	{
		maskSeg_ = offSegment(colDetect->parentWidget());
		QWidget *maskField = addField(
		    colDetect, ztr("ZCameraPlugin.Framing.MaskArea"), maskSeg_,
		    true);
		maskField->setToolTip(
		    ztr("ZCameraPlugin.Framing.MaskAreaTip"));
	}
	maskSeg_->onActivated = [this](int index) {
		if (painting_ || !client_)
			return;
		client_->setFramingMaskEnable(index == 1);
		/* Bug list 28: the rectangle follows this switch both ways and
		   nothing else. Off takes it off the picture at once, on puts the
		   last rectangle back at once — the next snapshot carries the flag
		   too, but it is a second away, and a frame left behind (or a frame
		   that will not come back until something else changes) reads as a
		   switch that does nothing. Switching this off used to latch the
		   overlay at "hidden" for good: setMaskVisible() pins a local
		   override that only a *different* rectangle from the camera could
		   clear, and switching back on never touched it. */
		if (link_) {
			link_->setMaskVisible(index == 1);
			/* The rectangle the camera holds is what has to appear; read it
			   back after the write so a camera-side default (not the seed)
			   is what the operator sees. */
			QTimer::singleShot(300, this, [this]() {
				refreshMaskRect();
			});
		}
		/* The "edit" button belongs to this switch (bug list 31). */
		updateGating();
	};

	/* "Edit detection area": the camera's own page lets the operator drag the
	   masked rectangle on the live view; the dock has no live view of its
	   own, so the drag happens on the source's Interact window instead, where
	   the rectangle is painted over the real picture (see ZcOverlayLink).
	   Bug list: the mask was an On/Off switch with no way to place it. */
	maskEditBtn_ = new QPushButton(
	    ztr("ZCameraPlugin.Framing.MaskEdit"), colDetect->parentWidget());
	maskEditBtn_->setCheckable(true);
	maskEditBtn_->setToolTip(ztr("ZCameraPlugin.Framing.MaskEditTip"));
	addField(colDetect, ztr("ZCameraPlugin.Framing.MaskEditLabel"),
		 maskEditBtn_, true);
	connect(maskEditBtn_, &QPushButton::toggled, this, [this](bool on) {
		if (!link_)
			return;
		if (!on) {
			link_->armMode(zc::OverlayInteraction::None);
			return;
		}
		/* Window first, mode last. Opening the view reuses the camera's
		   window and takes any previous one down, and taking a window down
		   disarms the overlay (a mode with no window would keep drawing
		   handles nobody can grab). Arming before that teardown is what made
		   the editor disarm itself a quarter second after it opened — the
		   rectangle appeared and could not be dragged (bug 28). */
		if (link_->openInteract)
			link_->openInteract();
		/* Bug 29: the editor opened with nothing on the picture — no
		   rectangle had ever been written, so there was nothing to grab and
		   nothing to drag. Seed a centred box for the operator to work from;
		   the first drag is what reaches the camera. */
		if (!link_->hasMask())
			link_->seedMask(0.2f, 0.2f, 0.8f, 0.8f);
		link_->armMode(zc::OverlayInteraction::MaskEdit);
	});

	indicatorSeg_ = offSegment(colDetect->parentWidget());
	indicatorField_ = addField(colDetect,
		ztr("ZCameraPlugin.Framing.Indicator"), indicatorSeg_, true);
	indicatorSeg_->onActivated = [this](int index) {
		if (painting_ || !client_)
			return;
		/* The page's "Detected indicator" is the camera's own marker, so it
		   keeps writing /ctrl/framing/marker for parity. It is also, in this
		   plugin, the switch that decides whether the AI boxes are drawn at
		   all: the boxes come from the source's own feed, not the camera's
		   marker, so the same click has to write the source property the
		   render path reads (zc_ai_overlay). The switch and the boxes can
		   then never disagree. */
		client_->setFramingMarker(index == 1);
		setCameraSourceOverlay(client_->host(), index == 1);
	};

	/* "Register faces": the camera keeps a small face database and its own
	   page edits it in a dialog. This opens the same editor
	   (ZcFaceRegDialog). It needs no live view because the camera captures
	   the face itself on /prepare, so it does not depend on the detected
	   indicator or on the Interact window. Left un-gated: the face store is
	   independent of the detection switch on the camera's page too. */
	faceRegBtn_ = new QPushButton(
	    ztr("ZCameraPlugin.Framing.FaceRegOpen"), colDetect->parentWidget());
	faceRegBtn_->setToolTip(ztr("ZCameraPlugin.Framing.FaceRegOpenTip"));
	{
		QWidget *faceField = addField(
		    colDetect, ztr("ZCameraPlugin.Framing.FaceRegOpenLabel"),
		    faceRegBtn_, false);
		faceField->setToolTip(
		    ztr("ZCameraPlugin.Framing.FaceRegOpenTip"));
	}
	connect(faceRegBtn_, &QPushButton::clicked, this, [this]() {
		if (!client_)
			return;
		if (faceRegDlg_) {
			faceRegDlg_->show();
			faceRegDlg_->raise();
			faceRegDlg_->activateWindow();
			return;
		}
		/* A dialog of its own, parented to the dock: it lives until the
		   operator closes it or the camera is switched. Created fresh so
		   it always holds the current client. */
		auto *dlg = new ZcFaceRegDialog(client_, this);
		/* Bug 13: the limits belong to the firmware. 1.0.5 registers and
		   tracks 20; the 1.0.3 generation is the one that says 10 / 8. */
		dlg->setDefaults(client_ && client_->firmwareAtLeast("1.0.5") ? 20
									     : 10,
				 client_ && client_->firmwareAtLeast("1.0.5") ? 20
									     : 8);
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		faceRegDlg_ = dlg;
		connect(dlg, &QObject::destroyed, this,
			[this]() { faceRegDlg_ = nullptr; });
		dlg->show();
	});
	placeColumn(colDetect);

	/* Enable/Disable toggles the camera's detection, Start/Stop its tracking,
	   Restart re-runs detection. Each acts at once, as on the page. The buttons
	   themselves live in the status row at the top of the page. */
	connect(enableBtn_, &QPushButton::clicked, this, [this]() {
		if (!client_)
			return;
		const bool want = !detectOn_;
		client_->setFramingEnabled(want);
		/* Optimistic: the button face follows the click now, and the refresh
		   below replaces this with what the camera stored. */
		detectOn_ = want;
		if (!want)
			traceOn_ = false;
		updateStatusLabel();
		updateGating();
		QTimer::singleShot(kFramingReloadMs, this, [this]() { refresh(); });
	});
	connect(startBtn_, &QPushButton::clicked, this, [this]() {
		/* The camera ignores Start while detection is off, so the button
		   does nothing there rather than pretending to have sent it. */
		if (!client_ || !detectOn_)
			return;
		client_->setFramingTrace(!traceOn_);
		QTimer::singleShot(kFramingReloadMs, this, [this]() { refresh(); });
	});
	connect(restartBtn_, &QPushButton::clicked, this, [this]() {
		if (!client_ || !detectOn_)
			return;
		client_->setFramingRestartNow();
		QTimer::singleShot(kFramingReloadMs, this, [this]() { refresh(); });
	});

	liveLay->addWidget(grid);
	liveLay->addWidget(gateHint);

	/* Watching the boxes live. The main preview pane copies the program
	   picture, so an overlay held out of the program (the default) cannot
	   appear there by construction; a projector window renders the source on
	   its own and can show them, and it is not part of the recording. The
	   button itself sits in the status row above, as a toggle. */
	connect(monitorBtn_, &QPushButton::clicked, this, [this](bool on) {
		if (on)
			openMonitorWindow();
		else
			closeMonitorWindow();
		/* Opening can fail — no client, or no source for this camera yet — so
		   the button face follows what actually happened, not the click. */
		syncMonitorButton();
	});
	/* The window can also be closed with its own title-bar X, or opened from
	   OBS's View menu, and neither goes through this button. Poll the one
	   question the button answers so its face never disagrees with reality. */
	monitorPoll_ = new QTimer(this);
	monitorPoll_->setInterval(1000);
	connect(monitorPoll_, &QTimer::timeout, this,
		&ZcFramingPanel::syncMonitorButton);
	monitorPoll_->start();

	/* ---- Composition presets: the tile row at the foot of the page. ----
	   The page keeps its preset block in the left column of the live grid,
	   between Auto Target and Lock Zoom, and section 3 of the dock's own
	   design note explains why this one is not there: the page's block is a
	   bare list whose only affordance is a `+` in its header, while these
	   buttons (Recall / Replace / Rename / Reset / Delete / Add) have no home
	   on the page at all. Putting a list that is taller than the columns
	   beside it *inside* the grid pushed every field after it off the strip
	   and away from the page's layout, which is what this used to do. It now
	   sits below the grid, so the grid above it reads exactly as the page
	   does, and it is hidden outright on a camera whose firmware has no
	   preset table. */
	{
		auto *presetBox = new QGroupBox(
		    ztr("ZCameraPlugin.Framing.GroupPreset"), live);
		/* Two columns wide: its buttons come three to a row, which is more
		   than one column holds. */
		presetBox->setFixedWidth(kFramingTileWidth * 2 + 10);
		/* Nothing is known about the camera's preset table until its first
		   answer arrives, and the answer decides whether this is shown at
		   all (see rebuildPresets). Start hidden rather than flashing a
		   block that a firmware without one would only have removed again. */
		presetBox->setVisible(false);
		auto *presetLay = new QVBoxLayout(presetBox);
		presetLay->setSpacing(6);

		presetList_ = new QListWidget(presetBox);
		presetList_->setFixedHeight(96);
		/* Room for the thumbnail the page draws per preset (bug 44). */
		presetList_->setIconSize(QSize(44, 36));
		presetLay->addWidget(presetList_);

		presetHint_ = new QLabel(presetBox);
		presetHint_->setWordWrap(true);
		presetLay->addWidget(presetHint_);

		auto *row1 = new QHBoxLayout;
		row1->setSpacing(6);
		auto *recallBtn =
		    new QPushButton(ztr("ZCameraPlugin.Dock.Recall"), presetBox);
		auto *replaceBtn =
		    new QPushButton(ztr("ZCameraPlugin.Dock.Replace"), presetBox);
		auto *renameBtn =
		    new QPushButton(ztr("ZCameraPlugin.Dock.Rename"), presetBox);
		row1->addWidget(recallBtn);
		row1->addWidget(replaceBtn);
		row1->addWidget(renameBtn);
		presetLay->addLayout(row1);

		auto *row2 = new QHBoxLayout;
		row2->setSpacing(6);
		auto *resetBtn = new QPushButton(
		    ztr("ZCameraPlugin.Framing.PresetReset"), presetBox);
		auto *deleteBtn =
		    new QPushButton(ztr("ZCameraPlugin.Dock.Delete"), presetBox);
		presetAddBtn_ = new QPushButton(
		    ztr("ZCameraPlugin.Framing.PresetAdd"), presetBox);
		row2->addWidget(resetBtn);
		row2->addWidget(deleteBtn);
		row2->addStretch(1);
		row2->addWidget(presetAddBtn_);
		presetLay->addLayout(row2);

		/* The camera re-reads and rewrites its preset table asynchronously, so
		   every mutation is followed by a fresh read. */
		const auto reloadPresetsSoon = [this]() {
			QTimer::singleShot(kFramingReloadMs, this,
					   [this]() { refreshPresets(); });
		};
		const auto selectedRow = [this]() {
			return presetList_->currentRow();
		};
		const auto askConfirm = [this](const QString &text) {
			return QMessageBox::question(
				   this, ztr("ZCameraPlugin.Framing.GroupPreset"),
				   text, QMessageBox::Yes | QMessageBox::No,
				   QMessageBox::No) == QMessageBox::Yes;
		};

		connect(presetList_, &QListWidget::itemDoubleClicked, this,
			[this, selectedRow](QListWidgetItem *) {
				const int i = selectedRow();
				if (client_ && i >= 0)
					client_->framingPresetRecall(i);
			});
		connect(recallBtn, &QPushButton::clicked, this,
			[this, selectedRow]() {
				const int i = selectedRow();
				if (client_ && i >= 0)
					client_->framingPresetRecall(i);
			});
		connect(replaceBtn, &QPushButton::clicked, this,
			[this, selectedRow, reloadPresetsSoon]() {
				const int i = selectedRow();
				if (!client_ || i < 0 || i >= presetNames_.size())
					return;
				client_->framingPresetUpdate(i, presetNames_.at(i));
				reloadPresetsSoon();
			});
		connect(renameBtn, &QPushButton::clicked, this,
			[this, selectedRow, reloadPresetsSoon]() {
				const int i = selectedRow();
				if (!client_ || i < 0 || i >= presetNames_.size())
					return;
				bool ok = false;
				const QString name = QInputDialog::getText(
				    this,
				    ztr("ZCameraPlugin.Framing.PresetRenameTitle"),
				    ztr("ZCameraPlugin.Framing.PresetName"),
				    QLineEdit::Normal, presetNames_.at(i), &ok);
				if (!ok)
					return;
				const QString trimmed = name.trimmed();
				if (trimmed.isEmpty())
					return;
				client_->framingPresetRename(
				    i, framingClampName(trimmed,
							presetMaxNameBytes_));
				reloadPresetsSoon();
			});
		connect(resetBtn, &QPushButton::clicked, this,
			[this, selectedRow, askConfirm, reloadPresetsSoon]() {
				const int i = selectedRow();
				if (!client_ || i < 0)
					return;
				if (!askConfirm(
					ztr("ZCameraPlugin.Framing.PresetResetConfirm")
					    .arg(presetNames_.value(i))))
					return;
				client_->framingPresetReset(i);
				reloadPresetsSoon();
			});
		connect(deleteBtn, &QPushButton::clicked, this,
			[this, selectedRow, askConfirm, reloadPresetsSoon]() {
				const int i = selectedRow();
				if (!client_ || i < 0)
					return;
				/* The five factory presets are permanent (#12.4): the
				   page never removes them, so a delete click on one is
				   answered instead of being sent to a camera that will
				   only refuse it. */
				if (i < kFramingPresetDefaultCount) {
					QMessageBox::information(
					    this,
					    ztr("ZCameraPlugin.Framing.GroupPreset"),
					    ztr("ZCameraPlugin.Framing."
						"PresetDefaultLocked"));
					return;
				}
				if (!askConfirm(
					ztr("ZCameraPlugin.Framing.PresetDeleteConfirm")
					    .arg(presetNames_.value(i))))
					return;
				client_->framingPresetDelete(i);
				reloadPresetsSoon();
			});
		connect(presetAddBtn_, &QPushButton::clicked, this,
			[this, reloadPresetsSoon]() {
				if (!client_)
					return;
				client_->framingPresetAdd();
				reloadPresetsSoon();
			});

		/* Bug lists 9/44: the slot-targeting buttons act on a selected
		   entry, so with no selection they all grey; and the five factory
		   presets are permanent (#12.4), so Delete greys on them — when
		   only the factory five remain the button is dead everywhere,
		   which is what the "delete does nothing" report saw. */
		const auto updatePresetButtons =
			[this, recallBtn, replaceBtn, renameBtn, resetBtn,
			 deleteBtn]() {
				const int i = presetList_->currentRow();
				const bool has = i >= 0;
				const bool named =
					has && i < presetNames_.size();
				recallBtn->setEnabled(has);
				replaceBtn->setEnabled(named);
				renameBtn->setEnabled(named);
				resetBtn->setEnabled(has);
				deleteBtn->setEnabled(
					has && i >= kFramingPresetDefaultCount);
			};
		presetButtons_ = updatePresetButtons;
		updatePresetButtons();
		connect(presetList_, &QListWidget::itemSelectionChanged, this,
			updatePresetButtons);

		/* At the foot of the live page, outside the grid -- see the note
		   where presetBox is created. Gating is handled on presetGroup_ by
		   updateGating(), which also has to AND in whether the camera has a
		   preset table at all. */
		liveLay->addWidget(presetBox);
		presetGroup_ = presetBox;
	}

	/* ---- Speed response: Sensitivity and Acceleration. The page keeps both
	   in its live block, in the speedDiv under the lock switches, so they are
	   on this page rather than on Settings (bug 67). A firmware without
	   /ctrl/framing/speed answers {"code":-1} and the key is absent from
	   all_status; the rows hide then, so nothing here takes a click it cannot
	   honour. ---- */
	{
		auto *speedBox = new QGroupBox(
		    ztr("ZCameraPlugin.Framing.GroupSpeed"), live);
		auto *speedLay = new ZcFlowLayout(speedBox, 0, 8, 6);
		sensitivityRow_ = makeRow(
		    speedBox, ztr("ZCameraPlugin.Framing.Sensitivity"),
		    sensitivitySp_ = new ZcSliderSpin(speedBox), 150);
		speedLay->addWidget(sensitivityRow_);
		sensitivitySp_->setRange(kFramingSpeedMin, kFramingSpeedMax, 10);
		sensitivitySp_->onCommitted = [this](int v) {
			if (painting_ || !client_)
				return;
			client_->setFramingSensitivity(v);
		};
		accelerationRow_ = makeRow(
		    speedBox, ztr("ZCameraPlugin.Framing.Acceleration"),
		    accelerationSp_ = new ZcSliderSpin(speedBox), 150);
		speedLay->addWidget(accelerationRow_);
		accelerationSp_->setRange(kFramingSpeedMin, kFramingSpeedMax, 10);
		accelerationSp_->onCommitted = [this](int v) {
			if (painting_ || !client_)
				return;
			client_->setFramingAcceleration(v);
		};
		liveLay->addWidget(speedBox);
		speedGroup_ = speedBox;
	}

	{
		auto *refreshRow = new QHBoxLayout;
		auto *refreshBtn =
		    new QPushButton(ztr("ZCameraPlugin.Framing.Refresh"), live);
		refreshRow->addStretch(1);
		refreshRow->addWidget(refreshBtn);
		liveLay->addLayout(refreshRow);
		connect(refreshBtn, &QPushButton::clicked, this,
			&ZcFramingPanel::refresh);
	}
	liveLay->addStretch(1);
	pages->addTab(live, ztr("ZCameraPlugin.Framing.TabLive"));

	/* ============ Settings: the page's Auto Framing settings page ========= */
	auto *settings = new QWidget(pages);
	auto *setLay = new QVBoxLayout(settings);
	setLay->setContentsMargins(8, 8, 8, 8);
	setLay->setSpacing(8);
	/* The page's settings page stays live whether detection runs or not, and so
	   does this one — nothing below is added to gatedGroups_. */
	const auto setRow = [&makeRow](QWidget *parentWidget, const QString &text,
				       QWidget *control) {
		return makeRow(parentWidget, text, control, 150);
	};

	/* ---- General. ---- */
	{
		auto *box = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupGeneral"),
					  settings);
		auto *lay = new ZcFlowLayout(box, 0, 8, 6);

		autoStartSeg_ = offSegment(box);
		lay->addWidget(setRow(box, ztr("ZCameraPlugin.Framing.AutoStart"),
				      autoStartSeg_));
		autoStartSeg_->onActivated = [this](int index) {
			if (painting_ || !client_)
				return;
			client_->setFramingAutoStart(index == 1);
		};

		tallySeg_ = offSegment(box);
		lay->addWidget(setRow(box, ztr("ZCameraPlugin.Framing.Tally"),
				      tallySeg_));
		tallySeg_->onActivated = [this](int index) {
			if (painting_ || !client_)
				return;
			client_->setFramingTally(index == 1);
		};

		trackingRecordSeg_ = offSegment(box);
		trackingRecordRow_ = setRow(
			box, ztr("ZCameraPlugin.Framing.TrackingRecord"),
			trackingRecordSeg_);
		lay->addWidget(trackingRecordRow_);
		trackingRecordSeg_->onActivated = [this](int index) {
			if (painting_ || !client_)
				return;
			client_->setFramingTrackingRecord(index == 1);
			setRecording(recording_);
		};

		setLay->addWidget(box);
	}

	/* ---- The two delays. On the page these sit inside the framing-box
	   control's own "Settings" mode; they are broken out here because they are
	   independent of it and do work on this firmware, and burying a working
	   control inside a dead one would be worse than the small departure. ---- */
	{
		auto *box = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupDelay"),
					  settings);
		auto *lay = new ZcFlowLayout(box, 0, 8, 6);

		waitTimeSp_ = new ZcSliderSpin(box);
		waitTimeSp_->setRange(0, 60);
		waitTimeSp_->setSuffix(ztr("ZCameraPlugin.Framing.SecondsSuffix"));
		/* The page puts a "?" next to the box and opens the explanation on a
		   click; a hover tooltip is not how it does it (bug 43). */
		lay->addWidget(setRow(
		    box, ztr("ZCameraPlugin.Framing.WaitTime"),
		    zcWithHelp(box, waitTimeSp_,
			       ztr("ZCameraPlugin.Framing.WaitTime"),
			       ztr("ZCameraPlugin.Framing.WaitTimeTip"))));
		waitTimeSp_->onCommitted = [this](int v) {
			if (painting_ || !client_)
				return;
			client_->setFramingWaitTime(v);
		};

		detectRestartSp_ = new ZcSliderSpin(box);
		detectRestartSp_->setRange(0, 600);
		detectRestartSp_->setSuffix(
		    ztr("ZCameraPlugin.Framing.SecondsSuffix"));
		lay->addWidget(setRow(
		    box, ztr("ZCameraPlugin.Framing.DetectRestart"),
		    zcWithHelp(box, detectRestartSp_,
			       ztr("ZCameraPlugin.Framing.DetectRestart"),
			       ztr("ZCameraPlugin.Framing.DetectRestartTip"))));
		detectRestartSp_->onCommitted = [this](int v) {
			if (painting_ || !client_)
				return;
			client_->setFramingRestart(v);
		};

		setLay->addWidget(box);
	}

	/* ---- Gesture control. ---- */
	{
		auto *box = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupGesture"),
					  settings);
		auto *lay = new ZcFlowLayout(box, 0, 8, 6);

		wavingSeg_ = offSegment(box);
		lay->addWidget(setRow(box, ztr("ZCameraPlugin.Framing.Waving"),
				      wavingSeg_));
		wavingSeg_->onActivated = [this](int index) {
			if (painting_ || !client_)
				return;
			client_->setFramingWaving(index == 1);
			/* View Angle Adjustment hangs off Follow Me (bug list 15). */
			updateGating();
		};

		gestureFovSeg_ = offSegment(box);
		lay->addWidget(setRow(box, ztr("ZCameraPlugin.Framing.GestureFov"),
				      gestureFovSeg_));
		gestureFovSeg_->onActivated = [this](int index) {
			if (painting_ || !client_)
				return;
			client_->setFramingGestureFov(index == 1);
		};

		/* The page explains both switches with hand-gesture pictures. The
		   dock carries those pictures in its resources and shows them in a
		   help dialog, instead of leaving the two rows explaining themselves
		   only through a tooltip (bug 42). */
		auto *helpBtn = new QToolButton(box);
		helpBtn->setText(QStringLiteral("?"));
		helpBtn->setAutoRaise(true);
		helpBtn->setToolTip(ztr("ZCameraPlugin.Framing.GestureHelp"));
		connect(helpBtn, &QToolButton::clicked, this,
			[this]() { showGestureHelp(); });
		lay->addWidget(helpBtn);

		setLay->addWidget(box);
	}

	/* ---- Framing box control: the page's Fix Angle Box / Tracking Box
	   segment and a Status switch. /ctrl/framing/{fix_angle_box,tracking_box}
	   answer {"code":-1} on a firmware without it, which leaves this group
	   disabled. ---- */
	{
		auto *box = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupBox"),
					  settings);
		auto *outer = new QVBoxLayout(box);
		outer->setSpacing(6);
		auto *fields = new QWidget(box);
		auto *lay = new ZcFlowLayout(fields, 0, 8, 6);
		outer->addWidget(fields);

		boxModeSeg_ = new ZcSegmentedControl(
		    {ztr("ZCameraPlugin.Framing.BoxFixAngle"),
		     ztr("ZCameraPlugin.Framing.BoxTracking")},
		    fields);
		boxModeSeg_->setCurrentIndex(0);
		lay->addWidget(setRow(fields, ztr("ZCameraPlugin.Framing.BoxMode"),
				      boxModeSeg_));

		boxEnableSeg_ = offSegment(fields);
		lay->addWidget(setRow(fields, ztr("ZCameraPlugin.Framing.BoxStatus"),
				      boxEnableSeg_));

		/* The two boxes are two independent flags on the camera. Switching
		   the mode therefore has to show that mode's own state again, which
		   is what makes the single status switch behave like the page's per
		   mode switch (bug lists 25/26: a range-on state used to be carried
		   over to the fix-angle mode and vice versa). */
		boxModeSeg_->onActivated = [this](int index) {
			if (painting_)
				return;
			if (boxEnableSeg_)
				boxEnableSeg_->setCurrentIndex(
				    index == 0 ? (fixAngleBoxOn_ ? 1 : 0)
					       : (trackingBoxOn_ ? 1 : 0));
			updateGating();
		};

		/* The note is a full-width line of text, not a field, so it goes under
		   the wrapped row of fields rather than into it. */
		auto *note = new QLabel(ztr("ZCameraPlugin.Framing.BoxNote"), box);
		note->setObjectName("FramingHint");
		note->setWordWrap(true);
		outer->addWidget(note);

		boxEnableSeg_->onActivated = [this](int index) {
			if (painting_ || !client_)
				return;
			/* Remember which box the operator just switched: the flag is
			   per mode on the camera and read back from all_status, but the
			   switch has to show the new state before that reply lands. */
			if (boxModeSeg_->currentIndex() == 0) {
				client_->setFramingFixAngleBoxEnabled(index == 1);
				fixAngleBoxOn_ = index == 1;
			} else {
				client_->setFramingTrackingBoxEnabled(index == 1);
				trackingBoxOn_ = index == 1;
			}
			/* Bug list #49: the box state drives the Lock Zoom /
			   Lock Tilt interlock, which lives in updateGating(). */
			updateGating();
			/* Bug 53: the monitor window's frame comes from the camera's
			   box rectangle, which is re-read here because our own write
			   does not come back as a settings-changed event. */
			if (onBoxStateChanged)
				onBoxStateChanged();
		};

		boxGroup_ = box;
		optionalRows_ << box;
		setLay->addWidget(box);
	}

	/* ---- Framing profile export / import. ---- */
	{
		auto *box = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupProfile"),
					  settings);
		auto *lay = new QVBoxLayout(box);
		lay->setSpacing(6);
		/* The page's profile row is a fixed-width button, not a full-width
		   bar. Export and Import sit side by side on that row. */
		auto *row = new QHBoxLayout();
		row->setSpacing(6);
		auto *exportBtn =
		    new QPushButton(ztr("ZCameraPlugin.Framing.Export"), box);
		exportBtn->setFixedWidth(kFramingActionWidth);
		row->addWidget(exportBtn);
		auto *importBtn =
		    new QPushButton(ztr("ZCameraPlugin.Framing.Import"), box);
		importBtn->setFixedWidth(kFramingActionWidth);
		row->addWidget(importBtn);
		row->addStretch(1);
		lay->addLayout(row);
		auto *note = new QLabel(ztr("ZCameraPlugin.Framing.ProfileNote"), box);
		note->setObjectName("FramingHint");
		note->setWordWrap(true);
		lay->addWidget(note);

		/* BUG-LIST #2: the button used to open its QFileDialog (or the
		   Unsupported warning) straight out of the HTTP completion
		   callback. A modal dialog spins a nested event loop while the
		   transport's readyRead emission is still on the stack; the
		   deferred delete of the finished exchange is processed inside
		   that loop and the socket is freed under the running signal
		   emission — OBS crashed the moment the dialog appeared. The
		   callback now only stashes the payload; a zero-timer queued on
		   this panel opens the dialog from a clean stack (and never
		   fires at all if the panel is gone).

		   BUG-LIST #3: /ctrl/framing/export answers only a work order
		   {"code":0,"filename":...}; the profile itself lives at
		   /app_data/preset/<filename>. framingExportProfileFile()
		   performs the two-step download, so the saved file carries the
		   camera's actual settings instead of the bare work order. */
		connect(exportBtn, &QPushButton::clicked, this, [this]() {
			if (!client_)
				return;
			client_->framingExportProfileFile(
			    [this](int result, const QByteArray &profile) {
				    if (result != 0) {
					    pendingExportProfile_.clear();
					    pendingExportError_ =
					        result < 0
						    ? ztr("ZCameraPlugin.Framing.Unsupported")
						    : ztr("ZCameraPlugin.Framing.ExportFailed");
				    } else {
					    pendingExportProfile_ = profile;
					    pendingExportError_.clear();
				    }
				    QTimer::singleShot(0, this, [this]() {
					    finishProfileExport();
				    });
			    });
		});

		/* BUG-LIST #4: import — the counterpart the page offers as
		   showUploadFramingProfileDialog (multipart POST
		   /ctrl/framing/import). The file dialog here opens from an
		   ordinary click slot, so it is already off the transport's
		   call stack; only the camera's reply arrives inside one, and
		   its notice is queued the same way as the export's. */
		connect(importBtn, &QPushButton::clicked, this, [this]() {
			if (!client_)
				return;
			const QString path = QFileDialog::getOpenFileName(
				this, ztr("ZCameraPlugin.Framing.Import"),
				QString(), QStringLiteral("JSON (*.json)"));
			if (path.isEmpty())
				return;
			QFile file(path);
			if (!file.open(QIODevice::ReadOnly))
				return;
			const QByteArray profile = file.readAll();
			file.close();
			if (profile.isEmpty())
				return;
			client_->framingImportProfile(
			    profile, QFileInfo(path).fileName(),
			    [this](bool ok, const QString &error) {
				    QTimer::singleShot(0, this, [this, ok,
								 error]() {
					    if (ok)
						    QMessageBox::information(
							    this,
							    ztr("ZCameraPlugin.Framing.Import"),
							    ztr("ZCameraPlugin.Framing.ImportDone"));
					    else
						    QMessageBox::warning(
							    this,
							    ztr("ZCameraPlugin.Framing.Import"),
							    ztr("ZCameraPlugin.Framing.ImportFailed") +
								QStringLiteral("\n") +
								error);
				    });
			    });
		});

		profileGroup_ = box;
		optionalRows_ << box;
		setLay->addWidget(box);
	}
	setLay->addStretch(1);
	pages->addTab(settings, ztr("ZCameraPlugin.Framing.TabSettings"));

	/* Start in the camera's own resting state: detection off, so Start and
	   Restart are greyed and the Live controls read as unavailable until the
	   first snapshot says otherwise. */
	updateStatusLabel();
	updateGating();
}

/* The export button's callback only stashes the payload; this runs from a
   queued zero-timer, on a clean stack, so its modal dialog can never spin an
   event loop inside the transport's signal emission (bug list #2). */
void ZcFramingPanel::finishProfileExport()
{
	if (!pendingExportError_.isEmpty()) {
		QMessageBox::warning(this, ztr("ZCameraPlugin.Framing.Export"),
				     pendingExportError_);
		pendingExportError_.clear();
		pendingExportProfile_.clear();
		return;
	}
	if (pendingExportProfile_.isEmpty())
		return;
	const QString path = QFileDialog::getSaveFileName(
		this, ztr("ZCameraPlugin.Framing.Export"),
		QStringLiteral("framing-profile.json"),
		QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		QMessageBox::warning(this, ztr("ZCameraPlugin.Framing.Export"),
				     file.errorString());
		return;
	}
	file.write(pendingExportProfile_);
	pendingExportProfile_.clear();
}

void ZcFramingPanel::setMaskFromCamera(bool enabled, float l, float t, float r,
				       float b)
{
	/* Keep the last-read rectangle for the panel's own record, and hand it to
	   the shared link so the source can paint it on its Interact window even
	   before the operator touches anything. A drag on that window overrides
	   this through the same link (ZcOverlayLink::commitMask). */
	maskOn_ = enabled;
	maskL_ = l;
	maskT_ = t;
	maskR_ = r;
	maskB_ = b;
	if (link_)
		link_->setMask(enabled, l, t, r, b);
}

void ZcFramingPanel::setClient(ZcCameraClient *client)
{
	/* A different camera: its rectangle has to be read again. */
	maskRectRead_ = false;
	client_ = client;
	/* Firmware generation first: /info may already be cached from the
	   previous connect, and the rows it decides must not flash the wrong
	   generation (setFirmwareNew stays subscribed via the dock's
	   cameraInfoChanged for the not-yet-fetched case). */
	setFirmwareNew(client && client->firmwareAtLeast("1.0.5"));
	/* A switch forgets the pad's nudge guard with everything else. */
	padNudgeGuard_.invalidate();
	/* A face dialog belongs to one camera. Close it on any switch, so it can
	   never act on a client it no longer owns. (NULL is handled here rather
	   than only in the reset branch below: that is also the "no camera"
	   state, where an editor could only fail.) */
	if (faceRegDlg_) {
		faceRegDlg_->close();
		faceRegDlg_ = nullptr;
	}
	/* A different camera: reset to the defaults and let the first refresh paint
	   what this camera actually has. */
	if (!client_) {
		painting_ = true;
		sceneCombo_->setCurrentIndex(0);
		startPosCombo_->setCurrentIndex(0);
		lostActionCombo_->setCurrentIndex(0);
		targetNumCombo_->setCurrentIndex(0);
		const QList<ZcSegmentedControl *> segs = {
		    autoTargetSeg_, zoomLockSeg_,   tiltLockSeg_, maskSeg_,
		    indicatorSeg_,  faceTrackSeg_,  autoStartSeg_, tallySeg_,
		    trackingRecordSeg_, wavingSeg_, gestureFovSeg_, boxModeSeg_,
		    boxEnableSeg_};
		for (ZcSegmentedControl *seg : segs) {
			if (seg)
				seg->setCurrentIndex(0);
		}
		const QList<ZcSliderSpin *> spins = {sensitivitySp_, accelerationSp_,
						     lostTimeoutSp_, waitTimeSp_,
						     detectRestartSp_};
		for (ZcSliderSpin *sp : spins) {
			if (sp)
				sp->setValue(0);
		}
		/* The area editor is a latched button: it stays pressed while the
		   operator drags the rectangle, which is a mode on THIS camera's
		   source. Left pressed across a switch it would read as "still
		   editing" on a camera that never armed it (the link itself is
		   disarmed by ZcOverlayLink::setClient, a moment later), so drop the
		   latch here without firing its handler. */
		if (maskEditBtn_) {
			QSignalBlocker b(maskEditBtn_);
			maskEditBtn_->setChecked(false);
		}
		viewAngleCombo_->setCurrentIndex(0);
		bodySizeSp_->setValue(kFramingBodySizeMin);
		placement_ = 0;
		faceHeight_ = 0;
		zoomLockOn_ = false;
		hasFovSize_ = false;
		/* The preset sizes and the config probe belong to one camera. */
		fovConfigProbed_ = false;
		fovPresetFullBody_ = 100;
		fovPresetWaist_ = 70;
		fovPresetCloseup_ = 40;
		if (pad_) {
			pad_->setOffsets(0, 0);
			pad_->setSubjectSize(50);
		}
		painting_ = false;
		/* Back to the resting state: detection off, so Start/Restart grey out
		   and the word reads "Idle" again. */
		detectOn_ = false;
		traceOn_ = false;
		statusIndex_ = 0;
		haveSnapshot_ = false;
		presetList_->clear();
		presetNames_.clear();
		presetCount_ = 0;
		presetMaxCount_ = 0;
		presetMaxNameBytes_ = 0;
		presetHint_->clear();
		presetAddBtn_->setEnabled(false);
		/* Unknown until this camera answers: hidden, not shown-dead. */
		presetSupported_ = false;
		if (presetGroup_)
			presetGroup_->setVisible(false);
		/* A new camera may or may not have the optional features; leave that
		   to its first snapshot. They are shown again here so a camera that
		   has them gets them back after one that did not (bug list 14). */
		optionalProbed_ = false;
		for (QWidget *w : optionalRows_) {
			if (w) {
				w->setVisible(true);
				w->setEnabled(true);
				w->setToolTip(QString());
			}
		}
		updateStatusLabel();
		updateGating();
		return;
	}
	refresh();
}

void ZcFramingPanel::refresh()
{
	if (!client_ || !client_->isConnected())
		return;
	/* One snapshot read carries every framing field. */
	client_->framingQuery(
	    [this](const QJsonObject &doc) { applyStatus(doc); });
	/* The status word is not part of that snapshot: /ctrl/framing/status
	   answers {"status":N} on its own, which is also what the camera's page
	   reads when it wants the word without a full repaint. */
	client_->framingStatusQuery([this](const QJsonObject &doc) {
		if (!doc.contains("status"))
			return;
		statusIndex_ = qBound(0, doc.value("status").toInt(),
				      kFramingStatusCount - 1);
		updateStatusLabel();
	});
	refreshFovOffsets();
	/* Once per camera is enough: the camera is the only other writer, and the
	   switch's own handler re-reads after it writes. */
	if (!maskRectRead_) {
		maskRectRead_ = true;
		refreshMaskRect();
	}
	refreshPresets();
	if (!fovConfigProbed_)
		refreshFovPresets();
	if (!optionalProbed_)
		probeOptionalFeatures();
}

/* The three preset sizes, from the camera's own capability object.

   They are NOT in all_status: the top-level fullbody / waist / closeup keys
   there are the three gesture switches ("View Angle Adjustment"), booleans
   written through /gesture, and the camera's page reads them as such
   (`response.fullbody == true`). Reading them as percentages — which this did
   — collapsed all three presets onto the same 0/1, so the tick comparison
   matched all three at once and clicking a button wrote a Size of 0 or 1
   (bug lists 16/17/180: "三个按钮同时选中，Size 也没有跟着变"). */
void ZcFramingPanel::refreshFovPresets()
{
	if (!client_ || !client_->isConnected())
		return;
	fovConfigProbed_ = true;
	client_->framingConfigQuery([this](const QJsonObject &doc) {
		const QJsonObject fov = doc.value("fov_size").toObject();
		const auto take = [&fov](const char *key, int fallback) {
			const QJsonValue v = fov.value(QLatin1String(key));
			return v.isDouble() ? qBound(0, v.toInt(), 100) : fallback;
		};
		fovPresetFullBody_ = take("fullbody", fovPresetFullBody_);
		fovPresetWaist_ = take("waist", fovPresetWaist_);
		fovPresetCloseup_ = take("closeup", fovPresetCloseup_);
		if (fovPresetButtons_)
			fovPresetButtons_();
		paintPad();
	});
}

/* The Size the subject figure should be drawn at. The >=1.0.5 camera keeps one
   0..100 value; the 1.0.3 camera's body-size stepper walks four steps between
   its own floor and ceiling, so its position is turned back into a percentage
   to keep one scale for the drawing. */
int ZcFramingPanel::subjectSizePct() const
{
	if (hasFovSize_ && fovSizeSp_)
		return fovSizeSp_->value();
	if (bodySizeSp_) {
		const int span = kFramingBodySizeMax - kFramingBodySizeMin;
		if (span > 0)
			return qBound(0, (bodySizeSp_->value() - kFramingBodySizeMin) *
					     100 / span,
				      100);
	}
	return 50;
}

void ZcFramingPanel::paintPad()
{
	if (pad_) {
		pad_->setOffsets(placement_, faceHeight_);
		pad_->setSubjectSize(subjectSizePct());
	}
	if (padReadout_) {
		/* Bug 185: the value follows the arrows at once, from the same numbers
		   the tile is drawn from, so the read-out and the picture can never
		   disagree. */
		padReadout_->setText(ztr("ZCameraPlugin.Framing.PadReadout")
					 .arg(faceHeight_)
					 .arg(subjectSizePct()));
	}
}

/* The framing-box control and the profile export exist only on some firmwares
   (/ctrl/framing/fix_angle_box and /export answer {"code":-1} on a camera
   without them). One probe each, on the first refresh after a camera is
   picked. A control the firmware does not have is *hidden*, not shown dead:
   the camera's own page does not render it either, and the testers asked for
   removal rather than a permanently greyed block (bug list 14). */
void ZcFramingPanel::probeOptionalFeatures()
{
	if (!client_ || !client_->isConnected())
		return;
	optionalProbed_ = true;
	const auto gate = [this](QWidget *w, bool supported) {
		if (!w)
			return;
		w->setVisible(supported);
		w->setEnabled(supported);
		w->setToolTip(supported ? QString()
					: ztr("ZCameraPlugin.Framing.Unsupported"));
	};
	client_->framingFixAngleBoxQuery([this, gate](const QJsonObject &doc) {
		const bool supported =
		    !(doc.contains("code") && doc.value("code").toInt() < 0);
		gate(boxGroup_, supported);
	});
	client_->framingExportProfile([this, gate](const QJsonObject &doc) {
		const bool supported =
		    !(doc.contains("code") && doc.value("code").toInt() < 0);
		gate(profileGroup_, supported);
	});
}

void ZcFramingPanel::refreshFovOffsets()
{
	if (!client_ || !client_->isConnected())
		return;
	client_->framingFovQuery([this](const QJsonObject &doc) {
		/* A fresh pad nudge makes any in-flight read a stale
		   pre-nudge value; the pad already shows what was sent. */
		if (padNudgeGuard_.isValid() && padNudgeGuard_.elapsed() < 800)
			return;
		if (doc.contains("placement"))
			placement_ = qBound(kFramingOffsetMin,
					    doc.value("placement").toInt(),
					    kFramingOffsetMax);
		if (doc.contains("face_height"))
			faceHeight_ = qBound(kFramingOffsetMin,
					     doc.value("face_height").toInt(),
					     kFramingOffsetMax);
		paintPad();
		/* /fov restates the View Angle and the Size the dropdown and the
		   stepper show, under a paint guard so filling them is not read back
		   as an operator edit. */
		painting_ = true;
		if (doc.contains("type")) {
			viewAngleCombo_->setCurrentIndex(
			    qBound(0, doc.value("type").toInt(),
				   int(sizeof(kFramingFovTypes) /
				       sizeof(kFramingFovTypes[0])) -
				       1));
		}
		if (doc.contains("body_size")) {
			bodySizeSp_->setValue(qBound(
			    kFramingBodySizeMin,
			    kFramingBodySizeMin +
				doc.value("body_size").toInt() * kFramingBodySizeStep,
			    kFramingBodySizeMax));
		}
		painting_ = false;
	});
}

/* The picture the camera's page puts next to each framing preset: a framed
   tile, its 3x3 grid, and the person placed from the preset's own
   info_face_placement / info_face_height / info_human_bottom. The page paints
   it on a canvas (_drawPresetPreview); the dock draws the same thing into a
   QPixmap so the preset list is a picture and not a bare "framing1" (bug 44).
   The silhouette is the page's own fullBody SVG, rasterised into the
   resources. A reply that does not carry the geometry falls back to the
   page's defaults (centre, 0.3 from the top, nothing cropped at the
   bottom). */
static QPixmap zcFramingPresetThumb(const QJsonObject &preset, int w, int h)
{
	QPixmap pm(w, h);
	pm.fill(Qt::transparent);
	if (w <= 2 || h <= 2)
		return pm;
	QPainter p(&pm);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);

	p.setPen(QColor("#3a3a3a"));
	p.drawRect(0, 0, w - 1, h - 1);
	p.setPen(QColor("#2a2a2a"));
	for (int i = 1; i < 3; ++i) {
		p.drawLine(w * i / 3, 0, w * i / 3, h);
		p.drawLine(0, h * i / 3, w, h * i / 3);
	}

	const double x = preset.value(QLatin1String("info_face_placement"))
			     .toDouble(0.5);
	const double y =
	    preset.value(QLatin1String("info_face_height")).toDouble(0.3);
	const double bottom =
	    preset.value(QLatin1String("info_human_bottom")).toDouble(0.0);
	const double lw = 1.0;
	const double iconY = (y - 0.05) * h + lw;
	const double iconH = qMax(4.0, h - iconY - lw / 2.0 - bottom * h);
	const double iconW = 98.0 * (iconH / 81.0) + 4.0;
	const double iconX = x * w - iconW / 2.0 + lw / 2.0;

	static const QPixmap body(QStringLiteral(":/zc/framing_full_body.png"));
	if (!body.isNull())
		p.drawPixmap(QRectF(iconX, iconY, iconW, iconH), body,
			     QRectF(body.rect()));
	return pm;
}

/* The framing page's gesture explanation, reproduced: one picture per gesture
   with the sentence that goes with it. The artwork is the page's own
   (zc_resources.qrc); QLabel draws it, so nothing here needs the QtSvg module
   the Qt build OBS ships does not carry (bug 42). */
void ZcFramingPanel::showGestureHelp()
{
	QDialog dlg(this);
	dlg.setWindowTitle(ztr("ZCameraPlugin.Framing.GestureHelpTitle"));
	auto *outer = new QVBoxLayout(&dlg);
	outer->setSpacing(10);

	const auto picture = [](const QString &file, const QString &caption,
				QWidget *parent) {
		auto *col = new QWidget(parent);
		auto *v = new QVBoxLayout(col);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(2);
		auto *img = new QLabel(col);
		const QPixmap pm(QStringLiteral(":/zc/") + file);
		img->setPixmap(pm.scaledToWidth(120, Qt::SmoothTransformation));
		img->setAlignment(Qt::AlignCenter);
		v->addWidget(img);
		if (!caption.isEmpty()) {
			auto *cap = new QLabel(caption, col);
			cap->setAlignment(Qt::AlignCenter);
			cap->setStyleSheet(QStringLiteral("color:#8a9bb2;"));
			v->addWidget(cap);
		}
		return col;
	};

	const auto section = [&](const QString &title, const QString &body,
				 const QList<QPair<QString, QString>> &shots) {
		auto *t = new QLabel(title, &dlg);
		t->setStyleSheet(QStringLiteral("font-weight:600;"));
		outer->addWidget(t);
		if (!shots.isEmpty()) {
			auto *row = new QHBoxLayout;
			row->setSpacing(14);
			for (const auto &shot : shots)
				row->addWidget(picture(shot.first, shot.second,
						       &dlg));
			row->addStretch(1);
			outer->addLayout(row);
		}
		auto *text = new QLabel(body, &dlg);
		text->setWordWrap(true);
		text->setMaximumWidth(420);
		outer->addWidget(text);
	};

	section(ztr("ZCameraPlugin.Framing.Waving"),
		ztr("ZCameraPlugin.Framing.WavingHelp"),
		{{QStringLiteral("gesture_waving.png"), QString()}});
	section(ztr("ZCameraPlugin.Framing.GestureFov"),
		ztr("ZCameraPlugin.Framing.GestureFovHelp"),
		{{QStringLiteral("gesture_full_body.png"),
		  ztr("ZCameraPlugin.Framing.GestureFullBody")},
		 {QStringLiteral("gesture_waist.png"),
		  ztr("ZCameraPlugin.Framing.GestureWaist")},
		 {QStringLiteral("gesture_closeup.png"),
		  ztr("ZCameraPlugin.Framing.GestureCloseUp")}});

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	outer->addWidget(buttons);
	dlg.exec();
}

void ZcFramingPanel::refreshMaskRect()
{
	if (!client_ || !client_->isConnected() || !link_)
		return;
	if (qEnvironmentVariableIsSet("ZC_TEST_MASK")) {
		/* Test hook: do exactly what an operator does — switch the camera's
		   detection area on, seed a rectangle, arm the editor — and open the
		   window, so a script can prove the overlay reaches a real picture.
		   The preview pane is the canvas mix, which the overlay is never
		   drawn into. QTimer rather than a straight call: the switch write is
		   HTTP, and the editor must be armed after the gating that follows
		   it, or `updateGating` disarms it again (it drops the mode whenever
		   the camera still reports the area as off). */
		if (client_)
			client_->setFramingMaskEnable(true);
		link_->seedMask(0.2f, 0.2f, 0.8f, 0.8f);
		if (!monitorWindow())
			openMonitorWindow();
		QTimer::singleShot(2500, this, [this]() {
			if (link_)
				link_->armMode(zc::OverlayInteraction::MaskEdit);
		});
		/* ...and then the other half of bug 28: switching the area off has
		   to take the rectangle off the picture and close the editor. Also
		   leaves the camera the way this hook found it. */
		QTimer::singleShot(12000, this, [this]() {
			if (link_) {
				link_->setMaskVisible(false);
				link_->armMode(zc::OverlayInteraction::None);
			}
			if (client_)
				client_->setFramingMaskEnable(false);
		});
	}
	client_->framingMaskQuery([this](const QJsonObject &doc) {
		if (!client_ || !link_)
			return;
		blog(LOG_INFO, "[obs-zcamera] mask_area answered %s",
		     QJsonDocument(doc).toJson(QJsonDocument::Compact)
			 .constData());
		if (doc.contains(QLatin1String("code")) &&
		    doc.value(QLatin1String("code")).toInt() < 0)
			return;
		/* The hook above owns the rectangle on a self-test run. */
		if (qEnvironmentVariableIsSet("ZC_TEST_MASK"))
			return;
		const auto num = [&doc](const char *key, float fallback) {
			const QJsonValue v = doc.value(QLatin1String(key));
			return v.isDouble() ? (float)v.toDouble() : fallback;
		};
		/* `enable` comes back as a bool on some builds and as 1/0 on others. */
		const QJsonValue en = doc.value(QLatin1String("enable"));
		const bool on = en.isBool() ? en.toBool() : en.toInt() != 0;
		link_->setMask(on, num("left", 0.0f), num("top", 0.0f),
			       num("right", 1.0f), num("bottom", 1.0f));
	});
}

void ZcFramingPanel::refreshPresets()
{
	if (!client_ || !client_->isConnected())
		return;
	client_->framingPresetQuery(
	    [this](const QJsonObject &doc) { rebuildPresets(doc); });
}

void ZcFramingPanel::rebuildPresets(const QJsonObject &doc)
{
	/* /ctrl/framing/preset answers {"code":-1} on a firmware without a preset
	   table -- which is what the camera this dock is developed against
	   answers. There is no table to fill, no add that can succeed and nothing
	   to recall, so the whole group is taken off the panel rather than shown
	   dead: an operator cannot use it, and an empty list with a greyed row of
	   buttons only invites the click it will refuse. This is what the user
	   asked for: if the block does nothing here, do not show it. */
	const bool supported =
	    doc.contains("max_count") ||
	    !(doc.contains("code") && doc.value("code").toInt() < 0);

	const QJsonArray presets = doc.value("presets").toArray();
	presetMaxCount_ = doc.value("max_count").toInt();
	presetMaxNameBytes_ = doc.value("max_name_len").toInt();

	const int selected = presetList_->currentRow();
	presetNames_.clear();
	presetList_->clear();
	for (const QJsonValue &v : presets) {
		const QJsonObject o = v.toObject();
		const QString name = o.value(QLatin1String("name")).toString();
		presetNames_ << name;
		/* Thumbnail first, name after: the page shows the same picture, and
		   two presets that differ only in framing read apart at a glance
		   (bug 44). */
		auto *item = new QListWidgetItem(name, presetList_);
		item->setIcon(QIcon(zcFramingPresetThumb(o, 44, 36)));
	}
	presetCount_ = presetNames_.size();
	if (selected >= 0 && selected < presetCount_)
		presetList_->setCurrentRow(selected);

	/* The camera refuses an add once the table is full. */
	presetAddBtn_->setEnabled(presetMaxCount_ <= 0 ||
				  presetCount_ < presetMaxCount_);
	if (supported)
		presetHint_->setText(ztr("ZCameraPlugin.Framing.PresetHint")
					 .arg(presetCount_)
					 .arg(presetMaxCount_));
	else
		presetHint_->setText(ztr("ZCameraPlugin.Framing.Unsupported"));

	/* Known now, so the group appears or does not. Its *enabled* state is a
	   separate matter -- it is also gated on the live detection switch -- and
	   updateGating() is what ANDs the two together. */
	presetSupported_ = supported;
	if (presetGroup_)
		presetGroup_->setVisible(supported);
	if (presetButtons_)
		presetButtons_();
	updateGating();
}

void ZcFramingPanel::nudgeOffset(bool isPlacement, int delta)
{
	if (!client_ || !haveSnapshot_)
		return;
	/* Arm the read-back guard: until it expires the camera's own reads may
	   still answer with the pre-nudge value, and painting that would bounce
	   the numbers (bug list "held arrow values jump"). */
	padNudgeGuard_.start();
	if (isPlacement) {
		const int next = qBound(kFramingOffsetMin, placement_ + delta,
					kFramingOffsetMax);
		if (next == placement_)
			return;
		placement_ = next;
		client_->setFramingPlacement(placement_);
	} else {
		const int next = qBound(kFramingOffsetMin, faceHeight_ + delta,
					kFramingOffsetMax);
		if (next == faceHeight_)
			return;
		faceHeight_ = next;
		client_->setFramingFaceHeight(faceHeight_);
	}
	paintPad();
}

/* Paint the panel from a framing snapshot (/ctrl/framing/all_status) or from a
   live FramingStatusChanged payload. Only the fields actually present are
   touched, so the same method serves both: the live payload carries the two
   switches but not the stored settings. A flag can arrive as a bool or as 1/0
   depending on the endpoint, so both are accepted. Never stored — the panel
   only ever shows what the camera has. */
void ZcFramingPanel::applyStatus(const QJsonObject &doc)
{
	const auto asBool = [&doc](const char *key, bool fallback) {
		const QJsonValue v = doc.value(QLatin1String(key));
		if (v.isBool())
			return v.toBool();
		if (v.isDouble())
			return v.toInt() != 0;
		return fallback;
	};
	const auto asInt = [&doc](const char *key, int fallback) {
		const QJsonValue v = doc.value(QLatin1String(key));
		if (v.isDouble())
			return v.toInt();
		if (v.isBool())
			return v.toBool() ? 1 : 0;
		return fallback;
	};

	/* The camera's live switches. `value` is the FramingStatusChanged status
	   index; `status` comes from the dedicated endpoint. Whichever is present
	   repaints the word. */
	if (doc.contains("detect"))
		detectOn_ = asBool("detect", detectOn_);
	if (doc.contains("trace"))
		traceOn_ = asBool("trace", traceOn_);
	if (doc.contains("value"))
		statusIndex_ = qBound(0, asInt("value", statusIndex_),
				      kFramingStatusCount - 1);
	else if (doc.contains("status"))
		statusIndex_ = qBound(0, asInt("status", statusIndex_),
				      kFramingStatusCount - 1);

	/* Everything below is painted, never written: painting_ makes every change
	   handler a no-op, so a value that arrives from the camera cannot go
	   straight back out as a command. */
	painting_ = true;
	const auto seg = [&asBool, &doc](const char *key, ZcSegmentedControl *s) {
		if (s && doc.contains(QLatin1String(key)))
			s->setCurrentIndex(asBool(key, false) ? 1 : 0);
	};
	const auto spin = [&asInt, &doc](const char *key, ZcSliderSpin *s, int lo,
					 int hi) {
		if (s && doc.contains(QLatin1String(key)))
			s->setValue(qBound(lo, asInt(key, lo), hi));
	};

	seg("auto_target", autoTargetSeg_);
	/* Latch the camera's own value: the detection-area button and the two
	   Auto Target dependants are gated on it (bug 88). */
	if (doc.contains("auto_target")) {
		autoTargetOn_ = asBool("auto_target", autoTargetOn_);
		autoTargetKnown_ = true;
	}
	seg("zoom_lock", zoomLockSeg_);
	seg("tilt_lock", tiltLockSeg_);
	seg("track_active_face", faceTrackSeg_);
	seg("auto_start", autoStartSeg_);
	seg("tally", tallySeg_);
	seg("tracking_record", trackingRecordSeg_);
	seg("maskarea", maskSeg_);
	/* The rectangle itself, for the source to paint on its Interact window.
	   all_status carries it alongside the flag on this firmware; when it is
	   absent the mask is a switch only and nothing is drawn. */
	if (link_ && doc.contains("mask_left") && doc.contains("mask_right") &&
	    doc.contains("mask_top") && doc.contains("mask_bottom")) {
		link_->setMask(asBool("maskarea", false),
			       (float)doc.value("mask_left").toDouble(),
			       (float)doc.value("mask_top").toDouble(),
			       (float)doc.value("mask_right").toDouble(),
			       (float)doc.value("mask_bottom").toDouble());
	}
	seg("waving", wavingSeg_);
	if (doc.contains("fullbody") || doc.contains("waist") ||
	    doc.contains("closeup")) {
		/* The page shows the three FOV gesture switches as one segment, so
		   the segment is On when any of them is. */
		if (gestureFovSeg_)
			gestureFovSeg_->setCurrentIndex(
			    (asBool("fullbody", false) ||
			     asBool("waist", false) ||
			     asBool("closeup", false))
				? 1
				: 0);
	}

	spin("detect_restart", detectRestartSp_, 0, 600);
	spin("lost_timeout", lostTimeoutSp_, 0, 60);
	spin("wait_time", waitTimeSp_, 0, 60);
	spin("sensitivity", sensitivitySp_, kFramingSpeedMin, kFramingSpeedMax);
	spin("acceleration", accelerationSp_, kFramingSpeedMin, kFramingSpeedMax);

	if (doc.contains("scene")) {
		sceneCombo_->setCurrentIndex(
		    qBound(0, asInt("scene", 0),
			   int(sizeof(kFramingScenes) / sizeof(kFramingScenes[0])) -
			       1));
	}
	if (doc.contains("lost_action")) {
		lostActionCombo_->setCurrentIndex(
		    qBound(0, asInt("lost_action", 0),
			   int(sizeof(kFramingLostActions) /
			       sizeof(kFramingLostActions[0])) -
			       1));
	}
	if (doc.contains("target_num")) {
		const int idx = targetNumCombo_->findData(
		    asInt("target_num", kFramingTargetNumMin));
		if (idx >= 0)
			targetNumCombo_->setCurrentIndex(idx);
	}
	/* The View Angle dropdown and the Size stepper behind it. all_status
	   carries both as indices: fov_type is the dropdown entry, body_size the
	   stepper's 0..4 step which this panel prints as a percentage. */
	if (doc.contains("fov_type")) {
		viewAngleCombo_->setCurrentIndex(
		    qBound(0, asInt("fov_type", 0),
			   int(sizeof(kFramingFovTypes) /
			       sizeof(kFramingFovTypes[0])) -
			       1));
	}
	if (doc.contains("body_size")) {
		bodySizeSp_->setValue(
		    qBound(kFramingBodySizeMin,
			   kFramingBodySizeMin +
			       asInt("body_size", 0) * kFramingBodySizeStep,
			   kFramingBodySizeMax));
	}
	/* The >=1.0.5 Size row and its preset buttons. `fov_size` here is the
	   current size; the three presets' own sizes come from
	   /ctrl/framing/config (refreshFovPresets), never from all_status — its
	   top-level fullbody/waist/closeup are the gesture switches, read as
	   booleans just below. View Angle == Keep sizes nothing (bug list #25),
	   which the Size stepper below then mirrors. */
	fovKeep_ = doc.contains("fov_type") && asInt("fov_type", 0) == 3;
	if (doc.contains("fov_size")) {
		hasFovSize_ = true;
		if (fovSizeSp_)
			fovSizeSp_->setValue(
			    qBound(0, asInt("fov_size", 0), 100));
	}
	/* Lock Zoom, kept as state rather than read back off the segment: the
	   interlock below turns that segment off whenever the fix-angle box owns
	   the head, and gating that read the widget it had just written latched
	   the Size row back to enabled (bug lists 50/182). */
	if (doc.contains("zoom_lock"))
		zoomLockOn_ = asBool("zoom_lock", false);
	/* The two composition offsets. all_status carries them and /fov carries
	   them again; either is authoritative for the pad — except while a
	   pad nudge is fresh, when the read-back can only be the stale
	   pre-nudge value (see padNudgeGuard_). */
	const bool padNudging =
	    padNudgeGuard_.isValid() && padNudgeGuard_.elapsed() < 800;
	if (doc.contains("placement") && !padNudging) {
		placement_ = qBound(kFramingOffsetMin, asInt("placement", 0),
				    kFramingOffsetMax);
		paintPad();
	}
	if (doc.contains("face_height") && !padNudging) {
		faceHeight_ = qBound(kFramingOffsetMin, asInt("face_height", 0),
				     kFramingOffsetMax);
		paintPad();
	}
	if (doc.contains("start_pos") || doc.contains("start_pos_opts")) {
		/* The camera names the slots itself (start_pos_opts) and pads the list
		   with empty strings, so fall back to the built-in labels. */
		const QJsonArray opts = doc.value("start_pos_opts").toArray();
		const int fallbackCount =
		    int(sizeof(kFramingStartPosFallback) /
			sizeof(kFramingStartPosFallback[0]));
		const int count = opts.isEmpty() ? fallbackCount : int(opts.size());
		const int keep = startPosCombo_->currentData().toInt();
		startPosCombo_->clear();
		for (int i = 0; i < count; i++) {
			QString label = i < opts.size()
					    ? opts.at(i).toString().trimmed()
					    : QString();
			if (label.isEmpty() && i < fallbackCount)
				label = QString::fromUtf8(
				    kFramingStartPosFallback[i]);
			if (label.isEmpty())
				label = QString::number(i);
			startPosCombo_->addItem(label, i);
		}
		const int idx = startPosCombo_->findData(
		    doc.contains("start_pos") ? asInt("start_pos", 0) : keep);
		if (idx >= 0)
			startPosCombo_->setCurrentIndex(idx);
	}
	/* The framing box keeps one enable flag per mode on the camera, and
	   all_status reports both (fix_angle_box.enable / tracking_box.enable).
	   Reading them back is what lets the panel follow a switch flipped on the
	   camera's own page, and what keeps the one status segment honest about
	   the mode it is currently showing (bug lists 23/24/25/26/27). */
	const auto boxFlag = [&doc](const char *key, bool fallback) {
		const QJsonValue v = doc.value(QLatin1String(key));
		if (v.isObject()) {
			const QJsonValue e =
			    v.toObject().value(QLatin1String("enable"));
			if (e.isBool())
				return e.toBool();
			if (e.isDouble())
				return e.toInt() != 0;
			return fallback;
		}
		if (v.isBool())
			return v.toBool();
		if (v.isDouble())
			return v.toInt() != 0;
		return fallback;
	};
	if (doc.contains(QLatin1String("fix_angle_box")))
		fixAngleBoxOn_ = boxFlag("fix_angle_box", fixAngleBoxOn_);
	if (doc.contains(QLatin1String("tracking_box")))
		trackingBoxOn_ = boxFlag("tracking_box", trackingBoxOn_);
	/* The camera never says which box its page is showing; it keeps one enable
	   flag per box. When exactly one of them is on, that is the mode — infer
	   it, so the status switch and the Lock Zoom/Tilt interlock follow the
	   camera instead of a segment left at its initial Fix-Angle position.
	   Both on or both off is ambiguous, and there the operator's selection
	   stands (bug lists 22/50/182). */
	if (boxModeSeg_ && (fixAngleBoxOn_ != trackingBoxOn_)) {
		const int mode = fixAngleBoxOn_ ? 0 : 1;
		if (boxModeSeg_->currentIndex() != mode)
			boxModeSeg_->setCurrentIndex(mode);
	}
	if (boxEnableSeg_ && boxModeSeg_)
		boxEnableSeg_->setCurrentIndex(
		    boxModeSeg_->currentIndex() == 0 ? (fixAngleBoxOn_ ? 1 : 0)
						     : (trackingBoxOn_ ? 1 : 0));
	painting_ = false;

	/* The Full-body / Waist / Close-up buttons follow the Size the snapshot
	   just painted (bug lists 16/17/20). */
	if (fovPresetButtons_)
		fovPresetButtons_();

	/* /ctrl/framing/all_status is the camera's complete framing snapshot, so a
	   key that never shows up is a feature the firmware does not have: its
	   /ctrl/framing/zoom_lock and /ctrl/framing/speed answer {"code":-1} and
	   both keys are simply absent (Z CAM P2-R1-18x, sw 1.0.3). Left live,
	   those controls would accept a click and silently do nothing — which is
	   how this panel came to be reported as "always off, and Start does
	   nothing". Disable them and say why.
	   Only a full snapshot may decide this: a FramingStatusChanged payload
	   carries nothing but detect/trace/value, and reading the missing keys as
	   "unsupported" there would switch the feature off for every camera. */
	if (doc.contains("scene") || doc.contains("target_num")) {
		/* Bug list #14: the 1.0.3 firmware has none of Lock Zoom /
		   Sensitivity / Acceleration / tracking-triggered recording, and
		   the tester asked for removal, not a dead control — hide the
		   rows when the snapshot does not carry the key (bug list #19
		   covered tracking_record the same way). A >=1.0.5 firmware
		   carries the keys, so its rows stay. */
		const auto hideRow = [this](QWidget *row, bool supported) {
			if (row)
				row->setVisible(supported);
		};
		hideRow(zoomLockRow_, doc.contains("zoom_lock"));
		/* A full snapshot is the whole truth: a firmware that stopped
		   reporting the lock does not have one, and the stale "locked"
		   state must not keep greying the Size row. */
		if (!doc.contains("zoom_lock"))
			zoomLockOn_ = false;
		hideRow(sensitivityRow_, doc.contains("sensitivity"));
		hideRow(accelerationRow_, doc.contains("acceleration"));
		/* Bug 186: the group holds only those two rows, so a firmware without
		   /ctrl/framing/speed would show an empty titled box. Hide the box
		   with them. */
		if (speedGroup_)
			speedGroup_->setVisible(
			    !(sensitivityRow_ && !sensitivityRow_->isVisible() &&
			      accelerationRow_ && !accelerationRow_->isVisible()));
		hideRow(trackingRecordRow_, doc.contains("tracking_record"));
		/* Bug lists #10/#11: the >=1.0.5 firmware removes View Angle,
		   Size and the Detected Indicator (its Size row replaces them;
		   visible only when all_status carries fov_size). The version
		   gate ANDs in, so a firmware that kept the keys around while
		   ignoring writes still loses the rows. */
		if (viewAngleField_)
			viewAngleField_->setVisible(doc.contains("fov_type") &&
						    !fw15_);
		if (bodySizeField_)
			bodySizeField_->setVisible(doc.contains("body_size") &&
						   !fw15_);
		if (indicatorField_)
			indicatorField_->setVisible(doc.contains("marker") &&
						    !fw15_);
		if (fovSizeField_)
			fovSizeField_->setVisible(doc.contains("fov_size"));

		/* Bug list #15/#16: the page greys the two delays against Auto
		   Target (_cam_framing.js: wait_time goes read-only while auto
		   target is On -- the camera re-picks a target itself, so a
		   lost-action wait never runs -- and detect_restart goes read-only
		   while it is Off -- with automatic target picking there is no
		   "detect again" delay to wait out). The delays sit on the Settings
		   page, which stays live with detection off, so this is the only
		   gate they have. */
		const bool autoTarget = autoTargetSeg_ &&
					autoTargetSeg_->currentIndex() == 1;
		if (waitTimeSp_)
			waitTimeSp_->setEnabled(!autoTarget);
		if (detectRestartSp_)
			detectRestartSp_->setEnabled(autoTarget);
		haveSnapshot_ = true;
	}

	/* The Detected Indicator mirrors this source's AI-box master switch
	   (zc_source.cpp's zc_ai_overlay), which is what actually decides whether
	   the boxes are painted — not the camera's own /ctrl/framing/marker. With
	   no source in OBS to draw on, the switch has nothing to act on, so it is
	   shown dead and says why. */
	if (indicatorSeg_ && client_) {
		indicatorSourcePresent_ = cameraSourceExists(client_->host());
		painting_ = true;
		indicatorSeg_->setCurrentIndex(
		    cameraSourceOverlay(client_->host(), false) ? 1 : 0);
		painting_ = false;
		indicatorSeg_->setToolTip(
		    indicatorSourcePresent_
			? QString()
			: ztr("ZCameraPlugin.Framing.IndicatorNoSource"));
	}

	updateStatusLabel();
	updateGating();
}

/* The camera prints one word next to its three buttons, and its buttons read
   Enable/Disable and Start/Stop according to the two live switches. Both are
   mirrored here so the panel and the page can never disagree about whether
   detection is running — the state the AI detection overlay depends on. */
/* Bug 93: "tracking triggers recording" makes the CAMERA start recording on
   its own once it tracks a face, so the switch alone never told the operator
   that anything happened. The row says so while it is recording, which is the
   only place the consequence of that switch is visible. */
void ZcFramingPanel::setRecording(bool on)
{
	/* No early return on an unchanged flag: this is also called when the
	   switch itself moves, and the row still has to be repainted then. */
	recording_ = on;
	if (!trackingRecordRow_)
		return;
	auto *label = trackingRecordRow_->findChild<QLabel *>();
	if (!label)
		return;
	const bool active =
	    recording_ && trackingRecordSeg_ &&
	    trackingRecordSeg_->currentIndex() == 1;
	label->setText(active ? ztr("ZCameraPlugin.Framing.TrackingRecordActive")
			      : ztr("ZCameraPlugin.Framing.TrackingRecord"));
	label->setStyleSheet(active ? QStringLiteral("color:#e5484d;")
				    : QString());
}

void ZcFramingPanel::updateStatusLabel()
{
	const int idx = qBound(0, statusIndex_, kFramingStatusCount - 1);
	statusLabel_->setText(ztr(kFramingStatusKeys[idx]));
	statusLabel_->setStyleSheet(QStringLiteral("color:%1;")
					.arg(QLatin1String(
					    kFramingStatusColors[idx])));
	if (enableBtn_)
		enableBtn_->setText(ztr(detectOn_ ? "ZCameraPlugin.Framing.Disable"
						  : "ZCameraPlugin.Framing.Enable"));
	if (startBtn_)
		startBtn_->setText(ztr(traceOn_ ? "ZCameraPlugin.Framing.Stop"
						: "ZCameraPlugin.Framing.Start"));
}

/* Detection off means the camera ignores its live controls, so the page greys
   its whole control block (`_updateFramingUiStatus` puts `ui-state-disabled` on
   `.aframe_left_layout`); so does this one. Nothing is touchable before the
   first snapshot either — a control that has never been filled from the camera
   must not be able to write its placeholder value back. The Settings page is
   left live throughout, as it is on the page. */
/* Firmware generation switch (bug lists #10/#11/#48/#49): hides the rows the
   new generation drops. The Settings-page rows and the Size row re-decide on
   every snapshot (key presence ANDs with this), so only the always/never rows
   move here. */
void ZcFramingPanel::setFirmwareNew(bool atLeast15)
{
	if (fw15_ == atLeast15)
		return;
	fw15_ = atLeast15;
	applyFirmwareGating();
}

void ZcFramingPanel::applyFirmwareGating()
{
	if (viewAngleField_)
		viewAngleField_->setVisible(!fw15_);
	if (bodySizeField_)
		bodySizeField_->setVisible(!fw15_);
	if (indicatorField_)
		indicatorField_->setVisible(!fw15_);
	/* A camera switching generation mid-session leaves the Size row's last
	   snapshot state behind; hide it until the next snapshot carries
	   fov_size (or not). */
	if (fovSizeField_)
		fovSizeField_->setVisible(false);
	hasFovSize_ = false;
	updateGating();
}

void ZcFramingPanel::updateGating()
{
	for (QWidget *w : gatedGroups_) {
		if (w)
			w->setEnabled(haveSnapshot_ && detectOn_);
	}
	/* The preset block is gated on detection like the rest of the live page,
	   but only while the camera has a preset table at all: with none it is
	   hidden (rebuildPresets), and this keeps it dead in the moment between
	   being shown and the first snapshot arriving. */
	if (presetGroup_)
		presetGroup_->setEnabled(presetSupported_ && haveSnapshot_ &&
					 detectOn_);
	if (startBtn_)
		startBtn_->setEnabled(detectOn_);
	if (restartBtn_)
		restartBtn_->setEnabled(detectOn_);
	/* The Detected Indicator has a third condition: it writes the source's own
	   property, so with no source in OBS there is nothing for it to act on. */
	if (indicatorSeg_)
		indicatorSeg_->setEnabled(indicatorSourcePresent_ && haveSnapshot_ &&
					  detectOn_);

	/* Bug list #13: Lock Zoom on greys the Size row (the page's
	   _updateViewAngleDisabledByLockZoom). Only reachable on a >=1.0.5
	   firmware, where the row exists at all. Taken from the camera's last
	   report, not from the segment: the segment is greyed and switched off by
	   the fix-angle interlock below, and reading it back meant the two Size
	   controls lit up again as soon as they were written to (bug lists
	   50/182). */
	const bool lockZoomOn = zoomLockOn_;

	/* Bug list 22: with detection off the page greys the view angle, the
	   size and the position pad — the camera is not framing, so nothing
	   there has anything to act on. (An older page left them live; the
	   current one does not, so neither does this.) */
	const bool liveBase = haveSnapshot_ && detectOn_;
	if (viewAngleField_)
		viewAngleField_->setEnabled(liveBase);
	if (bodySizeField_)
		bodySizeField_->setEnabled(liveBase && !fovKeep_);
	if (fovSizeField_)
		fovSizeField_->setEnabled(liveBase && !lockZoomOn);
	if (padField_)
		padField_->setEnabled(liveBase);

	/* Bug list #25: View Angle == Keep — the camera holds its framing, so
	   the Size stepper has nothing to size and the page greys it. */
	if (bodySizeSp_)
		bodySizeSp_->setEnabled(liveBase && !fovKeep_);

	/* The >=1.0.5 Size row answers to the same conditions as its 1.0.3
	   siblings above. */
	if (fovSizeSp_)
		fovSizeSp_->setEnabled(liveBase && !lockZoomOn);
	if (fovFullBodyBtn_)
		fovFullBodyBtn_->setEnabled(liveBase && !lockZoomOn);
	if (fovWaistBtn_)
		fovWaistBtn_->setEnabled(liveBase && !lockZoomOn);
	if (fovCloseupBtn_)
		fovCloseupBtn_->setEnabled(liveBase && !lockZoomOn);

	/* Bug list 18: once Start has the camera tracking, the framing-box mode
	   choices (fix angle / range) grey — the camera refuses a mode switch
	   mid-track; Stop brings them back. */
	if (boxModeSeg_)
		boxModeSeg_->setEnabled(liveBase && !traceOn_);

	/* Bug 34: Track Active Face on means the camera is following a face, so
	   the box's own enable switch cannot be flipped either — the camera is
	   already driving the framing from the face. */
	const bool faceTrackOn =
	    faceTrackSeg_ && faceTrackSeg_->currentIndex() == 1;
	if (boxEnableSeg_)
		boxEnableSeg_->setEnabled(liveBase && !traceOn_ && !faceTrackOn);

	/* Bug list 13: Tilt Lock on — the pad's up/down arrows grey, left/right
	   keeps working. */
	if (pad_)
		pad_->setVerticalEnabled(
		    !(tiltLockSeg_ && tiltLockSeg_->currentIndex() == 1));

	/* Bug list #49: the fix-angle box on forces Lock Zoom / Lock Tilt off
	   and greys them (the page's _updateLockZoomTiltReadonlyStatus). A 1.0.3
	   camera has neither the box nor the locks, so this only ever fires on
	   the new generation. Only the FIX-ANGLE box does this: the tracking box
	   ("range") leaves both locks usable, which is why the mode is part of
	   the condition (bug list 27 — a range-on state used to grey them).

	   The camera is what clears the locks here; this only greys the segments
	   and shows the cleared value. It used to write index 0 into
	   zoomLockSeg_ as well, which is a latch: the segment then read back as
	   "off", and every rule derived from it (the Size row) followed it off
	   while the camera still had the zoom locked (bug lists 22/50/182). */
	const bool fixAngleOn = fw15_ && boxModeSeg_ &&
				boxModeSeg_->currentIndex() == 0 && boxEnableSeg_ &&
				boxEnableSeg_->isEnabled() &&
				boxEnableSeg_->currentIndex() == 1;
	if (fixAngleOn && zoomLockSeg_ && zoomLockSeg_->currentIndex() != 0) {
		QSignalBlocker block(zoomLockSeg_);
		zoomLockSeg_->setCurrentIndex(0);
	}
	if (fixAngleOn && tiltLockSeg_ && tiltLockSeg_->currentIndex() != 0) {
		QSignalBlocker block(tiltLockSeg_);
		tiltLockSeg_->setCurrentIndex(0);
	}
	/* Bugs 184/189: these live on the Live page and belong to the framing
	   engine, so with detection off they grey with the rest of the block. */
	if (zoomLockRow_ && zoomLockRow_->isVisible() && zoomLockSeg_)
		zoomLockSeg_->setEnabled(liveBase && !fixAngleOn);
	if (sensitivitySp_)
		sensitivitySp_->setEnabled(liveBase);
	if (accelerationSp_)
		accelerationSp_->setEnabled(liveBase);
	if (tiltLockSeg_)
		tiltLockSeg_->setEnabled(haveSnapshot_ && detectOn_ &&
					 !fixAngleOn);

	/* Bug list #48: on a >=1.0.5 firmware the detection-area button greys
	   while Auto Target is off (the page's _updateMaskEditBtnStatus); the
	   1.0.3 page keeps it live, so this is version-gated.

	   Bug list 31: the editor belongs to the detection-area switch, so with
	   that switch off there is nothing to draw and the button greys on every
	   firmware. */
	/* Read the camera's own last word on Auto Target when it has given one; the
	   segment is only the fallback for a firmware whose status does not carry
	   the key. Reading the segment first was the same latch as #30/#74/#182:
	   the widget keeps whatever was last written into it, so a value changed
	   elsewhere (the camera's own page) never reached this test (bug 88). */
	const bool autoTargetOk =
	    !fw15_ || (autoTargetKnown_ ? autoTargetOn_
					: (autoTargetSeg_ &&
					   autoTargetSeg_->currentIndex() == 1));
	/* Bug 31: on a >=1.0.5 firmware Auto Target gates the detection area
	   itself — with it off there is no target area, so both the area switch
	   and its editor grey out. A 1.0.3 camera has no Auto Target and keeps the
	   switch live. */
	if (maskSeg_ && fw15_)
		maskSeg_->setEnabled(liveBase && autoTargetOk);
	const bool maskOn = maskSeg_ && maskSeg_->currentIndex() == 1;
	if (maskEditBtn_) {
		maskEditBtn_->setEnabled(liveBase && maskOn && autoTargetOk);
		/* Bug 189: with the area switched off the editor cannot stay armed —
		   it would keep painting handles on a rectangle that no longer
		   exists. Drop the tick and the mode with the switch. */
		if (!maskOn && maskEditBtn_->isChecked()) {
			QSignalBlocker block(maskEditBtn_);
			maskEditBtn_->setChecked(false);
			if (link_)
				link_->armMode(zc::OverlayInteraction::None);
		}
	}

	/* Bug list 15: the page only lets "View Angle Adjustment" be switched
	   while Follow Me (the hand-gesture follow) is on — a gesture that
	   changes the view angle needs the camera already following the
	   operator. The row is on the Settings page, which is otherwise live. */
	if (gestureFovSeg_)
		gestureFovSeg_->setEnabled(
		    haveSnapshot_ && wavingSeg_ &&
		    wavingSeg_->currentIndex() == 1);

	/* Bug lists 11/12: registering a face writes the camera's face store,
	   which the camera only accepts while detection runs, and not while it
	   is tracking. The page greys the button in both cases. */
	/* Bug 193: on the 1.0.3 generation the camera only registers faces while it
	   has a target and the detected indicator is on; the page greys the button
	   otherwise. The >=1.0.5 generation keeps its own rule (detection on, not
	   tracking). */
	if (faceRegBtn_) {
		const bool indicatorOn =
		    indicatorSeg_ && indicatorSeg_->currentIndex() == 1;
		faceRegBtn_->setEnabled(haveSnapshot_ && detectOn_ && !traceOn_ &&
					autoTargetOk &&
					(fw15_ || indicatorOn));
	}

	/* Whether framing currently drives the head, which is what the PTZ tab's
	   trace face has to know: the camera answers every trace command with
	   code -2 while it does (bug 55). Status 0 is Idle and 4 is Disabled —
	   neither is framing — so only Detecting / Tracking / Target-lost count,
	   alongside the detection switch itself. Pushed from here rather than
	   from the dock's websocket handler so one place owns the answer: the
	   panel is what receives the snapshot read at connect time, which is the
	   only way to learn that a camera was already framing before the dock
	   attached (bug 179). */
	if (onFramingOwnsPtz)
		onFramingOwnsPtz(detectOn_ ||
				 (statusIndex_ >= 1 && statusIndex_ <= 3));
}

/* A projector window on this camera's source.
   This is the answer to "I want to see the boxes while I am setting the shot,
   but not have them recorded". OBS renders the picture once, into the program
   canvas, and the standard preview pane copies that canvas rather than
   rendering the source again (UI/window-basic-main.cpp RenderMain ->
   obs_render_main_texture_src_color_only when Studio Mode is off), so a box
   held out of the program is invisible there by construction — no plugin can
   change it. A projector of the *source* renders it directly, which is a pass
   the overlay is allowed to draw in and which is not the recording. */

/* ---------------------------------------------------------------- */
/* Monitor (projector) window helpers                                */
/* ---------------------------------------------------------------- */

/* OBS's projector windows are internal OBSProjector instances, and that class
   has no header a plugin may include, so they are recognised the way any
   foreign widget can be: by class name. A windowed projector's title is
   "<localized prefix> - <source name>" (window-projector.cpp
   UpdateProjectorTitle), so matching the *suffix* keeps working whatever
   language OBS runs in — the prefix is the part that gets translated.

   The window this panel opens is also *tagged* with the name it was opened
   for. The title alone is not a reliable handle: OBS retitles a projector
   when its source is renamed, and two sources sharing a name get " (2)"
   appended, which breaks the suffix match — and a broken match means the next
   click opens ANOTHER projector (obs_frontend_open_projector never checks),
   which is what "repeatedly adding windows" was (bug list #6). */
static const char *kMonitorNameProp = "zcMonitorName";

static QList<QWidget *> zcProjectorWindows()
{
	QList<QWidget *> out;
	for (QWidget *w : zcAllWindows()) {
		if (w && w->inherits("OBSProjector"))
			out << w;
	}
	return out;
}

static QWidget *zcFindMonitorWindow(const QString &name)
{
	if (name.isEmpty())
		return nullptr;

	const QString suffix = QStringLiteral(" - ") + name;
	QWidget *fullScreen = nullptr;
	for (QWidget *w : zcProjectorWindows()) {
		if (!w || !w->isVisible())
			continue;
		const bool tagged =
		    w->property(kMonitorNameProp).toString() == name;
		if (!tagged && !w->windowTitle().endsWith(suffix))
			continue;
		/* The operator may also have promoted this source to a fullscreen
		   projector from the window's own menu. Prefer the windowed one —
		   that is the one this button is about — but reuse the fullscreen
		   one rather than stacking a second window if it is all there is. */
		if (!w->isFullScreen())
			return w;
		if (!fullScreen)
			fullScreen = w;
	}
	return fullScreen;
}



/* The title suffix of this camera's monitor window, and whether it names the
   camera's own source (as opposed to the current scene, which is the fallback
   when the camera is not in the scene yet). An empty name means there is
   nothing for a window to show. */
QString ZcFramingPanel::monitorWindowName(bool &isSource)
{
	isSource = false;
	if (!client_)
		return QString();

	ZcCameraSourceLookup lookup;
	lookup.want = client_->host();
	obs_enum_sources(zcFindCameraSourceProc, &lookup);

	QString name;
	if (lookup.found) {
		name = QString::fromUtf8(obs_source_get_name(lookup.found));
		isSource = !name.isEmpty();
	}
	if (!isSource) {
		/* No source for this camera yet: the scene it would live in is the
		   next best window, and it also covers a scene holding several
		   cameras. */
		obs_source_t *scene = obs_frontend_get_current_scene();
		if (scene) {
			name = QString::fromUtf8(obs_source_get_name(scene));
			obs_source_release(scene);
		}
	}
	return name;
}

/* The monitor window if one is up, else null. The handle kept from last time is
   tried first (exact, and it covers a window whose title OBS has since
   retitled), then a scan by title — which also finds a projector the operator
   opened from OBS's own View menu, so the button reflects it too. */
/* This camera's window, whatever kind it is: the source's Interaction window
   (the one the dock opens, and the one that can be dragged in), or a projector
   the operator opened from OBS's own View menu. */
static QWidget *zcCameraWindowFor(const QString &host)
{
	if (host.isEmpty())
		return nullptr;
	for (QWidget *w : zcAllWindows()) {
		if (!w->isVisible() || !w->inherits("OBSBasicInteraction"))
			continue;
		if (w->property(kInteractHostProp).toString() == host)
			return w;
	}
	return nullptr;
}

QWidget *ZcFramingPanel::monitorWindow()
{
	/* ONE window per camera. The monitor used to be an OBS projector, which
	   cannot be interacted with, so arming the detection-area editor had to
	   open the Interaction window as well — two views of the same camera. The
	   Interaction window renders exactly the same picture and the same overlay
	   and it takes the mouse, so it is the window for watching and for
	   editing; the projector stays only for one the operator opened himself. */
	if (client_) {
		if (QWidget *w = zcCameraWindowFor(client_->host())) {
			monitorWnd_ = w;
			monitorWndName_.clear();
			return w;
		}
	}

	bool isSource = false;
	const QString name = monitorWindowName(isSource);
	if (name.isEmpty())
		return nullptr;

	/* The kept handle only counts while it is still *this* camera's window: a
	   camera switch changes the source, and the window left over from the
	   previous camera must not be handed back as this one's. */
	if (monitorWnd_ && monitorWndName_ == name && monitorWnd_->isVisible())
		return monitorWnd_.data();

	QWidget *found = zcFindMonitorWindow(name);
	if (found) {
		monitorWnd_ = found;
		monitorWndName_ = name;
	} else {
		/* Closed, or gone with the source: drop the stale handle so the next
		   sync cannot report a window that is no longer there. */
		monitorWnd_.clear();
		monitorWndName_.clear();
	}
	return found;
}

void ZcFramingPanel::syncMonitorButton()
{
	if (!monitorBtn_)
		return;
	const bool open = monitorWindow() != nullptr;
	if (monitorBtn_->isChecked() == open)
		return;
	/* Setting the face must not run the handler: this is the handler's own
	   bookkeeping, and re-entering it would toggle the window again. */
	QSignalBlocker blocker(monitorBtn_);
	monitorBtn_->setChecked(open);
}

void ZcFramingPanel::closeMonitorWindow()
{
	QWidget *w = monitorWindow();
	if (!w)
		return;
	/* The editor draws into this view, so taking the window down disarms it:
	 * a mode with no window would keep the source drawing handles nobody can
	 * see or drag. */
	if (link_)
		link_->armMode(zc::OverlayInteraction::None);
	w->close();
	monitorWnd_.clear();
	monitorWndName_.clear();
}

void ZcFramingPanel::openMonitorWindow()
{
	if (!client_ || !link_)
		return;

	/* Already up for this camera: raise it. There is only ever one, and it is
	   the same window the detection-area editor draws into. */
	if (QWidget *existing = monitorWindow()) {
		existing->raise();
		existing->activateWindow();
		return;
	}

	/* No window yet: the dock opens the source's Interaction window (it owns
	   the source lookup and tags the window with this camera). Doing it this
	   way instead of a projector is what makes editing reuse the window
	   rather than stack a second one — a projector takes no mouse. */
	if (link_->openInteract)
		link_->openInteract();
	if (QWidget *w = zcCameraWindowFor(client_->host())) {
		monitorWnd_ = w;
		w->raise();
		w->activateWindow();
	}
}

/* ---------------------------------------------------------------- */
/* ZcPresetPanel                                                     */
/* ---------------------------------------------------------------- */

namespace {

/* Slot layout, matching the camera's own web UI preset page. */
constexpr int kPresetPerPage = 10;
constexpr int kPresetPages = 10;
/* The camera rejects names shorter than 1 or longer than 18 characters. */
constexpr int kPresetNameMax = 18;
/* Thumbnails are written asynchronously; re-read the slot a little later. */
constexpr int kPresetReloadMs = 400;

/* A preset slot's clickable frame. A QFrame subclass (not a plain widget with
   an event filter) so the click is caught even though the thumbnail and name
   child widgets cover the whole surface. */
class ZcPresetSlotFrame : public QFrame {
public:
	std::function<void()> onClicked;

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && onClicked)
			onClicked();
		event->accept();
	}
};

const QString kPresetPanelStyle =
    "QWidget#PresetPanel { background:#1f2927; }"
    "QLabel { color:#8a9bb2; font-size:11px; letter-spacing:1px; }"
    "QPushButton { color:#eef2ed; background:#2b3a38; border:1px solid "
    "#3c514e; border-radius:4px; padding:4px 8px; font-size:11px; }"
    "QPushButton:hover { background:#354845; }"
    "QPushButton:pressed { background:#d8b878; color:#1f2927; }"
    "QPushButton:disabled { color:#6b7a90; background:#232d2b; "
    "border-color:#2f3d3b; }"
    "QPushButton:checked { background:#d8b878; color:#1f2927; }"
    "QComboBox { color:#eef2ed; background:#2b3a38; border:1px solid "
    "#3c514e; border-radius:4px; padding:2px 6px; font-size:11px; }"
    "QComboBox:disabled { color:#6b7a90; }";

} // namespace

ZcPresetPanel::ZcPresetPanel(QWidget *parent) : QWidget(parent)
{
	setObjectName("PresetPanel");
	setStyleSheet(kPresetPanelStyle);
	buildUi();
	/* The camera's trace transport advances on its own (a playback ends without
	   the operator clicking Stop). Poll its state while it is busy so the buttons
	   follow the camera (bug 24). Started/stopped in onTraceState(). */
	tracePollTimer_ = new QTimer(this);
	tracePollTimer_->setInterval(800);
	connect(tracePollTimer_, &QTimer::timeout, this,
		&ZcPresetPanel::refreshTraceState);
}

void ZcPresetPanel::buildUi()
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(6);

	/* Family selector: the camera keeps presets and traces (trajectories) in
	   two separate slot families, both addressed with the same 0..99 index
	   (spec §3). */
	auto *modeRow = new QHBoxLayout;
	modeRow->setSpacing(3);
	presetModeBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Preset"), this);
	traceModeBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Trace"), this);
	for (QPushButton *btn : {presetModeBtn_, traceModeBtn_}) {
		btn->setCheckable(true);
		btn->setFixedWidth(64);
		modeRow->addWidget(btn);
	}
	presetModeBtn_->setChecked(true);
	modeRow->addStretch(1);
	modeLabel_ = new QLabel(ztr("ZCameraPlugin.Dock.Presets"), this);
	modeRow->addWidget(modeLabel_);
	layout->addLayout(modeRow);

	/* Clicking the active button toggles it visually unchecked before the
	   handler runs, which used to leave BOTH family buttons unchecked (bug
	   list "Preset/Trace toggle can leave both unselected"). Re-assert the
	   check states here; setTraceMode then ignores the no-op change. */
	connect(presetModeBtn_, &QPushButton::clicked, this, [this]() {
		presetModeBtn_->setChecked(true);
		traceModeBtn_->setChecked(false);
		setTraceMode(false);
	});
	connect(traceModeBtn_, &QPushButton::clicked, this, [this]() {
		traceModeBtn_->setChecked(true);
		presetModeBtn_->setChecked(false);
		setTraceMode(true);
	});

	/* Page selector: one button per page of 10 slots. */
	auto *pageRow = new QHBoxLayout;
	pageRow->setSpacing(3);
	for (int p = 0; p < kPresetPages; ++p) {
		auto *btn = new QPushButton(QString::number(p + 1), this);
		btn->setCheckable(true);
		btn->setFixedWidth(28);
		connect(btn, &QPushButton::clicked, this,
			[this, p]() { showPage(p); });
		pageRow->addWidget(btn);
		pageButtons_.append(btn);
	}
	pageRow->addStretch(1);
	layout->addLayout(pageRow);

	/* Slot grid, 5 columns x 2 rows. */
	auto *grid = new QGridLayout;
	grid->setSpacing(6);
	for (int s = 0; s < kPresetPerPage; ++s) {
		auto *frame = new ZcPresetSlotFrame;
		frame->setObjectName("PresetSlot");
		frame->setFixedSize(116, 88);
		frame->setCursor(Qt::PointingHandCursor);
		frame->onClicked = [this, s]() {
			onSlotClicked(page_ * kPresetPerPage + s);
		};

		auto *fl = new QVBoxLayout(frame);
		fl->setContentsMargins(4, 4, 4, 4);
		fl->setSpacing(2);

		auto *thumb = new QLabel(frame);
		thumb->setFixedSize(106, 56);
		thumb->setAlignment(Qt::AlignCenter);
		thumb->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		thumb->setStyleSheet("color:#6b7a90;font-size:10px;");

		auto *name = new QLineEdit(frame);
		name->setReadOnly(true);
		name->setMaxLength(kPresetNameMax);
		name->setAlignment(Qt::AlignCenter);
		name->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		name->setStyleSheet(
		    "QLineEdit { color:#eef2ed; background:transparent; border:none;"
		    " font-size:10px; }"
		    "QLineEdit:!read-only { background:#2b3a38; border:1px solid "
		    "#d8b878; }");
		/* `&` and `=` would break the query string of set_name. */
		connect(name, &QLineEdit::textChanged, this,
			[name](const QString &text) {
				if (!text.contains('&') && !text.contains('='))
					return;
				QString cleaned = text;
				cleaned.remove('&');
				cleaned.remove('=');
				const int pos = name->cursorPosition();
				QSignalBlocker block(name);
				name->setText(cleaned);
				name->setCursorPosition(qMin(pos, cleaned.size()));
			});
		connect(name, &QLineEdit::returnPressed, this, [this, s]() {
			commitRename(page_ * kPresetPerPage + s,
				     slots_[s].name->text());
		});
		connect(name, &QLineEdit::editingFinished, this, [this, s]() {
			if (!slots_[s].name->isReadOnly())
				commitRename(page_ * kPresetPerPage + s,
					     slots_[s].name->text());
		});

		fl->addWidget(thumb);
		fl->addWidget(name);
		slots_.append(Slot{frame, thumb, name});
		grid->addWidget(frame, s / 5, s % 5);
	}
	layout->addLayout(grid);

	hintLabel_ = new QLabel("", this);
	hintLabel_->setStyleSheet("color:#d8b878;font-size:10px;");
	layout->addWidget(hintLabel_);

	/* Per-preset recall speed. Traces have no speed/time of their own, so the
	   whole row is hidden in trace mode. */
	speedRow_ = new QWidget(this);
	auto *speedRow = new QHBoxLayout(speedRow_);
	speedRow->setContentsMargins(0, 0, 0, 0);
	speedRow->setSpacing(6);
	speedRow->addWidget(new QLabel(ztr("ZCameraPlugin.Dock.RecallSpeed"), speedRow_));
	unitCombo_ = new QComboBox(speedRow_);
	unitCombo_->addItem(ztr("ZCameraPlugin.Dock.Speed"));
	unitCombo_->addItem(ztr("ZCameraPlugin.Dock.Time"));
	speedRow->addWidget(unitCombo_);
	speedCombo_ = new QComboBox(speedRow_);
	speedCombo_->setMinimumWidth(70);
	speedRow->addWidget(speedCombo_);
	speedRow->addStretch(1);
	layout->addWidget(speedRow_);

	/* Actions on the selected slot. The first two buttons carry a different
	   meaning per family: Recall / Add-Replace for presets, Record /
	   Prepare-Play-Stop for traces. */
	auto *actionRow = new QHBoxLayout;
	actionRow->setSpacing(6);
	primaryBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Recall"), this);
	secondaryBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Add"), this);
	deleteBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Delete"), this);
	renameBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Rename"), this);
	actionRow->addWidget(primaryBtn_);
	actionRow->addWidget(secondaryBtn_);
	actionRow->addWidget(deleteBtn_);
	actionRow->addWidget(renameBtn_);
	actionRow->addStretch(1);
	layout->addLayout(actionRow);
	layout->addStretch(1);

	connect(primaryBtn_, &QPushButton::clicked, this, [this]() {
		if (!client_)
			return;
		if (!traceMode_) {
			if (selected_ >= 0)
				client_->ptzPresetRecall(selected_);
			return;
		}
	/* Trace mode: the button starts a recording, or stops the one that
	   is running. */
	if (traceStateFromInt(traceState_) == TraceState::Recording) {
		client_->ptraceRecordStop(
			[this](int code) { reportTraceAction(code); });
		QTimer::singleShot(kPresetReloadMs, this, [this]() {
			refreshTraceState();
			loadSlot(selected_);
		});
		return;
	}
	if (selected_ < 0)
		return;
	const int index = selected_;
	/* The camera overwrites the slot, so its cached name and thumbnail
	   are stale the moment the recording starts. */
	thumbs_.remove(index);
	info_[index].insert("name", QString());
	slots_[index % kPresetPerPage].name->setText(defaultName(index));
	client_->ptraceRecordStart(
		index, [this](int code) { reportTraceAction(code); });
		QTimer::singleShot(kPresetReloadMs, this, [this, index]() {
			refreshTraceState();
			loadSlot(index);
		});
	});
	connect(secondaryBtn_, &QPushButton::clicked, this, [this]() {
		if (!client_)
			return;
		if (!traceMode_) {
			if (selected_ < 0)
				return;
			const int index = selected_;
			/* The stored thumbnail is now stale; re-read once the camera
			   has written the new one. */
			thumbs_.remove(index);
			client_->ptzPresetSet(index);
			QTimer::singleShot(kPresetReloadMs, this,
					   [this, index]() { loadSlot(index); });
			return;
		}
	switch (traceStateFromInt(traceState_)) {
	case TraceState::Playing:
		client_->ptracePlayStop(
			[this](int code) { reportTraceAction(code); });
		break;
	case TraceState::ReadyForPlay:
		client_->ptracePlayStart(
			[this](int code) { reportTraceAction(code); });
		break;
	default:
		if (selected_ < 0)
			return;
		client_->ptracePlayPrepare(
			selected_, [this](int code) { reportTraceAction(code); });
		break;
	}
		QTimer::singleShot(kPresetReloadMs, this,
				   [this]() { refreshTraceState(); });
	});
	connect(deleteBtn_, &QPushButton::clicked, this, [this]() {
		if (selected_ < 0 || !client_)
			return;
		const int index = selected_;
		thumbs_.remove(index);
		info_[index].insert("name", QString());
		if (traceMode_)
			client_->ptraceDelete(
				index,
				[this](int code) { reportTraceAction(code); });
		else
			client_->ptzPresetDelete(index);
		QTimer::singleShot(kPresetReloadMs, this,
				   [this, index]() { loadSlot(index); });
	});
	connect(renameBtn_, &QPushButton::clicked, this,
		&ZcPresetPanel::beginRename);

	connect(unitCombo_, &QComboBox::currentIndexChanged, this,
		[this](int unit) {
			if (selected_ < 0 || !client_)
				return;
			client_->ptzPresetSetSpeedUnit(selected_, unit);
			info_[selected_].insert("unit", unit);
			updateSpeedControls();
		});
	connect(speedCombo_, &QComboBox::currentIndexChanged, this,
		[this](int row) {
			if (selected_ < 0 || !client_ || row < 0)
				return;
			const int value = speedCombo_->itemData(row).toInt();
			if (unitCombo_->currentIndex() == 1) {
				client_->ptzPresetSetSpeedByDuration(selected_,
								     value);
				info_[selected_].insert("time", value * 1000);
			} else {
				client_->ptzPresetSetSpeedByIndex(selected_, value);
				info_[selected_].insert("speed", value);
			}
		});

	showPage(0);
}

void ZcPresetPanel::setClient(ZcCameraClient *client)
{
	client_ = client;
	/* A different camera: forget what the previous one reported about the
	   preset speed mode and ask the new one. */
	speedModeKnown_ = false;
	speedModeCommon_ = false;
	speedModeAsked_ = false;
	if (client_)
		/* The per-preset speed unit / speed follow `ptz_speed_mode`: with the
		   "Common" preset speed mode the camera applies one shared speed to
		   every preset, so the per-preset controls are refused. The PTZ catalog
		   carries that key, so repaint the row's enablement whenever it is
		   refreshed (bug list: the preset speed unit must follow the PTZ preset
		   speed mode). */
		connect(client_, &ZcCameraClient::groupRefreshed, this,
			[this](const QString &catalog) {
				if (catalog != QLatin1String("ptz"))
					return;
				const QString v = client_
						      ? client_->settingValue(QStringLiteral(
							    "ptz_speed_mode"))
						      : QString();
				if (!v.isEmpty()) {
					speedModeKnown_ = true;
					speedModeCommon_ =
					    v.compare(QStringLiteral("Common"),
						      Qt::CaseInsensitive) == 0;
				}
				updateActions();
			});
	askSpeedMode();
	refresh();
}

/* One read of `ptz_speed_mode`. The key only arrives with the PTZ settings
   catalog, which is fetched when the operator opens that group; the preset
   panel can be used without ever opening it, and until it had a value the
   per-preset speed stayed editable even under the Common mode (bug 74). */
void ZcPresetPanel::askSpeedMode()
{
	if (!client_ || speedModeAsked_)
		return;
	speedModeAsked_ = true;
	ZcCameraClient *c = client_;
	c->readSetting(QStringLiteral("ptz_speed_mode"),
		       [this, c](const ZcSettingValue &v) {
			       if (c != client_ || v.value.isEmpty())
				       return;
			       speedModeKnown_ = true;
			       speedModeCommon_ =
				   v.value.compare(QStringLiteral("Common"),
						   Qt::CaseInsensitive) == 0;
			       updateActions();
		       });
}

void ZcPresetPanel::refresh()
{
	/* A different camera: drop everything cached and start over. */
	info_.clear();
	thumbs_.clear();
	maxSpeed_ = 0;
	maxTime_ = 0;
	traceState_ = 0;
	if (tracePollTimer_)
		tracePollTimer_->stop();
	showPage(page_);

	if (!client_)
		return;
	if (traceMode_)
		refreshTraceState();
	/* The speed/time menus are bounded by the camera's own maxima. */
	client_->readSetting("ptz_common_speed", [this](const ZcSettingValue &v) {
		maxSpeed_ = v.max > 0 ? static_cast<int>(v.max) : 0;
		updateSpeedControls();
	});
	client_->readSetting("ptz_common_time", [this](const ZcSettingValue &v) {
		maxTime_ = v.max > 0 ? static_cast<int>(v.max) : 0;
		updateSpeedControls();
	});
}

/* Spec §3: presets and traces are two independent slot families. Switching
   family is a full reload — they share the 0..99 index space but no entry. */
void ZcPresetPanel::setTraceMode(bool trace)
{
	if (traceMode_ == trace)
		return;
	traceMode_ = trace;
	info_.clear();
	thumbs_.clear();
	traceState_ = 0;
	if (tracePollTimer_)
		tracePollTimer_->stop();
	presetModeBtn_->setChecked(!trace);
	traceModeBtn_->setChecked(trace);
	modeLabel_->setText(trace ? ztr("ZCameraPlugin.Dock.Traces") : ztr("ZCameraPlugin.Dock.Presets"));
	speedRow_->setVisible(!trace);
	showPage(0);
	if (trace)
		refreshTraceState();
}

void ZcPresetPanel::refreshTraceState()
{
	if (!traceMode_ || !client_ || !client_->isConnected())
		return;
	client_->ptraceQuery([this](int stCode) {
		if (traceMode_)
			onTraceState(stCode);
	});
}

void ZcPresetPanel::onTraceState(int stCode)
{
	/* -1 means the camera did not answer; keep the last known state rather
	   than re-enabling buttons that may not be actionable. */
	if (stCode < 0)
		return;
	traceState_ = stCode;
	updateActions();

	/* The trace transport runs on the camera and advances on its own: while it
	   is busy (recording / preparing / playing / deleting) keep polling so the
	   buttons follow it — e.g. a playback that ends returns to "Prepare" without
	   the operator clicking Stop (bug 24). Once it is idle the poll is dropped. */
	const TraceState state = traceStateFromInt(stCode);
	const bool busy = (state == TraceState::Recording ||
			   state == TraceState::Preparing ||
			   state == TraceState::Playing ||
			   state == TraceState::Deleting);
	if (tracePollTimer_ && busy && !tracePollTimer_->isActive())
		tracePollTimer_->start();
	else if (tracePollTimer_ && !busy)
		tracePollTimer_->stop();
}

void ZcPresetPanel::reportTraceAction(int code)
{
	/* Accepted commands stay quiet — the state poll repaints the buttons.
	   A rejection (code -2 while auto-framing owns the head, the transport
	   is still finalizing the just-stopped recording, or the slot is gone)
	   used to leave the panel falling back to its idle face with no word,
	   which the operator read as "play is broken" (bug list #137). */
	if (code == 0)
		return;
	hintLabel_->setText(ztr("ZCameraPlugin.Dock.TraceRejected"));
	refreshTraceState();
}

QString ZcPresetPanel::defaultName(int index) const
{
	/* The wire index is 0-based, the label 1-based (spec §3). */
	return (traceMode_ ? ztr("ZCameraPlugin.Dock.TraceN") : ztr("ZCameraPlugin.Dock.PresetN"))
	    .arg(index + 1, 3, 10, QChar('0'));
}

bool ZcPresetPanel::selectedIsNamed() const
{
	if (selected_ < 0)
		return false;
	return !info_.value(selected_).value("name").toString().isEmpty();
}

void ZcPresetPanel::showPage(int page)
{
	if (page < 0 || page >= kPresetPages)
		return;
	page_ = page;
	selected_ = -1;
	for (int p = 0; p < pageButtons_.size(); ++p)
		pageButtons_[p]->setChecked(p == page);

	/* Reset the visible slots before the replies land, so a page switch never
	   shows the previous page's names or thumbnails. */
	for (int s = 0; s < kPresetPerPage; ++s) {
		slots_[s].thumb->setPixmap(QPixmap());
		slots_[s].thumb->setText("");
		slots_[s].name->setText(defaultName(page * kPresetPerPage + s));
	}
	hintLabel_->clear();
	updateSelection();
	updateActions();
	updateSpeedControls();

	for (int s = 0; s < kPresetPerPage; ++s)
		loadSlot(page * kPresetPerPage + s);
}

void ZcPresetPanel::loadSlot(int index)
{
	if (!client_ || !client_->isConnected())
		return;
	const bool trace = traceMode_;
	/* A reply that arrives after the user switched family must not overwrite
	   the other family's slot. */
	auto apply = [this, index, trace](const QJsonObject &info) {
		if (trace != traceMode_)
			return;
		applySlotInfo(index, info);
	};
	if (trace)
		client_->ptraceInfo(index, apply);
	else
		client_->ptzPresetInfo(index, apply);
}

void ZcPresetPanel::applySlotInfo(int index, const QJsonObject &info)
{
	/* A late reply for a page the user already left must not repaint it. */
	if (index / kPresetPerPage != page_)
		return;
	const bool trace = traceMode_;
	info_[index] = info;

	Slot &slot = slots_[index % kPresetPerPage];
	const QString name = info.value("name").toString();
	slot.name->setText(name.isEmpty() ? defaultName(index) : name);

	if (index == selected_) {
		updateActions();
		updateSpeedControls();
	}
	/* A non-zero status means the camera is mid-move; warn rather than
	   letting the operator recall into a moving head. Presets only — a trace
	   reply carries no status. The warning must also CLEAR: the next reply
	   for the selected slot with status back to 0 means the head settled,
	   and a stuck notice would keep warning about a move that ended. */
	if (!trace) {
		if (info.value("status").toInt(0) != 0) {
			hintLabel_->setText(ztr("ZCameraPlugin.Dock.PresetBusy"));
		} else if (index == selected_ &&
			   hintLabel_->text() ==
				   ztr("ZCameraPlugin.Dock.PresetBusy")) {
			hintLabel_->clear();
		}
	}

	if (name.isEmpty()) {
		slot.thumb->setPixmap(QPixmap());
		slot.thumb->setText("");
		return;
	}
	if (thumbs_.contains(index)) {
		slot.thumb->setPixmap(thumbs_.value(index));
		return;
	}
	slot.thumb->setText(ztr("ZCameraPlugin.Dock.Ellipsis"));
	auto got = [this, index, trace](const QByteArray &bytes) {
		if (trace != traceMode_)
			return;
		QPixmap pm;
		if (!bytes.isEmpty())
			pm.loadFromData(bytes);
		if (!pm.isNull())
			thumbs_.insert(index, pm);
		if (index / kPresetPerPage != page_)
			return;
		Slot &s = slots_[index % kPresetPerPage];
		if (pm.isNull()) {
			/* No thumbnail for this slot: the name alone identifies it. */
			s.thumb->setText("");
			return;
		}
		s.thumb->setText("");
		s.thumb->setPixmap(pm.scaled(s.thumb->size(),
					     Qt::KeepAspectRatio,
					     Qt::SmoothTransformation));
	};
	if (trace)
		client_->ptraceThumbnail(index, got);
	else
		client_->ptzPresetThumbnail(index, got);
}

void ZcPresetPanel::onSlotClicked(int index)
{
	/* While a trace is recording or playing the camera owns the transport;
	   moving the selection to another slot would target a different take. */
	if (traceMode_ && !traceActions(traceStateFromInt(traceState_)).navigate)
		return;
	/* Click toggles: clicking the selected slot again clears the selection,
	   and no action is available while nothing is selected (spec §3). */
	selected_ = (selected_ == index) ? -1 : index;
	updateSelection();
	updateActions();
	updateSpeedControls();
}

void ZcPresetPanel::updateSelection()
{
	for (int s = 0; s < slots_.size(); ++s) {
		const bool selected =
		    (page_ * kPresetPerPage + s) == selected_;
		slots_[s].frame->setStyleSheet(
		    selected
			? "QFrame#PresetSlot { border:2px solid #d8b878; "
			  "border-radius:4px; background:#22302e; }"
			: "QFrame#PresetSlot { border:1px solid #3c514e; "
			  "border-radius:4px; background:#1a2321; }");
		/* Leaving a slot always ends an in-progress rename. */
		slots_[s].name->setReadOnly(true);
		slots_[s].name->setAttribute(Qt::WA_TransparentForMouseEvents,
					     true);
	}
}

void ZcPresetPanel::setFramingOwned(bool owned)
{
	if (framingOwned_ == owned)
		return;
	framingOwned_ = owned;
	updateActions();
}

void ZcPresetPanel::updateActions()
{
	const bool has = selected_ >= 0;
	const bool named = selectedIsNamed();

	if (!traceMode_) {
		primaryBtn_->setText(ztr("ZCameraPlugin.Dock.Recall"));
		/* Recall / Delete / Rename act on a preset that already exists, so a
		   selected-but-empty slot must grey them; "Add" writes a new preset and
		   stays available (the camera's web UI toggles Add/Replace similarly). */
		primaryBtn_->setEnabled(named);
		/* "Add" writes a new preset, "Replace" overwrites a named one — the
		   same label toggle the camera's web UI uses. */
		secondaryBtn_->setText(named ? ztr("ZCameraPlugin.Dock.Replace") : ztr("ZCameraPlugin.Dock.Add"));
		secondaryBtn_->setEnabled(has);
		deleteBtn_->setEnabled(named);
		renameBtn_->setEnabled(named);
		/* With the PTZ preset speed mode "Common" the camera recalls every
		   preset at one shared speed, so a per-preset unit/speed is refused; the
		   camera's web UI greys its "Unit"/"Speed" entries in exactly this case.
		   An unread value leaves them usable rather than over-disabling the
		   row. Read from the latched camera value, never from a widget, so
		   re-selecting a preset cannot light the row up again (bug 74). */
		const bool perPresetSpeed =
		    !client_ || !speedModeKnown_ || !speedModeCommon_;
		unitCombo_->setEnabled(named && perPresetSpeed);
		speedCombo_->setEnabled(named && perPresetSpeed &&
					speedCombo_->count() > 0);
		for (QPushButton *btn : pageButtons_)
			btn->setEnabled(true);
		return;
	}

	/* Trace mode. Which control is actionable depends on where the camera's
	   trace transport currently is (spec §3). While auto framing owns the
	   head every trace action is refused with code -2, so the whole trace
	   face greys; the preset family stays usable (bug list 55). */
	if (framingOwned_) {
		primaryBtn_->setEnabled(false);
		secondaryBtn_->setEnabled(false);
		deleteBtn_->setEnabled(false);
		renameBtn_->setEnabled(false);
		unitCombo_->setEnabled(false);
		speedCombo_->setEnabled(false);
		for (QPushButton *btn : pageButtons_)
			btn->setEnabled(false);
		hintLabel_->setText(ztr("ZCameraPlugin.Dock.TraceOwnedByFraming"));
		return;
	}

	const TraceState state = traceStateFromInt(traceState_);
	const TraceActions act = traceActions(state);

	primaryBtn_->setText(state == TraceState::Recording ? ztr("ZCameraPlugin.Dock.StopRecord")
							   : ztr("ZCameraPlugin.Dock.StartRecord"));
	/* While recording the button is the stop control, so it needs no slot
	   selection; otherwise a slot must be selected to record into. */
	primaryBtn_->setEnabled(act.record &&
				(has || state == TraceState::Recording));

	secondaryBtn_->setText(state == TraceState::Playing
				   ? ztr("ZCameraPlugin.Dock.StopPlayback")
				   : state == TraceState::ReadyForPlay ? ztr("ZCameraPlugin.Dock.Play")
								      : ztr("ZCameraPlugin.Dock.Prepare"));
	/* Playing and ready-to-play act on the trace the camera already holds,
	   not on the selection. */
	secondaryBtn_->setEnabled(act.prepare &&
				  (named || state == TraceState::Playing ||
				   state == TraceState::ReadyForPlay));

	deleteBtn_->setEnabled(act.remove && named);
	/* Renaming writes into a slot, so it needs a selection and an idle
	   transport. */
	renameBtn_->setEnabled(has && act.navigate);

	unitCombo_->setEnabled(false);
	speedCombo_->setEnabled(false);
	for (QPushButton *btn : pageButtons_)
		btn->setEnabled(act.navigate);

	switch (state) {
	case TraceState::Recording:
		hintLabel_->setText(
		    ztr("ZCameraPlugin.Dock.TraceRecording"));
		break;
	case TraceState::ReadyForPlay:
		hintLabel_->setText(ztr("ZCameraPlugin.Dock.TraceReady"));
		break;
	case TraceState::Playing:
		hintLabel_->setText(ztr("ZCameraPlugin.Dock.TracePlaying"));
		break;
	case TraceState::Preparing:
	case TraceState::Deleting:
		hintLabel_->setText(ztr("ZCameraPlugin.Dock.TraceBusy"));
		break;
	default:
		hintLabel_->clear();
		break;
	}
}

void ZcPresetPanel::updateSpeedControls()
{
	/* Trace slots carry no unit/speed fields; the combos are disabled in
	   trace mode anyway (updateActions), and repainting them from a trace
	   reply would blank the preset family's selections while the user is
	   only browsing traces. */
	if (traceMode_)
		return;
	if (selected_ < 0) {
		QSignalBlocker block(speedCombo_);
		speedCombo_->clear();
		return;
	}
	const QJsonObject info = info_.value(selected_);
	const int unit = info.value("unit").toInt(0);
	{
		QSignalBlocker block(unitCombo_);
		unitCombo_->setCurrentIndex(unit == 1 ? 1 : 0);
	}

	QSignalBlocker block(speedCombo_);
	speedCombo_->clear();
	if (unit == 1) {
		/* Time: the camera stores milliseconds; offer whole seconds. */
		const int maxSeconds = maxTime_ / 1000;
		for (int s = 1; s <= maxSeconds; ++s)
			speedCombo_->addItem(ztr("ZCameraPlugin.Dock.Seconds").arg(s), s);
		const int current = info.value("time").toInt() / 1000;
		if (current > 0 && speedCombo_->count() > 0)
			speedCombo_->setCurrentIndex(
			    qBound(0, current - 1, speedCombo_->count() - 1));
	} else {
		for (int s = 1; s <= maxSpeed_; ++s)
			speedCombo_->addItem(QString::number(s), s);
		const int current = info.value("speed").toInt();
		if (current > 0 && speedCombo_->count() > 0)
			speedCombo_->setCurrentIndex(
			    qBound(0, current - 1, speedCombo_->count() - 1));
	}
	speedCombo_->setEnabled(speedCombo_->count() > 0);
}

void ZcPresetPanel::beginRename()
{
	if (selected_ < 0)
		return;
	Slot &slot = slots_[selected_ % kPresetPerPage];
	slot.name->setAttribute(Qt::WA_TransparentForMouseEvents, false);
	slot.name->setReadOnly(false);
	slot.name->setFocus();
	slot.name->selectAll();
}

void ZcPresetPanel::commitRename(int index, const QString &text)
{
	Slot &slot = slots_[index % kPresetPerPage];
	slot.name->setReadOnly(true);
	slot.name->setAttribute(Qt::WA_TransparentForMouseEvents, true);

	QString name = text;
	name.remove('&');
	name.remove('=');
	const QString stored = info_.value(index).value("name").toString();

	/* 1..18 bytes; anything else reverts to the stored name. The camera
	   counts UTF-8 BYTES (same limit the framing name clamp uses), so a
	   Chinese name of 18 characters is over the limit and must be rejected
	   here rather than silently truncated by the camera. */
	if (name.isEmpty() || name.toUtf8().size() > kPresetNameMax) {
		slot.name->setText(stored.isEmpty() ? defaultName(index) : stored);
		return;
	}
	if (name == stored)
		return;

	if (!client_)
		return;
	if (traceMode_)
		client_->ptraceSetName(index, name);
	else
		client_->ptzPresetSetName(index, name);
	/* The camera does not echo the name back, so show it now and re-read to
	   confirm it stuck. */
	info_[index].insert("name", name);
	slot.name->setText(name);
	if (index == selected_)
		updateActions();
	loadSlot(index);
}

/* ---------------------------------------------------------------- */
/* ZcControlDock                                                     */
/* ---------------------------------------------------------------- */

/* Framing ownership of the head is decided inside ZcFramingPanel now
   (updateGating pushes it out through onFramingOwnsPtz): that panel sees every
   snapshot, including the one read at connect time, and its status word has
   already separated Idle/Disabled from Detecting/Tracking/Target-lost — which
   the old `value != 0` test here did not (bug lists 55/179). */

/* Set by ~ZcControlDock. A connect probe runs a nested event loop inside
   client_->connect(); OBS can tear the dock down from an event delivered in that
   loop, leaving the probe's frames on the stack. When the loop unwinds they
   resume (negotiateOrigin -> openCameraNow -> openCamera -> the click slot)
   against the already-freed dock, so each resume point checks this and bails out
   before dereferencing a freed member. */
static bool g_controlDockClosing = false;

ZcControlDock::ZcControlDock(QWidget *parent) : QDockWidget(parent)
{
	/* A fresh dock: clear any flag left by a previous instance's teardown. */
	g_controlDockClosing = false;
	/* The source <-> dock interaction link. Registered as the global pointer
	   sink here and detached in the destructor, so a click arriving while no
	   dock exists has nowhere to go rather than a dangling one. */
	overlay_ = new ZcOverlayLink(this);
	zc::overlaySetPointerSink(overlay_);
	/* Opening the Interact window is the dock's job: it owns the source
	   lookup, and libobs only forwards mouse events to that one pane. */
	overlay_->openInteract = [this]() { openSourceInteraction(); };
	overlay_->closeInteract = [this]() { closeSourceInteraction(); };
	setWindowTitle(QString::fromUtf8(
	    obs_module_text("ZCameraPlugin.Dock.Title")));
	setObjectName("zcamera-control-dock");
	setAllowedAreas(Qt::AllDockWidgetAreas);
	/* The settings pane needs room for a label plus a combo/slider per row;
	   at the default size the controls would be clipped. */
	resize(1040, 640);

	auto *container = new QWidget(this);
	auto *outer = new QVBoxLayout(container);
	outer->setContentsMargins(0, 0, 0, 0);

	splitter_ = new QSplitter(Qt::Horizontal, container);
	rail_ = new ZcCameraRail(splitter_);

	auto *right = new QWidget(splitter_);
	auto *rightLayout = new QVBoxLayout(right);
	rightLayout->setContentsMargins(0, 0, 0, 0);

	/* Right side is a two-tab pane: Settings (existing panel) and PTZ. */
	tabs_ = new QTabWidget(right);
	tabs_->setDocumentMode(true);
	tabs_->setStyleSheet(
	    "QTabWidget::pane { border:none; background:#1f2927; }"
	    "QTabBar::tab { color:#8a9bb2; background:#22302e; padding:7px 16px;"
	    " border:1px solid #3c514e; border-bottom:none; font-size:11px;"
	    " letter-spacing:1px; }"
	    "QTabBar::tab:selected { color:#eef2ed; background:#1f2927;"
	    " border-top:2px solid #d8b878; }"
	    "QTabBar::tab:hover:!selected { color:#eef2ed; }");
	settings_ = new ZcSettingsPanel(tabs_);
	tabs_->addTab(settings_, ztr("ZCameraPlugin.Dock.Settings"));

	/* The PTZ pane (joystick + preset list) is built up front but only
	   inserted into the tab bar once a camera reports PTZ support (spec §3).
	   It must stay hidden until then: a widget parented to the QTabWidget
	   without being a tab is a plain child, so it would be painted on top of
	   the current page (it used to cover Settings until the first camera was
	   selected). addTab() reparents it into the stack, which shows it on
	   selection. */
	ptzTab_ = new QWidget(tabs_);
	ptzTab_->hide();
	auto *ptzLayout = new QHBoxLayout(ptzTab_);
	ptzLayout->setContentsMargins(0, 0, 0, 0);
	ptzLayout->setSpacing(0);
	/* The pad grew tall (joystick + position + the four range-limit rows), so it
	   scrolls rather than squeezing the joystick on a short dock. The scroll area
	   keeps the pad's fixed width, so the presets keep the rest. That width has
	   to hold the joystick (220) *and* the zoom column (52) plus the pad's own
	   margins — 300 in all. Pinned any narrower, and with the horizontal
	   scrollbar off, the zoom column was clipped off the right edge and never
	   appeared. */
	ptzScroll_ = new QScrollArea(ptzTab_);
	ptzScroll_->setWidgetResizable(true);
	ptzScroll_->setFrameShape(QFrame::NoFrame);
	ptzScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	ptzScroll_->setFixedWidth(320);
	ptzScroll_->viewport()->setStyleSheet("background:#1f2927;");
	ptz_ = new ZcPtzPad();
	ptz_->setOverlayLink(overlay_);
	ptzScroll_->setWidget(ptz_);
	presets_ = new ZcPresetPanel(ptzTab_);
	ptzLayout->addWidget(ptzScroll_, 0);
	ptzLayout->addWidget(presets_, 1);
	rightLayout->addWidget(tabs_, 1);

	/* The auto-framing tab is built up front and hidden like the PTZ tab; it is
	   inserted into the tab bar once a camera reports auto framing (see
	   kFramingPanelVisible in updatePtzTab). Building it here keeps the widget,
	   its client calls and its WebSocket handlers alive. */
	framingTab_ = new QWidget(tabs_);
	framingTab_->hide();
	auto *framingLayout = new QVBoxLayout(framingTab_);
	framingLayout->setContentsMargins(0, 0, 0, 0);
	framing_ = new ZcFramingPanel(framingTab_);
	framing_->setOverlayLink(overlay_);
	framingLayout->addWidget(framing_, 1);

	/* One armed mode at a time: the PTZ tab's "click to move" and the framing
	   tab's "edit detection area" are the same slot on the source, so arming
	   one clears the other's tick. The toggles are the panels', the state is
	   the link's; this keeps the two in step without either panel knowing
	   about the other. */
	overlay_->onModeChanged = [this](zc::OverlayInteraction mode) {
		if (ptz_) {
			if (QToolButton *b = ptz_->clickMoveButton()) {
				QSignalBlocker block(b);
				b->setChecked(mode ==
					      zc::OverlayInteraction::PtzClick);
			}
		}
		if (framing_) {
			if (QPushButton *b = framing_->maskEditButton()) {
				QSignalBlocker block(b);
				b->setChecked(mode ==
					      zc::OverlayInteraction::MaskEdit);
			}
		}
	};

	splitter_->addWidget(rail_);
	splitter_->addWidget(right);
	splitter_->setStretchFactor(0, 0);
	splitter_->setStretchFactor(1, 1);
	outer->addWidget(splitter_, 1);

	/* Status bar. */
	auto *statusBar = new QHBoxLayout;
	/* Recording badge (bug list #20): the camera can start recording itself
	   when "recording triggered by tracking" is on and tracking locks on,
	   and nothing else in the dock said so. Driven by the camera's own
	   RecStarted / RecStoped notifications, which cover every way recording
	   begins -- a track-triggered one included. */
	recBadge_ = new QLabel("● REC", container);
	recBadge_->setStyleSheet(
	    "color:#ff6b6b;background:rgba(0,0,0,.35);border-radius:3px;"
	    "padding:2px 8px;font-weight:bold;");
	recBadge_->setToolTip(ztr("ZCameraPlugin.Dock.RecBadgeTip"));
	recBadge_->hide();
	statusBar->addWidget(recBadge_);
	statusBarLabel_ = new QLabel(ztr("ZCameraPlugin.Dock.StatusDisconnected"), container);
	statusBarLabel_->setStyleSheet("color:#8a9bb2;padding:2px 6px;");
	statusBar->addWidget(statusBarLabel_);
	statusBar->addStretch();
#ifdef ZC_ENABLE_DEBUG_PANEL
	/* Development aid: only compiled in when the build asks for it, so a
	   release build ships no debug affordance (bug list: "Debug button not
	   removed from the release build"). */
	debugButton_ = new QPushButton(ztr("ZCameraPlugin.Dock.Debug"), container);
	debugButton_->setCheckable(true);
	statusBar->addWidget(debugButton_);
#endif
	outer->addLayout(statusBar);

	setWidget(container);

#ifdef ZC_ENABLE_DEBUG_PANEL
	/* Debug panel (dockable/floating, hidden by default). */
	debugPanel_ = new ZcDebugPanel(nullptr);
	debugPanel_->hide();

	connect(debugButton_, &QPushButton::toggled, this, [this](bool on) {
		if (on)
			debugPanel_->show();
		else
			debugPanel_->hide();
	});
#endif

	connect(rail_, &ZcCameraRail::cameraClicked, this,
		&ZcControlDock::onCameraClicked);
	/* A row's own chip: the only path that touches the scene (spec §1,
	   L2/L6). It names its camera, so nothing has to be selected first. */
	connect(rail_, &ZcCameraRail::addRequested, this,
		[this](const QString &host) {
			addCameraToSource(host);
			refreshRailPresence();
		});
	/* A gold check on an already-added row removes the source from OBS. */
	connect(rail_, &ZcCameraRail::removeRequested, this,
		[this](const QString &host) {
			removeCameraFromSource(host);
			refreshRailPresence();
		});
	connect(rail_, &ZcCameraRail::refreshRequested, this,
		&ZcControlDock::onCameraRefreshRequested);
	connect(ptz_, &ZcPtzPad::homeRequested, this,
		[this]() {
			if (client_)
				client_->ptzHome();
		});

	/* Presence is evaluated live, never cached (spec §1, L4): watch libobs's
	   own source create/destroy/rename signals so removing a source in OBS's
	   Sources list clears the rail badge immediately. The callbacks may arrive
	   from a non-Qt thread, so they hop onto the dock's thread. */
	obsSignals_ = obs_get_signal_handler();
	for (const char *signal : {"source_create", "source_destroy",
				   "source_rename"})
		signal_handler_connect(obsSignals_, signal,
				       &ZcControlDock::onObsSourceSignal, this);

	/* mDNS discovery: refresh the camera rail periodically. Runs on the Qt main
	   thread (the OBS frontend thread), which is also where the source is added,
	   so scene access is safe. */
	scanTimer_ = new QTimer(this);
	scanTimer_->setInterval(2500);
	connect(scanTimer_, &QTimer::timeout, this, &ZcControlDock::refreshCameras);
	scanTimer_->start();
	/* A closed dock does not need a discovery poll every 2.5 s: the rail is
	   not on screen, and the mDNS/source churn is pure overhead until the
	   dock is shown again. */
	connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
		if (!scanTimer_)
			return;
		if (visible)
			scanTimer_->start();
		else
			scanTimer_->stop();
	});

	/* A setting the camera applies by restarting its web service (HTTPS,
	   identity auth) or by moving to another address (network type) drops the
	   control connection until it comes back (bug 8). */
	connect(settings_, &ZcSettingsPanel::cameraRestarting, this,
		&ZcControlDock::onCameraRestarting);
	connect(settings_, &ZcSettingsPanel::loginRequested, this,
		&ZcControlDock::onLoginSubmitted);
	restartTimer_ = new QTimer(this);
	restartTimer_->setInterval(4000);
	connect(restartTimer_, &QTimer::timeout, this, &ZcControlDock::tryReconnect);
}

/* Query the mDNS discovery records for the currently advertised cameras and
   hand them to the rail, together with the cameras OBS holds as sources. The
   iterator holds the discovery lock for the duration of the loop only, so UI
   work afterwards (rebuilding the list, adding a source) never runs while
   records are being mutated. */
void ZcControlDock::refreshCameras()
{
	QList<QPair<QString, QString>> found;
	{
		SspMDnsIterator it;
		while (it.hasNext()) {
			ssp_device_item *d = it.next();
			if (!d || d->ip_address.empty())
				continue;
			found.append(qMakePair(
				QString::fromStdString(d->device_name),
				QString::fromStdString(d->ip_address)));
		}
	}

	QVector<ZcRailCamera> rows;
	/* Every host that already has a row, so the three passes below can never
	   list the same camera twice. */
	QSet<QString> listed;
	QSet<QString> live;
	for (const auto &f : found) {
		if (f.second.isEmpty())
			continue;
		/* Remember the name discovery advertises for this address, so the row
		   can survive as an offline one once the camera stops answering. */
		knownCameras_.insert(f.second, f.first);
		live.insert(f.second);
		if (listed.contains(f.second))
			continue;
		listed.insert(f.second);
		rows.append({f.first, f.second,
			     unreachableHosts_.contains(f.second)});
	}

	/* A camera that re-appears after being away is answering again: drop the
	   "does not answer" mark an earlier failed connect left on it, so a power
	   cycle does not leave a live camera greyed out until it is clicked. Only
	   the transition is cleared — a camera that advertises but keeps refusing
	   connections is listed every scan and so never re-enters here. */
	for (const QString &host : live) {
		if (!advertised_.contains(host))
			unreachableHosts_.remove(host);
	}
	advertised_ = live;

	/* Session memory: a camera discovery has seen before but that is not
	   advertising right now keeps its row, marked offline, instead of vanishing
	   from the rail. That is what lets an operator whose camera dropped off the
	   network still find it — and click it back — once it returns. Sorted by
	   address so these rows do not move around between scans. */
	QList<QString> goneHosts;
	for (auto it = knownCameras_.constBegin(); it != knownCameras_.constEnd();
	     ++it) {
		if (!listed.contains(it.key()))
			goneHosts.append(it.key());
	}
	std::sort(goneHosts.begin(), goneHosts.end());
	for (const QString &host : goneHosts) {
		listed.insert(host);
		rows.append({knownCameras_.value(host), host, true});
	}

	/* Spec §1, L11: a camera OBS holds as a source is listed even when discovery
	   is not advertising it — otherwise a configured camera whose mDNS record is
	   missing could not be selected at all. It is marked offline until it answers
	   a control connection, and cannot be operated. Sorted by address so the rows
	   this adds do not move around between scans. */
	QList<QString> extraHosts;
	for (const QString &host : sourceHosts()) {
		if (!listed.contains(host))
			extraHosts.append(host);
	}
	std::sort(extraHosts.begin(), extraHosts.end());
	for (const QString &host : extraHosts)
		rows.append({QString(), host,
			     unreachableHosts_.contains(host) ||
				 !reachableHosts_.contains(host)});

	if (rows.isEmpty())
		railEmptyScans_++;
	else
		railEmptyScans_ = 0;
	/* Discovery is answered asynchronously, so the first scans after the dock
	   opens can legitimately come back empty: keep the rail's busy spin up for
	   that window, then fall back to the plain "no camera" line rather than
	   sweeping a bar that will never finish. */
	constexpr int kRailScanSpins = 8;
	if (rail_) {
		rail_->setCameras(rows);
		rail_->setBusy(rows.isEmpty() &&
			       railEmptyScans_ <= kRailScanSpins);
	}
	/* Discovery and presence are independent (spec §1, L1/L4): a camera can be
	   listed without being a source, and a source can be removed at any time. */
	refreshRailPresence();
}

/* The rail header's rescan button. Discovery answers asynchronously — a query
   goes out, the cameras reply, the records land — so the rescan is held open for
   a window rather than for one round trip.

   mDNS is slow and lossy: a forced query is a single UDP packet with no reply
   guarantee, so a camera that misses it is only found by the loop's next
   second-by-second query. The window therefore has to span several of those
   retries. 900 ms was the first cut: it usually expired before the first reply
   had even arrived, so the spin stopped on an unchanged list and the press read
   as a no-op. Eight seconds covers ~8 background queries plus unicast answers
   and any startup delay of a camera just powered on. */
static constexpr int kRailRescanWindowMs = 8000;
/* Re-read the records and re-ask at this cadence inside the window, so cameras
   appear as their answers land instead of only at the end. */
static constexpr int kRailRescanStepMs = 1000;

void ZcControlDock::onCameraRefreshRequested()
{
	/* A deliberate rescan restarts the "nothing found yet" count, so pressing
	   the button on a rail that had given up brings the busy spin back for the
	   window in which the answers are still outstanding. */
	railEmptyScans_ = 0;

	if (rail_)
		rail_->setRefreshing(true);

	ssp_mdns_request_refresh();
	refreshCameras();

	if (!rescanTimer_) {
		rescanTimer_ = new QTimer(this);
		rescanTimer_->setInterval(kRailRescanStepMs);
		connect(rescanTimer_, &QTimer::timeout, this, [this]() {
			/* Hold the "still looking" count down for the whole
			   window: the tick cadence would otherwise run the
			   empty-scan budget out and drop the busy page before
			   the rescan has finished. */
			railEmptyScans_ = 0;
			refreshCameras();
			++rescanTicks_;
			if (rescanTicks_ * kRailRescanStepMs >=
			    kRailRescanWindowMs) {
				rescanTimer_->stop();
				rescanTicks_ = 0;
				if (rail_)
					rail_->setRefreshing(false);
				return;
			}
			/* Keep asking: the whole point of the window is to
			   outlast a lost query. */
			ssp_mdns_request_refresh();
		});
	}
	/* A second press while the window is open restarts it rather than
	   stacking a second one. */
	rescanTicks_ = 0;
	rescanTimer_->start();
}

/* The source name that represents a camera. It is the only identity used for
   the added/not-added decision (spec §1, L3). */
QString ZcControlDock::sourceNameForHost(const QString &host)
{
	return zc::cameraSourceName(host);
}

/* Enumerating the sources OBS holds is what makes the rail independent of
   discovery: a `ZCamera <ip>` source that no mDNS record mentions still yields a
   row. Names that are not `ZCamera <ip>` (scenes, other plugins' sources) are
   filtered out by hostFromSourceName. */
static bool collectSourceHost(void *data, obs_source_t *source)
{
	auto *hosts = static_cast<QSet<QString> *>(data);
	const QString host =
	    zc::hostFromSourceName(QString::fromUtf8(obs_source_get_name(source)));
	if (!host.isEmpty())
		hosts->insert(host);
	return true;
}

QSet<QString> ZcControlDock::sourceHosts()
{
	QSet<QString> hosts;
	obs_enum_sources(collectSourceHost, &hosts);
	return hosts;
}

/* Find a zcamera_source whose settings hold this camera's address. The rail's
   "added" state keys off the source name ("ZCamera <ip>"), but a source can be
   created from OBS's own Sources panel and only given the address in its
   properties, in which case its name is the default "ZCamera". Matching by the
   stored zc_source_ip makes that source count as "added" too and lets a second
   dock add reuse it instead of stacking a duplicate (bug 15). Returns the
   source addref'd, or null. */
struct ZcHostMatch {
	QString host;
	obs_source_t *found = nullptr;
};
static bool findSourceByHostProc(void *param, obs_source_t *src)
{
	auto *m = static_cast<ZcHostMatch *>(param);
	if (strcmp(obs_source_get_id(src), "zcamera_source") != 0)
		return true;
	obs_data_t *settings = obs_source_get_settings(src);
	const char *ip = settings ? obs_data_get_string(settings, "zc_source_ip")
				  : nullptr;
	const bool match = ip && m->host == QString::fromUtf8(ip);
	obs_data_release(settings);
	if (match) {
		m->found = obs_source_get_ref(src);
		return false;
	}
	return true;
}
static obs_source_t *findSourceByHost(const QString &host)
{
	ZcHostMatch m{host, nullptr};
	obs_enum_sources(findSourceByHostProc, &m);
	return m.found;
}

/* Live presence query against OBS, never a cached flag (spec §1, L4). */
bool ZcControlDock::isCameraAdded(const QString &host)
{
	if (host.isEmpty())
		return false;
	const QByteArray name = sourceNameForHost(host).toUtf8();
	obs_source_t *src = obs_get_source_by_name(name.constData());
	if (!src)
		src = findSourceByHost(host);
	if (!src)
		return false;
	obs_source_release(src);
	return true;
}

void ZcControlDock::refreshRailPresence()
{
	if (!rail_)
		return;
	QSet<QString> added;
	QListWidget *list = rail_->list();
	for (int i = 0; i < list->count(); ++i) {
		const QString host = list->item(i)->data(kHostRole).toString();
		if (isCameraAdded(host))
			added.insert(host);
	}
	rail_->setAddedHosts(added);
}

void ZcControlDock::onObsSourceSignal(void *data, calldata_t *)
{
	auto *dock = static_cast<ZcControlDock *>(data);
	if (!dock)
		return;
	/* Source signals can fire off the Qt thread; marshal the UI update. */
	QMetaObject::invokeMethod(
	    dock, [dock]() { dock->refreshRailPresence(); },
	    Qt::QueuedConnection);
}

/* A source added from the dock is "ZCamera <ip>", but OBS may already hold a
   single unconfigured default source created manually via its Sources panel —
   named by get_name ("ZCamera", or the older "ZCameraPlugin.SourceName") and
   never given an address. Repurpose that one for this camera (rename it to
   "ZCamera <ip>") instead of leaving a second, duplicate source behind. Returns
   the source addref'd, or null. */
static obs_source_t *reclaimDefaultSource(const std::string &newName)
{
	static const char *candidates[] = {"ZCamera", "ZCameraPlugin.SourceName"};
	for (const char *cand : candidates) {
		obs_source_t *src = obs_get_source_by_name(cand);
		if (!src)
			continue;
		if (strcmp(obs_source_get_id(src), "zcamera_source") != 0) {
			obs_source_release(src);
			continue;
		}
		obs_data_t *sd = obs_source_get_settings(src);
		const char *ip = sd ? obs_data_get_string(sd, "zc_source_ip") : nullptr;
		const bool configured = ip && *ip;
		obs_data_release(sd);
		if (configured) { /* this default is already pointing at a camera */
			obs_source_release(src);
			continue;
		}
		obs_source_set_name(src, newName.c_str());
		return src;
	}
	return nullptr;
}

/* Add the camera's source to the current scene (spec §1, L6/L7). Creates the
   source when it does not exist, and re-applies the address when it does — a
   self-heal for sources saved before get_defaults stopped clobbering
   create-time settings (they hold an empty IP and stay black forever). An
   unchanged IP is ignored by OBS, so a healthy stream is not restarted. Must
   run on the OBS frontend thread. */
void ZcControlDock::addCameraToSource(const QString &host)
{
	if (host.isEmpty())
		return;
	const std::string ip = host.toStdString();
	const std::string name = sourceNameForHost(host).toStdString();

	obs_source_t *src = obs_get_source_by_name(name.c_str());
	obs_data_t *s = obs_data_create();
	/* Same key the source's getproperties uses (PROP_SOURCE_IP). */
	obs_data_set_string(s, "zc_source_ip", ip.c_str());
	if (src) {
		obs_source_update(src, s);
	} else {
		/* A source may already carry this address under another name (e.g.
		   added via the source-IP box as the default "ZCamera"): reuse it and
		   give it the standard name so the rail's by-name check matches and no
		   duplicate is created (bug 15). */
		src = findSourceByHost(QString::fromStdString(ip));
		if (src) {
			obs_source_set_name(src, name.c_str());
			obs_source_update(src, s);
		} else {
			/* First try to reclaim an unconfigured default source rather than
			   stacking a second one. */
			src = reclaimDefaultSource(name);
			if (src)
				obs_source_update(src, s);
			else
				src = obs_source_create("zcamera_source", name.c_str(), s,
							nullptr);
		}
	}
	obs_data_release(s);

	if (src) {
		obs_source_t *sceneSrc = obs_frontend_get_current_scene();
		if (sceneSrc) {
			obs_scene_t *scene = obs_scene_from_source(sceneSrc);
			if (scene &&
			    !obs_scene_find_source(scene, name.c_str()))
				obs_scene_add(scene, src);
			obs_source_release(sceneSrc);
		}
		/* OBS activates the source (and the plugin calls start()) once the scene
		   it is in starts rendering; no manual activation is needed here. */
		obs_source_release(src);
	}
}

/* Remove the "ZCamera <host>" source from OBS entirely (out of every scene).
   A camera that is no longer a source is gone from the rail's "added" state and
   can be re-added later. Runs on the OBS frontend thread. */
void ZcControlDock::removeCameraFromSource(const QString &host)
{
	if (host.isEmpty())
		return;
	const std::string name = sourceNameForHost(host).toStdString();

	obs_source_t *src = obs_get_source_by_name(name.c_str());
	/* A source created from OBS's own Sources panel keys off its stored address,
	   not its name (the rail counts it as added the same way), so removing it must
	   find it the same way rather than leaving its gold check stuck on (bug list
	   "the +/check toggle is out of sync with the source"). */
	if (!src)
		src = findSourceByHost(host);
	if (!src)
		return;
	obs_source_remove(src);  /* out of all scenes; OBS drops its own refs */
	obs_source_release(src); /* release our look-up ref */
}

/* libobs forwards mouse events to a source through the Interaction dialog
   only (UI/window-basic-interaction.cpp is the one caller of
   obs_source_send_mouse_click/_move), so that window is where the crosshair
   and the detection rectangle are drawn and the only place a click reaches
   the plugin. Opening it is the frontend's job, through the source object,
   which is why the dock keeps the lookup. */
void ZcControlDock::openSourceInteraction()
{
	const QString host = currentHost_;
	if (host.isEmpty())
		return;
	/* Already up for this camera: raise it instead of rebuilding it. OBS's own
	   creator closes whatever window is there and constructs a new one, which
	   throws the window's size and position away and makes the picture blink on
	   every press. The tag is what tells one camera's window from another's —
	   the class itself carries no such notion, and its OBSSource member is
	   private. */
	if (QWidget *w = zcFindInteractionWindow();
	    w && w->property(kInteractHostProp).toString() == host) {
		w->raise();
		w->activateWindow();
		return;
	}
	ZcCameraSourceLookup lookup;
	lookup.want = host;
	obs_enum_sources(zcFindCameraSourceProc, &lookup);
	if (!lookup.found) {
		if (statusBarLabel_)
			statusBarLabel_->setText(
			    ztr("ZCameraPlugin.Dock.InteractNoSource").arg(host));
		return;
	}
	blog(LOG_INFO, "[obs-zcamera] opening interaction window for %s",
	     host.toUtf8().constData());
	obs_frontend_open_source_interaction(lookup.found);
	/* The window is built and shown inside that call, so it is already in the
	   widget tree here — no need to wait for an event-loop turn to tag it. */
	if (QWidget *w = zcFindInteractionWindow())
		w->setProperty(kInteractHostProp, host);
}

/* The other half of openSourceInteraction(), driven by the click-to-move
   target's off state. Nothing in the frontend API closes the window, so it is
   closed the same way it is found. */
void ZcControlDock::closeSourceInteraction()
{
	if (QWidget *w = zcFindInteractionWindow())
		w->close();
}

ZcControlDock::~ZcControlDock()
{
	/* Detach before anything else: the source must not be left pointing at a
	   dock whose widgets are gone, and no mode may stay armed. */
	if (overlay_) {
		zc::overlaySetPointerSink(nullptr);
		overlay_->disarm();
	}
	if (obsSignals_) {
		for (const char *signal : {"source_create", "source_destroy",
					   "source_rename"})
			signal_handler_disconnect(obsSignals_, signal,
						  &ZcControlDock::onObsSourceSignal,
						  this);
	}
#ifdef ZC_ENABLE_DEBUG_PANEL
	if (debugPanel_)
		delete debugPanel_;
#endif
	/* A connect probe runs a nested event loop inside client_->connect(); OBS can
	   tear the dock down from an event delivered in it, leaving the probe's frames
	   on the stack to resume when the loop unwinds. Deleting the client here would
	   free its transport under negotiateOrigin(); letting Qt free it as this
	   dock's child would do the same, so when a probe is in flight detach it off
	   the dock and leak it (the process is going down). The resume points check
	   g_controlDockClosing and bail out before touching a freed member. */
	g_controlDockClosing = true;
	if (client_) {
		settings_->setClient(nullptr);
		ptz_->setClient(nullptr);
		presets_->setClient(nullptr);
		framing_->setClient(nullptr);
		if (overlay_)
			overlay_->setClient(nullptr, QString());
		if (opening_)
			client_->setParent(nullptr); /* the probe is still using it */
		else
			delete client_;
		client_ = nullptr;
	}
}

/* Spec §3: the PTZ tab exists only for a camera that reports PTZ support, and
   the auto-framing tab only for one that reports auto framing
   (feature.autoFraming). The two capabilities are independent, so each tab is
   added/removed on its own gate and only when the camera changed. */
void ZcControlDock::updatePtzTab()
{
	if (!tabs_ || !ptzTab_)
		return;

	/* Remember what was on screen: removing a tab does not fall back cleanly,
	   so switch to Settings if the visible pane just went away. */
	QWidget *shown = tabs_->currentWidget();

	const bool wantPtz = client_ && client_->cameraSupportsPtz();
	const int ptzIndex = tabs_->indexOf(ptzTab_);
	if (wantPtz && ptzIndex < 0)
		tabs_->addTab(ptzTab_, ztr("ZCameraPlugin.Dock.Ptz"));
	else if (!wantPtz && ptzIndex >= 0)
		tabs_->removeTab(ptzIndex);

	/* The auto-framing pane is inserted into the tab bar for a camera that
	   reports auto framing (see kFramingPanelVisible). The gate lives here, not
	   at construction, so hiding it again is a one-line change. */
	if (framingTab_ && kFramingPanelVisible) {
		const bool wantFraming =
		    client_ && client_->cameraSupportsAutoFraming();
		const int framingIndex = tabs_->indexOf(framingTab_);
		if (wantFraming && framingIndex < 0)
			tabs_->addTab(framingTab_,
				      ztr("ZCameraPlugin.Dock.Framing"));
		else if (!wantFraming && framingIndex >= 0)
			tabs_->removeTab(framingIndex);
	}

	/* Never leave the pane on a tab that just went away. */
	if (shown && tabs_->indexOf(shown) < 0)
		tabs_->setCurrentWidget(settings_);
}

/* Connect the control client to `host`. Returns false when the camera does not
   answer; the panels are then left without a client, so nothing can be operated
   (spec §1, L11). */
bool ZcControlDock::openCamera(const QString &host)
{
	return openCamera(host, creds_.value(host));
}

bool ZcControlDock::openCamera(const QString &host, const ZcCredentials &creds)
{
	/* connect() answers synchronously but runs a nested event loop (the origin
	   probe), so an event delivered during it can re-enter this path — a queued
	   selectCamera() from the source Properties dialog, or a second click. A
	   re-entrant call would run `delete client_` in openCameraNow() while the
	   current client's connect() frame (and its transport) is still on the stack,
	   freeing the object mid-call: the crash is in negotiateOrigin, at the
	   base_url_ build. Never serve a second request while one is live; remember
	   the pick and serve it after the current attempt returns. */
	if (opening_)
		return false;

	opening_ = true;
	const bool ok = openCameraNow(host, creds);
	/* openCameraNow() ran a nested event loop; OBS may have torn the dock down
	   from an event delivered in it (see g_controlDockClosing). If so, do not
	   touch any member of the freed dock. */
	if (g_controlDockClosing)
		return false;
	opening_ = false;

	/* Serve a pick that arrived while we were connecting. Deferred so the
	   caller's stack (openCameraNow's frames are gone) unwinds first. */
	if (pendingClick_) {
		pendingClick_ = false;
		const QString next = pendingClickHost_;
		pendingClickHost_.clear();
		QTimer::singleShot(0, this, [this, next]() { onCameraClicked(next); });
	}
	return ok;
}

bool ZcControlDock::openCameraNow(const QString &host, const ZcCredentials &creds)
{
	if (client_) {
		/* Detach the panels before the client goes away: a panel holding a
		   pointer to a deleted client would write through it. */
		settings_->setClient(nullptr);
		ptz_->setClient(nullptr);
		presets_->setClient(nullptr);
		framing_->setClient(nullptr);
		if (overlay_)
			overlay_->setClient(nullptr, QString());
		delete client_;
		client_ = nullptr;
	}
	client_ = new ZcCameraClient(this);
	settings_->setClient(client_);
	ptz_->setClient(client_);
	presets_->setClient(client_);
	framing_->setClient(client_);
	/* Point the interaction link at the new camera: the source is found by
	   address, and the old camera's source must lose its mode. */
	if (overlay_)
		overlay_->setClient(client_, host);
	/* The preset/trace transport is greyed while auto-framing drives the head.
	   That state belongs to the PREVIOUS camera, so clear it on every switch;
	   the first framingStatusChanged from the new camera greys it again if it
	   actually needs to be (bug review §4 UI: stuck-disabled panel). */
	presets_->setEnabled(true);
#ifdef ZC_ENABLE_DEBUG_PANEL
	/* Feed the debug panel from this client's transport (see ZcCameraClient). */
	if (debugPanel_)
		connect(client_, &ZcCameraClient::requestLogged, debugPanel_,
			&ZcDebugPanel::append);
#endif

	bool ok = client_->connect(host, 80, creds);
	/* connect() ran a nested event loop (the origin probe); OBS may have torn the
	   dock down from an event delivered in it (see g_controlDockClosing). Bail
	   before touching any member of the freed dock. */
	if (g_controlDockClosing)
		return false;
	if (!ok) {
		/* Distinguish "the camera wants a login we do not have" from "the
		   camera is not there", so the caller can ask for credentials
		   (bug 4) instead of reporting it offline. */
		lastAuthRequired_ = client_->authRequired();
		lastTlsUnavailable_ = client_->tlsUnavailable();
		settings_->setClient(nullptr);
		ptz_->setClient(nullptr);
		presets_->setClient(nullptr);
		framing_->setClient(nullptr);
		if (overlay_)
			overlay_->setClient(nullptr, QString());
		delete client_;
		client_ = nullptr;
		/* No client: the PTZ tab must go, and every control is inert. */
		updatePtzTab();
		statusBarLabel_->setText(
		    ztr("ZCameraPlugin.Dock.OfflineReason").arg(host));
		return false;
	}
	lastAuthRequired_ = false;
	lastTlsUnavailable_ = false;

	statusBarLabel_->setText(ztr("ZCameraPlugin.Dock.Connected").arg(host));
	settings_->hidePrompts();
	client_->refreshStatus();
	settings_->refreshGroup();

	/* The status line, with the playback caveat (bug list #94): a camera left
	   in its playback mode ignores every setting write, and the only symptom
	   was "nothing works". The client already forces record mode on connect
	   (RD note), but the operator can still switch it on the camera itself;
	   when that happens the line says so in amber instead of the plain
	   connected text. */
	const auto updateModeStatus = [this]() {
		if (!client_)
			return;
		const QString mode = client_->mode();
		if (mode.contains(QLatin1String("play"),
				  Qt::CaseInsensitive)) {
			statusBarLabel_->setStyleSheet(
			    "color:#d8b878;padding:2px 6px;");
			statusBarLabel_->setText(
			    ztr("ZCameraPlugin.Dock.PlaybackMode")
				.arg(currentHost_));
		} else {
			statusBarLabel_->setStyleSheet(
			    "color:#8a9bb2;padding:2px 6px;");
			statusBarLabel_->setText(
			    ztr("ZCameraPlugin.Dock.ConnectedMode")
				.arg(currentHost_)
				.arg(mode));
		}
	};
	connect(client_, &ZcCameraClient::statusChanged, this,
		[this, updateModeStatus](const QJsonObject &status) {
			if (client_)
				updateModeStatus();
			if (recBadge_ &&
			    status.contains(QLatin1String("recording")))
				recBadge_->setVisible(
				    status.value(QLatin1String("recording"))
					.toBool());
		});
	connect(client_, &ZcCameraClient::modeChanged, this,
		[updateModeStatus](const QString &) { updateModeStatus(); });
	/* The REC badge (bug list #20). A fresh client starts not recording;
	   from here the camera's own notifications keep it true. Some firmwares
	   restate the recording flag in camera_status, so honour it when the
	   payload carries it -- that also covers "already recording when we
	   connected", which no RecStarted event will arrive for. */
	if (recBadge_)
		recBadge_->hide();
	connect(client_, &ZcCameraClient::recordingStarted, this,
		[this]() {
			if (recBadge_)
				recBadge_->show();
			if (framing_)
				framing_->setRecording(true);
		});
	connect(client_, &ZcCameraClient::recordingStopped, this,
		[this]() {
			if (recBadge_)
				recBadge_->hide();
			if (framing_)
				framing_->setRecording(false);
		});
	/* A duration tick means the camera is recording right now -- this is
	   also how a dock that attached mid-recording learns it, when the
	   firmware's camera_status carries no recording flag for it. */
	connect(client_, &ZcCameraClient::recordingDuration, this,
		[this](int seconds) {
			if (recBadge_ && seconds > 0)
				recBadge_->show();
			if (framing_)
				framing_->setRecording(seconds > 0);
		});
	/* The camera's notification socket is a live heartbeat: when it drops (power
	   loss / network drop) the row must show offline without the operator
	   clicking or refreshing (bug 49), and when it comes back the row clears. The
	   WS layer auto-reconnects, so a blip marks offline briefly then online
	   again — which is what actually happened. */
	connect(client_, &ZcCameraClient::notifierStateChanged, this,
		[this](bool live) {
			if (!client_ || currentHost_.isEmpty())
				return;
			if (live) {
				reachableHosts_.insert(currentHost_);
				unreachableHosts_.remove(currentHost_);
			} else {
				reachableHosts_.remove(currentHost_);
				unreachableHosts_.insert(currentHost_);
			}
			if (rail_)
				rail_->setHostOffline(currentHost_, !live);
		});
	/* Bug 17: read the camera's tracking-box ("range") ROI and mirror it
	   into the matching source's overlay, so the monitor window draws the
	   region frame the web preview shows. Re-run whenever the framing
	   configuration changes. */
	const auto pushTrackBox = [this]() {
		if (!client_)
			return;
		client_->framingTrackingBoxQuery([this](const QJsonObject &o) {
			if (!client_ || !overlay_)
				return;
			const QJsonValue en = o.value("enable");
			const bool on = en.type() == QJsonValue::Double
						? en.toInt() == 1
						: en.toBool();
			overlay_->setTrackBox(on,
					      (float)o.value("left").toDouble(),
					      (float)o.value("top").toDouble(),
					      (float)o.value("width").toDouble(),
					      (float)o.value("height")
						  .toDouble());
		});
	};

	/* The operator's own box switch does not come back as a settings-changed
	   event (that one is for changes made elsewhere), so the frame the monitor
	   window draws would only appear after some other write (bug 53). */
	if (framing_)
		framing_->onBoxStateChanged = pushTrackBox;

	/* /info arrives asynchronously: the tab appears (or disappears) and the
	   preset list loads once the model is known. */
	connect(client_, &ZcCameraClient::cameraInfoChanged, this,
		[this, pushTrackBox]() {
			updatePtzTab();
			if (framing_)
				framing_->setFirmwareNew(
					client_ &&
					client_->firmwareAtLeast("1.0.5"));
			if (client_ && client_->cameraSupportsPtz()) {
				presets_->refresh();
				/* Seed the position read-out and the range-limit
				   sliders from the camera (bug list: the Range
				   Limit group). */
				ptz_->refreshState();
				/* The PTZ catalog carries the keys the PTZ tab
				   and the preset pane depend on (`ptz_limit` for
				   the Range Limit group, `ptz_speed_mode` for
				   the per-preset speed row); read it once so
				   those dependencies are known without the
				   operator having to open the group. */
				client_->readGroup(QLatin1String("ptz"),
						   nullptr);
			}
			if (client_ && client_->cameraSupportsAutoFraming())
				framing_->refresh();
			pushTrackBox();
		});
	/* Live position (ptzfChange): the read-out follows the head as it moves. */
	connect(client_, &ZcCameraClient::ptzPositionChanged, this,
		[this](double pan, double tilt, double) {
			if (ptz_)
				ptz_->setPosition(pan, tilt);
		});
	/* Live framing notifications: the status event carries the switches, so the
	   panel repaints straight from it; a settings change just means re-read. */
	connect(client_, &ZcCameraClient::framingStatusChanged, this,
		[this](const QJsonObject &status) {
			if (framing_)
				framing_->applyStatus(status);
		});
	connect(client_, &ZcCameraClient::framingSettingsChanged, this,
		[this, pushTrackBox]() {
			if (framing_)
				framing_->refresh();
			/* The box ROI may be what changed (bug 17). */
			pushTrackBox();
		});
	/* Framing ownership of the head: the panel reports it, because the panel is
	   what every snapshot lands in — the websocket event, the re-read after a
	   settings change, and the one read below. While framing drives the head
	   the camera refuses every trace command with code -2, so the trace face
	   greys rather than swallowing the click; the preset family keeps working
	   (bug lists 55/179). */
	if (framing_)
		framing_->onFramingOwnsPtz = [this](bool owned) {
			if (presets_)
				presets_->setFramingOwned(owned);
		};
	/* The WS event only reports framing CHANGES. If the camera was already
	   framing when we connected, the transport must be greyed now — waiting
	   for the next event would leave the panel claiming to work while every
	   trace click is ignored. One snapshot read settles it (and reaches the
	   trace face through the callback above). */
	if (client_->cameraSupportsAutoFraming())
		client_->framingQuery([this](const QJsonObject &status) {
			if (framing_)
				framing_->applyStatus(status);
		});
	/* A camera change must not leave the previous camera's PTZ tab behind. */
	updatePtzTab();
	return true;
}

/* Reconnect schedule after a write that makes the camera restart its web service
   (HTTPS, identity auth, Crop Sensor). The camera is unreachable for tens of
   seconds, so the retry gap grows 20 s -> 40 s -> 90 s (RD feedback). */
static constexpr int kRestartBackoffMs[] = {20000, 40000, 90000};
static constexpr int kRestartBackoffCount =
    sizeof(kRestartBackoffMs) / sizeof(kRestartBackoffMs[0]);

/* Bug 8: a write that makes the camera restart its web service (HTTPS,
   identity auth) or move to another address (network type) is applied by a
   camera that is briefly unreachable. Say so, then poll for it rather than
   leaving the pane dead. */
void ZcControlDock::onCameraRestarting()
{
	if (currentHost_.isEmpty() || !restartTimer_)
		return;
	statusBarLabel_->setText(
	    ztr("ZCameraPlugin.Dock.CameraRestarting").arg(currentHost_));
	restartAttempts_ = 0;
	/* A restarting camera is offline for tens of seconds, so the first probe
	   waits rather than hammering it (RD feedback: back off ~20 s, 40 s, 90 s). */
	restartTimer_->setInterval(kRestartBackoffMs[0]);
	restartTimer_->start();
}

void ZcControlDock::tryReconnect()
{
	const QString host = currentHost_;
	if (host.isEmpty()) {
		restartTimer_->stop();
		return;
	}
	/* A bounded number of tries: a camera that never comes back, or comes back
	   on another address, must not leave a timer polling for ever. */
	if (++restartAttempts_ > 8) {
		restartTimer_->stop();
		statusBarLabel_->setText(
		    ztr("ZCameraPlugin.Dock.OfflineReason").arg(host));
		return;
	}
	const bool reconnected = openCamera(host);
	/* The probe may have torn the dock down (see g_controlDockClosing): stop
	   before touching a freed member. */
	if (g_controlDockClosing)
		return;
	if (reconnected) {
		restartTimer_->stop();
		reachableHosts_.insert(host);
		unreachableHosts_.remove(host);
		refreshRailPresence();
		return;
	}
	/* The restart applied a login (identity authentication): ask for it rather
	   than polling for a camera that will keep answering 401 (bug 4). */
	if (lastAuthRequired_) {
		restartTimer_->stop();
		settings_->showLoginPrompt(host);
		return;
	}
	/* HTTPS-only camera with no TLS backend: point the operator to http, in the
	   pane. */
	if (lastTlsUnavailable_) {
		restartTimer_->stop();
		settings_->showTlsPrompt(host);
		return;
	}
	/* Still down: widen the gap for the next probe (20 s, 40 s, then 90 s). */
	restartTimer_->setInterval(
	    kRestartBackoffMs[qMin(restartAttempts_, kRestartBackoffCount - 1)]);
	/* openCamera() reported the failed attempt; keep the restarting notice up
	   while the camera is still coming back. */
	statusBarLabel_->setText(
	    ztr("ZCameraPlugin.Dock.CameraRestarting").arg(host));
}

/* The in-page login form was submitted: keep the credentials for this session
   and reconnect so the settings pane appears (bug 4). */
void ZcControlDock::onLoginSubmitted(const QString &user, const QString &password)
{
	if (currentHost_.isEmpty())
		return;
	ZcCredentials creds;
	creds.username = user;
	creds.password = password;
	creds_[currentHost_] = creds;
	/* Mirror into the shared store so the source's Properties bitrate push
	   can authenticate to this camera too — the source has no login UI of
	   its own and reads credentials back from the store (memory-only). */
	zc::rememberCredentials(currentHost_, creds);
	/* The form can be submitted while a connect is still probing (the probe runs
	   a nested event loop). openCamera() would refuse and this handler would then
	   wrongly report the password as rejected, so defer the retry instead. */
	if (opening_) {
		pendingClick_ = true;
		pendingClickHost_ = currentHost_;
		return;
	}
	const bool ok = openCamera(currentHost_, creds);
	/* The probe may have torn the dock down (see g_controlDockClosing): stop
	   before touching a freed member. */
	if (g_controlDockClosing)
		return;
	if (ok) {
		loginAttempts_ = 0;
		reachableHosts_.insert(currentHost_);
		unreachableHosts_.remove(currentHost_);
	} else if (lastAuthRequired_) {
		/* The camera still challenged us: the credentials are wrong. Keep the
		   form up and say the login was rejected, so the click does not look
		   like it did nothing (bug list "wrong credentials not validated").
		   Only claim rejection when the camera actually challenged: a network
		   timeout here is not a wrong password, and telling the operator the
		   password was rejected sends them chasing the wrong thing.
		   Five rejections in a row is the camera's own lockout (bug list #3),
		   so at that point the form says the account is locked and stops
		   accepting clicks. */
		++loginAttempts_;
		if (loginAttempts_ >= 5)
			settings_->showLoginLocked();
		else
			settings_->showLoginError();
	} else if (g_controlDockClosing == false) {
		/* A failure that is not a rejection (timeout, unreachable) does not
		   count toward the lockout. */
		loginAttempts_ = 0;
	}
	refreshRailPresence();
}

/* Select the camera an OBS source represents. The source's Properties dialog
   opens the dock for that camera, so it must follow the source the operator
   clicked rather than leave the last one selected (bug 27). Selecting the same
   host again is a no-op: re-opening Properties must not reset the page or drop a
   half-entered login. */
void ZcControlDock::selectCamera(const QString &host)
{
	if (host.isEmpty())
		return;
	if (host == currentHost_ && client_)
		return;
	onCameraClicked(host);
}

/* Spec §1, L2: selecting a camera connects the control client only. Adding it
   to OBS is a separate, explicit action. Spec §1, L11: whether it answered is
   remembered, so a source-only row that is really there stays usable and one
   that is not stays marked offline. */
void ZcControlDock::onCameraClicked(const QString &host)
{
	/* An explicit pick ends any wait for a restarting camera — before the
	   deferral below too, otherwise the previous camera's pending restart probe
	   fires after the new camera connects and tears its connection down. Clearing
	   the attempt count makes a later restart start its backoff afresh. */
	if (restartTimer_)
		restartTimer_->stop();
	restartAttempts_ = 0;

	/* A connect is still in flight (its origin probe runs a nested event loop, so
	   this handler can be re-entered from an event it delivers). openCamera()
	   cannot be served now without freeing the live client, so remember the pick
	   and let openCamera() replay it once the current attempt returns. Skip the
	   offline bookkeeping: nothing was attempted for this host yet. */
	if (opening_) {
		qWarning().noquote()
			<< "[obs-zcamera] click deferred, connect in progress" << host;
		currentHost_ = host;
		pendingClick_ = true;
		pendingClickHost_ = host;
		return;
	}
	currentHost_ = host;
	bool ok = openCamera(host);
	/* The probe may have torn the dock down (see g_controlDockClosing): stop
	   before touching a freed member. */
	if (g_controlDockClosing)
		return;
	qWarning().noquote() << "[obs-zcamera] click" << host << "ok" << ok
			     << "authReq" << lastAuthRequired_
			     << "tlsUnavail" << lastTlsUnavailable_;
	/* The camera answered 401: it wants a login we do not have. Show the in-page
	   login form instead of a modal dialog. */
	if (!ok && lastAuthRequired_)
		settings_->showLoginPrompt(host);
	/* HTTPS-only camera with no TLS backend: show the switch-to-http notice in
	   the pane. */
	else if (!ok && lastTlsUnavailable_)
		settings_->showTlsPrompt(host);
	/* A camera that answered 401 is a camera that is THERE, waiting for a
	   login; an HTTPS-only camera we cannot reach because this build has no TLS
	   backend is likewise present. Marking either row offline told the operator
	   the camera had gone away when it was really only refusing control (bug:
	   an auth-enabled camera with no password entered showed as offline). The
	   pane already says what is wanted — the login form, or the switch-to-http
	   notice — so the row keeps its normal look. */
	const bool reachable = ok || lastAuthRequired_ || lastTlsUnavailable_;
	if (reachable) {
		reachableHosts_.insert(host);
		unreachableHosts_.remove(host);
	} else {
		reachableHosts_.remove(host);
		unreachableHosts_.insert(host);
	}
	if (rail_)
		rail_->setHostOffline(host, !reachable);
	refreshRailPresence();
}

} // namespace zc